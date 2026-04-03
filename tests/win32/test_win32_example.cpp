#include <windows.h>

#include <array>
#include <stdio.h>
#include <string.h>
#include <string>
#include <wchar.h>

#include "examples/win32/example_app.h"

namespace {

constexpr DWORD test_timeout_ms = 5000u;
constexpr DWORD test_poll_interval_ms = 10u;

struct window_search_context {
  DWORD process_id;
  HWND window_handle;
};

static BOOL CALLBACK test_find_window_callback(
  HWND window_handle,
  LPARAM lparam) {
  auto *context = reinterpret_cast<window_search_context *>(lparam);
  DWORD process_id = 0u;
  RECT window_rect {};

  GetWindowThreadProcessId(window_handle, &process_id);
  if (process_id != context->process_id) {
    return TRUE;
  }

  if (!IsWindowVisible(window_handle) || (GetWindow(window_handle, GW_OWNER) != NULL)) {
    return TRUE;
  }

  if (!GetWindowRect(window_handle, &window_rect) ||
      ((window_rect.right - window_rect.left) < 100) ||
      ((window_rect.bottom - window_rect.top) < 50)) {
    return TRUE;
  }

  if (FindWindowExW(
        window_handle,
        NULL,
        L"Button",
        libbounce_example::win32::button_text) == NULL) {
    return TRUE;
  }

  context->window_handle = window_handle;
  return FALSE;
}

static std::wstring test_runtime_directory(void) {
  std::array<wchar_t, 32768u> path {};
  DWORD length = GetModuleFileNameW(NULL, path.data(), (DWORD)path.size());
  wchar_t *separator;

  if ((length == 0u) || (length >= path.size())) {
    return L"";
  }

  separator = wcsrchr(path.data(), L'\\');
  if (separator == nullptr) {
    return L"";
  }
  *separator = L'\0';
  return path.data();
}

static std::wstring test_join_path(
  const std::wstring &directory,
  const wchar_t *file_name) {
  if (directory.empty()) {
    return std::wstring(file_name);
  }

  return directory + L"\\" + file_name;
}

static HWND test_wait_for_window(
  HANDLE process_handle,
  DWORD process_id,
  DWORD timeout_ms) {
  const DWORD started_at = GetTickCount();

  for (;;) {
    window_search_context context { process_id, NULL };

    EnumWindows(test_find_window_callback, reinterpret_cast<LPARAM>(&context));
    if (context.window_handle != NULL) {
      return context.window_handle;
    }

    if (WaitForSingleObject(process_handle, 0u) == WAIT_OBJECT_0) {
      return NULL;
    }

    if ((GetTickCount() - started_at) >= timeout_ms) {
      return NULL;
    }

    Sleep(test_poll_interval_ms);
  }
}

static bool test_read_file_contents(
  const std::wstring &path,
  std::array<char, sizeof(libbounce_example::win32::sample_file_text)> &buffer) {
  HANDLE file_handle = CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE,
    NULL,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    NULL);

  if (file_handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD bytes_read = 0u;
  const BOOL read_result = ReadFile(
    file_handle,
    buffer.data(),
    (DWORD)(buffer.size() - 1u),
    &bytes_read,
    NULL);

  (void)CloseHandle(file_handle);
  if ((read_result == 0) ||
      (bytes_read != libbounce_example::win32::sample_file_text_length)) {
    return false;
  }

  buffer[bytes_read] = '\0';
  return memcmp(
           buffer.data(),
           libbounce_example::win32::sample_file_text,
           libbounce_example::win32::sample_file_text_length) == 0;
}

static bool test_wait_for_sample_file(
  HANDLE process_handle,
  const std::wstring &sample_path,
  DWORD timeout_ms) {
  const DWORD started_at = GetTickCount();
  std::array<char, sizeof(libbounce_example::win32::sample_file_text)> buffer {};

  for (;;) {
    if (test_read_file_contents(sample_path, buffer)) {
      return true;
    }

    if (WaitForSingleObject(process_handle, 0u) == WAIT_OBJECT_0) {
      return false;
    }

    if ((GetTickCount() - started_at) >= timeout_ms) {
      return false;
    }

    Sleep(test_poll_interval_ms);
  }
}

}  // namespace

extern "C" void test_win32_example_button_click_writes_sample_file(void) {
  PROCESS_INFORMATION process_info {};
  STARTUPINFOW startup_info {};
  std::wstring runtime_directory;
  std::wstring example_path;
  std::wstring sample_path;
  std::wstring command_line;
  HWND window_handle = NULL;
  HWND button_handle = NULL;
  bool created = false;
  bool success = false;

#define CHECK_TRUE(expr)                                                        \
  do {                                                                          \
    if (!(expr)) {                                                              \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      goto cleanup;                                                             \
    }                                                                           \
  } while (0)

  startup_info.cb = sizeof(startup_info);

  runtime_directory = test_runtime_directory();
  example_path = test_join_path(
    runtime_directory,
    libbounce_example::win32::example_executable_name);
  sample_path = test_join_path(
    runtime_directory,
    libbounce_example::win32::output_file_name);
  command_line = L"\"" + example_path + L"\"";

  (void)DeleteFileW(sample_path.c_str());

  CHECK_TRUE(!runtime_directory.empty());
  CHECK_TRUE(
    CreateProcessW(
      example_path.c_str(),
      command_line.data(),
      NULL,
      NULL,
      FALSE,
      0u,
      NULL,
      runtime_directory.c_str(),
      &startup_info,
      &process_info) != 0);
  created = true;

  (void)WaitForInputIdle(process_info.hProcess, test_timeout_ms);
  window_handle = test_wait_for_window(
    process_info.hProcess,
    process_info.dwProcessId,
    test_timeout_ms);
  CHECK_TRUE(window_handle != NULL);

  button_handle = FindWindowExW(
    window_handle,
    NULL,
    L"Button",
    libbounce_example::win32::button_text);
  CHECK_TRUE(button_handle != NULL);

  (void)SendMessageW(button_handle, BM_CLICK, 0u, 0u);
  CHECK_TRUE(test_wait_for_sample_file(
    process_info.hProcess,
    sample_path,
    test_timeout_ms));

  CHECK_TRUE(PostMessageW(window_handle, WM_CLOSE, 0u, 0u) != 0);
  CHECK_TRUE(WaitForSingleObject(process_info.hProcess, test_timeout_ms) == WAIT_OBJECT_0);

  {
    DWORD exit_code = 1u;

    CHECK_TRUE(GetExitCodeProcess(process_info.hProcess, &exit_code) != 0);
    CHECK_TRUE(exit_code == 0u);
  }

  success = true;

cleanup:
  if (created && (WaitForSingleObject(process_info.hProcess, 0u) == WAIT_TIMEOUT)) {
    (void)TerminateProcess(process_info.hProcess, 1u);
    (void)WaitForSingleObject(process_info.hProcess, test_timeout_ms);
  }
  if (process_info.hThread != NULL) {
    (void)CloseHandle(process_info.hThread);
  }
  if (process_info.hProcess != NULL) {
    (void)CloseHandle(process_info.hProcess);
  }
  (void)DeleteFileW(sample_path.c_str());

  if (!success) {
    ExitProcess(1);
  }

#undef CHECK_TRUE
}
