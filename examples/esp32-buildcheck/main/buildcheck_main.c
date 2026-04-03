#include "libbounce/bounce.h"
#include "libbounce/freertos.h"

static BOUNCE_CORE g_bounce;
static BOUNCE_TIMER g_timer;
static BOUNCE_FREERTOS_CONDITION g_condition;

typedef void (*bounce_fd_api_fn)(
  BOUNCE_CORE *r,
  int fd,
  unsigned int events,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation);

static void buildcheck_completion(
  BOUNCE_COMPLETION_RESULT result,
  void *completion_state) {
  (void)result;
  (void)completion_state;
}

static const bounce_fd_api_fn g_bounce_fd_api = bounce_await_freertos_fd;
static const BOUNCE_COMPLETION g_buildcheck_completion = buildcheck_completion;
static const unsigned int g_buildcheck_fd_events = BOUNCE_FREERTOS_FD_EVENT_READ;

void app_main(void) {
  bounce_init(&g_bounce);
  bounce_freertos_condition_init(&g_condition);
  bounce_timer_init(&g_timer);

  (void)g_bounce_fd_api;
  (void)g_buildcheck_completion;
  (void)g_buildcheck_fd_events;

  bounce_shutdown(&g_bounce, false);
  bounce_timer_deinit(&g_timer);
  bounce_deinit(&g_bounce);
}
