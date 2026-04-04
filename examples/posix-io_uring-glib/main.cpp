#include "examples/posix-io_uring-glib/example_app.h"

#include <libbounce/posix_glib.h>

int main(int argc, char **argv) {
  libbounce::bounce bounce_instance(g_main_context_default());

  bounce_instance.set_default();
  return libbounce_example::posix_io_uring_glib::run(bounce_instance, &argc, &argv);
}
