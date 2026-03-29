/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "esp_vfs_eventfd.h"

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

#define BOUNCE_FREERTOS_INTERNAL_ONLY 1
#include "bounce_freertos_internal.h"

//////////////////////////////////////////////////////////////////////////////////

static volatile int bounce_freertos_eventfd_registration_state = 0;
static const unsigned int bounce_freertos_fd_event_mask =
  BOUNCE_FREERTOS_FD_EVENT_READ |
  BOUNCE_FREERTOS_FD_EVENT_WRITE |
  BOUNCE_FREERTOS_FD_EVENT_EXCEPT;

typedef bool (*BOUNCE_FREERTOS_FD_ITEM_VISITOR)(
  BOUNCE_CORE *bounce,
  BOUNCE_COMPLETION_ITEM *item,
  void *state);

typedef struct BOUNCE_FREERTOS_FD_BUILD_CONTEXT {
  BOUNCE_CORE *bounce;
  __BOUNCE_FREERTOS_FD_BACKEND *backend;
  fd_set *read_fds;
  fd_set *write_fds;
  fd_set *except_fds;
  int max_fd;
  bool wake_parkers;
} BOUNCE_FREERTOS_FD_BUILD_CONTEXT;

typedef struct BOUNCE_FREERTOS_FD_READY_CONTEXT {
  BOUNCE_CORE *bounce;
  __BOUNCE_FREERTOS_FD_BACKEND *backend;
  fd_set *read_fds;
  fd_set *write_fds;
  fd_set *except_fds;
  bool wake_parkers;
} BOUNCE_FREERTOS_FD_READY_CONTEXT;

typedef struct BOUNCE_FREERTOS_FD_ABORT_CONTEXT {
  BOUNCE_CORE *bounce;
  __BOUNCE_FREERTOS_FD_BACKEND *backend;
  bool wake_parkers;
} BOUNCE_FREERTOS_FD_ABORT_CONTEXT;

static bool bounce_freertos_fd_eventfd_ensure_registered(void) {
  for (;;) {
    const int state =
      __atomic_load_n(&bounce_freertos_eventfd_registration_state, __ATOMIC_ACQUIRE);
    int expected_state = 0;

    if (state > 0) {
      return true;
    }
    if (state < 0) {
      return false;
    }
    if (__atomic_compare_exchange_n(
          &bounce_freertos_eventfd_registration_state,
          &expected_state,
          2,
          false,
          __ATOMIC_ACQ_REL,
          __ATOMIC_ACQUIRE)) {
      esp_vfs_eventfd_config_t config = ESP_VFS_EVENTD_CONFIG_DEFAULT();
      esp_err_t register_result;
      bool register_succeeded;

      config.max_fds = BOUNCE_FREERTOS_EVENTFD_MAX_FDS;
      register_result = esp_vfs_eventfd_register(&config);
      register_succeeded =
        (register_result == ESP_OK) ||
        (register_result == ESP_ERR_INVALID_STATE);
      __atomic_store_n(
        &bounce_freertos_eventfd_registration_state,
        register_succeeded ? 1 : -1,
        __ATOMIC_RELEASE);
      return register_succeeded;
    }
    taskYIELD();
  }
}

static bool bounce_freertos_fd_visit_completion_items(
  BOUNCE_CORE *bounce,
  BOUNCE_FREERTOS_FD_ITEM_VISITOR visitor,
  void *state) {
  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    if (!visitor(bounce, &bounce->static_completion_items[index], state)) {
      return false;
    }
  }

  for (BOUNCE_DYNAMIC_BLOCK *block = bounce->dynamic_item_blocks.head;
       block != NULL;
       block = block->next) {
    BOUNCE_COMPLETION_ITEM *items =
      (BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_items(block);

    for (size_t index = 0u; index < block->item_count; index++) {
      if (!visitor(bounce, &items[index], state)) {
        return false;
      }
    }
  }

  return true;
}

static void bounce_freertos_fd_signal_waiter(__BOUNCE_FREERTOS_FD_BACKEND *backend) {
  static const uint64_t signal_value = 1u;

  if ((backend == NULL) ||
      (backend->control_event_fd < 0)) {
    return;
  }

  while (write(
           backend->control_event_fd,
           &signal_value,
           sizeof signal_value) < 0) {
    if (errno != EINTR) {
      return;
    }
  }
}

static void bounce_freertos_fd_drain_waiter_signal(int control_event_fd) {
  uint64_t signal_value;

  if (control_event_fd < 0) {
    return;
  }

  while (read(control_event_fd, &signal_value, sizeof signal_value) < 0) {
    if (errno != EINTR) {
      return;
    }
  }
}

static bool bounce_freertos_fd_is_waiting_item(
  const __BOUNCE_FREERTOS_FD_BACKEND *backend,
  const BOUNCE_COMPLETION_ITEM *item) {
  return
    (backend != NULL) &&
    (item != NULL) &&
    (item->fd_backend == backend) &&
    (item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
    (item->fd >= 0) &&
    ((item->events & bounce_freertos_fd_event_mask) != 0u);
}

static void bounce_freertos_fd_abort_item_locked(
  BOUNCE_CORE *bounce,
  __BOUNCE_FREERTOS_FD_BACKEND *backend,
  BOUNCE_COMPLETION_ITEM *item,
  bool *wake_parkers) {
  if (!bounce_freertos_fd_is_waiting_item(backend, item)) {
    return;
  }

  bounce_freertos_detach_wait_locked(item, true);
  bounce_freertos_queue_ready_locked(
    bounce,
    item,
    BOUNCE_COMPLETION_ABORTED);
  *wake_parkers = true;
}

static bool bounce_freertos_fd_is_ready(
  const BOUNCE_COMPLETION_ITEM *item,
  const fd_set *read_fds,
  const fd_set *write_fds,
  const fd_set *except_fds) {
  return
    ((item->events & BOUNCE_FREERTOS_FD_EVENT_READ) != 0u &&
     FD_ISSET(item->fd, read_fds)) ||
    ((item->events & BOUNCE_FREERTOS_FD_EVENT_WRITE) != 0u &&
     FD_ISSET(item->fd, write_fds)) ||
    ((item->events & BOUNCE_FREERTOS_FD_EVENT_EXCEPT) != 0u &&
     FD_ISSET(item->fd, except_fds));
}

static bool bounce_freertos_fd_build_sets_visitor(
  BOUNCE_CORE *bounce,
  BOUNCE_COMPLETION_ITEM *item,
  void *state) {
  BOUNCE_FREERTOS_FD_BUILD_CONTEXT *context =
    (BOUNCE_FREERTOS_FD_BUILD_CONTEXT *)state;

  if (!bounce_freertos_fd_is_waiting_item(context->backend, item)) {
    return true;
  }
#if defined(FD_SETSIZE)
  if (item->fd >= FD_SETSIZE) {
    bounce_freertos_fd_abort_item_locked(
      bounce,
      context->backend,
      item,
      &context->wake_parkers);
    return true;
  }
#endif
  if ((item->events & BOUNCE_FREERTOS_FD_EVENT_READ) != 0u) {
    FD_SET(item->fd, context->read_fds);
  }
  if ((item->events & BOUNCE_FREERTOS_FD_EVENT_WRITE) != 0u) {
    FD_SET(item->fd, context->write_fds);
  }
  if ((item->events & BOUNCE_FREERTOS_FD_EVENT_EXCEPT) != 0u) {
    FD_SET(item->fd, context->except_fds);
  }
  if (item->fd > context->max_fd) {
    context->max_fd = item->fd;
  }
  return true;
}

static bool bounce_freertos_fd_collect_ready_items_visitor(
  BOUNCE_CORE *bounce,
  BOUNCE_COMPLETION_ITEM *item,
  void *state) {
  BOUNCE_FREERTOS_FD_READY_CONTEXT *context =
    (BOUNCE_FREERTOS_FD_READY_CONTEXT *)state;

  if (!bounce_freertos_fd_is_waiting_item(context->backend, item)) {
    return true;
  }
  if (!bounce_freertos_fd_is_ready(
        item,
        context->read_fds,
        context->write_fds,
        context->except_fds)) {
    return true;
  }

  bounce_freertos_detach_wait_locked(item, false);
  bounce_freertos_queue_ready_locked(
    bounce,
    item,
    BOUNCE_COMPLETION_COMPLETED);
  context->wake_parkers = true;
  return true;
}

static bool bounce_freertos_fd_abort_all_visitor(
  BOUNCE_CORE *bounce,
  BOUNCE_COMPLETION_ITEM *item,
  void *state) {
  BOUNCE_FREERTOS_FD_ABORT_CONTEXT *context =
    (BOUNCE_FREERTOS_FD_ABORT_CONTEXT *)state;

  bounce_freertos_fd_abort_item_locked(
    bounce,
    context->backend,
    item,
    &context->wake_parkers);
  return true;
}

static void bounce_freertos_fd_waiter_task(void *parameter) {
  __BOUNCE_FREERTOS_FD_BACKEND *backend =
    (__BOUNCE_FREERTOS_FD_BACKEND *)parameter;
  BOUNCE_CORE *bounce = (backend != NULL) ? backend->bounce : NULL;

  for (;;) {
    BOUNCE_FREERTOS_FD_BUILD_CONTEXT build_context;
    BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;
    fd_set read_fds;
    fd_set write_fds;
    fd_set except_fds;
    int select_result;

    if ((backend == NULL) || (bounce == NULL)) {
      break;
    }

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);

    build_context.bounce = bounce;
    build_context.backend = backend;
    build_context.read_fds = &read_fds;
    build_context.write_fds = &write_fds;
    build_context.except_fds = &except_fds;
    build_context.max_fd = -1;
    build_context.wake_parkers = false;

    if (backend->control_event_fd >= 0) {
      FD_SET(backend->control_event_fd, &read_fds);
      build_context.max_fd = backend->control_event_fd;
    }

    bounce_freertos_critical_enter(&critical_context, false);
    (void)bounce_freertos_fd_visit_completion_items(
      bounce,
      bounce_freertos_fd_build_sets_visitor,
      &build_context);
    bounce_freertos_critical_exit(&critical_context);

    if (build_context.wake_parkers) {
      bounce_freertos_signal_parkers(bounce);
    }
    if (bounce->shutting_down != 0) {
      break;
    }
    if (build_context.max_fd < 0) {
      vTaskDelay(1u);
      continue;
    }

    for (;;) {
      select_result = select(
        build_context.max_fd + 1,
        &read_fds,
        &write_fds,
        &except_fds,
        NULL);
      if (select_result >= 0) {
        break;
      }
      if (errno != EINTR) {
        break;
      }
    }

    if (select_result < 0) {
      if (errno == EINTR) {
        continue;
      }
      {
        BOUNCE_FREERTOS_FD_ABORT_CONTEXT abort_context = {
          bounce,
          backend,
          false
        };

        bounce_freertos_critical_enter(&critical_context, false);
        (void)bounce_freertos_fd_visit_completion_items(
          bounce,
          bounce_freertos_fd_abort_all_visitor,
          &abort_context);
        bounce_freertos_critical_exit(&critical_context);

        if (abort_context.wake_parkers) {
          bounce_freertos_signal_parkers(bounce);
        }
      }
      continue;
    }

    if ((backend->control_event_fd >= 0) &&
        FD_ISSET(backend->control_event_fd, &read_fds)) {
      bounce_freertos_fd_drain_waiter_signal(backend->control_event_fd);
      if (bounce->shutting_down != 0) {
        break;
      }
    }
    if (bounce->shutting_down != 0) {
      break;
    }

    {
      BOUNCE_FREERTOS_FD_READY_CONTEXT ready_context = {
        bounce,
        backend,
        &read_fds,
        &write_fds,
        &except_fds,
        false
      };

      bounce_freertos_critical_enter(&critical_context, false);
      (void)bounce_freertos_fd_visit_completion_items(
        bounce,
        bounce_freertos_fd_collect_ready_items_visitor,
        &ready_context);
      bounce_freertos_critical_exit(&critical_context);

      if (ready_context.wake_parkers) {
        bounce_freertos_signal_parkers(bounce);
      }
    }
  }

  if (backend != NULL) {
    backend->waiter_task = NULL;
    if (backend->finished_semaphore != NULL) {
      (void)xSemaphoreGive(backend->finished_semaphore);
    }
  }
  vTaskDelete(NULL);
}

void bounce_freertos_fd_backend_init(BOUNCE_CORE *r) {
  __BOUNCE_FREERTOS_FD_BACKEND *backend;

  if (r == NULL) {
    return;
  }

  backend = &r->fd_backend;
  memset(backend, 0, sizeof *backend);
  backend->bounce = r;
  backend->control_event_fd = -1;

  if (!bounce_freertos_fd_eventfd_ensure_registered()) {
    return;
  }

  backend->finished_semaphore =
    xSemaphoreCreateBinaryStatic(&backend->finished_semaphore_buffer);
  if (backend->finished_semaphore == NULL) {
    return;
  }
  while (xSemaphoreTake(backend->finished_semaphore, 0u) == pdTRUE) {
  }

  backend->control_event_fd = eventfd(0u, 0);
  if (backend->control_event_fd < 0) {
    return;
  }

  backend->waiter_task = xTaskCreateStatic(
    bounce_freertos_fd_waiter_task,
    "bounce-fd",
    BOUNCE_FREERTOS_FD_WAITER_STACK_DEPTH,
    backend,
    (configMAX_PRIORITIES - 3u),
    &backend->waiter_task_stack[0],
    &backend->waiter_task_tcb);
  if (backend->waiter_task == NULL) {
    (void)close(backend->control_event_fd);
    backend->control_event_fd = -1;
  }
}

void bounce_freertos_fd_backend_shutdown(BOUNCE_CORE *r) {
  if (r != NULL) {
    bounce_freertos_fd_signal_waiter(&r->fd_backend);
  }
}

void bounce_freertos_fd_backend_deinit(BOUNCE_CORE *r) {
  __BOUNCE_FREERTOS_FD_BACKEND *backend;

  if (r == NULL) {
    return;
  }

  backend = &r->fd_backend;
  if (backend->waiter_task != NULL) {
    bounce_freertos_fd_signal_waiter(backend);
    if (backend->finished_semaphore != NULL) {
      (void)xSemaphoreTake(backend->finished_semaphore, portMAX_DELAY);
    }
  }
  if (backend->control_event_fd >= 0) {
    (void)close(backend->control_event_fd);
    backend->control_event_fd = -1;
  }
}

bool bounce_freertos_fd_register_waiter_locked(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item) {
  __BOUNCE_FREERTOS_FD_BACKEND *backend;

  if ((r == NULL) || (item == NULL)) {
    return false;
  }

  backend = &r->fd_backend;
  if ((backend->waiter_task == NULL) ||
      (backend->control_event_fd < 0)) {
    return false;
  }

  item->fd_backend = backend;
  return true;
}

void bounce_freertos_fd_remove_waiter_locked(
  BOUNCE_COMPLETION_ITEM *item,
  bool signal_refresh) {
  __BOUNCE_FREERTOS_FD_BACKEND *backend;

  if (item == NULL) {
    return;
  }

  backend = item->fd_backend;
  item->fd_backend = NULL;
  if (signal_refresh) {
    bounce_freertos_fd_signal_waiter(backend);
  }
}

void bounce_await_freertos_fd(
  BOUNCE_CORE *r,
  int fd,
  unsigned int events,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_FREERTOS_START_RESULT start_result;
  bool signal_waiter = false;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) ||
      (fd < 0) ||
#if defined(FD_SETSIZE)
      (fd >= FD_SETSIZE) ||
#endif
      (events == 0u) ||
      ((events & ~bounce_freertos_fd_event_mask) != 0u) ||
      (completion == NULL)) {
    bounce_freertos_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item = bounce_freertos_free_item_acquire(r, false);
  if (item == NULL) {
    bounce_freertos_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->fd = fd;
  item->events = events;

  bounce_freertos_critical_enter(&critical_context, false);
  start_result = bounce_freertos_activate_wait_item_locked(
    r,
    item,
    cancellation);
  if (start_result == __BOUNCE_FREERTOS_START_RESULT_WAITING) {
    if (bounce_freertos_fd_register_waiter_locked(r, item)) {
      signal_waiter = true;
    } else {
      bounce_freertos_detach_wait_locked(item, false);
      start_result = bounce_freertos_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_ABORTED);
    }
  }
  bounce_freertos_critical_exit(&critical_context);

  if (signal_waiter) {
    bounce_freertos_fd_signal_waiter(item->fd_backend);
  }

  switch (start_result) {
    case __BOUNCE_FREERTOS_START_RESULT_WAITING:
      return;
    case __BOUNCE_FREERTOS_START_RESULT_READY_QUEUED:
      bounce_freertos_signal_parkers(r);
      return;
    case __BOUNCE_FREERTOS_START_RESULT_READY_INLINE:
      bounce_freertos_execute_claimed_item(r, item, 0u);
      return;
    case __BOUNCE_FREERTOS_START_RESULT_FAILED:
    default:
      bounce_freertos_free_item_release(r, item, false);
      bounce_freertos_complete_direct(
        completion,
        completion_state,
        BOUNCE_COMPLETION_ABORTED);
      return;
  }
}

#endif
