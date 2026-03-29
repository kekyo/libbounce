/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_FREERTOS_INTERNAL_H
#define _LIBBOUNCE_FREERTOS_INTERNAL_H

#if BOUNCE_FREERTOS_INTERNAL_ONLY

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "libbounce/bounce.h"

typedef __BOUNCE_COMPLETION_ITEM BOUNCE_COMPLETION_ITEM;

#if ( configNUM_THREAD_LOCAL_STORAGE_POINTERS < 1 )
#error "libbounce freertos backend requires at least one thread-local storage slot"
#endif

#if defined(ESP_PLATFORM)
extern portMUX_TYPE bounce_freertos_global_lock;
#endif

#define BOUNCE_FREERTOS_CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))
#define BOUNCE_FREERTOS_DISPATCH_TOKEN_FLAG ((uintptr_t)0x1u)
#define BOUNCE_FREERTOS_TASK_LOCAL_STORAGE_INDEX 0
#define BOUNCE_FREERTOS_WAKE_NOTIFICATION_INDEX 0

typedef struct BOUNCE_FREERTOS_CRITICAL_CONTEXT {
  bool from_isr;
  UBaseType_t interrupt_state;
} BOUNCE_FREERTOS_CRITICAL_CONTEXT;

typedef enum __BOUNCE_FREERTOS_START_RESULT {
  __BOUNCE_FREERTOS_START_RESULT_FAILED = 0,
  __BOUNCE_FREERTOS_START_RESULT_WAITING = 1,
  __BOUNCE_FREERTOS_START_RESULT_READY_QUEUED = 2,
  __BOUNCE_FREERTOS_START_RESULT_READY_INLINE = 3
} __BOUNCE_FREERTOS_START_RESULT;

typedef struct BOUNCE_FREERTOS_DISPATCH_CONTEXT {
  uintptr_t previous_token;
  BOUNCE_CORE *bounce;
  unsigned int depth;
  unsigned int max_inline_depth;
} BOUNCE_FREERTOS_DISPATCH_CONTEXT;

#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
extern void bounce_freertos_fd_backend_init(BOUNCE_CORE *r);
extern void bounce_freertos_fd_backend_shutdown(BOUNCE_CORE *r);
extern void bounce_freertos_fd_backend_deinit(BOUNCE_CORE *r);
extern bool bounce_freertos_fd_register_waiter_locked(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item);
extern void bounce_freertos_fd_remove_waiter_locked(
  BOUNCE_COMPLETION_ITEM *item,
  bool signal_refresh);
#endif

static inline void bounce_freertos_critical_enter(
  BOUNCE_FREERTOS_CRITICAL_CONTEXT *context,
  bool from_isr) {
  context->from_isr = from_isr;
  if (from_isr) {
    context->interrupt_state = taskENTER_CRITICAL_FROM_ISR();
  } else {
    context->interrupt_state = 0u;
#if defined(ESP_PLATFORM)
    taskENTER_CRITICAL(&bounce_freertos_global_lock);
#else
    taskENTER_CRITICAL();
#endif
  }
}

static inline void bounce_freertos_critical_exit(
  const BOUNCE_FREERTOS_CRITICAL_CONTEXT *context) {
  if (context->from_isr) {
    taskEXIT_CRITICAL_FROM_ISR(context->interrupt_state);
  } else {
#if defined(ESP_PLATFORM)
    taskEXIT_CRITICAL(&bounce_freertos_global_lock);
#else
    taskEXIT_CRITICAL();
#endif
  }
}

static inline TaskHandle_t bounce_freertos_current_task(void) {
  return xTaskGetCurrentTaskHandle();
}

static inline uintptr_t bounce_freertos_tls_token_get(TaskHandle_t task) {
  return (task != NULL) ?
           (uintptr_t)pvTaskGetThreadLocalStoragePointer(
             task,
             BOUNCE_FREERTOS_TASK_LOCAL_STORAGE_INDEX) :
           (uintptr_t)0u;
}

static inline void bounce_freertos_tls_token_set(TaskHandle_t task, uintptr_t token) {
  if (task != NULL) {
    vTaskSetThreadLocalStoragePointer(
      task,
      BOUNCE_FREERTOS_TASK_LOCAL_STORAGE_INDEX,
      (void *)token);
  }
}

static inline bool bounce_freertos_token_is_dispatch_context(uintptr_t token) {
  return (token & BOUNCE_FREERTOS_DISPATCH_TOKEN_FLAG) != 0u;
}

static inline uintptr_t bounce_freertos_encode_dispatch_context(
  BOUNCE_FREERTOS_DISPATCH_CONTEXT *context) {
  return ((uintptr_t)context) | BOUNCE_FREERTOS_DISPATCH_TOKEN_FLAG;
}

static inline BOUNCE_FREERTOS_DISPATCH_CONTEXT *
bounce_freertos_decode_dispatch_context(uintptr_t token) {
  return bounce_freertos_token_is_dispatch_context(token) ?
           (BOUNCE_FREERTOS_DISPATCH_CONTEXT *)
             (token & ~BOUNCE_FREERTOS_DISPATCH_TOKEN_FLAG) :
           NULL;
}

static inline BOUNCE_CORE *bounce_freertos_decode_core(uintptr_t token) {
  BOUNCE_FREERTOS_DISPATCH_CONTEXT *context =
    bounce_freertos_decode_dispatch_context(token);

  return (context != NULL) ?
           context->bounce :
           (BOUNCE_CORE *)token;
}

static inline BOUNCE_FREERTOS_DISPATCH_CONTEXT *
bounce_freertos_get_dispatch_context(void) {
  return bounce_freertos_decode_dispatch_context(
    bounce_freertos_tls_token_get(bounce_freertos_current_task()));
}

static inline void bounce_freertos_complete_direct(
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_COMPLETION_RESULT result) {
  if (completion != NULL) {
    completion(result, completion_state);
  }
}

static inline bool bounce_freertos_list_item_is_linked(
  const BOUNCE_LIST *list,
  const BOUNCE_LIST_ITEM *item) {
  return (list != NULL) &&
         (item != NULL) &&
         ((item->previous != NULL) ||
          (item->next != NULL) ||
          (list->head == item));
}

static inline void bounce_freertos_completion_item_reset(BOUNCE_COMPLETION_ITEM *item) {
  if (item == NULL) {
    return;
  }

  memset(item, 0, sizeof *item);
#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  item->fd = -1;
#endif
  item->state = BOUNCE_COMPLETION_ITEM_STATE_FREE;
}

static inline void bounce_freertos_prepare_claimed_item(
  BOUNCE_COMPLETION_ITEM *item) {
  bounce_freertos_completion_item_reset(item);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
}

static inline BOUNCE_COMPLETION_ITEM *bounce_freertos_pop_free_item_locked(
  BOUNCE_CORE *r,
  bool from_isr) {
  BOUNCE_COMPLETION_ITEM *item;

  if ((r == NULL) ||
      (!from_isr &&
       (r->free_item_count <= BOUNCE_FREERTOS_ISR_RESERVED_COMPLETION_ITEMS))) {
    return NULL;
  }

  item = (BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  if ((item != NULL) &&
      (r->free_item_count > 0u)) {
    r->free_item_count -= 1u;
  }
  return item;
}

static inline void bounce_freertos_push_free_item_locked(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item) {
  if ((r == NULL) || (item == NULL)) {
    return;
  }

  bounce_stack_push(&r->free_items, &item->ready_or_free_link);
  r->free_item_count += 1u;
}

static inline BOUNCE_COMPLETION_ITEM *bounce_freertos_free_item_acquire(
  BOUNCE_CORE *r,
  bool from_isr) {
  BOUNCE_COMPLETION_ITEM *item;
  BOUNCE_DYNAMIC_BLOCK *block = NULL;
  BOUNCE_COMPLETION_ITEM *dynamic_items = NULL;
  size_t index;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if (r == NULL) {
    return NULL;
  }

  bounce_freertos_critical_enter(&critical_context, from_isr);
  item = bounce_freertos_pop_free_item_locked(r, from_isr);
  if (item != NULL) {
    bounce_freertos_critical_exit(&critical_context);
    bounce_freertos_prepare_claimed_item(item);
    return item;
  }
  if (from_isr ||
      (BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP == 0u) ||
      (r->grow_in_progress != 0u)) {
    bounce_freertos_critical_exit(&critical_context);
    return NULL;
  }
  r->grow_in_progress = 1u;
  bounce_freertos_critical_exit(&critical_context);

  block = bounce_dynamic_block_allocate(
    sizeof(BOUNCE_COMPLETION_ITEM),
    BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP);
  if (block != NULL) {
    dynamic_items =
      (BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_items(block);
    for (index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
      bounce_freertos_completion_item_reset(&dynamic_items[index]);
    }
  }

  bounce_freertos_critical_enter(&critical_context, false);
  if (block != NULL) {
    bounce_dynamic_block_list_prepend(&r->dynamic_item_blocks, block);
    for (index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
      bounce_freertos_push_free_item_locked(r, &dynamic_items[index]);
    }
  }
  r->grow_in_progress = 0u;
  item = bounce_freertos_pop_free_item_locked(r, false);
  bounce_freertos_critical_exit(&critical_context);

  if (item != NULL) {
    bounce_freertos_prepare_claimed_item(item);
  }
  return item;
}

static inline void bounce_freertos_free_item_release(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item,
  bool from_isr) {
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if ((r == NULL) || (item == NULL)) {
    return;
  }

  bounce_freertos_completion_item_reset(item);
  bounce_freertos_critical_enter(&critical_context, from_isr);
  bounce_freertos_push_free_item_locked(r, item);
  bounce_freertos_critical_exit(&critical_context);
}

static inline void bounce_freertos_queue_ready_locked(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  item->completion_result = result;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_READY;
  bounce_queue_enqueue(&r->ready_queue, &item->ready_or_free_link);
}

static inline bool bounce_freertos_can_inline_locked(BOUNCE_CORE *r) {
  BOUNCE_FREERTOS_DISPATCH_CONTEXT *context =
    bounce_freertos_get_dispatch_context();

  return (context != NULL) &&
         (context->bounce == r) &&
         (context->depth < context->max_inline_depth) &&
         (r->shutting_down == 0) &&
         (r->ready_queue.head == NULL);
}

static inline __BOUNCE_FREERTOS_START_RESULT
bounce_freertos_finish_immediate_item_locked(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  if (bounce_freertos_can_inline_locked(r)) {
    item->completion_result = result;
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
    return __BOUNCE_FREERTOS_START_RESULT_READY_INLINE;
  }

  bounce_freertos_queue_ready_locked(r, item, result);
  return __BOUNCE_FREERTOS_START_RESULT_READY_QUEUED;
}

static inline bool bounce_freertos_item_is_cancellation_linked(
  const BOUNCE_COMPLETION_ITEM *item) {
  return (item->cancellation != NULL) &&
         bounce_freertos_list_item_is_linked(
           &item->cancellation->listeners,
           &item->cancellation_link);
}

static inline bool bounce_freertos_item_is_condition_linked(
  const BOUNCE_COMPLETION_ITEM *item) {
  return (item->condition != NULL) &&
         bounce_freertos_list_item_is_linked(
           &item->condition->waiters,
           &item->backend_link);
}

static inline void bounce_freertos_unlink_cancellation_locked(
  BOUNCE_COMPLETION_ITEM *item) {
  if ((item != NULL) &&
      bounce_freertos_item_is_cancellation_linked(item)) {
    bounce_list_remove(&item->cancellation->listeners, &item->cancellation_link);
  }
  if (item != NULL) {
    item->cancellation = NULL;
  }
}

static inline void bounce_freertos_unlink_condition_locked(
  BOUNCE_COMPLETION_ITEM *item) {
  if ((item != NULL) &&
      bounce_freertos_item_is_condition_linked(item)) {
    bounce_list_remove(&item->condition->waiters, &item->backend_link);
  }
  if (item != NULL) {
    item->condition = NULL;
  }
}

static inline void bounce_freertos_detach_wait_locked(
  BOUNCE_COMPLETION_ITEM *item,
  bool signal_fd_refresh) {
  if (item == NULL) {
    return;
  }

  bounce_freertos_unlink_cancellation_locked(item);
  bounce_freertos_unlink_condition_locked(item);
#if defined(ESP_PLATFORM) && defined(BOUNCE_FREERTOS_ENABLE_FD_AWAIT)
  bounce_freertos_fd_remove_waiter_locked(item, signal_fd_refresh);
#endif
  if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }
}

static inline void bounce_freertos_execute_claimed_item(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item,
  unsigned int max_inline_depth) {
  TaskHandle_t current_task = bounce_freertos_current_task();
  uintptr_t previous_token = bounce_freertos_tls_token_get(current_task);
  BOUNCE_FREERTOS_DISPATCH_CONTEXT *context =
    bounce_freertos_decode_dispatch_context(previous_token);
  BOUNCE_FREERTOS_DISPATCH_CONTEXT local_context;
  const bool installed_context =
    (context == NULL) || (context->bounce != r);
  const BOUNCE_COMPLETION completion = item->completion;
  void *completion_state = item->completion_state;
  const BOUNCE_COMPLETION_RESULT result = item->completion_result;

  if (installed_context) {
    local_context.previous_token = previous_token;
    local_context.bounce = r;
    local_context.depth = 0u;
    local_context.max_inline_depth = max_inline_depth;
    bounce_freertos_tls_token_set(
      current_task,
      bounce_freertos_encode_dispatch_context(&local_context));
    context = &local_context;
  }

  context->depth += 1u;
  bounce_freertos_complete_direct(completion, completion_state, result);
  context->depth -= 1u;

  if (installed_context) {
    bounce_freertos_tls_token_set(current_task, local_context.previous_token);
  }

  bounce_freertos_free_item_release(r, item, false);
}

static inline void bounce_freertos_signal_parkers_common(
  BOUNCE_CORE *r,
  bool from_isr) {
  TaskHandle_t parkers[BOUNCE_MAX_PARKERS];
  size_t parker_count = 0u;
  BOUNCE_FREERTOS_CRITICAL_CONTEXT critical_context;

  if (r == NULL) {
    return;
  }

  bounce_freertos_critical_enter(&critical_context, from_isr);
  for (size_t index = 0u; index < BOUNCE_MAX_PARKERS; index++) {
    if (r->parkers[index] != NULL) {
      parkers[parker_count] = r->parkers[index];
      parker_count += 1u;
    }
  }
  bounce_freertos_critical_exit(&critical_context);

  if (from_isr) {
    BaseType_t higher_priority_task_woken = pdFALSE;

    for (size_t index = 0u; index < parker_count; index++) {
      vTaskNotifyGiveIndexedFromISR(
        parkers[index],
        BOUNCE_FREERTOS_WAKE_NOTIFICATION_INDEX,
        &higher_priority_task_woken);
    }
    portYIELD_FROM_ISR(higher_priority_task_woken);
  } else {
    for (size_t index = 0u; index < parker_count; index++) {
      (void)xTaskNotifyGiveIndexed(
        parkers[index],
        BOUNCE_FREERTOS_WAKE_NOTIFICATION_INDEX);
    }
  }
}

static inline void bounce_freertos_signal_parkers(BOUNCE_CORE *r) {
  bounce_freertos_signal_parkers_common(r, false);
}

static inline void bounce_freertos_signal_parkers_from_isr(BOUNCE_CORE *r) {
  bounce_freertos_signal_parkers_common(r, true);
}

static inline __BOUNCE_FREERTOS_START_RESULT
bounce_freertos_activate_wait_item_locked(
  BOUNCE_CORE *r,
  BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_CANCELLATION *cancellation) {
  if (r->shutting_down != 0) {
    return __BOUNCE_FREERTOS_START_RESULT_FAILED;
  }

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item != NULL) {
      return __BOUNCE_FREERTOS_START_RESULT_FAILED;
    }
    item->registration_owner->item = item;
  }

  item->state = BOUNCE_COMPLETION_ITEM_STATE_WAITING;
  if (cancellation != NULL) {
    if (cancellation->canceled != 0) {
      if (item->registration_owner != NULL) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
      item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
      return bounce_freertos_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
    }

    item->cancellation = cancellation;
    bounce_list_insert_tail(&cancellation->listeners, &item->cancellation_link);
  }
  return __BOUNCE_FREERTOS_START_RESULT_WAITING;
}

#endif

#endif
