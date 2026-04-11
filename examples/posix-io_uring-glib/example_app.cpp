#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "examples/posix-io_uring-glib/example_app.h"

#include <libbounce/promise.h>
#include <libbounce/posix_glib.h>

#include <errno.h>
#include <fcntl.h>
#include <gtk/gtk.h>
#include <liburing.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <string>

namespace libbounce_example::posix_io_uring_glib {
namespace {

// Keep path handling generous so deep build directories still fit.
constexpr size_t path_buffer_length = 32768u;

/**
 * Example-local mutable state shared between GTK callbacks and the coroutine.
 */
struct example_app {
  libbounce::promise<void> write_operation;
  GtkWidget *window_handle = nullptr;
  GtkWidget *button_handle = nullptr;
  bool write_in_flight = false;
  bool automate = false;  // For testing purpose
};

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

/////////////////////////////////////////////////////////////////////////////

/**
 * Prepare one `io_uring` write request from coroutine-owned state.
 */
struct write_request {
  int fd = -1;
  const char *buffer = nullptr;
  size_t length = 0u;
  off_t offset = 0;
};

static void prepare_write_request(
  struct io_uring_sqe *sqe,
  void *prepare_state) noexcept {
  write_request *request = static_cast<write_request *>(prepare_state);

  io_uring_prep_write(
    sqe,
    request->fd,
    request->buffer,
    request->length,
    request->offset);
}

/**
 * Write the whole payload through true async `io_uring` write submissions.
 */
static libbounce::promise<bool> write_all_bytes_async(
  libbounce::bounce_ref bounce_handle,
  int fd,
  const char *buffer,
  size_t length) {
  size_t written = 0u;

  while (written < length) {
    write_request request {};
    libbounce::io_uring_operation operation(
      &prepare_write_request,
      &request);
    const size_t remaining = length - written;

    request.fd = fd;
    request.buffer = buffer + written;
    request.length = remaining;
    request.offset = (off_t)written;
    const libbounce::await_result await_result =
      co_await bounce_handle.await(*operation.get_operation(), nullptr);
    if (!await_result.completed()) {
      co_return false;
    }

    if (operation.result() <= 0) {
      co_return false;
    }
    written += (size_t)operation.result();
  }

  co_return true;
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
 * Open the destination fd and write the sample payload through awaitable
 * `io_uring` completions.
 *
 * The sequence is:
 * 1. Build the destination path next to the running executable.
 * 2. Open `sample.txt` for writing.
 * 3. Resolve the current parked bounce as `bounce_ref`.
 * 4. `co_await` one `io_uring` write submission at a time until the payload
 *    is fully written.
 */
static libbounce::promise<void> write_sample_file_async(example_app *app) {
  std::string output_path;
  if (!build_output_path(output_path)) {
    finish_write(app, app->automate);
    co_return;
  }

  int file_fd = open(
    output_path.c_str(),
    O_CREAT | O_TRUNC | O_WRONLY,
    S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (file_fd < 0) {
    finish_write(app, app->automate);
    co_return;
  }

  auto current_bounce = libbounce::bounce::get_current();
  if (!current_bounce) {
    (void)close(file_fd);
    finish_write(app, app->automate);
    co_return;
  }

  (void)co_await write_all_bytes_async(
    current_bounce,
    file_fd,
    sample_file_text,
    sample_file_text_length);

  (void)close(file_fd);
  finish_write(app, app->automate);
  co_return;
}

/**
 * Start the write coroutine once per button press.
 * Repeated clicks while the previous write is in flight are ignored.
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

/////////////////////////////////////////////////////////////////////////////

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

int run(libbounce::bounce &bounce_instance, int *argc, char ***argv) noexcept {
  example_app app;

  app.automate = getenv(automation_env_name) != nullptr;
  // `gtk_init_check()` is used so the sample can return a clean error code in
  // headless or misconfigured environments instead of aborting the process.
  if (!gtk_init_check(argc, argv)) {
    return 1;
  }

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
  // `main()` already published this bounce as the GUI thread default, and
  // `bounce_init_with_main_context()` bound it to GTK's
  // `GMainContext`, libbounce now pumps both GTK work and bounce-ready work
  // from the same thread until `shutdown()` drains any pending wait work.
  (void)bounce_instance.park();

  return 0;
}

}  // namespace libbounce_example::posix_io_uring_glib
