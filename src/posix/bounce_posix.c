/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(BOUNCE_POSIX)

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/eventfd.h>
#include <liburing.h>
#include "bounce_io_uring_compat.h"
#endif

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

//////////////////////////////////////////////////////////////////////////////////
/*
 * POSIX bounce architecture:
 *
 * - Producer paths and parker execution are intentionally separated.
 *   `post()`, backend-local condition raising, and backend-local fd waiter shards
 *   all turn work into ready completion items, but they do not execute
 *   callbacks directly unless inline-dispatch rules explicitly allow it.
 * - Ready items are pushed into the shared ready queue.
 * - Parker threads drain that queue and execute completions on the parked
 *   thread context.
 * - Linux parkers also own the sleep set for wake-fd, fd readiness, and
 *   io_uring completion eventfd integration.
 * - Non-Linux fd readiness stays backend-local. Fixed waiter shards call
 *   `poll()`, claim the corresponding completion items, and enqueue them into
 *   the shared ready queue.
 * - The completion flow stays aligned with the other backends:
 *     post()/condition/fd-ready -> ready queue -> wake parker -> drain queue
 *     -> execute completion.
 */

//////////////////////////////////////////////////////////////////////////////////

#define BOUNCE_POSIX_INVALID_WAITER_SLOT ((unsigned int)~0u)
#define BOUNCE_POSIX_CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

#if defined(__linux__)
#define BOUNCE_POSIX_IO_URING_QUEUE_DEPTH 256u
#define BOUNCE_POSIX_IO_URING_USER_DATA_CANCEL_FLAG UINT64_C(1)
#endif

typedef struct BOUNCE_POSIX_DISPATCH_CONTEXT {
  struct BOUNCE_POSIX_DISPATCH_CONTEXT *previous;
  BOUNCE_CORE *bounce;
  unsigned int depth;
  unsigned int max_inline_depth;
} BOUNCE_POSIX_DISPATCH_CONTEXT;

typedef enum __BOUNCE_POSIX_START_RESULT {
  __BOUNCE_POSIX_START_RESULT_FAILED = 0,
  __BOUNCE_POSIX_START_RESULT_WAITING = 1,
  __BOUNCE_POSIX_START_RESULT_READY_QUEUED = 2,
  __BOUNCE_POSIX_START_RESULT_READY_INLINE = 3
} __BOUNCE_POSIX_START_RESULT;

#if defined(__linux__)
typedef struct __BOUNCE_POSIX_IO_URING_WAIT {
  struct __BOUNCE_POSIX_IO_URING_WAIT *next;
  BOUNCE_CORE *bounce;
  __BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_POSIX_IO_URING_OP *operation;
  unsigned int pending_cqe_count;
} __BOUNCE_POSIX_IO_URING_WAIT;

static inline bool bounce_posix_submit_cancel_io_uring_wait_locked(
  BOUNCE_CORE *r,
  __BOUNCE_POSIX_IO_URING_WAIT *wait);
#endif

static pthread_once_t bounce_posix_dispatch_tls_once_state = PTHREAD_ONCE_INIT;
static pthread_key_t bounce_posix_dispatch_tls_key;

static void bounce_posix_dispatch_tls_once(void) {
  (void)pthread_key_create(&bounce_posix_dispatch_tls_key, NULL);
}

static inline bool bounce_posix_ensure_dispatch_tls(void) {
  return pthread_once(
           &bounce_posix_dispatch_tls_once_state,
           bounce_posix_dispatch_tls_once) == 0;
}

static inline BOUNCE_POSIX_DISPATCH_CONTEXT *bounce_posix_get_dispatch_context(void) {
  return bounce_posix_ensure_dispatch_tls() ?
           (BOUNCE_POSIX_DISPATCH_CONTEXT *)pthread_getspecific(
             bounce_posix_dispatch_tls_key) :
           NULL;
}

static inline void bounce_posix_set_dispatch_context(
  BOUNCE_POSIX_DISPATCH_CONTEXT *context) {
  if (bounce_posix_ensure_dispatch_tls()) {
    (void)pthread_setspecific(bounce_posix_dispatch_tls_key, context);
  }
}

static inline int bounce_posix_lock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_lock(mutex) : EINVAL;
}

static inline int bounce_posix_unlock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_unlock(mutex) : EINVAL;
}

static inline void bounce_posix_complete_direct(
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_COMPLETION_RESULT result) {
  if (completion != NULL) {
    completion(result, completion_state);
  }
}

static inline bool bounce_posix_list_item_is_linked(
  const BOUNCE_LIST *list,
  const BOUNCE_LIST_ITEM *item) {
  return (list != NULL) &&
         (item != NULL) &&
         ((item->previous != NULL) ||
          (item->next != NULL) ||
          (list->head == item));
}

static inline int bounce_posix_set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);

  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline void bounce_posix_drain_fd(int fd) {
  char buffer[256];

  if (fd < 0) {
    return;
  }

  for (;;) {
    const ssize_t read_size = read(fd, &buffer[0], sizeof buffer);

    if (read_size > 0) {
      continue;
    }
    if (read_size == 0) {
      return;
    }
    if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      return;
    }
    return;
  }
}

static inline void bounce_posix_consume_fd_signal(int fd) {
  unsigned char signal_byte = 0u;

  if (fd < 0) {
    return;
  }

  for (;;) {
    const ssize_t read_size = read(fd, &signal_byte, sizeof signal_byte);

    if (read_size == (ssize_t)sizeof signal_byte) {
      return;
    }
    if (read_size == 0) {
      return;
    }
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
        return;
      }
    }
    return;
  }
}

static inline bool bounce_posix_create_pipe_nonblocking(int pipe_fds[2]) {
  if (pipe(pipe_fds) != 0) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    return false;
  }

  if ((bounce_posix_set_nonblocking(pipe_fds[0]) != 0) ||
      (bounce_posix_set_nonblocking(pipe_fds[1]) != 0)) {
    (void)close(pipe_fds[0]);
    (void)close(pipe_fds[1]);
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    return false;
  }
  return true;
}

#if defined(__linux__)
static inline int bounce_posix_create_eventfd(void) {
  return eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}

static inline void bounce_posix_drain_eventfd(int fd) {
  eventfd_t counter = 0;

  if (fd < 0) {
    return;
  }

  for (;;) {
    if (eventfd_read(fd, &counter) == 0) {
      continue;
    }
    if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      return;
    }
    return;
  }
}
#endif

static inline void bounce_posix_signal_fd(int fd) {
  static const unsigned char signal_byte = 1u;

  if (fd < 0) {
    return;
  }

  for (;;) {
    const ssize_t written = write(fd, &signal_byte, sizeof signal_byte);

    if (written == (ssize_t)sizeof signal_byte) {
      return;
    }
    if (written >= 0) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    return;
  }
}

static inline void bounce_posix_signal_parkers(BOUNCE_CORE *r) {
  if (r != NULL) {
#if defined(__linux__)
    if (r->linux_unified_wait_enabled &&
        (r->wake_pipe_fds[1] >= 0)) {
      bounce_posix_signal_fd(r->wake_pipe_fds[1]);
      return;
    }
#endif
    (void)pthread_cond_broadcast(&r->parkers_cond);
  }
}

static inline void bounce_posix_signal_waiter(__BOUNCE_POSIX_WAITER *waiter) {
  if (waiter != NULL) {
    bounce_posix_signal_fd(waiter->control_pipe_fds[1]);
  }
}

#if defined(__linux__)
static inline void bounce_posix_signal_all_linux_parkers(BOUNCE_CORE *r) {
  if ((r == NULL) || (r->wake_pipe_fds[1] < 0)) {
    return;
  }

  for (unsigned int index = 0u; index < BOUNCE_MAX_PARKERS; index++) {
    bounce_posix_signal_fd(r->wake_pipe_fds[1]);
  }
}
#endif

static inline void bounce_posix_init_free_item(__BOUNCE_COMPLETION_ITEM *item) {
  memset(item, 0, sizeof *item);
  item->fd = -1;
  item->waiter_slot = BOUNCE_POSIX_INVALID_WAITER_SLOT;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_FREE;
}

static inline void bounce_posix_prepare_reserved_item(__BOUNCE_COMPLETION_ITEM *item) {
  item->ready_or_free_link.next = NULL;
  item->backend_link.previous = NULL;
  item->backend_link.next = NULL;
  item->cancellation_link.previous = NULL;
  item->cancellation_link.next = NULL;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  item->completion = NULL;
  item->completion_state = NULL;
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->bounce = NULL;
  item->fd = -1;
  item->events = 0;
  item->cancellation = NULL;
  item->registration_owner = NULL;
  item->condition = NULL;
  item->waiter = NULL;
  item->waiter_slot = BOUNCE_POSIX_INVALID_WAITER_SLOT;
#if defined(__linux__)
  item->io_uring_wait = NULL;
#endif
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_posix_allocate_completion_item(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item = NULL;
  BOUNCE_DYNAMIC_BLOCK *block;
  __BOUNCE_COMPLETION_ITEM *dynamic_items;
  size_t index;

  if (r == NULL) {
    return NULL;
  }

  (void)bounce_posix_lock(&r->lock);
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  (void)bounce_posix_unlock(&r->lock);
  if (item != NULL) {
    bounce_posix_prepare_reserved_item(item);
    return item;
  }

  block = bounce_dynamic_block_allocate(
    sizeof(__BOUNCE_COMPLETION_ITEM),
    BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP);
  if (block == NULL) {
    return NULL;
  }

  dynamic_items =
    (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_items(block);
  for (index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
    bounce_posix_init_free_item(&dynamic_items[index]);
  }

  (void)bounce_posix_lock(&r->lock);
  bounce_dynamic_block_list_prepend(&r->dynamic_completion_blocks, block);
  for (index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
    bounce_stack_push(&r->free_items, &dynamic_items[index].ready_or_free_link);
  }
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  (void)bounce_posix_unlock(&r->lock);

  if (item != NULL) {
    bounce_posix_prepare_reserved_item(item);
  }
  return item;
}

static inline void bounce_posix_recycle_completion_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  if ((r == NULL) || (item == NULL)) {
    return;
  }

  bounce_posix_init_free_item(item);
  (void)bounce_posix_lock(&r->lock);
  bounce_stack_push(&r->free_items, &item->ready_or_free_link);
  (void)bounce_posix_unlock(&r->lock);
}

static inline void bounce_posix_queue_ready_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  item->completion_result = result;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_READY;
  bounce_queue_enqueue(&r->ready_queue, &item->ready_or_free_link);
}

static inline bool bounce_posix_has_pending_waits_locked(BOUNCE_CORE *r) {
  BOUNCE_DYNAMIC_BLOCK *block;

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    if (r->static_completion_items[index].state ==
        BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      return true;
    }
  }

  block = r->dynamic_completion_blocks.head;
  while (block != NULL) {
    __BOUNCE_COMPLETION_ITEM *items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);

    for (size_t index = 0u; index < block->item_count; index++) {
      if (items[index].state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
        return true;
      }
    }
    block = block->next;
  }
  return false;
}

static inline bool bounce_posix_should_exit_locked(BOUNCE_CORE *r) {
  return (r->shutting_down != 0) &&
         (r->ready_queue.head == NULL) &&
         ((r->shutdown_wait_for_idle == 0) ||
          !bounce_posix_has_pending_waits_locked(r));
}

static inline bool bounce_posix_can_inline_locked(BOUNCE_CORE *r) {
  BOUNCE_POSIX_DISPATCH_CONTEXT *context =
    bounce_posix_get_dispatch_context();

  return (context != NULL) &&
         (context->bounce == r) &&
         (context->depth < context->max_inline_depth) &&
         (r->shutting_down == 0) &&
         (r->ready_queue.head == NULL);
}

static inline __BOUNCE_POSIX_START_RESULT
bounce_posix_finish_immediate_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  if (bounce_posix_can_inline_locked(r)) {
    item->completion_result = result;
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
    return __BOUNCE_POSIX_START_RESULT_READY_INLINE;
  }

  bounce_posix_queue_ready_locked(r, item, result);
  return __BOUNCE_POSIX_START_RESULT_READY_QUEUED;
}

static inline bool bounce_posix_item_is_cancellation_linked(
  const __BOUNCE_COMPLETION_ITEM *item) {
  return (item->cancellation != NULL) &&
         bounce_posix_list_item_is_linked(
           &item->cancellation->listeners,
           &item->cancellation_link);
}

static inline bool bounce_posix_item_is_condition_linked(
  const __BOUNCE_COMPLETION_ITEM *item) {
  return (item->condition != NULL) &&
         bounce_posix_list_item_is_linked(
           &item->condition->waiters,
           &item->backend_link);
}

static inline void bounce_posix_unlink_cancellation_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  BOUNCE_CANCELLATION *cancellation = item->cancellation;

  if (cancellation == NULL) {
    return;
  }

  (void)bounce_posix_lock(&cancellation->lock);
  if (bounce_posix_item_is_cancellation_linked(item)) {
    bounce_list_remove(&cancellation->listeners, &item->cancellation_link);
  }
  (void)bounce_posix_unlock(&cancellation->lock);

  item->cancellation = NULL;
}

static inline void bounce_posix_unlink_condition_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  BOUNCE_POSIX_CONDITION *condition = item->condition;

  if (condition == NULL) {
    return;
  }

  (void)bounce_posix_lock(&condition->lock);
  if (bounce_posix_item_is_condition_linked(item)) {
    bounce_list_remove(&condition->waiters, &item->backend_link);
  }
  (void)bounce_posix_unlock(&condition->lock);

  item->condition = NULL;
}

static inline bool bounce_posix_register_waiter_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  unsigned int waiter_index;
  unsigned int best_waiter_index = BOUNCE_POSIX_INVALID_WAITER_SLOT;
  unsigned int best_slot = BOUNCE_POSIX_INVALID_WAITER_SLOT;
  unsigned int best_active_count = UINT32_MAX;

  for (waiter_index = 0u; waiter_index < BOUNCE_MAX_POSIX_WAITERS; waiter_index++) {
    __BOUNCE_POSIX_WAITER *waiter = &r->waiters[waiter_index];
    unsigned int slot;

    if (!waiter->thread_created ||
        (waiter->control_pipe_fds[0] < 0) ||
        (waiter->control_pipe_fds[1] < 0)) {
      continue;
    }

    (void)bounce_posix_lock(&waiter->lock);
    if (waiter->active_count < best_active_count) {
      for (slot = 0u; slot < BOUNCE_MAX_POSIX_WAITS_PER_WAITER; slot++) {
        if (waiter->slots[slot] == NULL) {
          best_waiter_index = waiter_index;
          best_slot = slot;
          best_active_count = waiter->active_count;
          break;
        }
      }
    }
    (void)bounce_posix_unlock(&waiter->lock);
  }

  if (best_waiter_index == BOUNCE_POSIX_INVALID_WAITER_SLOT) {
    return false;
  }

  (void)bounce_posix_lock(&r->waiters[best_waiter_index].lock);
  if (r->waiters[best_waiter_index].slots[best_slot] != NULL) {
    for (best_slot = 0u;
         best_slot < BOUNCE_MAX_POSIX_WAITS_PER_WAITER;
         best_slot++) {
      if (r->waiters[best_waiter_index].slots[best_slot] == NULL) {
        break;
      }
    }
    if (best_slot >= BOUNCE_MAX_POSIX_WAITS_PER_WAITER) {
      (void)bounce_posix_unlock(&r->waiters[best_waiter_index].lock);
      return false;
    }
  }

  r->waiters[best_waiter_index].slots[best_slot] = item;
  r->waiters[best_waiter_index].active_count += 1u;
  item->waiter = &r->waiters[best_waiter_index];
  item->waiter_slot = best_slot;
  (void)bounce_posix_unlock(&r->waiters[best_waiter_index].lock);

  bounce_posix_signal_waiter(&r->waiters[best_waiter_index]);
  return true;
}

static inline void bounce_posix_remove_waiter_locked(
  __BOUNCE_COMPLETION_ITEM *item,
  bool signal_refresh) {
  __BOUNCE_POSIX_WAITER *waiter = item->waiter;
  unsigned int waiter_slot = item->waiter_slot;

  if ((waiter == NULL) ||
      (waiter_slot == BOUNCE_POSIX_INVALID_WAITER_SLOT)) {
    return;
  }

  (void)bounce_posix_lock(&waiter->lock);
  if ((waiter_slot < BOUNCE_MAX_POSIX_WAITS_PER_WAITER) &&
      (waiter->slots[waiter_slot] == item)) {
    waiter->slots[waiter_slot] = NULL;
    if (waiter->active_count > 0u) {
      waiter->active_count -= 1u;
    }
  }
  (void)bounce_posix_unlock(&waiter->lock);

  item->waiter = NULL;
  item->waiter_slot = BOUNCE_POSIX_INVALID_WAITER_SLOT;
  if (signal_refresh) {
    bounce_posix_signal_waiter(waiter);
  }
}

static inline void bounce_posix_detach_wait_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  bounce_posix_unlink_cancellation_locked(item);
  bounce_posix_unlink_condition_locked(item);
  bounce_posix_remove_waiter_locked(item, true);
#if defined(__linux__)
  if (item->io_uring_wait != NULL) {
    __BOUNCE_POSIX_IO_URING_WAIT *wait = item->io_uring_wait;

    item->io_uring_wait = NULL;
    wait->item = NULL;
    if (wait->operation != NULL) {
      wait->operation->active = 0;
      wait->operation = NULL;
    }
    (void)bounce_posix_submit_cancel_io_uring_wait_locked(item->bounce, wait);
  }
#endif

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_posix_dequeue_ready_locked(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item =
    (__BOUNCE_COMPLETION_ITEM *)bounce_queue_dequeue(&r->ready_queue);

  if (item != NULL) {
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  }
  return item;
}

static inline void bounce_posix_execute_claimed_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  unsigned int max_inline_depth) {
  BOUNCE_POSIX_DISPATCH_CONTEXT *context =
    bounce_posix_get_dispatch_context();
  BOUNCE_POSIX_DISPATCH_CONTEXT local_context;
  const bool installed_context =
    (context == NULL) || (context->bounce != r);
  const BOUNCE_COMPLETION completion = item->completion;
  void *completion_state = item->completion_state;
  const BOUNCE_COMPLETION_RESULT result = item->completion_result;

  if (installed_context) {
    local_context.previous = context;
    local_context.bounce = r;
    local_context.depth = 0u;
    local_context.max_inline_depth = max_inline_depth;
    bounce_posix_set_dispatch_context(&local_context);
    context = &local_context;
  }

  context->depth += 1u;
  bounce_posix_complete_direct(completion, completion_state, result);
  context->depth -= 1u;

  if (installed_context) {
    bounce_posix_set_dispatch_context(local_context.previous);
  }

  bounce_posix_recycle_completion_item(r, item);
}

static inline bool bounce_posix_dispatch_next_ready(
  BOUNCE_CORE *r,
  unsigned int max_inline_depth) {
  __BOUNCE_COMPLETION_ITEM *item;

  (void)bounce_posix_lock(&r->lock);
  item = bounce_posix_dequeue_ready_locked(r);
  (void)bounce_posix_unlock(&r->lock);

  if (item == NULL) {
    return false;
  }

  bounce_posix_execute_claimed_item(r, item, max_inline_depth);
  return true;
}

static inline void bounce_posix_abort_pending_item_locked(
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_QUEUE *abort_queue) {
  if ((item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_READY)) {
    return;
  }

  bounce_posix_detach_wait_locked(item);
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_queue_enqueue(abort_queue, &item->ready_or_free_link);
}

static inline void bounce_posix_collect_abort_items_locked(
  BOUNCE_CORE *r,
  BOUNCE_QUEUE *abort_queue) {
  BOUNCE_DYNAMIC_BLOCK *block;

  bounce_queue_init(&r->ready_queue);
  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_posix_abort_pending_item_locked(
      &r->static_completion_items[index],
      abort_queue);
  }

  block = r->dynamic_completion_blocks.head;
  while (block != NULL) {
    __BOUNCE_COMPLETION_ITEM *items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);
    size_t index;

    for (index = 0u; index < block->item_count; index++) {
      bounce_posix_abort_pending_item_locked(&items[index], abort_queue);
    }
    block = block->next;
  }
}

#if !defined(__linux__)
static void *bounce_posix_waiter_proc(void *parameter) {
  __BOUNCE_POSIX_WAITER *waiter = (__BOUNCE_POSIX_WAITER *)parameter;
  BOUNCE_CORE *bounce = waiter->bounce;
  struct pollfd poll_fds[1 + BOUNCE_MAX_POSIX_WAITS_PER_WAITER];
  __BOUNCE_COMPLETION_ITEM *snapshot_items[BOUNCE_MAX_POSIX_WAITS_PER_WAITER];

  for (;;) {
    nfds_t poll_count = 1u;

    poll_fds[0].fd = waiter->control_pipe_fds[0];
    poll_fds[0].events = POLLIN;
    poll_fds[0].revents = 0;

    (void)bounce_posix_lock(&waiter->lock);
    for (unsigned int slot = 0u; slot < BOUNCE_MAX_POSIX_WAITS_PER_WAITER; slot++) {
      __BOUNCE_COMPLETION_ITEM *item = waiter->slots[slot];

      if (item != NULL) {
        snapshot_items[poll_count - 1u] = item;
        poll_fds[poll_count].fd = item->fd;
        poll_fds[poll_count].events = item->events;
        poll_fds[poll_count].revents = 0;
        poll_count += 1u;
      }
    }
    (void)bounce_posix_unlock(&waiter->lock);

    for (;;) {
      const int poll_result = poll(&poll_fds[0], poll_count, -1);

      if (poll_result >= 0) {
        break;
      }
      if (errno != EINTR) {
        break;
      }
    }

    if ((poll_fds[0].revents & POLLIN) != 0) {
      bounce_posix_drain_fd(waiter->control_pipe_fds[0]);
      if ((bounce->shutting_down != 0) &&
          (bounce->shutdown_wait_for_idle == 0)) {
        return NULL;
      }
      continue;
    }
    if ((bounce->shutting_down != 0) &&
        (bounce->shutdown_wait_for_idle == 0)) {
      return NULL;
    }

    for (nfds_t index = 1u; index < poll_count; index++) {
      __BOUNCE_COMPLETION_ITEM *item = snapshot_items[index - 1u];
      const short revents = poll_fds[index].revents;

      if ((item == NULL) ||
          (revents == 0)) {
        continue;
      }
      if ((revents & (item->events | POLLERR | POLLHUP | POLLNVAL)) == 0) {
        continue;
      }

      (void)bounce_posix_lock(&bounce->lock);
      if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
          (item->waiter == waiter)) {
        bounce_posix_remove_waiter_locked(item, false);
        bounce_posix_unlink_cancellation_locked(item);
        if (item->registration_owner != NULL) {
          if (item->registration_owner->item == item) {
            item->registration_owner->item = NULL;
          }
          item->registration_owner = NULL;
        }
        bounce_posix_queue_ready_locked(
          bounce,
          item,
          BOUNCE_COMPLETION_COMPLETED);
        (void)bounce_posix_unlock(&bounce->lock);
        bounce_posix_signal_parkers(bounce);
      } else {
        (void)bounce_posix_unlock(&bounce->lock);
      }
    }
  }
}
#endif

static inline __BOUNCE_POSIX_START_RESULT
bounce_posix_activate_wait_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_CANCELLATION *cancellation) {
  if (r->shutting_down != 0) {
    return __BOUNCE_POSIX_START_RESULT_FAILED;
  }

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item != NULL) {
      return __BOUNCE_POSIX_START_RESULT_FAILED;
    }
    item->registration_owner->item = item;
  }

  item->state = BOUNCE_COMPLETION_ITEM_STATE_WAITING;
  if (cancellation != NULL) {
    (void)bounce_posix_lock(&cancellation->lock);
    if (cancellation->canceled != 0) {
      (void)bounce_posix_unlock(&cancellation->lock);
      if (item->registration_owner != NULL) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
      item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
      return bounce_posix_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
    }

    item->cancellation = cancellation;
    bounce_list_insert_tail(&cancellation->listeners, &item->cancellation_link);
    (void)bounce_posix_unlock(&cancellation->lock);
  }
  return __BOUNCE_POSIX_START_RESULT_WAITING;
}

#if defined(__linux__)
static inline uint64_t bounce_posix_io_uring_make_user_data(
  const __BOUNCE_POSIX_IO_URING_WAIT *wait,
  bool cancel_entry) {
  return ((uint64_t)(uintptr_t)wait) |
         (cancel_entry ? BOUNCE_POSIX_IO_URING_USER_DATA_CANCEL_FLAG : 0u);
}

static inline __BOUNCE_POSIX_IO_URING_WAIT *
bounce_posix_io_uring_wait_from_user_data(uint64_t user_data) {
  return (__BOUNCE_POSIX_IO_URING_WAIT *)(uintptr_t)(
    user_data & ~BOUNCE_POSIX_IO_URING_USER_DATA_CANCEL_FLAG);
}

static inline bool bounce_posix_io_uring_user_data_is_cancel(uint64_t user_data) {
  return (user_data & BOUNCE_POSIX_IO_URING_USER_DATA_CANCEL_FLAG) != 0u;
}

static inline void bounce_posix_link_io_uring_wait_locked(
  BOUNCE_CORE *r,
  __BOUNCE_POSIX_IO_URING_WAIT *wait) {
  wait->next = r->io_uring_waits;
  r->io_uring_waits = wait;
}

static inline void bounce_posix_unlink_io_uring_wait_locked(
  BOUNCE_CORE *r,
  __BOUNCE_POSIX_IO_URING_WAIT *wait) {
  __BOUNCE_POSIX_IO_URING_WAIT **current = &r->io_uring_waits;

  while (*current != NULL) {
    if (*current == wait) {
      *current = wait->next;
      wait->next = NULL;
      return;
    }
    current = &(*current)->next;
  }
}

static inline __BOUNCE_POSIX_IO_URING_WAIT *
bounce_posix_allocate_io_uring_wait(void) {
  return (__BOUNCE_POSIX_IO_URING_WAIT *)calloc(1u, sizeof(__BOUNCE_POSIX_IO_URING_WAIT));
}

static inline void bounce_posix_free_io_uring_wait_locked(
  BOUNCE_CORE *r,
  __BOUNCE_POSIX_IO_URING_WAIT *wait) {
  if (wait == NULL) {
    return;
  }

  bounce_posix_unlink_io_uring_wait_locked(r, wait);
  free(wait);
}

static inline bool bounce_posix_submit_cancel_io_uring_wait_locked(
  BOUNCE_CORE *r,
  __BOUNCE_POSIX_IO_URING_WAIT *wait) {
  struct io_uring_sqe *sqe;
  int submit_result;

  if ((r == NULL) ||
      (wait == NULL) ||
      (r->io_uring_ring == NULL)) {
    return false;
  }

  sqe = io_uring_get_sqe(r->io_uring_ring);
  if (sqe == NULL) {
    submit_result = io_uring_submit(r->io_uring_ring);
    if (submit_result < 0) {
      return false;
    }
    sqe = io_uring_get_sqe(r->io_uring_ring);
    if (sqe == NULL) {
      return false;
    }
  }

  bounce_io_uring_prep_cancel_user_data(
    sqe,
    bounce_posix_io_uring_make_user_data(wait, false),
    0);
  bounce_io_uring_sqe_set_user_data(
    sqe,
    bounce_posix_io_uring_make_user_data(wait, true));
  wait->pending_cqe_count += 1u;
  submit_result = io_uring_submit(r->io_uring_ring);
  if (submit_result < 0) {
    wait->pending_cqe_count -= 1u;
    return false;
  }
  return true;
}

static inline void bounce_posix_complete_io_uring_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  __BOUNCE_POSIX_IO_URING_WAIT *wait;

  if ((r == NULL) || (item == NULL)) {
    return;
  }

  wait = item->io_uring_wait;
  if (wait != NULL) {
    item->io_uring_wait = NULL;
    wait->item = NULL;
    if (wait->operation != NULL) {
      wait->operation->active = 0;
      wait->operation = NULL;
    }
    (void)bounce_posix_submit_cancel_io_uring_wait_locked(r, wait);
  }

  bounce_posix_unlink_cancellation_locked(item);
  if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }
  bounce_posix_queue_ready_locked(r, item, result);
}

static inline void bounce_posix_drain_io_uring_locked(BOUNCE_CORE *r) {
  struct io_uring_cqe *cqe = NULL;

  if ((r == NULL) || (r->io_uring_ring == NULL)) {
    return;
  }

  while (io_uring_peek_cqe(r->io_uring_ring, &cqe) == 0) {
    __BOUNCE_POSIX_IO_URING_WAIT *wait =
      bounce_posix_io_uring_wait_from_user_data(cqe->user_data);
    const bool cancel_entry =
      bounce_posix_io_uring_user_data_is_cancel(cqe->user_data);

    if (wait != NULL) {
      if (!cancel_entry && (wait->operation != NULL)) {
        wait->operation->result = cqe->res;
        wait->operation->cqe_flags = cqe->flags;
        wait->operation->active = 0;
      }

      if (!cancel_entry && (wait->item != NULL)) {
        __BOUNCE_COMPLETION_ITEM *item = wait->item;

        item->io_uring_wait = NULL;
        wait->item = NULL;
        wait->operation = NULL;
        bounce_posix_unlink_cancellation_locked(item);
        if (item->registration_owner != NULL) {
          if (item->registration_owner->item == item) {
            item->registration_owner->item = NULL;
          }
          item->registration_owner = NULL;
        }
        bounce_posix_queue_ready_locked(
          r,
          item,
          BOUNCE_COMPLETION_COMPLETED);
      }

      if (wait->pending_cqe_count > 0u) {
        wait->pending_cqe_count -= 1u;
      }
      if ((wait->pending_cqe_count == 0u) &&
          (wait->item == NULL)) {
        bounce_posix_free_io_uring_wait_locked(r, wait);
      }
    }

    io_uring_cqe_seen(r->io_uring_ring, cqe);
  }
}
#endif

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_init(BOUNCE_CORE *r) {
  if (r == NULL) {
    return;
  }

  memset(r, 0, sizeof *r);
  (void)pthread_mutex_init(&r->lock, NULL);
  (void)pthread_cond_init(&r->parkers_cond, NULL);
  bounce_queue_init(&r->ready_queue);
  bounce_stack_init(&r->free_items);
  bounce_dynamic_block_list_init(&r->dynamic_completion_blocks);
#if defined(__linux__)
  r->wake_pipe_fds[0] = -1;
  r->wake_pipe_fds[1] = -1;
  r->io_uring_event_fd = -1;
#endif

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_posix_init_free_item(&r->static_completion_items[index]);
    bounce_stack_push(
      &r->free_items,
      &r->static_completion_items[index].ready_or_free_link);
  }

#if defined(__linux__)
  if (bounce_posix_create_pipe_nonblocking(&r->wake_pipe_fds[0])) {
    r->linux_unified_wait_enabled = true;
  }

  r->io_uring_ring =
    (struct io_uring *)calloc(1u, sizeof(struct io_uring));
  if (r->io_uring_ring != NULL) {
    if (io_uring_queue_init(
          BOUNCE_POSIX_IO_URING_QUEUE_DEPTH,
          r->io_uring_ring,
          0u) == 0) {
      r->io_uring_event_fd = bounce_posix_create_eventfd();
      if ((r->io_uring_event_fd >= 0) &&
          (io_uring_register_eventfd(
             r->io_uring_ring,
             r->io_uring_event_fd) == 0)) {
      } else {
        if (r->io_uring_event_fd >= 0) {
          (void)close(r->io_uring_event_fd);
          r->io_uring_event_fd = -1;
        }
        io_uring_queue_exit(r->io_uring_ring);
        free(r->io_uring_ring);
        r->io_uring_ring = NULL;
      }
    } else {
      free(r->io_uring_ring);
      r->io_uring_ring = NULL;
    }
  }
#endif

  for (unsigned int waiter_index = 0u;
       waiter_index < BOUNCE_MAX_POSIX_WAITERS;
       waiter_index++) {
    __BOUNCE_POSIX_WAITER *waiter = &r->waiters[waiter_index];

    memset(waiter, 0, sizeof *waiter);
    waiter->bounce = r;
    waiter->control_pipe_fds[0] = -1;
    waiter->control_pipe_fds[1] = -1;
    (void)pthread_mutex_init(&waiter->lock, NULL);
#if !defined(__linux__)
    if (!bounce_posix_create_pipe_nonblocking(&waiter->control_pipe_fds[0])) {
      continue;
    }
    if (pthread_create(&waiter->thread, NULL, bounce_posix_waiter_proc, waiter) == 0) {
      waiter->thread_created = true;
      continue;
    }

    (void)close(waiter->control_pipe_fds[0]);
    (void)close(waiter->control_pipe_fds[1]);
    waiter->control_pipe_fds[0] = -1;
    waiter->control_pipe_fds[1] = -1;
#endif
  }
}

/**
 * @brief Initialize a backend-local POSIX condition object.
 * @param condition Condition storage provided by the caller.
 */
void bounce_posix_condition_init(BOUNCE_POSIX_CONDITION *condition) {
  if (condition == NULL) {
    return;
  }

  memset(condition, 0, sizeof *condition);
  (void)pthread_mutex_init(&condition->lock, NULL);
  bounce_list_init(&condition->waiters);
}

#if defined(__linux__)
void bounce_posix_io_uring_op_init(
  BOUNCE_POSIX_IO_URING_OP *op,
  BOUNCE_POSIX_IO_URING_PREPARE prepare,
  void *prepare_state) {
  if (op == NULL) {
    return;
  }

  memset(op, 0, sizeof *op);
  op->prepare = prepare;
  op->prepare_state = prepare_state;
}

void bounce_posix_io_uring_op_deinit(BOUNCE_POSIX_IO_URING_OP *op) {
  if (op == NULL) {
    return;
  }

  op->prepare = NULL;
  op->prepare_state = NULL;
  op->result = 0;
  op->cqe_flags = 0u;
  op->active = 0;
}

int bounce_posix_io_uring_op_result(const BOUNCE_POSIX_IO_URING_OP *op) {
  return (op != NULL) ? op->result : 0;
}

unsigned int bounce_posix_io_uring_op_cqe_flags(
  const BOUNCE_POSIX_IO_URING_OP *op) {
  return (op != NULL) ? op->cqe_flags : 0u;
}
#endif

#if defined(__linux__)
static inline size_t bounce_posix_count_fd_waits_locked(BOUNCE_CORE *r) {
  size_t count = 0u;
  BOUNCE_DYNAMIC_BLOCK *block;

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    __BOUNCE_COMPLETION_ITEM *item = &r->static_completion_items[index];

    if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
        (item->fd >= 0) &&
        (item->io_uring_wait == NULL)) {
      count += 1u;
    }
  }

  block = r->dynamic_completion_blocks.head;
  while (block != NULL) {
    __BOUNCE_COMPLETION_ITEM *items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);

    for (size_t index = 0u; index < block->item_count; index++) {
      if ((items[index].state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
          (items[index].fd >= 0) &&
          (items[index].io_uring_wait == NULL)) {
        count += 1u;
      }
    }
    block = block->next;
  }
  return count;
}

static inline void bounce_posix_collect_fd_waits_locked(
  BOUNCE_CORE *r,
  struct pollfd *poll_fds,
  __BOUNCE_COMPLETION_ITEM **items,
  nfds_t first_index) {
  nfds_t index = first_index;
  BOUNCE_DYNAMIC_BLOCK *block;

  for (size_t item_index = 0u;
       item_index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS;
       item_index++) {
    __BOUNCE_COMPLETION_ITEM *item = &r->static_completion_items[item_index];

    if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
        (item->fd >= 0) &&
        (item->io_uring_wait == NULL)) {
      poll_fds[index].fd = item->fd;
      poll_fds[index].events = item->events;
      poll_fds[index].revents = 0;
      items[index - first_index] = item;
      index += 1u;
    }
  }

  block = r->dynamic_completion_blocks.head;
  while (block != NULL) {
    __BOUNCE_COMPLETION_ITEM *dynamic_items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);

    for (size_t item_index = 0u; item_index < block->item_count; item_index++) {
      __BOUNCE_COMPLETION_ITEM *item = &dynamic_items[item_index];

      if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
          (item->fd >= 0) &&
          (item->io_uring_wait == NULL)) {
        poll_fds[index].fd = item->fd;
        poll_fds[index].events = item->events;
        poll_fds[index].revents = 0;
        items[index - first_index] = item;
        index += 1u;
      }
    }
    block = block->next;
  }
}

static inline bool bounce_posix_wait_linux_once(
  BOUNCE_CORE *r,
  int timeout_ms) {
  struct pollfd *poll_fds = NULL;
  __BOUNCE_COMPLETION_ITEM **items = NULL;
  nfds_t poll_count = 1u;
  nfds_t first_fd_wait_index;
  size_t fd_wait_count = 0u;
  bool wake_parkers = false;
  int poll_result;

  if ((r == NULL) || !r->linux_unified_wait_enabled) {
    return false;
  }

  (void)bounce_posix_lock(&r->lock);
  if (bounce_posix_should_exit_locked(r)) {
    (void)bounce_posix_unlock(&r->lock);
    return true;
  }

  fd_wait_count = bounce_posix_count_fd_waits_locked(r);
  poll_count += (r->io_uring_event_fd >= 0) ? 1u : 0u;
  poll_count += (nfds_t)fd_wait_count;

  poll_fds =
    (struct pollfd *)calloc((size_t)poll_count, sizeof(struct pollfd));
  if (fd_wait_count > 0u) {
    items =
      (__BOUNCE_COMPLETION_ITEM **)calloc(
        fd_wait_count,
        sizeof(__BOUNCE_COMPLETION_ITEM *));
  }

  if ((poll_fds == NULL) ||
      ((fd_wait_count > 0u) && (items == NULL))) {
    free(items);
    free(poll_fds);
    (void)bounce_posix_unlock(&r->lock);
    return false;
  }

  poll_fds[0].fd = r->wake_pipe_fds[0];
  poll_fds[0].events = POLLIN;
  poll_fds[0].revents = 0;

  first_fd_wait_index = 1u;
  if (r->io_uring_event_fd >= 0) {
    poll_fds[first_fd_wait_index].fd = r->io_uring_event_fd;
    poll_fds[first_fd_wait_index].events = POLLIN;
    poll_fds[first_fd_wait_index].revents = 0;
    first_fd_wait_index += 1u;
  }
  if (fd_wait_count > 0u) {
    bounce_posix_collect_fd_waits_locked(
      r,
      poll_fds,
      items,
      first_fd_wait_index);
  }
  (void)bounce_posix_unlock(&r->lock);

  for (;;) {
    poll_result = poll(poll_fds, poll_count, timeout_ms);
    if ((poll_result >= 0) || (errno != EINTR)) {
      break;
    }
  }

  if (poll_result > 0) {
    if ((poll_fds[0].revents & POLLIN) != 0) {
      bounce_posix_consume_fd_signal(r->wake_pipe_fds[0]);
    }

    if ((r->io_uring_event_fd >= 0) &&
        ((poll_fds[1].revents & POLLIN) != 0)) {
      (void)bounce_posix_lock(&r->lock);
      bounce_posix_drain_eventfd(r->io_uring_event_fd);
      bounce_posix_drain_io_uring_locked(r);
      wake_parkers = (r->ready_queue.head != NULL);
      (void)bounce_posix_unlock(&r->lock);
    }

    for (nfds_t index = first_fd_wait_index; index < poll_count; index++) {
      __BOUNCE_COMPLETION_ITEM *item = items[index - first_fd_wait_index];
      const short revents = poll_fds[index].revents;

      if ((item == NULL) || (revents == 0)) {
        continue;
      }
      if ((revents & (item->events | POLLERR | POLLHUP | POLLNVAL)) == 0) {
        continue;
      }

      (void)bounce_posix_lock(&r->lock);
      if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
          (item->fd == poll_fds[index].fd) &&
          (item->io_uring_wait == NULL)) {
        bounce_posix_remove_waiter_locked(item, false);
        bounce_posix_unlink_cancellation_locked(item);
        if (item->registration_owner != NULL) {
          if (item->registration_owner->item == item) {
            item->registration_owner->item = NULL;
          }
          item->registration_owner = NULL;
        }
        bounce_posix_queue_ready_locked(
          r,
          item,
          BOUNCE_COMPLETION_COMPLETED);
        wake_parkers = true;
      }
      (void)bounce_posix_unlock(&r->lock);
    }
  }

  free(items);
  free(poll_fds);

  if (wake_parkers) {
    bounce_posix_signal_parkers(r);
  }
  return false;
}
#endif

/**
 * @brief Park current thread and run continuation repeatedly.
 * @param r Initialized BOUNCE_CORE.
 * @return True when succeeded continuation pumps.
 * @remarks The thread will block inside. Release when `shutdown()` called.
 */
bool bounce_park(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  if (r == NULL) {
    return false;
  }

  for (;;) {
    while (bounce_posix_dispatch_next_ready(r, max_inline_depth)) {
    }

#if defined(__linux__)
    if (r->linux_unified_wait_enabled) {
      if (bounce_posix_wait_linux_once(r, -1)) {
        return true;
      }
      continue;
    }
#endif

    (void)bounce_posix_lock(&r->lock);
    while (r->ready_queue.head == NULL) {
      if (bounce_posix_should_exit_locked(r)) {
        (void)bounce_posix_unlock(&r->lock);
        return true;
      }
      (void)pthread_cond_wait(&r->parkers_cond, &r->lock);
    }
    (void)bounce_posix_unlock(&r->lock);
  }
}

/**
 * @brief Pump current thread once without waiting for new completion work.
 * @param r Initialized BOUNCE_CORE.
 * @param max_inline_depth Maximum number of inline nested completion executions.
 * @return True when succeeded continuation pumps.
 */
bool bounce_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  if (r == NULL) {
    return false;
  }

  while (bounce_posix_dispatch_next_ready(r, max_inline_depth)) {
  }
#if defined(__linux__)
  if (r->linux_unified_wait_enabled) {
    (void)bounce_posix_wait_linux_once(r, 0);
    while (bounce_posix_dispatch_next_ready(r, max_inline_depth)) {
    }
  }
#endif
  return true;
}

/**
 * @brief Post completion, will continue completion on the parked thread.
 * @param r Initialized BOUNCE_CORE.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @return True when succeeded.
 */
bool bounce_post(BOUNCE_CORE *r, BOUNCE_COMPLETION completion, void *completion_state) {
  __BOUNCE_COMPLETION_ITEM *item;

  if ((r == NULL) ||
      (completion == NULL)) {
    return false;
  }

  (void)bounce_posix_lock(&r->lock);
  if (bounce_posix_can_inline_locked(r)) {
    BOUNCE_POSIX_DISPATCH_CONTEXT *context =
      bounce_posix_get_dispatch_context();

    (void)bounce_posix_unlock(&r->lock);
    context->depth += 1u;
    completion(BOUNCE_COMPLETION_COMPLETED, completion_state);
    context->depth -= 1u;
    return true;
  }
  (void)bounce_posix_unlock(&r->lock);

  item = bounce_posix_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;

  (void)bounce_posix_lock(&r->lock);
  if (r->shutting_down != 0) {
    (void)bounce_posix_unlock(&r->lock);
    bounce_posix_recycle_completion_item(r, item);
    return false;
  }

  bounce_posix_queue_ready_locked(
    r,
    item,
    BOUNCE_COMPLETION_COMPLETED);
  (void)bounce_posix_unlock(&r->lock);

  bounce_posix_signal_parkers(r);
  return true;
}

/**
 * @brief Await a backend-local POSIX condition.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 */
void bounce_await_posix_condition(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_CONDITION *condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  __BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_POSIX_START_RESULT start_result;

  if ((r == NULL) ||
      (condition == NULL) ||
      (completion == NULL)) {
    bounce_posix_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item = bounce_posix_allocate_completion_item(r);
  if (item == NULL) {
    bounce_posix_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->condition = condition;

  (void)bounce_posix_lock(&r->lock);
  start_result = bounce_posix_activate_wait_item_locked(
    r,
    item,
    cancellation);
  if (start_result == __BOUNCE_POSIX_START_RESULT_WAITING) {
    (void)bounce_posix_lock(&condition->lock);
    bounce_list_insert_tail(&condition->waiters, &item->backend_link);
    (void)bounce_posix_unlock(&condition->lock);
  }
  (void)bounce_posix_unlock(&r->lock);

  switch (start_result) {
    case __BOUNCE_POSIX_START_RESULT_WAITING:
      return;
    case __BOUNCE_POSIX_START_RESULT_READY_QUEUED:
      bounce_posix_signal_parkers(r);
      return;
    case __BOUNCE_POSIX_START_RESULT_READY_INLINE:
      bounce_posix_execute_claimed_item(r, item, 0u);
      return;
    case __BOUNCE_POSIX_START_RESULT_FAILED:
    default:
      bounce_posix_recycle_completion_item(r, item);
      bounce_posix_complete_direct(
        completion,
        completion_state,
        BOUNCE_COMPLETION_ABORTED);
      return;
  }
}

/**
 * @brief Raise a backend-local POSIX condition.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 */
void bounce_posix_condition_raise(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_CONDITION *condition) {
  bool wake_parkers = false;

  if ((r == NULL) ||
      (condition == NULL)) {
    return;
  }

  (void)bounce_posix_lock(&r->lock);
  (void)bounce_posix_lock(&condition->lock);
  for (;;) {
    BOUNCE_LIST_ITEM *list_item =
      (BOUNCE_LIST_ITEM *)bounce_list_pop_head(&condition->waiters);
    __BOUNCE_COMPLETION_ITEM *item;

    if (list_item == NULL) {
      break;
    }

    item = BOUNCE_POSIX_CONTAINER_OF(
      list_item,
      __BOUNCE_COMPLETION_ITEM,
      backend_link);
    item->condition = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

    bounce_posix_unlink_cancellation_locked(item);
    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_posix_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_COMPLETED);
    wake_parkers = true;
  }
  (void)bounce_posix_unlock(&condition->lock);
  (void)bounce_posix_unlock(&r->lock);

  if (wake_parkers) {
    bounce_posix_signal_parkers(r);
  }
}

/**
 * @brief Await POSIX file-descriptor readiness through a backend-local waiter.
 * @param r Initialized BOUNCE_CORE.
 * @param fd File descriptor watched by the backend-local waiter thread.
 * @param events Bitmask of `poll(2)` events such as `POLLIN` or `POLLOUT`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 */
void bounce_await_posix_fd(
  BOUNCE_CORE *r,
  int fd,
  short events,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  __BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_POSIX_START_RESULT start_result;

  if ((r == NULL) ||
      (fd < 0) ||
      (events == 0) ||
      (completion == NULL)) {
    bounce_posix_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item = bounce_posix_allocate_completion_item(r);
  if (item == NULL) {
    bounce_posix_complete_direct(
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

  (void)bounce_posix_lock(&r->lock);
  start_result = bounce_posix_activate_wait_item_locked(
    r,
    item,
    cancellation);
  if (start_result == __BOUNCE_POSIX_START_RESULT_WAITING) {
#if defined(__linux__)
    if (r->linux_unified_wait_enabled) {
      bounce_posix_signal_parkers(r);
    } else
#endif
    if (!bounce_posix_register_waiter_locked(r, item)) {
      bounce_posix_detach_wait_locked(item);
      start_result = bounce_posix_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_ABORTED);
    }
  }
  (void)bounce_posix_unlock(&r->lock);

  switch (start_result) {
    case __BOUNCE_POSIX_START_RESULT_WAITING:
      return;
    case __BOUNCE_POSIX_START_RESULT_READY_QUEUED:
      bounce_posix_signal_parkers(r);
      return;
    case __BOUNCE_POSIX_START_RESULT_READY_INLINE:
      bounce_posix_execute_claimed_item(r, item, 0u);
      return;
    case __BOUNCE_POSIX_START_RESULT_FAILED:
    default:
      bounce_posix_recycle_completion_item(r, item);
      bounce_posix_complete_direct(
        completion,
        completion_state,
        BOUNCE_COMPLETION_ABORTED);
      return;
  }
}

#if defined(__linux__)
void bounce_await_posix_io_uring_op(
  BOUNCE_CORE *r,
  BOUNCE_POSIX_IO_URING_OP *op,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  __BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_POSIX_IO_URING_WAIT *wait = NULL;
  __BOUNCE_POSIX_START_RESULT start_result;
  bool free_wait = false;

  if ((r == NULL) ||
      (op == NULL) ||
      (op->prepare == NULL) ||
      (completion == NULL)) {
    bounce_posix_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item = bounce_posix_allocate_completion_item(r);
  wait = bounce_posix_allocate_io_uring_wait();
  if ((item == NULL) || (wait == NULL)) {
    bounce_posix_recycle_completion_item(r, item);
    free(wait);
    bounce_posix_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  wait->bounce = r;
  wait->item = item;
  wait->operation = op;
  wait->pending_cqe_count = 1u;

  (void)bounce_posix_lock(&r->lock);
  start_result = bounce_posix_activate_wait_item_locked(
    r,
    item,
    cancellation);
  if (start_result == __BOUNCE_POSIX_START_RESULT_WAITING) {
    struct io_uring_sqe *sqe = NULL;
    int submit_result;

    if (!r->linux_unified_wait_enabled ||
        (r->io_uring_ring == NULL) ||
        (op->active != 0)) {
      if (item->registration_owner != NULL) {
        item->registration_owner->item = NULL;
        item->registration_owner = NULL;
      }
      bounce_posix_unlink_cancellation_locked(item);
      item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
      start_result = bounce_posix_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_ABORTED);
      free_wait = true;
    } else {
      sqe = io_uring_get_sqe(r->io_uring_ring);
      if (sqe == NULL) {
        submit_result = io_uring_submit(r->io_uring_ring);
        if (submit_result >= 0) {
          sqe = io_uring_get_sqe(r->io_uring_ring);
        }
      }

      if (sqe == NULL) {
        if (item->registration_owner != NULL) {
          item->registration_owner->item = NULL;
          item->registration_owner = NULL;
        }
        bounce_posix_unlink_cancellation_locked(item);
        item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
        start_result = bounce_posix_finish_immediate_item_locked(
          r,
          item,
          BOUNCE_COMPLETION_ABORTED);
        free_wait = true;
      } else {
        item->io_uring_wait = wait;
        bounce_posix_link_io_uring_wait_locked(r, wait);
        op->result = 0;
        op->cqe_flags = 0u;
        op->active = 1;
        op->prepare(sqe, op->prepare_state);
        bounce_io_uring_sqe_set_user_data(
          sqe,
          bounce_posix_io_uring_make_user_data(wait, false));
        submit_result = io_uring_submit(r->io_uring_ring);
        if (submit_result < 0) {
          item->io_uring_wait = NULL;
          bounce_posix_unlink_io_uring_wait_locked(r, wait);
          op->active = 0;
          if (item->registration_owner != NULL) {
            item->registration_owner->item = NULL;
            item->registration_owner = NULL;
          }
          bounce_posix_unlink_cancellation_locked(item);
          item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
          start_result = bounce_posix_finish_immediate_item_locked(
            r,
            item,
            BOUNCE_COMPLETION_ABORTED);
          free_wait = true;
        } else {
          bounce_posix_signal_parkers(r);
        }
      }
    }
  }
  (void)bounce_posix_unlock(&r->lock);

  if (free_wait) {
    free(wait);
  }

  switch (start_result) {
    case __BOUNCE_POSIX_START_RESULT_WAITING:
      return;
    case __BOUNCE_POSIX_START_RESULT_READY_QUEUED:
      bounce_posix_signal_parkers(r);
      return;
    case __BOUNCE_POSIX_START_RESULT_READY_INLINE:
      bounce_posix_execute_claimed_item(r, item, 0u);
      return;
    case __BOUNCE_POSIX_START_RESULT_FAILED:
    default:
      bounce_posix_recycle_completion_item(r, item);
      bounce_posix_complete_direct(
        completion,
        completion_state,
        BOUNCE_COMPLETION_ABORTED);
      return;
  }
}
#endif

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Shutdown parking threads.
 * @param r Initialized BOUNCE_CORE.
 */
void bounce_shutdown(BOUNCE_CORE *r, bool wait_for_idle) {
  bool signal_waiters = false;

  if (r == NULL) {
    return;
  }

  (void)bounce_posix_lock(&r->lock);
  r->shutting_down = 1;
  if (wait_for_idle) {
    r->shutdown_wait_for_idle = 1;
  } else {
    r->shutdown_wait_for_idle = 0;
    signal_waiters = true;
  }
  (void)bounce_posix_unlock(&r->lock);

  bounce_posix_signal_parkers(r);
#if defined(__linux__)
  if (r->linux_unified_wait_enabled) {
    bounce_posix_signal_all_linux_parkers(r);
  }
#endif
  if (signal_waiters) {
    for (unsigned int waiter_index = 0u;
         waiter_index < BOUNCE_MAX_POSIX_WAITERS;
         waiter_index++) {
      bounce_posix_signal_waiter(&r->waiters[waiter_index]);
    }
  }
}

/**
 * @brief Deinitialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_deinit(BOUNCE_CORE *r) {
  BOUNCE_QUEUE abort_queue;
  BOUNCE_NODE_ITEM *node;

  if (r == NULL) {
    return;
  }

  bounce_shutdown(r, false);
  bounce_queue_init(&abort_queue);

  for (unsigned int waiter_index = 0u;
       waiter_index < BOUNCE_MAX_POSIX_WAITERS;
       waiter_index++) {
    __BOUNCE_POSIX_WAITER *waiter = &r->waiters[waiter_index];

    if (waiter->thread_created) {
      (void)pthread_join(waiter->thread, NULL);
      waiter->thread_created = false;
    }
  }

  (void)bounce_posix_lock(&r->lock);
  bounce_posix_collect_abort_items_locked(r, &abort_queue);
  (void)bounce_posix_unlock(&r->lock);

  while ((node = (BOUNCE_NODE_ITEM *)bounce_queue_dequeue(&abort_queue)) != NULL) {
    __BOUNCE_COMPLETION_ITEM *item =
      BOUNCE_POSIX_CONTAINER_OF(
        node,
        __BOUNCE_COMPLETION_ITEM,
        ready_or_free_link);

    bounce_posix_complete_direct(
      item->completion,
      item->completion_state,
      BOUNCE_COMPLETION_ABORTED);
  }

#if defined(__linux__)
  if (r->io_uring_ring != NULL) {
    io_uring_queue_exit(r->io_uring_ring);
    free(r->io_uring_ring);
    r->io_uring_ring = NULL;
  }
  if (r->io_uring_event_fd >= 0) {
    (void)close(r->io_uring_event_fd);
    r->io_uring_event_fd = -1;
  }
  while (r->io_uring_waits != NULL) {
    __BOUNCE_POSIX_IO_URING_WAIT *wait = r->io_uring_waits;

    r->io_uring_waits = wait->next;
    free(wait);
  }
  if (r->wake_pipe_fds[0] >= 0) {
    (void)close(r->wake_pipe_fds[0]);
    r->wake_pipe_fds[0] = -1;
  }
  if (r->wake_pipe_fds[1] >= 0) {
    (void)close(r->wake_pipe_fds[1]);
    r->wake_pipe_fds[1] = -1;
  }
#endif

  for (unsigned int waiter_index = 0u;
       waiter_index < BOUNCE_MAX_POSIX_WAITERS;
       waiter_index++) {
    __BOUNCE_POSIX_WAITER *waiter = &r->waiters[waiter_index];

    if (waiter->control_pipe_fds[0] >= 0) {
      (void)close(waiter->control_pipe_fds[0]);
      waiter->control_pipe_fds[0] = -1;
    }
    if (waiter->control_pipe_fds[1] >= 0) {
      (void)close(waiter->control_pipe_fds[1]);
      waiter->control_pipe_fds[1] = -1;
    }
    (void)pthread_mutex_destroy(&waiter->lock);
  }

  bounce_dynamic_block_list_free_all(&r->dynamic_completion_blocks);
  (void)pthread_cond_destroy(&r->parkers_cond);
  (void)pthread_mutex_destroy(&r->lock);
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a cancellation.
 * @param cancellation Cancellation storage provided by the caller.
 */
void bounce_cancellation_init(BOUNCE_CANCELLATION *cancellation) {
  if (cancellation == NULL) {
    return;
  }

  memset(cancellation, 0, sizeof *cancellation);
  (void)pthread_mutex_init(&cancellation->lock, NULL);
  bounce_list_init(&cancellation->listeners);
}

/**
 * @brief Initialize a cancellation registration.
 * @param registration Cancellation registration storage provided by the caller.
 */
void bounce_cancellation_registration_init(
  BOUNCE_CANCELLATION_REGISTRATION *registration) {
  if (registration != NULL) {
    registration->item = NULL;
  }
}

/**
 * @brief Register a completion that is queued when a cancellation is canceled.
 * @param r Initialized BOUNCE_CORE.
 * @param cancellation Initialized cancellation to observe.
 * @param registration Caller-owned registration storage.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @return True when succeeded.
 */
bool bounce_register_canceled(
  BOUNCE_CORE *r,
  BOUNCE_CANCELLATION *cancellation,
  BOUNCE_CANCELLATION_REGISTRATION *registration,
  BOUNCE_COMPLETION completion,
  void *completion_state) {
  __BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_POSIX_START_RESULT start_result;

  if ((r == NULL) ||
      (cancellation == NULL) ||
      (registration == NULL) ||
      (completion == NULL)) {
    return false;
  }

  item = bounce_posix_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->registration_owner = registration;

  (void)bounce_posix_lock(&r->lock);
  start_result = bounce_posix_activate_wait_item_locked(
    r,
    item,
    cancellation);
  (void)bounce_posix_unlock(&r->lock);

  switch (start_result) {
    case __BOUNCE_POSIX_START_RESULT_WAITING:
      return true;
    case __BOUNCE_POSIX_START_RESULT_READY_QUEUED:
      bounce_posix_signal_parkers(r);
      return true;
    case __BOUNCE_POSIX_START_RESULT_READY_INLINE:
      bounce_posix_execute_claimed_item(r, item, 0u);
      return true;
    case __BOUNCE_POSIX_START_RESULT_FAILED:
    default:
      bounce_posix_recycle_completion_item(r, item);
      return false;
  }
}

/**
 * @brief Unregister a previously registered cancellation completion.
 * @param registration Initialized cancellation registration.
 * @return True when unregister wins.
 */
bool bounce_unregister_canceled(BOUNCE_CANCELLATION_REGISTRATION *registration) {
  __BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_CORE *bounce;

  if (registration == NULL) {
    return false;
  }

  item = registration->item;
  if (item == NULL) {
    return false;
  }

  bounce = item->bounce;
  if (bounce == NULL) {
    return false;
  }

  (void)bounce_posix_lock(&bounce->lock);
  item = registration->item;
  if ((item == NULL) ||
      (item->registration_owner != registration) ||
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING)) {
    (void)bounce_posix_unlock(&bounce->lock);
    return false;
  }

  bounce_posix_detach_wait_locked(item);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  (void)bounce_posix_unlock(&bounce->lock);

  bounce_posix_recycle_completion_item(bounce, item);
  return true;
}

/**
 * @brief Deinitialize a cancellation registration.
 * @param registration Cancellation registration storage provided by the caller.
 */
void bounce_cancellation_registration_deinit(
  BOUNCE_CANCELLATION_REGISTRATION *registration) {
  if (registration != NULL) {
    registration->item = NULL;
  }
}

typedef struct BOUNCE_CANCEL_CONTEXT {
  BOUNCE_CORE *bounce;
  BOUNCE_CANCELLATION *cancellation;
} BOUNCE_CANCEL_CONTEXT;

/**
 * @brief Cancel a cancellation.
 * @param r Initialized BOUNCE_CORE.
 * @param cancellation Initialized cancellation.
 * @remarks The first successful call wins. Later calls are ignored.
 */
void bounce_cancel(
  BOUNCE_CORE *r,
  BOUNCE_CANCELLATION *cancellation) {
  bool wake_parkers = false;

  if ((r == NULL) ||
      (cancellation == NULL)) {
    return;
  }

  (void)bounce_posix_lock(&r->lock);
  (void)bounce_posix_lock(&cancellation->lock);
  if (cancellation->canceled != 0) {
    (void)bounce_posix_unlock(&cancellation->lock);
    (void)bounce_posix_unlock(&r->lock);
    return;
  }

  cancellation->canceled = 1;
  for (;;) {
    BOUNCE_LIST_ITEM *list_item =
      (BOUNCE_LIST_ITEM *)bounce_list_pop_head(&cancellation->listeners);
    __BOUNCE_COMPLETION_ITEM *item;

    if (list_item == NULL) {
      break;
    }

    item = BOUNCE_POSIX_CONTAINER_OF(
      list_item,
      __BOUNCE_COMPLETION_ITEM,
      cancellation_link);
    item->cancellation = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

#if defined(__linux__)
    if (item->io_uring_wait != NULL) {
      bounce_posix_complete_io_uring_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
      wake_parkers = true;
      continue;
    }
#endif

    bounce_posix_unlink_condition_locked(item);
    bounce_posix_remove_waiter_locked(item, true);
    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_posix_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_CANCELED);
    wake_parkers = true;
  }
  (void)bounce_posix_unlock(&cancellation->lock);
  (void)bounce_posix_unlock(&r->lock);

  if (wake_parkers) {
    bounce_posix_signal_parkers(r);
  }
}

/**
 * @brief Deinitialize a cancellation.
 * @param cancellation Cancellation storage provided by the caller.
 */
void bounce_cancellation_deinit(BOUNCE_CANCELLATION *cancellation) {
  if (cancellation != NULL) {
    (void)pthread_mutex_destroy(&cancellation->lock);
  }
}

#endif
