#include "examples/posix-glib/example_app.h"

#include <libbounce/promise.h>
#include <libbounce/posix_glib.h>

#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

#include <array>
#include <string>
#include <utility>

namespace libbounce_example::posix_glib {
namespace {

// Keep path handling generous so deep build directories still fit.
constexpr size_t path_buffer_length = 32768u;

/**
 * Example-local mutable state shared between GTK callbacks, the libbounce
 * coroutine, and the worker thread that performs the blocking file write.
 */
struct example_app {
  libbounce::promise<void> write_operation;
  std::thread write_thread;
  GtkWidget *window_handle = nullptr;
  GtkWidget *button_handle = nullptr;
  GMainContext *gtk_main_context = nullptr;
  int notify_read_fd = -1;
  bool write_in_flight = false;
  bool automate = false;
};

/**
 * Close a POSIX file descriptor when it is still open and reset the slot.
 */
static void close_fd_if_needed(int *fd) noexcept {
  if (*fd >= 0) {
    (void)close(*fd);
    *fd = -1;
  }
}

/**
 * Join a worker thread only when it owns a live thread object.
 */
static void join_thread_if_needed(std::thread &thread) noexcept {
  if (thread.joinable()) {
    thread.join();
  }
}

/**
 * Make the helper pipe unavailable to unrelated exec children.
 */
static bool mark_fd_close_on_exec(int fd) noexcept {
  const int flags = fcntl(fd, F_GETFD);

  return (flags >= 0) && (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0);
}

/**
 * Resolve `<executable-directory>/sample.txt`.
 * This keeps the sample independent from the process working directory.
 */
static bool build_output_path(std::string &output_path) noexcept {
  std::array<char, path_buffer_length> executable_path {};
  ssize_t length = readlink(
    "/proc/self/exe",
    executable_path.data(),
    executable_path.size() - 1u);
  char *last_separator;

  if ((length <= 0) || ((size_t)length >= (executable_path.size() - 1u))) {
    return false;
  }

  executable_path[(size_t)length] = '\0';
  last_separator = strrchr(executable_path.data(), '/');
  if (last_separator == nullptr) {
    return false;
  }

  output_path.assign(executable_path.data(), (size_t)(last_separator - executable_path.data()));
  output_path.push_back('/');
  output_path.append(output_file_name);
  return true;
}

/**
 * Write the whole payload even if `write(2)` completes partially.
 */
static bool write_all_bytes(int fd, const char *buffer, size_t length) noexcept {
  size_t written = 0u;

  while (written < length) {
    const ssize_t result = write(fd, buffer + written, length - written);

    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    written += (size_t)result;
  }
  return true;
}

/**
 * Send a one-byte completion marker through the worker-to-GUI pipe.
 */
static void write_signal_byte(int fd, unsigned char value) noexcept {
  while (write(fd, &value, sizeof value) < 0) {
    if (errno != EINTR) {
      break;
    }
  }
}

/**
 * Read the worker completion marker from the pipe.
 * The exact value is not important to the example, but consuming the byte
 * closes the loop and lets the fd watch settle cleanly.
 */
static void read_signal_byte(int fd, unsigned char *value) noexcept {
  for (;;) {
    const ssize_t result = read(fd, value, sizeof *value);

    if (result >= 0) {
      return;
    }
    if (errno != EINTR) {
      return;
    }
  }
}

/**
 * Perform the blocking file write on a helper thread.
 *
 * The POSIX+GLib backend naturally awaits file descriptor readiness, but
 * regular file writes are usually immediately writable and do not become
 * meaningfully asynchronous on their own. This worker thread turns the sample
 * write into an externally completed operation and notifies the GUI thread
 * through a pipe that libbounce can await.
 */
static void write_sample_file_worker(
  std::string output_path,
  int notify_write_fd,
  GMainContext *gtk_main_context) noexcept {
  unsigned char result_byte = 0u;
  int file_fd;

  file_fd = open(
    output_path.c_str(),
    O_CREAT | O_TRUNC | O_WRONLY,
    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (file_fd >= 0) {
    if (write_all_bytes(file_fd, sample_file_text, sample_file_text_length)) {
      result_byte = 1u;
    }
    (void)close(file_fd);
  }

  write_signal_byte(notify_write_fd, result_byte);
  if (gtk_main_context != nullptr) {
    g_main_context_wakeup(gtk_main_context);
  }
  (void)close(notify_write_fd);
}

/**
 * Restore the UI to its idle state once the async write path settles.
 */
static void finish_write(example_app *app, bool close_after_write) noexcept {
  app->write_in_flight = false;
  if ((app->button_handle != nullptr) && GTK_IS_WIDGET(app->button_handle)) {
    gtk_widget_set_sensitive(app->button_handle, TRUE);
  }
  if (close_after_write &&
      (app->window_handle != nullptr) &&
      GTK_IS_WINDOW(app->window_handle)) {
    gtk_window_close(GTK_WINDOW(app->window_handle));
  }
}

/**
 * Write the sample file and resume on the parked GTK thread via libbounce.
 *
 * The sequence is:
 * 1. Build the destination path next to the running executable.
 * 2. Create a pipe used only as a libbounce-awaitable completion signal.
 * 3. Start a worker thread that writes the file and signals the pipe.
 * 4. Resolve the current parked bounce as `bounce_ref` and `co_await` the
 *    readable end.
 * 5. Close the pipe, join the worker, and re-enable the button.
 */
static libbounce::promise<void> write_sample_file_async(example_app *app) {
  std::string output_path;
  int pipe_fds[ 2 ] = { -1, -1 };
  unsigned char result_byte = 0u;

  if (!build_output_path(output_path)) {
    finish_write(app, app->automate);
    co_return;
  }

  if (pipe(pipe_fds) != 0) {
    finish_write(app, app->automate);
    co_return;
  }
  if (!mark_fd_close_on_exec(pipe_fds[0]) ||
      !mark_fd_close_on_exec(pipe_fds[1])) {
    close_fd_if_needed(&pipe_fds[0]);
    close_fd_if_needed(&pipe_fds[1]);
    finish_write(app, app->automate);
    co_return;
  }

  app->notify_read_fd = pipe_fds[0];
  try {
    // The worker thread does the actual blocking write, while the parked GTK
    // thread waits only on the pipe's readable end through libbounce.
    app->write_thread = std::thread(
      write_sample_file_worker,
      std::move(output_path),
      pipe_fds[1],
      app->gtk_main_context);
    pipe_fds[1] = -1;
  } catch (...) {
    close_fd_if_needed(&pipe_fds[0]);
    close_fd_if_needed(&pipe_fds[1]);
    app->notify_read_fd = -1;
    finish_write(app, app->automate);
    co_return;
  }

  auto current_bounce = libbounce::bounce::get_current();

  auto await_result =
    co_await current_bounce.await(app->notify_read_fd, G_IO_IN, nullptr);

  if (await_result.completed()) {
    read_signal_byte(app->notify_read_fd, &result_byte);
    (void)result_byte;
  }

  close_fd_if_needed(&app->notify_read_fd);
  join_thread_if_needed(app->write_thread);
  finish_write(app, app->automate);
  co_return;
}

/**
 * Start the write coroutine once per click and disable the button while active.
 */
static void begin_write(example_app *app) noexcept {
  if (app->write_in_flight) {
    return;
  }

  app->write_in_flight = true;
  if (app->button_handle != nullptr) {
    gtk_widget_set_sensitive(app->button_handle, FALSE);
  }

  app->write_operation = write_sample_file_async(app);
  if (!app->write_operation.start()) {
    finish_write(app, app->automate);
  }
}

/**
 * GTK button callback that kicks off the sample write.
 */
static void on_button_clicked(GtkButton * /*button*/, gpointer parameter) {
  begin_write(static_cast<example_app *>(parameter));
}

/**
 * Window-destroy callback.
 * Closing the GTK window is the application shutdown signal, so it forwards to
 * `bounce.shutdown()` so the parked GUI thread leaves only after in-flight
 * bounce waits have settled.
 */
static void on_window_destroy(GtkWidget *widget, gpointer parameter) {
  example_app *app = static_cast<example_app *>(parameter);
  auto current_bounce = libbounce::bounce::get_current();

  (void)widget;
  app->button_handle = nullptr;
  app->window_handle = nullptr;
  current_bounce.shutdown();
}

/**
 * Hidden smoke-test helper that clicks the button after startup.
 */
static gboolean automate_click_on_main_thread(gpointer parameter) {
  example_app *app = static_cast<example_app *>(parameter);

  if ((app->button_handle != nullptr) && GTK_IS_BUTTON(app->button_handle)) {
    gtk_button_clicked(GTK_BUTTON(app->button_handle));
  }
  return G_SOURCE_REMOVE;
}

}  // namespace

int run(int *argc, char ***argv) noexcept {
  example_app app;

  app.automate = getenv(automation_env_name) != nullptr;
  // `gtk_init_check()` is used so the sample can return a clean error code in
  // headless or misconfigured environments instead of aborting the process.
  if (!gtk_init_check(argc, argv)) {
    return 1;
  }

  // The key point of this example is that libbounce is bound to the same GLib
  // main context that GTK already uses, so the GUI thread itself can be parked
  // without creating a separate parker thread.
  app.gtk_main_context = g_main_context_default();
  libbounce::bounce bounce_instance(app.gtk_main_context);

  app.window_handle = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  if (app.window_handle == nullptr) {
    return 1;
  }

  gtk_window_set_title(GTK_WINDOW(app.window_handle), window_title);
  gtk_window_set_default_size(GTK_WINDOW(app.window_handle), 240, 90);
  gtk_window_set_resizable(GTK_WINDOW(app.window_handle), FALSE);

  app.button_handle = gtk_button_new_with_label(button_text);
  if (app.button_handle == nullptr) {
    gtk_widget_destroy(app.window_handle);
    return 1;
  }

  gtk_container_add(GTK_CONTAINER(app.window_handle), app.button_handle);
  g_signal_connect(
    app.button_handle,
    "clicked",
    G_CALLBACK(on_button_clicked),
    &app);
  g_signal_connect(
    app.window_handle,
    "destroy",
    G_CALLBACK(on_window_destroy),
    &app);

  gtk_widget_show_all(app.window_handle);
  if (app.automate) {
    g_idle_add(automate_click_on_main_thread, &app);
  }

  // This becomes the only blocking loop on the GTK thread. Because
  // `bounce_init_with_main_context()` bound the bounce to GTK's
  // `GMainContext`, libbounce now pumps both GTK work and bounce-ready work
  // from the same thread until `shutdown()` drains any pending wait work. The
  // C++ wrapper publishes the current core while parked so callbacks and
  // coroutine continuations can resolve it on this GUI thread.
  (void)bounce_instance.park();

  close_fd_if_needed(&app.notify_read_fd);
  join_thread_if_needed(app.write_thread);

  return 0;
}

}  // namespace libbounce_example::posix_glib
