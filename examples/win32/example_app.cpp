#include "examples/win32/example_app.h"

#if defined(_WIN32)

#include <libbounce/promise.h>
#include <libbounce/win32.h>

#include <array>
#include <string.h>
#include <wchar.h>

namespace libbounce_example::win32 {
namespace {

// The UI is intentionally tiny: one window and one push button.
constexpr int example_button_id = 1001;
// Use a large path buffer so the example works even in deep runtime paths.
constexpr DWORD path_buffer_length = 32768u;

/**
 * Example-local mutable state shared between Win32 callbacks and the coroutine.
 * The owning bounce itself lives in `run()`, while the coroutine is stored
 * here so its lifetime stays tied to the window.
 */
struct example_app {
  libbounce::promise<void> write_operation;
  HWND window_handle = NULL;
  HWND button_handle = NULL;
  bool write_in_flight = false;
};

/**
 * Resolve `<exe-directory>\\sample.txt`.
 * This keeps the example independent from the current working directory.
 */
static bool build_output_path(
  std::array<wchar_t, path_buffer_length> &output_path) noexcept {
  const DWORD length = GetModuleFileNameW(
    NULL,
    output_path.data(),
    static_cast<DWORD>(output_path.size()));
  wchar_t *last_separator;
  const size_t file_name_length = wcslen(output_file_name);

  if ((length == 0u) || (length >= output_path.size())) {
    return false;
  }

  last_separator = wcsrchr(output_path.data(), L'\\');
  if (last_separator == nullptr) {
    return false;
  }

  last_separator[1] = L'\0';
  if (((size_t)(last_separator + 1 - output_path.data()) + file_name_length + 1u) >
    output_path.size()) {
    return false;
  }

  memcpy(
    last_separator + 1,
    output_file_name,
    (file_name_length + 1u) * sizeof(wchar_t));
  return true;
}

/////////////////////////////////////////////////////////////////////////////

/**
 * Return the UI to the idle state after the async write path settles.
 * The button is only re-enabled while the control still belongs to a live
 * window because shutdown destroys the window before the coroutine may finish.
 */
static void finish_write(example_app *app) noexcept {
  app->write_in_flight = false;
  if ((app->button_handle != NULL) && IsWindow(app->button_handle)) {
    (void)EnableWindow(app->button_handle, TRUE);
  }
}

/**
 * Write the whole payload, awaiting the OVERLAPPED completion event before
 * every `WriteFile()` step.
 */
static libbounce::promise<bool> write_all_bytes_async(
  libbounce::bounce_ref bounce_handle,
  HANDLE file_handle,
  const char *buffer,
  size_t length) {
  HANDLE signal_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (signal_event == NULL) {
    co_return false;
  }

  size_t written = 0u;
  while (written < length) {
    if (!ResetEvent(signal_event)) {
      (void)CloseHandle(signal_event);
      co_return false;
    }

    const size_t remaining_length = length - written;
    const DWORD write_length =
      remaining_length > (size_t)MAXDWORD ? MAXDWORD : (DWORD)remaining_length;
    const unsigned long long file_offset = (unsigned long long)written;

    OVERLAPPED overlapped {};
    overlapped.hEvent = signal_event;
    overlapped.Offset = (DWORD)(file_offset & 0xffffffffull);
    overlapped.OffsetHigh = (DWORD)(file_offset >> 32);

    // Do write
    if (!WriteFile(
          file_handle,
          buffer + written,
          write_length,
          NULL,
          &overlapped)) {
      if (GetLastError() != ERROR_IO_PENDING) {
        (void)CloseHandle(signal_event);
        co_return false;
      }
    } else {
      if (!SetEvent(signal_event)) {
        (void)CloseHandle(signal_event);
        co_return false;
      }
    }

    // Asynchronous await wrote
    auto await_result =
      co_await bounce_handle.await(signal_event, nullptr);
    if (!await_result.completed()) {
      (void)CancelIoEx(file_handle, &overlapped);
      (void)CloseHandle(signal_event);
      co_return false;
    }

    DWORD bytes_written = 0u;
    if (!GetOverlappedResult(file_handle, &overlapped, &bytes_written, FALSE)) {
      (void)CloseHandle(signal_event);
      co_return false;
    }
    if (bytes_written == 0u) {
      (void)CloseHandle(signal_event);
      co_return false;
    }

    written += (size_t)bytes_written;
  }

  (void)CloseHandle(signal_event);
  co_return true;
}

/**
 * Write the sample file with native overlapped I/O and resume on libbounce.
 *
 * The sequence is:
 * 1. Create the destination file with `FILE_FLAG_OVERLAPPED`.
 * 2. Resolve the current parked bounce as `bounce_ref`.
 * 3. `co_await` `write_all_bytes_async(...)` until the payload is fully written.
 * 4. Close the file handle after the write coroutine settles.
 */
static libbounce::promise<void> write_sample_file_async(example_app *app) {
  std::array<wchar_t, path_buffer_length> output_path {};
  if (!build_output_path(output_path)) {
    finish_write(app);
    co_return;
  }

  HANDLE file_handle = CreateFileW(
    output_path.data(),
    GENERIC_WRITE,
    FILE_SHARE_READ,
    NULL,
    CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
    NULL);
  if (file_handle == INVALID_HANDLE_VALUE) {
    finish_write(app);
    co_return;
  }

  auto current_bounce = libbounce::bounce::get_current();
  if (!current_bounce) {
    (void)CloseHandle(file_handle);
    finish_write(app);
    co_return;
  }

  (void)co_await write_all_bytes_async(
    current_bounce,
    file_handle,
    sample_file_text,
    sample_file_text_length);

  (void)CloseHandle(file_handle);
  finish_write(app);
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
  if (app->button_handle != NULL) {
    (void)EnableWindow(app->button_handle, FALSE);
  }

  app->write_operation = write_sample_file_async(app);
  if (!app->write_operation.start()) {
    finish_write(app);
  }
}

/////////////////////////////////////////////////////////////////////////////

/**
 * Minimal window procedure.
 *
 * `WM_NCCREATE` stores the application state pointer so later messages can
 * access the bounce instance and UI handles without globals.
 */
static LRESULT CALLBACK example_window_proc(
  HWND window_handle,
  UINT message,
  WPARAM wparam,
  LPARAM lparam) noexcept {
  example_app *app = reinterpret_cast<example_app *>(
    GetWindowLongPtrW(window_handle, GWLP_USERDATA));

  switch (message) {
    case WM_NCCREATE: {
      auto *create_struct = reinterpret_cast<CREATESTRUCTW *>(lparam);

      app = static_cast<example_app *>(create_struct->lpCreateParams);
      if (app == nullptr) {
        return FALSE;
      }

      app->window_handle = window_handle;
      (void)SetWindowLongPtrW(
        window_handle,
        GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(app));
      return TRUE;
    }

    case WM_COMMAND:
      // The example reacts only to the single push button.
      if ((LOWORD(wparam) == example_button_id) &&
          (HIWORD(wparam) == BN_CLICKED) &&
          (app != nullptr)) {
        begin_write(app);
        return 0;
      }
      break;

    case WM_CLOSE:
      if (app != nullptr) {
        auto current_bounce = libbounce::bounce::get_current();

        // Closing the native window comes first, then `shutdown()` asks the
        // parker to leave only after any in-flight bounce wait has settled.
        (void)DestroyWindow(window_handle);
        if (current_bounce) {
          current_bounce.shutdown();
        }
        return 0;
      }
      break;

    case WM_DESTROY:
      if (app != nullptr) {
        app->button_handle = NULL;
        app->window_handle = NULL;
      }
      return 0;
  }

  return DefWindowProcW(window_handle, message, wparam, lparam);
}

/**
 * Register the one-off window class used by this example process.
 * Re-register attempts are treated as success to keep repeated launches simple.
 */
static bool register_window_class(HINSTANCE instance) noexcept {
  WNDCLASSW window_class {};

  window_class.lpfnWndProc = example_window_proc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512u));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  window_class.lpszClassName = window_class_name;

  if (RegisterClassW(&window_class) != 0) {
    return true;
  }
  return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

}  // namespace

int run(
  libbounce::bounce &bounce_instance,
  HINSTANCE instance,
  int show_command) noexcept {
  example_app app;

  if (!register_window_class(instance)) {
    return 1;
  }

  app.window_handle = CreateWindowExW(
    0u,
    window_class_name,
    window_title,
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    240,
    110,
    NULL,
    NULL,
    instance,
    &app);
  if (app.window_handle == NULL) {
    return 1;
  }
  (void)SetWindowTextW(app.window_handle, window_title);

  app.button_handle = CreateWindowExW(
    0u,
    L"BUTTON",
    button_text,
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
    16,
    16,
    192,
    30,
    app.window_handle,
    reinterpret_cast<HMENU>(static_cast<INT_PTR>(example_button_id)),
    instance,
    NULL);
  if (app.button_handle == NULL) {
    (void)DestroyWindow(app.window_handle);
    return 1;
  }

  ShowWindow(app.window_handle, show_command);
  (void)UpdateWindow(app.window_handle);

  // `main()` already published this bounce as the GUI thread default, so
  // coroutine continuations and helper lookups on this thread resolve through
  // the same bounce instance while the GUI thread is parked.
  (void)bounce_instance.park();

  return 0;
}

}  // namespace libbounce_example::win32

#endif
