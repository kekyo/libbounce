#pragma once

#ifndef LIBBOUNCE_EXAMPLES_WIN32_EXAMPLE_APP_H
#define LIBBOUNCE_EXAMPLES_WIN32_EXAMPLE_APP_H

#if defined(_WIN32) && defined(__cplusplus)

#include <stddef.h>
#include <windows.h>

namespace libbounce {
class bounce;
}

namespace libbounce_example::win32 {

/**
 * @brief File name of the built example executable staged by the test runtime.
 * @remarks Tests launch the example by this exact file name from the Wine
 * runtime directory, so keep it synchronized with the build rule.
 */
inline constexpr wchar_t example_executable_name[] = L"libbounce_win32_example.exe";

/**
 * @brief Registered Win32 window class name for the top-level example window.
 */
inline constexpr wchar_t window_class_name[] = L"libbounce-win32-example-window";

/**
 * @brief Caption text shown on the example window.
 */
inline constexpr wchar_t window_title[] = L"libbounce Win32 Example";

/**
 * @brief Visible label of the only interactive control in the example UI.
 */
inline constexpr wchar_t button_text[] = L"Write sample.txt";

/**
 * @brief Output file name created next to the running executable.
 */
inline constexpr wchar_t output_file_name[] = L"sample.txt";

/**
 * @brief Sample text written to the output file.
 * @remarks The text is intentionally tiny because the example is meant to show
 * the asynchronous control flow rather than large-file handling.
 */
inline constexpr char sample_file_text[] =
  "This file was written by the libbounce Win32 example.\r\n";

/**
 * @brief Byte length of the sample file payload without the trailing NUL.
 */
inline constexpr size_t sample_file_text_length = sizeof(sample_file_text) - 1u;

/**
 * @brief Run the minimal Win32 example application.
 * @param bounce_instance Bounce instance already selected as the current
 * default for the GUI thread.
 * @param instance Current process module instance.
 * @param show_command Initial show command passed from WinMain.
 * @return Process exit code.
 * @remarks This uses the caller-owned `libbounce::bounce`, creates the
 * top-level window and button, parks the GUI thread inside `bounce.park()`,
 * and returns after a window close requests `bounce.shutdown()`.
 */
int run(
  libbounce::bounce &bounce_instance,
  HINSTANCE instance,
  int show_command) noexcept;

}  // namespace libbounce_example::win32

#endif

#endif
