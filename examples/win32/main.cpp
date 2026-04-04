#include "examples/win32/example_app.h"

#include <libbounce/win32.h>

#if defined(_WIN32)

int WINAPI WinMain(
  HINSTANCE instance,
  HINSTANCE /*previous_instance*/,
  LPSTR /*command_line*/,
  int show_command) {
  libbounce::bounce bounce_instance;

  bounce_instance.set_default();
  return libbounce_example::win32::run(
    bounce_instance,
    instance,
    show_command);
}

#endif
