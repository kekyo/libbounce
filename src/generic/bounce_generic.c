/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_GENERIC)

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

//////////////////////////////////////////////////////////////////////////////////
/*
 * Generic bounce architecture:
 *
 * - This backend is intentionally single-parker.
 * - Producer paths publish ready completion items into the shared ready queue,
 *   but there is no platform wake primitive. The parked thread observes ready
 *   work through busy spinning.
 * - Timer waiting is backend-local as well. The parked thread polls armed
 *   timer deadlines and turns elapsed timeouts into ready items.
 * - Completion execution stays centralized on the parked thread:
 *     post()/timer elapsed -> ready queue -> parked thread drain -> execute
 *     completion.
 */

//////////////////////////////////////////////////////////////////////////////////

#define BOUNCE_GENERIC_CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct BOUNCE_GENERIC_DISPATCH_CONTEXT {
  struct BOUNCE_GENERIC_DISPATCH_CONTEXT *previous;
  BOUNCE_CORE *bounce;
  unsigned int depth;
  unsigned int max_inline_depth;
} BOUNCE_GENERIC_DISPATCH_CONTEXT;

typedef enum __BOUNCE_GENERIC_START_RESULT {
  __BOUNCE_GENERIC_START_RESULT_FAILED = 0,
  __BOUNCE_GENERIC_START_RESULT_WAITING = 1,
  __BOUNCE_GENERIC_START_RESULT_READY_QUEUED = 2,
  __BOUNCE_GENERIC_START_RESULT_READY_INLINE = 3
} __BOUNCE_GENERIC_START_RESULT;

static _Thread_local BOUNCE_GENERIC_DISPATCH_CONTEXT
  *bounce_generic_dispatch_context = NULL;

static inline void bounce_generic_spin_lock(__BOUNCE_GENERIC_SPINLOCK *lock) {
  for (;;) {
    unsigned int expected = 0u;

    if (__atomic_compare_exchange_n(
          &lock->state,
          &expected,
          1u,
          false,
          __ATOMIC_ACQUIRE,
          __ATOMIC_RELAXED)) {
      return;
    }

    while (__atomic_load_n(&lock->state, __ATOMIC_RELAXED) != 0u) {
    }
  }
}

static inline void bounce_generic_spin_unlock(__BOUNCE_GENERIC_SPINLOCK *lock) {
  __atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}

static inline bool bounce_generic_register_parker(BOUNCE_CORE *r) {
  int expected = 0;

  return __atomic_compare_exchange_n(
    &r->parker_active,
    &expected,
    1,
    false,
    __ATOMIC_ACQ_REL,
    __ATOMIC_ACQUIRE);
}

static inline void bounce_generic_unregister_parker(BOUNCE_CORE *r) {
  __atomic_store_n(&r->parker_active, 0, __ATOMIC_RELEASE);
}

static inline BOUNCE_GENERIC_DISPATCH_CONTEXT *bounce_generic_get_dispatch_context(void) {
  return bounce_generic_dispatch_context;
}

static inline void bounce_generic_set_dispatch_context(
  BOUNCE_GENERIC_DISPATCH_CONTEXT *context) {
  bounce_generic_dispatch_context = context;
}

static inline void bounce_generic_complete_direct(
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_COMPLETION_RESULT result) {
  if (completion != NULL) {
    completion(result, completion_state);
  }
}

static inline bool bounce_generic_list_item_is_linked(
  const BOUNCE_LIST *list,
  const BOUNCE_LIST_ITEM *item) {
  return (list != NULL) &&
         (item != NULL) &&
         ((item->previous != NULL) ||
          (item->next != NULL) ||
          (list->head == item));
}

static inline bool bounce_generic_item_is_cancellation_linked(
  const __BOUNCE_COMPLETION_ITEM *item) {
  return (item->cancellation != NULL) &&
         bounce_generic_list_item_is_linked(
           &item->cancellation->listeners,
           &item->cancellation_link);
}

static inline void bounce_generic_init_free_item(__BOUNCE_COMPLETION_ITEM *item) {
  memset(item, 0, sizeof *item);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_FREE;
}

static inline void bounce_generic_prepare_reserved_item(__BOUNCE_COMPLETION_ITEM *item) {
  item->ready_or_free_link.next = NULL;
  item->cancellation_link.previous = NULL;
  item->cancellation_link.next = NULL;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  item->completion = NULL;
  item->completion_state = NULL;
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->bounce = NULL;
  item->cancellation = NULL;
  item->registration_owner = NULL;
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_generic_allocate_completion_item(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item = NULL;
  BOUNCE_DYNAMIC_BLOCK *block;
  __BOUNCE_COMPLETION_ITEM *dynamic_items;

  if (r == NULL) {
    return NULL;
  }

  bounce_generic_spin_lock(&r->lock);
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  bounce_generic_spin_unlock(&r->lock);
  if (item != NULL) {
    bounce_generic_prepare_reserved_item(item);
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
  for (size_t index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
    bounce_generic_init_free_item(&dynamic_items[index]);
  }

  bounce_generic_spin_lock(&r->lock);
  bounce_dynamic_block_list_prepend(&r->dynamic_completion_blocks, block);
  for (size_t index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
    bounce_stack_push(&r->free_items, &dynamic_items[index].ready_or_free_link);
  }
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  bounce_generic_spin_unlock(&r->lock);

  if (item != NULL) {
    bounce_generic_prepare_reserved_item(item);
  }
  return item;
}

static inline void bounce_generic_recycle_completion_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  if ((r == NULL) || (item == NULL)) {
    return;
  }

  bounce_generic_init_free_item(item);
  bounce_generic_spin_lock(&r->lock);
  bounce_stack_push(&r->free_items, &item->ready_or_free_link);
  bounce_generic_spin_unlock(&r->lock);
}

static inline void bounce_generic_queue_ready_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  item->completion_result = result;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_READY;
  bounce_queue_enqueue(&r->ready_queue, &item->ready_or_free_link);
}

static inline bool bounce_generic_can_inline_locked(BOUNCE_CORE *r) {
  BOUNCE_GENERIC_DISPATCH_CONTEXT *context =
    bounce_generic_get_dispatch_context();

  return (context != NULL) &&
         (context->bounce == r) &&
         (context->depth < context->max_inline_depth) &&
         (r->shutting_down == 0) &&
         (r->ready_queue.head == NULL);
}

static inline __BOUNCE_GENERIC_START_RESULT
bounce_generic_finish_immediate_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  if (bounce_generic_can_inline_locked(r)) {
    item->completion_result = result;
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
    return __BOUNCE_GENERIC_START_RESULT_READY_INLINE;
  }

  bounce_generic_queue_ready_locked(r, item, result);
  return __BOUNCE_GENERIC_START_RESULT_READY_QUEUED;
}

static inline void bounce_generic_unlink_cancellation_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  BOUNCE_CANCELLATION *cancellation = item->cancellation;

  if (cancellation == NULL) {
    return;
  }

  bounce_generic_spin_lock(&cancellation->lock);
  if (bounce_generic_item_is_cancellation_linked(item)) {
    bounce_list_remove(&cancellation->listeners, &item->cancellation_link);
  }
  bounce_generic_spin_unlock(&cancellation->lock);

  item->cancellation = NULL;
}

static inline void bounce_generic_detach_wait_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  bounce_generic_unlink_cancellation_locked(item);
  if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_generic_dequeue_ready_locked(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item =
    (__BOUNCE_COMPLETION_ITEM *)bounce_queue_dequeue(&r->ready_queue);

  if (item != NULL) {
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  }
  return item;
}

static inline void bounce_generic_execute_claimed_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  unsigned int max_inline_depth) {
  BOUNCE_GENERIC_DISPATCH_CONTEXT *context =
    bounce_generic_get_dispatch_context();
  BOUNCE_GENERIC_DISPATCH_CONTEXT local_context;
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
    bounce_generic_set_dispatch_context(&local_context);
    context = &local_context;
  }

  context->depth += 1u;
  bounce_generic_complete_direct(completion, completion_state, result);
  context->depth -= 1u;

  if (installed_context) {
    bounce_generic_set_dispatch_context(local_context.previous);
  }

  bounce_generic_recycle_completion_item(r, item);
}

static inline bool bounce_generic_dispatch_next_ready(
  BOUNCE_CORE *r,
  unsigned int max_inline_depth) {
  __BOUNCE_COMPLETION_ITEM *item;

  bounce_generic_spin_lock(&r->lock);
  item = bounce_generic_dequeue_ready_locked(r);
  bounce_generic_spin_unlock(&r->lock);

  if (item == NULL) {
    return false;
  }

  bounce_generic_execute_claimed_item(r, item, max_inline_depth);
  return true;
}

static inline void bounce_generic_abort_pending_item_locked(
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_QUEUE *abort_queue) {
  if ((item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_READY)) {
    return;
  }

  if (item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
    bounce_generic_detach_wait_locked(item);
  } else if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }

  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_queue_enqueue(abort_queue, &item->ready_or_free_link);
}

static inline void bounce_generic_collect_abort_items_locked(
  BOUNCE_CORE *r,
  BOUNCE_QUEUE *abort_queue) {
  BOUNCE_DYNAMIC_BLOCK *block;

  bounce_queue_init(&r->ready_queue);
  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_generic_abort_pending_item_locked(
      &r->static_completion_items[index],
      abort_queue);
  }

  block = r->dynamic_completion_blocks.head;
  while (block != NULL) {
    __BOUNCE_COMPLETION_ITEM *items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);

    for (size_t index = 0u; index < block->item_count; index++) {
      bounce_generic_abort_pending_item_locked(&items[index], abort_queue);
    }
    block = block->next;
  }
}

static inline __BOUNCE_GENERIC_START_RESULT
bounce_generic_activate_wait_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_CANCELLATION *cancellation) {
  if (r->shutting_down != 0) {
    return __BOUNCE_GENERIC_START_RESULT_FAILED;
  }

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item != NULL) {
      return __BOUNCE_GENERIC_START_RESULT_FAILED;
    }
    item->registration_owner->item = item;
  }

  item->state = BOUNCE_COMPLETION_ITEM_STATE_WAITING;
  if (cancellation != NULL) {
    bounce_generic_spin_lock(&cancellation->lock);
    if (cancellation->canceled != 0) {
      bounce_generic_spin_unlock(&cancellation->lock);
      if (item->registration_owner != NULL) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
      item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
      return bounce_generic_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
    }

    item->cancellation = cancellation;
    bounce_list_insert_tail(&cancellation->listeners, &item->cancellation_link);
    bounce_generic_spin_unlock(&cancellation->lock);
  }
  return __BOUNCE_GENERIC_START_RESULT_WAITING;
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
  bounce_dynamic_block_list_init(&r->dynamic_completion_blocks);

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_generic_init_free_item(&r->static_completion_items[index]);
    bounce_stack_push(
      &r->free_items,
      &r->static_completion_items[index].ready_or_free_link);
  }
}

/**
 * @brief Park current thread and run continuation repeatedly.
 * @param r Initialized BOUNCE_CORE.
 * @return True when succeeded continuation pumps.
 * @remarks The thread busy-spins while idle. Only one parker is valid.
 */
bool bounce_park(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  if ((r == NULL) ||
      !bounce_generic_register_parker(r)) {
    return false;
  }

  for (;;) {
    bool should_return;

    while (bounce_generic_dispatch_next_ready(r, max_inline_depth)) {
    }

    bounce_generic_spin_lock(&r->lock);
    should_return =
      (r->shutting_down != 0) &&
      (r->ready_queue.head == NULL);
    bounce_generic_spin_unlock(&r->lock);
    if (should_return) {
      break;
    }
  }

  bounce_generic_unregister_parker(r);
  return true;
}

/**
 * @brief Pump current thread once without waiting for new completion work.
 * @param r Initialized BOUNCE_CORE.
 * @param max_inline_depth Maximum number of inline nested completion executions.
 * @return True when succeeded continuation pumps.
 */
bool bounce_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  if ((r == NULL) ||
      !bounce_generic_register_parker(r)) {
    return false;
  }

  while (bounce_generic_dispatch_next_ready(r, max_inline_depth)) {
  }

  bounce_generic_unregister_parker(r);
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
  BOUNCE_GENERIC_DISPATCH_CONTEXT *context;

  if ((r == NULL) || (completion == NULL)) {
    return false;
  }

  bounce_generic_spin_lock(&r->lock);
  if (bounce_generic_can_inline_locked(r)) {
    context = bounce_generic_get_dispatch_context();
    bounce_generic_spin_unlock(&r->lock);

    context->depth += 1u;
    completion(BOUNCE_COMPLETION_COMPLETED, completion_state);
    context->depth -= 1u;
    return true;
  }
  if (r->shutting_down != 0) {
    bounce_generic_spin_unlock(&r->lock);
    return false;
  }
  bounce_generic_spin_unlock(&r->lock);

  item = bounce_generic_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;

  bounce_generic_spin_lock(&r->lock);
  if (r->shutting_down != 0) {
    bounce_generic_spin_unlock(&r->lock);
    bounce_generic_recycle_completion_item(r, item);
    return false;
  }
  bounce_generic_queue_ready_locked(
    r,
    item,
    BOUNCE_COMPLETION_COMPLETED);
  bounce_generic_spin_unlock(&r->lock);
  return true;
}

/**
 * @brief Shutdown parking threads.
 * @param r Initialized BOUNCE_CORE.
 */
void bounce_shutdown(BOUNCE_CORE *r) {
  if (r == NULL) {
    return;
  }

  __atomic_store_n(&r->shutting_down, 1, __ATOMIC_RELEASE);
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

  bounce_shutdown(r);
  bounce_queue_init(&abort_queue);

  while (__atomic_load_n(&r->parker_active, __ATOMIC_ACQUIRE) != 0) {
  }

  bounce_generic_spin_lock(&r->lock);
  bounce_generic_collect_abort_items_locked(r, &abort_queue);
  bounce_generic_spin_unlock(&r->lock);

  while ((node = (BOUNCE_NODE_ITEM *)bounce_queue_dequeue(&abort_queue)) != NULL) {
    __BOUNCE_COMPLETION_ITEM *item =
      BOUNCE_GENERIC_CONTAINER_OF(
        node,
        __BOUNCE_COMPLETION_ITEM,
        ready_or_free_link);

    bounce_generic_complete_direct(
      item->completion,
      item->completion_state,
      BOUNCE_COMPLETION_ABORTED);
  }

  bounce_dynamic_block_list_free_all(&r->dynamic_completion_blocks);
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
  __BOUNCE_GENERIC_START_RESULT start_result;

  if ((r == NULL) ||
      (cancellation == NULL) ||
      (registration == NULL) ||
      (completion == NULL)) {
    return false;
  }

  item = bounce_generic_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->registration_owner = registration;

  bounce_generic_spin_lock(&r->lock);
  start_result = bounce_generic_activate_wait_item_locked(
    r,
    item,
    cancellation);
  bounce_generic_spin_unlock(&r->lock);

  switch (start_result) {
    case __BOUNCE_GENERIC_START_RESULT_WAITING:
      return true;
    case __BOUNCE_GENERIC_START_RESULT_READY_QUEUED:
      return true;
    case __BOUNCE_GENERIC_START_RESULT_READY_INLINE:
      bounce_generic_execute_claimed_item(r, item, 0u);
      return true;
    case __BOUNCE_GENERIC_START_RESULT_FAILED:
    default:
      bounce_generic_recycle_completion_item(r, item);
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
  if ((item == NULL) ||
      (item->bounce == NULL)) {
    return false;
  }

  bounce = item->bounce;
  bounce_generic_spin_lock(&bounce->lock);
  item = registration->item;
  if ((item == NULL) ||
      (item->registration_owner != registration) ||
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING)) {
    bounce_generic_spin_unlock(&bounce->lock);
    return false;
  }

  bounce_generic_detach_wait_locked(item);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_generic_spin_unlock(&bounce->lock);

  bounce_generic_recycle_completion_item(bounce, item);
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
 * @param cancellation Initialized cancellation.
 * @remarks The first successful call wins. Later calls are ignored.
 */
void bounce_cancel(
  BOUNCE_CORE *r,
  BOUNCE_CANCELLATION *cancellation) {
  if ((r == NULL) ||
      (cancellation == NULL)) {
    return;
  }

  bounce_generic_spin_lock(&r->lock);
  bounce_generic_spin_lock(&cancellation->lock);
  if (cancellation->canceled != 0) {
    bounce_generic_spin_unlock(&cancellation->lock);
    bounce_generic_spin_unlock(&r->lock);
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

    item = BOUNCE_GENERIC_CONTAINER_OF(
      list_item,
      __BOUNCE_COMPLETION_ITEM,
      cancellation_link);
    item->cancellation = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_generic_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_CANCELED);
  }
  bounce_generic_spin_unlock(&cancellation->lock);
  bounce_generic_spin_unlock(&r->lock);
}

/**
 * @brief Deinitialize a cancellation.
 * @param cancellation Cancellation storage provided by the caller.
 */
void bounce_cancellation_deinit(BOUNCE_CANCELLATION *cancellation) {
  (void)cancellation;
}

#endif
