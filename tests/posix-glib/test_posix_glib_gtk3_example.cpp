#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <string>

#include <libbounce/posix_glib.h>

#include "examples/posix-glib/example_app.h"

namespace {

constexpr unsigned int test_timeout_ms = 5000u;
constexpr size_t path_buffer_length = 32768u;

static bool build_runtime_paths(
  std::string &example_path,
  std::string &sample_path) {
  std::array<char, path_buffer_length> executable_path {};
  ssize_t length = readlink(
    "/proc/self/exe",
    executable_path.data(),
    executable_path.size() - 1u);
  char *separator;
  std::string build_directory;

  if ((length <= 0) || ((size_t)length >= (executable_path.size() - 1u))) {
    return false;
  }

  executable_path[(size_t)length] = '\0';
  separator = strrchr(executable_path.data(), '/');
  if (separator == nullptr) {
    return false;
  }
  *separator = '\0';

  build_directory = executable_path.data();
  separator = strrchr(build_directory.data(), '/');
  if (separator == nullptr) {
    return false;
  }
  *separator = '\0';

  example_path = build_directory + "/examples/posix-glib/" +
                 libbounce_example::posix_glib::example_executable_name;
  sample_path = executable_path.data();
  sample_path.push_back('/');
  sample_path.append(
    libbounce_example::posix_glib::output_file_name);
  return true;
}

static bool read_sample_file(std::string &contents, const std::string &sample_path) {
  FILE *file = fopen(sample_path.c_str(), "rb");
  std::array<char, sizeof(libbounce_example::posix_glib::sample_file_text)> buffer {};
  size_t bytes_read;

  if (file == nullptr) {
    return false;
  }

  bytes_read = fread(buffer.data(), 1u, buffer.size() - 1u, file);
  (void)fclose(file);
  if (bytes_read != libbounce_example::posix_glib::sample_file_text_length) {
    return false;
  }

  buffer[bytes_read] = '\0';
  contents.assign(buffer.data(), bytes_read);
  return true;
}

static bool write_sample_file(
  const std::string &sample_path,
  const char *contents,
  size_t length) {
  FILE *file = fopen(sample_path.c_str(), "wb");

  if (file == nullptr) {
    return false;
  }

  const size_t bytes_written = fwrite(contents, 1u, length, file);
  (void)fclose(file);
  return bytes_written == length;
}

}  // namespace

extern "C" void test_posix_glib_gtk3_example_button_click_writes_sample_file(void) {
  std::string example_path;
  std::string sample_path;
  struct stat example_stat {};
  struct stat sample_stat {};
  bool success = false;

#define CHECK_TRUE(expr)                                                        \
  do {                                                                          \
    if (!(expr)) {                                                              \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
      goto cleanup;                                                             \
    }                                                                           \
  } while (0)

  CHECK_TRUE(build_runtime_paths(example_path, sample_path));
  CHECK_TRUE(stat(example_path.c_str(), &example_stat) == 0);
  (void)unlink(sample_path.c_str());
  CHECK_TRUE(write_sample_file(sample_path, "stale\n", sizeof("stale\n") - 1u));
  {
    int argc = 1;
    char program_name[] = "libbounce_posix_glib_gtk3_example";
    char *argv_storage[] = { program_name, NULL };
    char **argv = argv_storage;
    libbounce::bounce bounce_instance(g_main_context_default());
    std::string contents;

    CHECK_TRUE(setenv(
      libbounce_example::posix_glib::automation_env_name,
      "1",
      1) == 0);
    bounce_instance.set_default();
    CHECK_TRUE(
      libbounce_example::posix_glib::run(bounce_instance, &argc, &argv) == 0);
    (void)unsetenv(libbounce_example::posix_glib::automation_env_name);

    CHECK_TRUE(stat(sample_path.c_str(), &sample_stat) == 0);
    CHECK_TRUE(sample_stat.st_size ==
      (off_t)libbounce_example::posix_glib::sample_file_text_length);
    CHECK_TRUE(read_sample_file(contents, sample_path));
    CHECK_TRUE(
      contents == libbounce_example::posix_glib::sample_file_text);
  }

  success = true;

cleanup:
  (void)unlink(sample_path.c_str());

  if (!success) {
    abort();
  }

#undef CHECK_TRUE
}
