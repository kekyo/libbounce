#include "examples/win32/example_app.h"

#if defined(_WIN32)

int WINAPI WinMain(
  HINSTANCE instance,
  HINSTANCE /*previous_instance*/,
  LPSTR /*command_line*/,
  int show_command) {
  return libbounce_example::win32::run(instance, show_command);
}

#endif
