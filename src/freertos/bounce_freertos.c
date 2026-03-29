/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_FREERTOS) || defined(FREERTOS) || defined(ESP_PLATFORM)

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

#define BOUNCE_FREERTOS_INTERNAL_ONLY 1
#include "bounce_freertos_internal.h"

//////////////////////////////////////////////////////////////////////////////////
/*
 * FreeRTOS bounce architecture:
 *
 * - Producer paths and parker execution are intentionally separated.
 *   `bounce_post()` and backend-local await producers turn work into ready
 *   completion items, but they do not execute callbacks directly.
 * - Ready items are pushed into the shared ready queue, and only a bounded
 *   number of parker task wakeups are emitted while a burst is building.
 * - `bounce_park()` owns only the execution side: it waits for bounce wake or
 *   shutdown, drains the ready queue, and executes completions on the parked
 *   task context.
 * - The baseline backend keeps waiting simple. Backend-local condition producers
 *   schedule ready items directly, so the parker does not own a rich wait
 *   subsystem.
 * - Optional ESP-IDF fd waiting stays backend-local as well. A dedicated waiter
 *   task may translate `select()` readiness into ready items, but the `post()`
 *   fast path and parker contract remain unchanged:
 *     post()/backend ready -> ready queue -> wake one parker -> drain queue
 *     -> execute completion.
 */

//////////////////////////////////////////////////////////////////////////////////

#if defined(ESP_PLATFORM)
portMUX_TYPE bounce_freertos_global_lock = portMUX_INITIALIZER_UNLOCKED;
#endif

static inline BOUNCE_COMPLETION_ITEM *bounce_freertos_dequeue_ready_locked(
  BOUNCE_CORE *r) {
  BOUNCE_COMPLETION_ITEM *item =
    (BOUNCE_COMPLETION_ITEM *)bounce_queue_dequeue(&r->ready_queue);

  if (item != NULL) {
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  }
  return item;
}

static inline bool bounce_freertos_dispatch_next_ready(
  BOUNCE_CORE *r,
  unsigned int max_inline_depth) {
  BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  bounce_freertos_critical_enter(&critical_context, false);
  item = bounce_freertos_dequeue_ready_locked(r);
  bounce_freertos_critical_exit(&critical_context);

  if (item == NULL) {
    return false;
  }

  bounce_freertos_execute_claimed_item(r, item, max_inline_depth);
  return true;
}

static inline void bounce_freertos_abort_pending_item_locked(
  BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_QUEUE *abort_queue) {
  if ((item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_READY)) {
    return;
  }

  bounce_freertos_detach_wait_locked(item, true);
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_queue_enqueue(abort_queue, &item->ready_or_free_link);
}

static inline void bounce_freertos_collect_abort_items_locked(
  BOUNCE_CORE *r,
  BOUNCE_QUEUE *abort_queue) {
  BOUNCE_DYNAMIC_BLOCK *block;

  bounce_queue_init(&r->ready_queue);
  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_freertos_abort_pending_item_locked(
      &r->static_completion_items[index],
      abort_queue);
  }

  block = r->dynamic_item_blocks.head;
  while (block != NULL) {
    BOUNCE_COMPLETION_ITEM *items =
      (BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);

    for (size_t index = 0u; index < block->item_count; index++) {
      bounce_freertos_abort_pending_item_locked(&items[index], abort_queue);
    }
    block = block->next;
  }
}

static bool bounce_freertos_register_parker(
  BOUNCE_CORE *r,
  TaskHandle_t task) {
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;
  bool registered = false;

  if ((r == NULL) || (task == NULL)) {
    return false;
  }

  bounce_freertos_critical_enter(&critical_context, false);
  for (size_t index = 0u; index < BOUNCE_MAX_PARKERS; index++) {
    if (r->parkers[index] == task) {
      registered = true;
      break;
    }
    if ((r->parkers[index] == NULL) && !registered) {
      r->parkers[index] = task;
      registered = true;
      break;
    }
  }
  bounce_freertos_critical_exit(&critical_context);
  return registered;
}

static void bounce_freertos_unregister_parker(
  BOUNCE_CORE *r,
  TaskHandle_t task) {
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) || (task == NULL)) {
    return;
  }

  bounce_freertos_critical_enter(&critical_context, false);
  for (size_t index = 0u; index < BOUNCE_MAX_PARKERS; index++) {
    if (r->parkers[index] == task) {
      r->parkers[index] = NULL;
      break;
    }
  }
  bounce_freertos_critical_exit(&critical_context);
}

/**
 * @brief Initialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_init(BOUNCE_CORE *r) {
  if (r == NULL) {
    return;
  }

  memset(r, 0, sizeof *r);
  bounce_queue_init(&r->ready_queue);
  bounce_stack_init(&r->free_items);
  bounce_dynamic_block_list_init(&r->dynamic_item_blocks);

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_freertos_completion_item_reset(&r->static_completion_items[index]);
    bounce_stack_push(
      &r->free_items,
      &r->static_completion_items[index].ready_or_free_link);
    r->free_item_count += 1u;
  }
#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  bounce_freertos_fd_backend_init(r);
#endif
}

/**
 * @brief Initialize a backend-local FreeRTOS condition object.
 * @param condition Condition storage provided by the caller.
 */
void bounce_freertos_condition_init(BOUNCE_FREERTOS_CONDITION *condition) {
  if (condition != NULL) {
    memset(condition, 0, sizeof *condition);
    bounce_list_init(&condition->waiters);
  }
}

/**
 * @brief Park current thread and run continuation repeatedly.
 * @param r Initialized BOUNCE_CORE.
 * @return True when succeeded continuation pumps.
 * @remarks The thread will block inside. Release when `shutdown()` called.
 */
bool bounce_park(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  TaskHandle_t current_task = bounce_freertos_current_task();

  if ((r == NULL) || (current_task == NULL)) {
    return false;
  }
  if (!bounce_freertos_register_parker(r, current_task)) {
    return false;
  }

  while (ulTaskNotifyTakeIndexed(
           BOUNCE_FREERTOS_WAKE_NOTIFICATION_INDEX,
           pdTRUE,
           0u) != 0u) {
  }

  for (;;) {
    BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;
    bool should_return;

    while (bounce_freertos_dispatch_next_ready(r, max_inline_depth)) {
    }

    bounce_freertos_critical_enter(&critical_context, false);
    should_return =
      (r->shutting_down != 0) &&
      (r->ready_queue.head == NULL);
    bounce_freertos_critical_exit(&critical_context);
    if (should_return) {
      break;
    }

    (void)ulTaskNotifyTakeIndexed(
      BOUNCE_FREERTOS_WAKE_NOTIFICATION_INDEX,
      pdTRUE,
      portMAX_DELAY);
  }

  bounce_freertos_unregister_parker(r, current_task);
  return true;
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

  while (bounce_freertos_dispatch_next_ready(r, max_inline_depth)) {
  }
  return true;
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Post completion, will continue completion on the parked thread.
 * @param r Initialized BOUNCE_CORE.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @return True when succeeded.
 */
bool bounce_post(BOUNCE_CORE *r, BOUNCE_COMPLETION completion, void *completion_state) {
  BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) || (completion == NULL)) {
    return false;
  }

  bounce_freertos_critical_enter(&critical_context, false);
  if (bounce_freertos_can_inline_locked(r)) {
    BOUNCE_FREERTOS_DISPATCH_CONTEXT *context =
      bounce_freertos_get_dispatch_context();

    bounce_freertos_critical_exit(&critical_context);
    context->depth += 1u;
    completion(BOUNCE_COMPLETION_COMPLETED, completion_state);
    context->depth -= 1u;
    return true;
  }
  bounce_freertos_critical_exit(&critical_context);

  item = bounce_freertos_free_item_acquire(r, false);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;

  bounce_freertos_critical_enter(&critical_context, false);
  if (r->shutting_down != 0) {
    bounce_freertos_critical_exit(&critical_context);
    bounce_freertos_free_item_release(r, item, false);
    return false;
  }
  bounce_freertos_queue_ready_locked(
    r,
    item,
    BOUNCE_COMPLETION_COMPLETED);
  bounce_freertos_critical_exit(&critical_context);

  bounce_freertos_signal_parkers(r);
  return true;
}

/**
 * @brief Await a backend-local FreeRTOS condition.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 */
void bounce_await_freertos_condition(
  BOUNCE_CORE *r,
  BOUNCE_FREERTOS_CONDITION *condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_FREERTOS_START_RESULT start_result;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) ||
      (condition == NULL) ||
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
  item->condition = condition;

  bounce_freertos_critical_enter(&critical_context, false);
  start_result = bounce_freertos_activate_wait_item_locked(
    r,
    item,
    cancellation);
  if (start_result == __BOUNCE_FREERTOS_START_RESULT_WAITING) {
    bounce_list_insert_tail(&condition->waiters, &item->backend_link);
  }
  bounce_freertos_critical_exit(&critical_context);

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

/**
 * @brief Raise a backend-local FreeRTOS condition from task context.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 */
void bounce_freertos_condition_raise(
  BOUNCE_CORE *r,
  BOUNCE_FREERTOS_CONDITION *condition) {
  bool wake_parkers = false;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) || (condition == NULL)) {
    return;
  }

  bounce_freertos_critical_enter(&critical_context, false);
  for (;;) {
    BOUNCE_LIST_ITEM *list_item =
      (BOUNCE_LIST_ITEM *)bounce_list_pop_head(&condition->waiters);
    BOUNCE_COMPLETION_ITEM *item;

    if (list_item == NULL) {
      break;
    }

    item = BOUNCE_FREERTOS_CONTAINER_OF(
      list_item,
      BOUNCE_COMPLETION_ITEM,
      backend_link);
    item->condition = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

    bounce_freertos_unlink_cancellation_locked(item);
    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_freertos_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_COMPLETED);
    wake_parkers = true;
  }
  bounce_freertos_critical_exit(&critical_context);

  if (wake_parkers) {
    bounce_freertos_signal_parkers(r);
  }
}

/**
 * @brief Raise a backend-local FreeRTOS condition from ISR context.
 * @param r Initialized BOUNCE_CORE.
 * @param condition Initialized condition object.
 */
void bounce_freertos_condition_raise_from_isr(
  BOUNCE_CORE *r,
  BOUNCE_FREERTOS_CONDITION *condition) {
  bool wake_parkers = false;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) || (condition == NULL)) {
    return;
  }

  bounce_freertos_critical_enter(&critical_context, true);
  for (;;) {
    BOUNCE_LIST_ITEM *list_item =
      (BOUNCE_LIST_ITEM *)bounce_list_pop_head(&condition->waiters);
    BOUNCE_COMPLETION_ITEM *item;

    if (list_item == NULL) {
      break;
    }

    item = BOUNCE_FREERTOS_CONTAINER_OF(
      list_item,
      BOUNCE_COMPLETION_ITEM,
      backend_link);
    item->condition = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

    bounce_freertos_unlink_cancellation_locked(item);
    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_freertos_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_COMPLETED);
    wake_parkers = true;
  }
  bounce_freertos_critical_exit(&critical_context);

  if (wake_parkers) {
    bounce_freertos_signal_parkers_from_isr(r);
  }
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Shutdown parking threads.
 * @param r Initialized BOUNCE_CORE.
 */
void bounce_shutdown(BOUNCE_CORE *r) {
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if (r == NULL) {
    return;
  }

  bounce_freertos_critical_enter(&critical_context, false);
  r->shutting_down = 1;
  bounce_freertos_critical_exit(&critical_context);

  bounce_freertos_signal_parkers(r);
#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  bounce_freertos_fd_backend_shutdown(r);
#endif
}

/**
 * @brief Deinitialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_deinit(BOUNCE_CORE *r) {
  BOUNCE_QUEUE abort_queue;
  BOUNCE_NODE_ITEM *node;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if (r == NULL) {
    return;
  }

  bounce_shutdown(r);
  bounce_queue_init(&abort_queue);

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  bounce_freertos_fd_backend_deinit(r);
#endif

  bounce_freertos_critical_enter(&critical_context, false);
  bounce_freertos_collect_abort_items_locked(r, &abort_queue);
  bounce_freertos_critical_exit(&critical_context);

  while ((node = (BOUNCE_NODE_ITEM *)bounce_queue_dequeue(&abort_queue)) != NULL) {
    BOUNCE_COMPLETION_ITEM *item =
      BOUNCE_FREERTOS_CONTAINER_OF(
        node,
        BOUNCE_COMPLETION_ITEM,
        ready_or_free_link);

    bounce_freertos_complete_direct(
      item->completion,
      item->completion_state,
      BOUNCE_COMPLETION_ABORTED);
  }

  bounce_dynamic_block_list_free_all(&r->dynamic_item_blocks);
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Initialize a cancellation.
 * @param source Cancellation storage provided by the caller.
 */
void bounce_cancellation_init(BOUNCE_CANCELLATION *cancellation) {
  if (cancellation != NULL) {
    memset(cancellation, 0, sizeof *cancellation);
    bounce_list_init(&cancellation->listeners);
  }
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
  BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_FREERTOS_START_RESULT start_result;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) ||
      (cancellation == NULL) ||
      (registration == NULL) ||
      (completion == NULL)) {
    return false;
  }

  item = bounce_freertos_free_item_acquire(r, false);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->registration_owner = registration;

  bounce_freertos_critical_enter(&critical_context, false);
  start_result = bounce_freertos_activate_wait_item_locked(
    r,
    item,
    cancellation);
  bounce_freertos_critical_exit(&critical_context);

  switch (start_result) {
    case __BOUNCE_FREERTOS_START_RESULT_WAITING:
      return true;
    case __BOUNCE_FREERTOS_START_RESULT_READY_QUEUED:
      bounce_freertos_signal_parkers(r);
      return true;
    case __BOUNCE_FREERTOS_START_RESULT_READY_INLINE:
      bounce_freertos_execute_claimed_item(r, item, 0u);
      return true;
    case __BOUNCE_FREERTOS_START_RESULT_FAILED:
    default:
      bounce_freertos_free_item_release(r, item, false);
      return false;
  }
}

/**
 * @brief Unregister a previously registered cancellation completion.
 * @param registration Initialized cancellation registration.
 * @return True when unregister wins.
 */
bool bounce_unregister_canceled(BOUNCE_CANCELLATION_REGISTRATION *registration) {
  BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_CORE *bounce;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if (registration == NULL) {
    return false;
  }

  item = registration->item;
  if ((item == NULL) ||
      (item->bounce == NULL)) {
    return false;
  }

  bounce = item->bounce;
  bounce_freertos_critical_enter(&critical_context, false);
  item = registration->item;
  if ((item == NULL) ||
      (item->registration_owner != registration) ||
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING)) {
    bounce_freertos_critical_exit(&critical_context);
    return false;
  }

  bounce_freertos_detach_wait_locked(item, true);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_freertos_critical_exit(&critical_context);

  bounce_freertos_free_item_release(bounce, item, false);
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
  bool wake_parkers = false;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) || (cancellation == NULL)) {
    return;
  }

  bounce_freertos_critical_enter(&critical_context, false);
  if (cancellation->canceled != 0) {
    bounce_freertos_critical_exit(&critical_context);
    return;
  }

  cancellation->canceled = 1;
  for (;;) {
    BOUNCE_LIST_ITEM *list_item =
      (BOUNCE_LIST_ITEM *)bounce_list_pop_head(&cancellation->listeners);
    BOUNCE_COMPLETION_ITEM *item;

    if (list_item == NULL) {
      break;
    }

    item = BOUNCE_FREERTOS_CONTAINER_OF(
      list_item,
      BOUNCE_COMPLETION_ITEM,
      cancellation_link);
    item->cancellation = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

    bounce_freertos_detach_wait_locked(item, true);
    bounce_freertos_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_CANCELED);
    wake_parkers = true;
  }
  bounce_freertos_critical_exit(&critical_context);

  if (wake_parkers) {
    bounce_freertos_signal_parkers(r);
  }
}

/**
 * @brief Deinitialize a cancellation.
 * @param source Cancellation storage provided by the caller.
 */
void bounce_cancellation_deinit(BOUNCE_CANCELLATION *cancellation) {
  (void)cancellation;
}

#endif
