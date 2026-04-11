/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(_WIN32)

#if !defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <limits.h>
#include <stddef.h>
#include <string.h>
#include <windows.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

/*
 * Win32 bounce architecture overview:
 *
 * The parker side and the backend wait side are intentionally separated.
 *
 * - Parker threads do not watch arbitrary Win32 handles directly.
 * - Backend waiter shards watch Win32 handles and act as backend-local producers.
 * - The common bounce path between backends is the ready queue.
 *
 * Parker responsibilities:
 *
 * - block on the bounce shutdown handle and a parker wake handle through
 *   MsgWaitForMultipleObjects()
 * - drain the ready queue
 * - execute completion callbacks on the parked thread
 * - continue to pump the Win32 message queue
 *
 * Backend waiter responsibilities:
 *
 * - own a fixed-size subset of pending Win32 waits
 * - wait for kernel handle transitions with WaitForMultipleObjectsEx()
 * - claim the raised completion item
 * - remove it from the backend wait list
 * - enqueue it into the shared ready queue
 *
 * Completion flow:
 *
 * 1. A producer enters the bounce through post() or through a backend wait.
 * 2. Backend-local code turns that work item into a ready completion item.
 * 3. The item is pushed into the shared ready queue.
 * 4. If the queue was near-empty, one or more parker wakeups are emitted.
 * 5. A parker wakes, drains the ready queue, and runs the completion callback.
 *
 * This split keeps the common execution model close to an embedded target:
 * backend code detects readiness, while parker code only performs bounded wake,
 * dequeue, and completion execution work.
 */

//////////////////////////////////////////////////////////////////////////////////

#define PARK_WAIT_INDEX_SHUTDOWN 0
#define PARK_WAIT_INDEX_WAKE 1
#define PARK_WAIT_INDEX_COUNT 2

#define WAITER_WAIT_INDEX_SHUTDOWN 0
#define WAITER_WAIT_INDEX_REFRESH 1
#define WAITER_WAIT_INDEX_EXTERNAL_BEGIN 2

#define BOUNCE_WIN32_INVALID_WAITER_SLOT ((DWORD)MAXDWORD)
#define BOUNCE_WIN32_CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct BOUNCE_WIN32_DISPATCH_CONTEXT {
  struct BOUNCE_WIN32_DISPATCH_CONTEXT *previous;
  BOUNCE_CORE *bounce;
  unsigned int depth;
  unsigned int max_inline_depth;
} BOUNCE_WIN32_DISPATCH_CONTEXT;

static INIT_ONCE bounce_win32_dispatch_tls_once_state = INIT_ONCE_STATIC_INIT;
static DWORD bounce_win32_dispatch_tls_index = TLS_OUT_OF_INDEXES;

static BOOL CALLBACK bounce_win32_dispatch_tls_once(
  PINIT_ONCE init_once,
  PVOID parameter,
  PVOID *context) {
  (void)init_once;
  (void)parameter;
  (void)context;

  bounce_win32_dispatch_tls_index = TlsAlloc();
  return bounce_win32_dispatch_tls_index != TLS_OUT_OF_INDEXES;
}

static inline BOOL bounce_win32_ensure_dispatch_tls(void) {
  return InitOnceExecuteOnce(
    &bounce_win32_dispatch_tls_once_state,
    bounce_win32_dispatch_tls_once,
    NULL,
    NULL);
}

static inline BOUNCE_WIN32_DISPATCH_CONTEXT *bounce_win32_get_dispatch_context(void) {
  return bounce_win32_ensure_dispatch_tls() ?
           (BOUNCE_WIN32_DISPATCH_CONTEXT *)TlsGetValue(
             bounce_win32_dispatch_tls_index) :
           NULL;
}

static inline void bounce_win32_set_dispatch_context(
  BOUNCE_WIN32_DISPATCH_CONTEXT *context) {
  if (bounce_win32_ensure_dispatch_tls()) {
    (void)TlsSetValue(bounce_win32_dispatch_tls_index, context);
  }
}

typedef enum BOUNCE_WIN32_MESSAGE_PUMP_RESULT {
  BOUNCE_WIN32_MESSAGE_PUMP_NONE = 0,
  BOUNCE_WIN32_MESSAGE_PUMP_DISPATCHED = 1,
  BOUNCE_WIN32_MESSAGE_PUMP_QUIT = 2
} BOUNCE_WIN32_MESSAGE_PUMP_RESULT;

typedef enum __BOUNCE_WIN32_START_RESULT {
  __BOUNCE_WIN32_START_RESULT_FAILED = 0,
  __BOUNCE_WIN32_START_RESULT_WAITING = 1,
  __BOUNCE_WIN32_START_RESULT_READY = 2
} __BOUNCE_WIN32_START_RESULT;

static inline void bounce_win32_ensure_message_queue(void) {
  MSG message;

  (void)PeekMessage(&message, NULL, 0u, 0u, PM_NOREMOVE);
}

static inline BOUNCE_WIN32_MESSAGE_PUMP_RESULT bounce_win32_pump_messages(void) {
  MSG message;
  BOUNCE_WIN32_MESSAGE_PUMP_RESULT result =
    BOUNCE_WIN32_MESSAGE_PUMP_NONE;

  while (PeekMessage(&message, NULL, 0u, 0u, PM_REMOVE)) {
    if (message.message == WM_QUIT) {
      PostQuitMessage((int)message.wParam);
      return BOUNCE_WIN32_MESSAGE_PUMP_QUIT;
    }

    TranslateMessage(&message);
    DispatchMessage(&message);
    result = BOUNCE_WIN32_MESSAGE_PUMP_DISPATCHED;
  }
  return result;
}

static inline void bounce_win32_init_free_item(__BOUNCE_COMPLETION_ITEM *item) {
  memset(item, 0, sizeof *item);
  item->waiter_slot = BOUNCE_WIN32_INVALID_WAITER_SLOT;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_FREE;
}

static inline void bounce_win32_prepare_reserved_item(__BOUNCE_COMPLETION_ITEM *item) {
  item->ready_or_free_link.next = NULL;
  item->cancellation_link.previous = NULL;
  item->cancellation_link.next = NULL;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  item->completion = NULL;
  item->completion_state = NULL;
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->bounce = NULL;
  item->wait_handle = NULL;
  item->cancellation = NULL;
  item->registration_owner = NULL;
  item->waiter = NULL;
  item->waiter_slot = BOUNCE_WIN32_INVALID_WAITER_SLOT;
}

static inline bool bounce_win32_item_is_cancellation_linked(
  const __BOUNCE_COMPLETION_ITEM *item) {
  return (item->cancellation != NULL) &&
         ((item->cancellation_link.previous != NULL) ||
          (item->cancellation_link.next != NULL) ||
          (item->cancellation->listeners.head == &item->cancellation_link));
}

static inline void bounce_win32_release_wakeups(
  BOUNCE_CORE *r,
  LONG wake_count) {
  if ((r != NULL) &&
      (r->wake_semaphore != NULL) &&
      (wake_count > 0)) {
    (void)ReleaseSemaphore(r->wake_semaphore, wake_count, NULL);
  }
}

static inline void bounce_win32_complete_direct(
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_COMPLETION_RESULT result) {
  if (completion != NULL) {
    completion(result, completion_state);
  }
}

static __BOUNCE_COMPLETION_ITEM *bounce_win32_allocate_completion_item(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item = NULL;
  BOUNCE_DYNAMIC_BLOCK *block;
  __BOUNCE_COMPLETION_ITEM *dynamic_items;
  size_t index;

  if (r == NULL) {
    return NULL;
  }

  EnterCriticalSection(&r->lock);
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  LeaveCriticalSection(&r->lock);
  if (item != NULL) {
    bounce_win32_prepare_reserved_item(item);
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
    bounce_win32_init_free_item(&dynamic_items[index]);
  }

  EnterCriticalSection(&r->lock);
  bounce_dynamic_block_list_prepend(&r->dynamic_completion_blocks, block);
  for (index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
    bounce_stack_push(&r->free_items, &dynamic_items[index].ready_or_free_link);
  }
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  LeaveCriticalSection(&r->lock);

  if (item != NULL) {
    bounce_win32_prepare_reserved_item(item);
  }
  return item;
}

static void bounce_win32_recycle_completion_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  if ((r == NULL) || (item == NULL)) {
    return;
  }

  bounce_win32_init_free_item(item);
  EnterCriticalSection(&r->lock);
  bounce_stack_push(&r->free_items, &item->ready_or_free_link);
  LeaveCriticalSection(&r->lock);
}

static inline bool bounce_win32_register_waiter_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  DWORD waiter_index;
  DWORD best_waiter_index = MAXDWORD;
  DWORD best_slot = BOUNCE_WIN32_INVALID_WAITER_SLOT;
  DWORD best_active_count = MAXDWORD;

  for (waiter_index = 0u; waiter_index < BOUNCE_MAX_WIN32_WAITERS; waiter_index++) {
    __BOUNCE_WIN32_WAITER *waiter = &r->waiters[waiter_index];
    DWORD slot;

    if ((waiter->refresh_event == NULL) ||
        (waiter->thread_handle == NULL)) {
      continue;
    }

    EnterCriticalSection(&waiter->lock);
    if (waiter->active_count < best_active_count) {
      for (slot = 0u; slot < BOUNCE_MAX_WIN32_WAITS_PER_WAITER; slot++) {
        if (waiter->slots[slot] == NULL) {
          best_waiter_index = waiter_index;
          best_slot = slot;
          best_active_count = waiter->active_count;
          break;
        }
      }
    }
    LeaveCriticalSection(&waiter->lock);
  }

  if (best_waiter_index == MAXDWORD) {
    return false;
  }

  EnterCriticalSection(&r->waiters[best_waiter_index].lock);
  if (r->waiters[best_waiter_index].slots[best_slot] != NULL) {
    for (best_slot = 0u;
         best_slot < BOUNCE_MAX_WIN32_WAITS_PER_WAITER;
         best_slot++) {
      if (r->waiters[best_waiter_index].slots[best_slot] == NULL) {
        break;
      }
    }
    if (best_slot >= BOUNCE_MAX_WIN32_WAITS_PER_WAITER) {
      LeaveCriticalSection(&r->waiters[best_waiter_index].lock);
      return false;
    }
  }

  r->waiters[best_waiter_index].slots[best_slot] = item;
  r->waiters[best_waiter_index].active_count += 1u;
  item->waiter = &r->waiters[best_waiter_index];
  item->waiter_slot = best_slot;
  LeaveCriticalSection(&r->waiters[best_waiter_index].lock);
  (void)SetEvent(r->waiters[best_waiter_index].refresh_event);
  return true;
}

static inline void bounce_win32_remove_waiter_locked(
  __BOUNCE_COMPLETION_ITEM *item,
  bool signal_refresh) {
  __BOUNCE_WIN32_WAITER *waiter = item->waiter;
  DWORD waiter_slot = item->waiter_slot;

  if ((waiter == NULL) ||
      (waiter_slot == BOUNCE_WIN32_INVALID_WAITER_SLOT)) {
    return;
  }

  EnterCriticalSection(&waiter->lock);
  if ((waiter_slot < BOUNCE_MAX_WIN32_WAITS_PER_WAITER) &&
      (waiter->slots[waiter_slot] == item)) {
    waiter->slots[waiter_slot] = NULL;
    if (waiter->active_count > 0u) {
      waiter->active_count -= 1u;
    }
    if (signal_refresh && (waiter->refresh_event != NULL)) {
      (void)SetEvent(waiter->refresh_event);
    }
  }
  LeaveCriticalSection(&waiter->lock);

  item->waiter = NULL;
  item->waiter_slot = BOUNCE_WIN32_INVALID_WAITER_SLOT;
}

static inline void bounce_win32_unlink_cancellation_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  BOUNCE_CANCELLATION *cancellation = item->cancellation;

  if (cancellation == NULL) {
    return;
  }

  EnterCriticalSection(&cancellation->lock);
  if (bounce_win32_item_is_cancellation_linked(item)) {
    bounce_list_remove(&cancellation->listeners, &item->cancellation_link);
  }
  LeaveCriticalSection(&cancellation->lock);

  item->cancellation = NULL;
}

static inline void bounce_win32_detach_wait_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  bounce_win32_remove_waiter_locked(item, true);
  bounce_win32_unlink_cancellation_locked(item);

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }
}

static inline void bounce_win32_queue_ready_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  item->completion_result = result;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_READY;
  bounce_queue_enqueue(&r->ready_queue, &item->ready_or_free_link);
}

static inline bool bounce_win32_has_pending_waits_locked(BOUNCE_CORE *r) {
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

static inline bool bounce_win32_should_exit_locked(BOUNCE_CORE *r) {
  return (r->shutting_down != 0) &&
         (r->ready_queue.head == NULL) &&
         ((r->shutdown_wait_for_idle == 0) ||
          !bounce_win32_has_pending_waits_locked(r));
}

static __BOUNCE_WIN32_START_RESULT bounce_win32_activate_wait_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  HANDLE handle,
  BOUNCE_CANCELLATION *cancellation) {
  LONG wake_count = 0;

  if ((r == NULL) ||
      (item == NULL) ||
      ((handle == NULL) && (item->registration_owner == NULL))) {
    return __BOUNCE_WIN32_START_RESULT_FAILED;
  }

  item->wait_handle = handle;
  item->cancellation = NULL;
  item->waiter = NULL;
  item->waiter_slot = BOUNCE_WIN32_INVALID_WAITER_SLOT;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_WAITING;

  EnterCriticalSection(&r->lock);
  if (r->shutting_down != 0) {
    LeaveCriticalSection(&r->lock);
    return __BOUNCE_WIN32_START_RESULT_FAILED;
  }

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item != NULL) {
      LeaveCriticalSection(&r->lock);
      return __BOUNCE_WIN32_START_RESULT_FAILED;
    }
    item->registration_owner->item = item;
  }

  if (cancellation != NULL) {
    EnterCriticalSection(&cancellation->lock);
    if (cancellation->canceled != 0) {
      LeaveCriticalSection(&cancellation->lock);
      bounce_win32_detach_wait_locked(item);
      bounce_win32_queue_ready_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
      wake_count = 1;
      LeaveCriticalSection(&r->lock);
      bounce_win32_release_wakeups(r, wake_count);
      return __BOUNCE_WIN32_START_RESULT_READY;
    }

    item->cancellation = cancellation;
    bounce_list_insert_tail(
      &cancellation->listeners,
      &item->cancellation_link);
    LeaveCriticalSection(&cancellation->lock);
  }

  if ((handle != NULL) &&
      !bounce_win32_register_waiter_locked(r, item)) {
    bounce_win32_detach_wait_locked(item);
    bounce_win32_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_ABORTED);
    wake_count = 1;
    LeaveCriticalSection(&r->lock);
    bounce_win32_release_wakeups(r, wake_count);
    return __BOUNCE_WIN32_START_RESULT_READY;
  }

  LeaveCriticalSection(&r->lock);
  return __BOUNCE_WIN32_START_RESULT_WAITING;
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_win32_dequeue_ready_locked(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item =
    (__BOUNCE_COMPLETION_ITEM *)bounce_queue_dequeue(&r->ready_queue);

  if (item != NULL) {
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  }
  return item;
}

static inline void bounce_win32_execute_claimed_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  unsigned int max_inline_depth) {
  BOUNCE_WIN32_DISPATCH_CONTEXT *context =
    bounce_win32_get_dispatch_context();
  BOUNCE_WIN32_DISPATCH_CONTEXT local_context;
  bool installed_context =
    (context == NULL) || (context->bounce != r);
  BOUNCE_COMPLETION completion = item->completion;
  void *completion_state = item->completion_state;
  BOUNCE_COMPLETION_RESULT result = item->completion_result;

  if (installed_context) {
    local_context.previous = context;
    local_context.bounce = r;
    local_context.depth = 0u;
    local_context.max_inline_depth = max_inline_depth;
    bounce_win32_set_dispatch_context(&local_context);
    context = &local_context;
  }

  context->depth += 1u;
  bounce_win32_complete_direct(completion, completion_state, result);
  context->depth -= 1u;

  if (installed_context) {
    bounce_win32_set_dispatch_context(local_context.previous);
  }

  bounce_win32_recycle_completion_item(r, item);
}

static inline bool bounce_win32_dispatch_next_ready(
  BOUNCE_CORE *r,
  unsigned int max_inline_depth) {
  __BOUNCE_COMPLETION_ITEM *item;

  EnterCriticalSection(&r->lock);
  item = bounce_win32_dequeue_ready_locked(r);
  LeaveCriticalSection(&r->lock);

  if (item == NULL) {
    return false;
  }

  bounce_win32_execute_claimed_item(r, item, max_inline_depth);
  return true;
}

static inline void bounce_win32_abort_pending_item_locked(
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_QUEUE *abort_queue) {
  if ((item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_READY)) {
    return;
  }

  bounce_win32_detach_wait_locked(item);
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_queue_enqueue(abort_queue, &item->ready_or_free_link);
}

static inline void bounce_win32_collect_abort_items_locked(
  BOUNCE_CORE *r,
  BOUNCE_QUEUE *abort_queue) {
  BOUNCE_DYNAMIC_BLOCK *block;

  bounce_queue_init(&r->ready_queue);
  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_win32_abort_pending_item_locked(
      &r->static_completion_items[index],
      abort_queue);
  }

  block = r->dynamic_completion_blocks.head;
  while (block != NULL) {
    __BOUNCE_COMPLETION_ITEM *items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);
    size_t index;

    for (index = 0u; index < block->item_count; index++) {
      bounce_win32_abort_pending_item_locked(&items[index], abort_queue);
    }
    block = block->next;
  }
}

static DWORD WINAPI bounce_win32_waiter_proc(LPVOID parameter) {
  __BOUNCE_WIN32_WAITER *waiter = parameter;
  BOUNCE_CORE *bounce = waiter->bounce;
  HANDLE wait_handles[
    WAITER_WAIT_INDEX_EXTERNAL_BEGIN + BOUNCE_MAX_WIN32_WAITS_PER_WAITER];
  __BOUNCE_COMPLETION_ITEM *snapshot_items[
    WAITER_WAIT_INDEX_EXTERNAL_BEGIN + BOUNCE_MAX_WIN32_WAITS_PER_WAITER];
  DWORD wait_count;
  DWORD slot;

  for (;;) {
    wait_handles[WAITER_WAIT_INDEX_SHUTDOWN] = bounce->shutdown_event;
    wait_handles[WAITER_WAIT_INDEX_REFRESH] = waiter->refresh_event;
    snapshot_items[WAITER_WAIT_INDEX_SHUTDOWN] = NULL;
    snapshot_items[WAITER_WAIT_INDEX_REFRESH] = NULL;
    wait_count = WAITER_WAIT_INDEX_EXTERNAL_BEGIN;

    EnterCriticalSection(&waiter->lock);
    for (slot = 0u; slot < BOUNCE_MAX_WIN32_WAITS_PER_WAITER; slot++) {
      __BOUNCE_COMPLETION_ITEM *item = waiter->slots[slot];

      if (item != NULL) {
        wait_handles[wait_count] = item->wait_handle;
        snapshot_items[wait_count] = item;
        wait_count += 1u;
      }
    }
    LeaveCriticalSection(&waiter->lock);

    {
      DWORD wait_result = WaitForMultipleObjects(
        wait_count,
        wait_handles,
        FALSE,
        INFINITE);

      if (wait_result == WAIT_OBJECT_0 + WAITER_WAIT_INDEX_SHUTDOWN) {
        return 0u;
      }
      if (wait_result == WAIT_OBJECT_0 + WAITER_WAIT_INDEX_REFRESH) {
        continue;
      }
      if ((wait_result >= WAIT_OBJECT_0 + WAITER_WAIT_INDEX_EXTERNAL_BEGIN) &&
          (wait_result < WAIT_OBJECT_0 + wait_count)) {
        __BOUNCE_COMPLETION_ITEM *item =
          snapshot_items[wait_result - WAIT_OBJECT_0];

        if (item == NULL) {
          continue;
        }

        EnterCriticalSection(&bounce->lock);
        if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
            (item->waiter == waiter)) {
          bounce_win32_remove_waiter_locked(item, false);
          bounce_win32_unlink_cancellation_locked(item);
          if (item->registration_owner != NULL) {
            if (item->registration_owner->item == item) {
              item->registration_owner->item = NULL;
            }
            item->registration_owner = NULL;
          }
          bounce_win32_queue_ready_locked(
            bounce,
            item,
            BOUNCE_COMPLETION_COMPLETED);
          LeaveCriticalSection(&bounce->lock);
          bounce_win32_release_wakeups(bounce, 1);
        } else {
          LeaveCriticalSection(&bounce->lock);
        }
        continue;
      }
      if (wait_result == WAIT_FAILED) {
        continue;
      }
    }
  }
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_init(BOUNCE_CORE *r) {
  DWORD waiter_index;

  memset(r, 0, sizeof *r);
  InitializeCriticalSection(&r->lock);
  r->shutdown_event = CreateEvent(NULL, TRUE, FALSE, NULL);
  r->wake_semaphore = CreateSemaphore(NULL, 0, LONG_MAX, NULL);
  r->shutting_down = 0;
  bounce_queue_init(&r->ready_queue);
  bounce_stack_init(&r->free_items);
  bounce_dynamic_block_list_init(&r->dynamic_completion_blocks);

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_win32_init_free_item(&r->static_completion_items[index]);
    bounce_stack_push(
      &r->free_items,
      &r->static_completion_items[index].ready_or_free_link);
  }

  for (waiter_index = 0u; waiter_index < BOUNCE_MAX_WIN32_WAITERS; waiter_index++) {
    __BOUNCE_WIN32_WAITER *waiter = &r->waiters[waiter_index];

    InitializeCriticalSection(&waiter->lock);
    waiter->bounce = r;
    waiter->refresh_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (waiter->refresh_event != NULL) {
      waiter->thread_handle = CreateThread(
        NULL,
        0u,
        bounce_win32_waiter_proc,
        waiter,
        0u,
        NULL);
    }
  }
}

/**
 * @brief Park current thread and run continuation repeatedly.
 * @param r Initialized BOUNCE_CORE.
 * @return True when succeeded continuation pumps.
 * @remarks The thread will block inside. Release when `shutdown()` called.
 */
bool bounce_park(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  HANDLE wait_handles[PARK_WAIT_INDEX_COUNT];
  DWORD wait_result;
  BOUNCE_WIN32_MESSAGE_PUMP_RESULT pump_result;

  if (r == NULL) {
    return false;
  }

  bounce_win32_ensure_message_queue();
  wait_handles[PARK_WAIT_INDEX_SHUTDOWN] = r->shutdown_event;
  wait_handles[PARK_WAIT_INDEX_WAKE] = r->wake_semaphore;

  for (;;) {
    bool should_return;

    while (bounce_win32_dispatch_next_ready(r, max_inline_depth)) {
    }

    EnterCriticalSection(&r->lock);
    should_return = bounce_win32_should_exit_locked(r);
    LeaveCriticalSection(&r->lock);
    if (should_return) {
      return true;
    }

    pump_result = bounce_win32_pump_messages();
    if (pump_result == BOUNCE_WIN32_MESSAGE_PUMP_QUIT) {
      return true;
    }
    if (pump_result == BOUNCE_WIN32_MESSAGE_PUMP_DISPATCHED) {
      continue;
    }

    wait_result = MsgWaitForMultipleObjects(
      PARK_WAIT_INDEX_COUNT,
      wait_handles,
      FALSE,
      INFINITE,
      QS_ALLINPUT);
    switch (wait_result) {
      case WAIT_OBJECT_0 + PARK_WAIT_INDEX_SHUTDOWN:
      case WAIT_OBJECT_0 + PARK_WAIT_INDEX_WAKE:
      case WAIT_OBJECT_0 + PARK_WAIT_INDEX_COUNT:
        break;
      default:
        return false;
    }
  }
}

/**
 * @brief Pump current thread once without waiting for new completion work.
 * @param r Initialized BOUNCE_CORE.
 * @param max_inline_depth Maximum number of inline nested completion executions.
 * @return True when succeeded continuation pumps.
 */
bool bounce_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  BOUNCE_WIN32_MESSAGE_PUMP_RESULT pump_result;

  if (r == NULL) {
    return false;
  }

  bounce_win32_ensure_message_queue();
  for (;;) {
    while (bounce_win32_dispatch_next_ready(r, max_inline_depth)) {
    }

    pump_result = bounce_win32_pump_messages();
    if (pump_result == BOUNCE_WIN32_MESSAGE_PUMP_QUIT) {
      return true;
    }
    if (pump_result == BOUNCE_WIN32_MESSAGE_PUMP_NONE) {
      break;
    }
  }
  return true;
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Post completion, will continue completion on the parked thread.
 * @param r Initialized BOUNCE_CORE.
 * @param completion Completion callback entry point
 * @param completion_state User provided completion callback state.
 * @return True when succeeded.
 */
bool bounce_post(BOUNCE_CORE *r, BOUNCE_COMPLETION completion, void *completion_state) {
  BOUNCE_WIN32_DISPATCH_CONTEXT *context;
  __BOUNCE_COMPLETION_ITEM *item;

  if ((r == NULL) ||
      (completion == NULL)) {
    return false;
  }

  context = bounce_win32_get_dispatch_context();
  if ((context != NULL) &&
      (context->bounce == r) &&
      (context->depth < context->max_inline_depth)) {
    context->depth += 1u;
    bounce_win32_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_COMPLETED);
    context->depth -= 1u;
    return true;
  }

  item = bounce_win32_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;

  EnterCriticalSection(&r->lock);
  if (r->shutting_down != 0) {
    LeaveCriticalSection(&r->lock);
    bounce_win32_recycle_completion_item(r, item);
    return false;
  }

  bounce_win32_queue_ready_locked(
    r,
    item,
    BOUNCE_COMPLETION_COMPLETED);
  LeaveCriticalSection(&r->lock);

  bounce_win32_release_wakeups(r, 1);
  return true;
}

/**
 * @brief Await raising kernel object, will continue completion on the parked thread.
 * @param r Initialized BOUNCE_CORE.
 * @param handle Win32 kernel object handle.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 */
void bounce_await_win32_handle(
  BOUNCE_CORE *r,
  HANDLE handle,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  __BOUNCE_COMPLETION_ITEM *item;

  item = bounce_win32_allocate_completion_item(r);
  if (item == NULL) {
    bounce_win32_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  if (bounce_win32_activate_wait_item(
        r,
        item,
        handle,
        cancellation) == __BOUNCE_WIN32_START_RESULT_FAILED) {
    bounce_win32_recycle_completion_item(r, item);
    bounce_win32_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
  }
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Shutdown parking threads.
 * @param r Initialized BOUNCE_CORE.
 */
void bounce_shutdown(BOUNCE_CORE *r, bool wait_for_idle) {
  bool signal_shutdown_event = false;

  if (r == NULL) {
    return;
  }

  EnterCriticalSection(&r->lock);
  r->shutting_down = 1;
  if (wait_for_idle) {
    r->shutdown_wait_for_idle = 1;
  } else {
    r->shutdown_wait_for_idle = 0;
    signal_shutdown_event = true;
  }
  LeaveCriticalSection(&r->lock);

  if (signal_shutdown_event &&
      (r->shutdown_event != NULL)) {
    (void)SetEvent(r->shutdown_event);
  }
  if (wait_for_idle) {
    bounce_win32_release_wakeups(r, BOUNCE_MAX_PARKERS);
  }
}

/**
 * @brief Deinitialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_deinit(BOUNCE_CORE *r) {
  BOUNCE_QUEUE abort_queue;
  __BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_NODE_ITEM *node;
  DWORD waiter_index;

  if (r == NULL) {
    return;
  }

  bounce_shutdown(r, false);
  bounce_queue_init(&abort_queue);

  for (waiter_index = 0u; waiter_index < BOUNCE_MAX_WIN32_WAITERS; waiter_index++) {
    if (r->waiters[waiter_index].thread_handle != NULL) {
      (void)WaitForSingleObject(r->waiters[waiter_index].thread_handle, INFINITE);
    }
  }

  EnterCriticalSection(&r->lock);
  bounce_win32_collect_abort_items_locked(r, &abort_queue);
  LeaveCriticalSection(&r->lock);

  while ((node = (BOUNCE_NODE_ITEM *)bounce_queue_dequeue(&abort_queue)) != NULL) {
    item = BOUNCE_WIN32_CONTAINER_OF(
      node,
      __BOUNCE_COMPLETION_ITEM,
      ready_or_free_link);
    bounce_win32_complete_direct(
      item->completion,
      item->completion_state,
      BOUNCE_COMPLETION_ABORTED);
  }

  for (waiter_index = 0u; waiter_index < BOUNCE_MAX_WIN32_WAITERS; waiter_index++) {
    if (r->waiters[waiter_index].thread_handle != NULL) {
      CloseHandle(r->waiters[waiter_index].thread_handle);
      r->waiters[waiter_index].thread_handle = NULL;
    }
    if (r->waiters[waiter_index].refresh_event != NULL) {
      CloseHandle(r->waiters[waiter_index].refresh_event);
      r->waiters[waiter_index].refresh_event = NULL;
    }
    DeleteCriticalSection(&r->waiters[waiter_index].lock);
  }

  if (r->wake_semaphore != NULL) {
    CloseHandle(r->wake_semaphore);
    r->wake_semaphore = NULL;
  }
  if (r->shutdown_event != NULL) {
    CloseHandle(r->shutdown_event);
    r->shutdown_event = NULL;
  }

  bounce_dynamic_block_list_free_all(&r->dynamic_completion_blocks);
  DeleteCriticalSection(&r->lock);
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a cancellation.
 * @param source Cancellation storage provided by the caller.
 */
void bounce_cancellation_init(BOUNCE_CANCELLATION *cancellation) {
  memset(cancellation, 0, sizeof *cancellation);
  InitializeCriticalSection(&cancellation->lock);
  bounce_list_init(&cancellation->listeners);
}

/**
 * @brief Initialize a cancellation registration.
 * @param registration Cancellation registration storage provided by the caller.
 */
void bounce_cancellation_registration_init(
  BOUNCE_CANCELLATION_REGISTRATION *registration) {
  registration->item = NULL;
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

  if ((r == NULL) ||
      (cancellation == NULL) ||
      (registration == NULL) ||
      (completion == NULL)) {
    return false;
  }

  item = bounce_win32_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->registration_owner = registration;

  if (bounce_win32_activate_wait_item(
        r,
        item,
        NULL,
        cancellation) == __BOUNCE_WIN32_START_RESULT_FAILED) {
    bounce_win32_recycle_completion_item(r, item);
    return false;
  }

  return true;
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

  EnterCriticalSection(&bounce->lock);
  item = registration->item;
  if ((item == NULL) ||
      (item->registration_owner != registration) ||
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING)) {
    LeaveCriticalSection(&bounce->lock);
    return false;
  }

  bounce_win32_detach_wait_locked(item);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  LeaveCriticalSection(&bounce->lock);

  bounce_win32_recycle_completion_item(bounce, item);
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

/**
 * @brief Cancel a cancellation.
 * @param r Initialized BOUNCE_CORE.
 * @param source Initialized cancellation.
 * @remarks The first successful call wins. Later calls are ignored.
 */
void bounce_cancel(
  BOUNCE_CORE *r,
  BOUNCE_CANCELLATION *cancellation) {
  LONG wake_count = 0;
  BOUNCE_LIST_ITEM *list_item;

  if ((r == NULL) ||
      (cancellation == NULL)) {
    return;
  }

  if (InterlockedCompareExchange(&cancellation->canceled, 1, 0) != 0) {
    return;
  }

  EnterCriticalSection(&r->lock);
  EnterCriticalSection(&cancellation->lock);
  while ((list_item = (BOUNCE_LIST_ITEM *)bounce_list_pop_head(
            &cancellation->listeners)) != NULL) {
    __BOUNCE_COMPLETION_ITEM *item =
      BOUNCE_WIN32_CONTAINER_OF(
        list_item,
        __BOUNCE_COMPLETION_ITEM,
        cancellation_link);

    item->cancellation = NULL;
    if (item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      bounce_win32_remove_waiter_locked(item, true);
      if (item->registration_owner != NULL) {
        if (item->registration_owner->item == item) {
          item->registration_owner->item = NULL;
        }
        item->registration_owner = NULL;
      }
      bounce_win32_queue_ready_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
      wake_count += 1;
    }
  }
  LeaveCriticalSection(&cancellation->lock);
  LeaveCriticalSection(&r->lock);

  bounce_win32_release_wakeups(r, wake_count);
}

/**
 * @brief Deinitialize a cancellation.
 * @param source Cancellation source storage provided by the caller.
 */
void bounce_cancellation_deinit(BOUNCE_CANCELLATION *cancellation) {
  if (cancellation != NULL) {
    DeleteCriticalSection(&cancellation->lock);
  }
}

//////////////////////////////////////////////////////////////////////////////////

#endif
