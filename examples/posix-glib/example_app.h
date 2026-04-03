#pragma once

#ifndef LIBBOUNCE_EXAMPLES_POSIX_GLIB_EXAMPLE_APP_H
#define LIBBOUNCE_EXAMPLES_POSIX_GLIB_EXAMPLE_APP_H

#if defined(__cplusplus)

#include <stddef.h>

namespace libbounce_example::posix_glib {

/**
 * @brief File name of the built GTK3 example executable.
 * @remarks Tests and local sample builds both use this exact file name, so keep
 * it synchronized with the build rules.
 */
inline constexpr char example_executable_name[] =
  "libbounce_posix_glib_gtk3_example";

/**
 * @brief Window title shown by the GTK3 example.
 */
inline constexpr char window_title[] = "libbounce POSIX+GLib Example";

/**
 * @brief Visible label of the only interactive control in the example UI.
 */
inline constexpr char button_text[] = "Write sample.txt";

/**
 * @brief Output file name created next to the running executable.
 */
inline constexpr char output_file_name[] = "sample.txt";

/**
 * @brief Sample text written to the output file.
 * @remarks The payload is intentionally tiny because the example focuses on the
 * integration pattern rather than large-file throughput.
 */
inline constexpr char sample_file_text[] =
  "This file was written by the libbounce POSIX+GLib GTK3 example.\n";

/**
 * @brief Byte length of the sample file payload without the trailing NUL.
 */
inline constexpr size_t sample_file_text_length = sizeof(sample_file_text) - 1u;

/**
 * @brief Hidden environment variable used by the smoke test.
 * @remarks When this variable is set to a non-empty value, the example clicks
 * its own button after startup and closes the window after the write settles.
 */
inline constexpr char automation_env_name[] =
  "LIBBOUNCE_POSIX_GLIB_EXAMPLE_AUTOMATE";

/**
 * @brief Run the minimal GTK3 + libbounce POSIX+GLib example application.
 * @param argc Address of the argument count passed to `main()`.
 * @param argv Address of the argument vector passed to `main()`.
 * @return Process exit code.
 * @remarks This binds libbounce to the GTK thread's `GMainContext`, creates a
 * single top-level window and button, and parks the GUI thread inside
 * `bounce.park()` until the window is closed.
 */
int run(int *argc, char ***argv) noexcept;

}  // namespace libbounce_example::posix_glib

#endif

#endif
