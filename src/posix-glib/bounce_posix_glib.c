/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#if defined(BOUNCE_POSIX_GLIB)

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <glib.h>
#include <glib-unix.h>

#define BOUNCE_UTILS_EXTERN static inline
#include "libbounce/bounce.h"
#include "../utils.c"

//////////////////////////////////////////////////////////////////////////////////
/*
 * POSIX GLib bounce architecture:
 *
 * - This backend is intentionally single-parker. The parked thread is the
 *   thread that drives the target `GMainContext`.
 * - Producer paths still publish ready completion items into the shared ready
 *   queue, but wakeup is expressed through `g_main_context_wakeup()`.
 * - A custom ready `GSource` is attached to the main context and drains the
 *   shared ready queue when the context dispatches it.
 * - fd waiting is integrated through one-shot `GSource` objects attached to the
 *   same main context. These sources claim the completion item, enqueue it into
 *   the ready queue, and then remove themselves.
 * - The completion flow therefore remains:
 *     post()/fd-ready -> ready queue -> main-context wake -> ready source
 *     dispatch -> execute completion.
 */

//////////////////////////////////////////////////////////////////////////////////

#define BOUNCE_POSIX_GLIB_INVALID_FD (-1)
#define BOUNCE_POSIX_GLIB_CONTAINER_OF(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

typedef struct BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT {
  struct BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *previous;
  BOUNCE_CORE *bounce;
  unsigned int depth;
  unsigned int max_inline_depth;
} BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT;

typedef struct BOUNCE_POSIX_GLIB_READY_SOURCE {
  GSource source;
  BOUNCE_CORE *bounce;
} BOUNCE_POSIX_GLIB_READY_SOURCE;

typedef struct BOUNCE_POSIX_GLIB_FD_SOURCE {
  GSource source;
  __BOUNCE_COMPLETION_ITEM *item;
  gpointer tag;
} BOUNCE_POSIX_GLIB_FD_SOURCE;

typedef enum __BOUNCE_POSIX_GLIB_START_RESULT {
  __BOUNCE_POSIX_GLIB_START_RESULT_FAILED = 0,
  __BOUNCE_POSIX_GLIB_START_RESULT_WAITING = 1,
  __BOUNCE_POSIX_GLIB_START_RESULT_READY_QUEUED = 2,
  __BOUNCE_POSIX_GLIB_START_RESULT_READY_INLINE = 3
} __BOUNCE_POSIX_GLIB_START_RESULT;

static pthread_once_t bounce_posix_glib_dispatch_tls_once_state = PTHREAD_ONCE_INIT;
static pthread_key_t bounce_posix_glib_dispatch_tls_key;

static void bounce_posix_glib_dispatch_tls_once(void) {
  (void)pthread_key_create(&bounce_posix_glib_dispatch_tls_key, NULL);
}

static inline bool bounce_posix_glib_ensure_dispatch_tls(void) {
  return pthread_once(
           &bounce_posix_glib_dispatch_tls_once_state,
           bounce_posix_glib_dispatch_tls_once) == 0;
}

static inline BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *
bounce_posix_glib_get_dispatch_context(void) {
  return bounce_posix_glib_ensure_dispatch_tls() ?
           (BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *)pthread_getspecific(
             bounce_posix_glib_dispatch_tls_key) :
           NULL;
}

static inline void bounce_posix_glib_set_dispatch_context(
  BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *context) {
  if (bounce_posix_glib_ensure_dispatch_tls()) {
    (void)pthread_setspecific(bounce_posix_glib_dispatch_tls_key, context);
  }
}

static inline int bounce_posix_glib_lock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_lock(mutex) : -1;
}

static inline int bounce_posix_glib_unlock(pthread_mutex_t *mutex) {
  return (mutex != NULL) ? pthread_mutex_unlock(mutex) : -1;
}

static inline void bounce_posix_glib_complete_direct(
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_COMPLETION_RESULT result) {
  if (completion != NULL) {
    completion(result, completion_state);
  }
}

static inline bool bounce_posix_glib_list_item_is_linked(
  const BOUNCE_LIST *list,
  const BOUNCE_LIST_ITEM *item) {
  return (list != NULL) &&
         (item != NULL) &&
         ((item->previous != NULL) ||
          (item->next != NULL) ||
          (list->head == item));
}

static inline void bounce_posix_glib_signal_parker(BOUNCE_CORE *r) {
  if ((r != NULL) && (r->main_context != NULL)) {
    g_main_context_wakeup(r->main_context);
  }
}

static inline void bounce_posix_glib_init_free_item(__BOUNCE_COMPLETION_ITEM *item) {
  memset(item, 0, sizeof *item);
  item->fd = BOUNCE_POSIX_GLIB_INVALID_FD;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_FREE;
}

static inline void bounce_posix_glib_prepare_reserved_item(__BOUNCE_COMPLETION_ITEM *item) {
  item->ready_or_free_link.next = NULL;
  item->cancellation_link.previous = NULL;
  item->cancellation_link.next = NULL;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  item->completion = NULL;
  item->completion_state = NULL;
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->bounce = NULL;
  item->fd = BOUNCE_POSIX_GLIB_INVALID_FD;
  item->condition = (GIOCondition)0;
  item->cancellation = NULL;
  item->registration_owner = NULL;
  item->source = NULL;
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_posix_glib_allocate_completion_item(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item = NULL;
  BOUNCE_DYNAMIC_BLOCK *block;
  __BOUNCE_COMPLETION_ITEM *dynamic_items;
  size_t index;

  if (r == NULL) {
    return NULL;
  }

  (void)bounce_posix_glib_lock(&r->lock);
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  (void)bounce_posix_glib_unlock(&r->lock);
  if (item != NULL) {
    bounce_posix_glib_prepare_reserved_item(item);
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
    bounce_posix_glib_init_free_item(&dynamic_items[index]);
  }

  (void)bounce_posix_glib_lock(&r->lock);
  bounce_dynamic_block_list_prepend(&r->dynamic_completion_blocks, block);
  for (index = 0u; index < BOUNCE_DYNAMIC_COMPLETION_ITEMS_STEP; index++) {
    bounce_stack_push(&r->free_items, &dynamic_items[index].ready_or_free_link);
  }
  item = (__BOUNCE_COMPLETION_ITEM *)bounce_stack_pop(&r->free_items);
  (void)bounce_posix_glib_unlock(&r->lock);

  if (item != NULL) {
    bounce_posix_glib_prepare_reserved_item(item);
  }
  return item;
}

static inline void bounce_posix_glib_recycle_completion_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  if ((r == NULL) || (item == NULL)) {
    return;
  }

  bounce_posix_glib_init_free_item(item);
  (void)bounce_posix_glib_lock(&r->lock);
  bounce_stack_push(&r->free_items, &item->ready_or_free_link);
  (void)bounce_posix_glib_unlock(&r->lock);
}

static inline void bounce_posix_glib_queue_ready_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  item->completion_result = result;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_READY;
  bounce_queue_enqueue(&r->ready_queue, &item->ready_or_free_link);
}

static inline bool bounce_posix_glib_has_pending_waits_locked(BOUNCE_CORE *r) {
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

static inline bool bounce_posix_glib_should_exit_locked(BOUNCE_CORE *r) {
  return (r->shutting_down != 0) &&
         (r->ready_queue.head == NULL) &&
         ((r->shutdown_wait_for_idle == 0) ||
          !bounce_posix_glib_has_pending_waits_locked(r));
}

static inline bool bounce_posix_glib_can_inline_locked(BOUNCE_CORE *r) {
  BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *context =
    bounce_posix_glib_get_dispatch_context();

  return (context != NULL) &&
         (context->bounce == r) &&
         (context->depth < context->max_inline_depth) &&
         (r->shutting_down == 0) &&
         (r->ready_queue.head == NULL);
}

static inline __BOUNCE_POSIX_GLIB_START_RESULT
bounce_posix_glib_finish_immediate_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_COMPLETION_RESULT result) {
  if (bounce_posix_glib_can_inline_locked(r)) {
    item->completion_result = result;
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
    return __BOUNCE_POSIX_GLIB_START_RESULT_READY_INLINE;
  }

  bounce_posix_glib_queue_ready_locked(r, item, result);
  return __BOUNCE_POSIX_GLIB_START_RESULT_READY_QUEUED;
}

static inline bool bounce_posix_glib_item_is_cancellation_linked(
  const __BOUNCE_COMPLETION_ITEM *item) {
  return (item->cancellation != NULL) &&
         bounce_posix_glib_list_item_is_linked(
           &item->cancellation->listeners,
           &item->cancellation_link);
}

static inline void bounce_posix_glib_unlink_cancellation_locked(
  __BOUNCE_COMPLETION_ITEM *item) {
  BOUNCE_CANCELLATION *cancellation = item->cancellation;

  if (cancellation == NULL) {
    return;
  }

  (void)bounce_posix_glib_lock(&cancellation->lock);
  if (bounce_posix_glib_item_is_cancellation_linked(item)) {
    bounce_list_remove(&cancellation->listeners, &item->cancellation_link);
  }
  (void)bounce_posix_glib_unlock(&cancellation->lock);

  item->cancellation = NULL;
}

static inline GSource *bounce_posix_glib_take_source_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  GSource *source = item->source;

  if (source != NULL) {
    item->source = NULL;
    if ((item->fd != BOUNCE_POSIX_GLIB_INVALID_FD) &&
        (r != NULL) &&
        (r->active_watch_count > 0u)) {
      r->active_watch_count -= 1u;
    }
  }
  return source;
}

static inline GSource *bounce_posix_glib_detach_wait_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item) {
  GSource *source = bounce_posix_glib_take_source_locked(r, item);

  bounce_posix_glib_unlink_cancellation_locked(item);
  if (item->registration_owner != NULL) {
    if (item->registration_owner->item == item) {
      item->registration_owner->item = NULL;
    }
    item->registration_owner = NULL;
  }
  return source;
}

static inline __BOUNCE_COMPLETION_ITEM *bounce_posix_glib_dequeue_ready_locked(
  BOUNCE_CORE *r) {
  __BOUNCE_COMPLETION_ITEM *item =
    (__BOUNCE_COMPLETION_ITEM *)bounce_queue_dequeue(&r->ready_queue);

  if (item != NULL) {
    item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  }
  return item;
}

static inline void bounce_posix_glib_execute_claimed_item(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  unsigned int max_inline_depth) {
  BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *context =
    bounce_posix_glib_get_dispatch_context();
  BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT local_context;
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
    bounce_posix_glib_set_dispatch_context(&local_context);
    context = &local_context;
  }

  context->depth += 1u;
  bounce_posix_glib_complete_direct(completion, completion_state, result);
  context->depth -= 1u;

  if (installed_context) {
    bounce_posix_glib_set_dispatch_context(local_context.previous);
  }

  bounce_posix_glib_recycle_completion_item(r, item);
}

static inline bool bounce_posix_glib_dispatch_next_ready(
  BOUNCE_CORE *r,
  unsigned int max_inline_depth) {
  __BOUNCE_COMPLETION_ITEM *item;

  (void)bounce_posix_glib_lock(&r->lock);
  item = bounce_posix_glib_dequeue_ready_locked(r);
  (void)bounce_posix_glib_unlock(&r->lock);

  if (item == NULL) {
    return false;
  }

  bounce_posix_glib_execute_claimed_item(r, item, max_inline_depth);
  return true;
}

static inline void bounce_posix_glib_destroy_source(GSource *source) {
  if (source != NULL) {
    g_source_destroy(source);
  }
}

static gboolean bounce_posix_glib_ready_source_prepare(
  GSource *source,
  gint *timeout_) {
  BOUNCE_POSIX_GLIB_READY_SOURCE *ready_source =
    (BOUNCE_POSIX_GLIB_READY_SOURCE *)source;
  bool ready = false;

  if (timeout_ != NULL) {
    *timeout_ = -1;
  }
  if (ready_source->bounce == NULL) {
    return FALSE;
  }

  (void)bounce_posix_glib_lock(&ready_source->bounce->lock);
  ready = (ready_source->bounce->ready_queue.head != NULL) ||
          bounce_posix_glib_should_exit_locked(ready_source->bounce);
  (void)bounce_posix_glib_unlock(&ready_source->bounce->lock);
  return ready;
}

static gboolean bounce_posix_glib_ready_source_check(GSource *source) {
  return bounce_posix_glib_ready_source_prepare(source, NULL);
}

static gboolean bounce_posix_glib_ready_source_dispatch(
  GSource *source,
  GSourceFunc callback,
  gpointer user_data) {
  (void)source;
  (void)callback;
  (void)user_data;
  return G_SOURCE_CONTINUE;
}

static GSourceFuncs bounce_posix_glib_ready_source_funcs = {
  bounce_posix_glib_ready_source_prepare,
  bounce_posix_glib_ready_source_check,
  bounce_posix_glib_ready_source_dispatch,
  NULL,
  NULL,
  NULL
};

static inline __BOUNCE_POSIX_GLIB_START_RESULT
bounce_posix_glib_activate_wait_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_CANCELLATION *cancellation) {
  if (r->shutting_down != 0) {
    return __BOUNCE_POSIX_GLIB_START_RESULT_FAILED;
  }

  if (item->registration_owner != NULL) {
    if (item->registration_owner->item != NULL) {
      return __BOUNCE_POSIX_GLIB_START_RESULT_FAILED;
    }
    item->registration_owner->item = item;
  }

  item->state = BOUNCE_COMPLETION_ITEM_STATE_WAITING;
  if (cancellation != NULL) {
    (void)bounce_posix_glib_lock(&cancellation->lock);
    if (cancellation->canceled != 0) {
      (void)bounce_posix_glib_unlock(&cancellation->lock);
      if (item->registration_owner != NULL) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
      item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
      return bounce_posix_glib_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_CANCELED);
    }

    item->cancellation = cancellation;
    bounce_list_insert_tail(&cancellation->listeners, &item->cancellation_link);
    (void)bounce_posix_glib_unlock(&cancellation->lock);
  }
  return __BOUNCE_POSIX_GLIB_START_RESULT_WAITING;
}

static gboolean bounce_posix_glib_fd_source_prepare(
  GSource *source,
  gint *timeout_) {
  (void)source;
  if (timeout_ != NULL) {
    *timeout_ = -1;
  }
  return FALSE;
}

static gboolean bounce_posix_glib_fd_source_check(GSource *source) {
  BOUNCE_POSIX_GLIB_FD_SOURCE *fd_source =
    (BOUNCE_POSIX_GLIB_FD_SOURCE *)source;
  __BOUNCE_COMPLETION_ITEM *item = fd_source->item;
  GIOCondition revents;

  if ((item == NULL) ||
      (fd_source->tag == NULL)) {
    return FALSE;
  }

  revents = g_source_query_unix_fd(source, fd_source->tag);
  return (revents & (item->condition | G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0;
}

static gboolean bounce_posix_glib_fd_source_dispatch(
  GSource *source,
  GSourceFunc callback,
  gpointer user_data) {
  BOUNCE_POSIX_GLIB_FD_SOURCE *fd_source =
    (BOUNCE_POSIX_GLIB_FD_SOURCE *)source;
  __BOUNCE_COMPLETION_ITEM *item = fd_source->item;
  BOUNCE_CORE *bounce;
  bool wake_parker = false;

  (void)callback;
  (void)user_data;
  if (item == NULL) {
    return G_SOURCE_REMOVE;
  }

  bounce = item->bounce;
  if (bounce == NULL) {
    return G_SOURCE_REMOVE;
  }

  (void)bounce_posix_glib_lock(&bounce->lock);
  if ((item->state == BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
      (item->source != NULL)) {
    (void)bounce_posix_glib_take_source_locked(bounce, item);
    bounce_posix_glib_unlink_cancellation_locked(item);
    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_posix_glib_queue_ready_locked(
      bounce,
      item,
      BOUNCE_COMPLETION_COMPLETED);
    wake_parker = true;
  }
  (void)bounce_posix_glib_unlock(&bounce->lock);

  if (wake_parker) {
    bounce_posix_glib_signal_parker(bounce);
  }
  return G_SOURCE_REMOVE;
}

static GSourceFuncs bounce_posix_glib_fd_source_funcs = {
  bounce_posix_glib_fd_source_prepare,
  bounce_posix_glib_fd_source_check,
  bounce_posix_glib_fd_source_dispatch,
  NULL,
  NULL,
  NULL
};

static inline GSource *bounce_posix_glib_abort_pending_item_locked(
  BOUNCE_CORE *r,
  __BOUNCE_COMPLETION_ITEM *item,
  BOUNCE_QUEUE *abort_queue) {
  GSource *source;

  if ((item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) &&
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_READY)) {
    return NULL;
  }

  source = bounce_posix_glib_detach_wait_locked(r, item);
  item->completion_result = BOUNCE_COMPLETION_ABORTED;
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  bounce_queue_enqueue(abort_queue, &item->ready_or_free_link);
  return source;
}

static void bounce_posix_glib_init_core(
  BOUNCE_CORE *r,
  GMainContext *main_context) {
  BOUNCE_POSIX_GLIB_READY_SOURCE *ready_source;

  if (r == NULL) {
    return;
  }

  memset(r, 0, sizeof *r);
  (void)pthread_mutex_init(&r->lock, NULL);
  bounce_queue_init(&r->ready_queue);
  bounce_stack_init(&r->free_items);
  bounce_dynamic_block_list_init(&r->dynamic_completion_blocks);
  r->main_context =
    (main_context != NULL) ?
      g_main_context_ref(main_context) :
      g_main_context_new();

  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    bounce_posix_glib_init_free_item(&r->static_completion_items[index]);
    bounce_stack_push(
      &r->free_items,
      &r->static_completion_items[index].ready_or_free_link);
  }

  if (r->main_context == NULL) {
    return;
  }

  ready_source = (BOUNCE_POSIX_GLIB_READY_SOURCE *)g_source_new(
    &bounce_posix_glib_ready_source_funcs,
    sizeof *ready_source);
  if (ready_source == NULL) {
    return;
  }

  ready_source->bounce = r;
  g_source_set_priority(&ready_source->source, G_PRIORITY_HIGH);
  (void)g_source_attach(&ready_source->source, r->main_context);
  r->ready_source = &ready_source->source;
}

/**
 * @brief Initialize the bounce.
 * @param r BOUNCE_CORE structure space provided by the caller.
 */
void bounce_init(BOUNCE_CORE *r) {
  bounce_posix_glib_init_core(r, NULL);
}

/**
 * @brief Initialize the GLib backend with an explicit `GMainContext`.
 * @param r BOUNCE_CORE structure space provided by the caller.
 * @param main_context GLib main context to drive from `bounce_park()`, or
 * `NULL` to create a private context like `bounce_init()`.
 */
void bounce_init_with_main_context(
  BOUNCE_CORE *r,
  GMainContext *main_context) {
  bounce_posix_glib_init_core(r, main_context);
}

/**
 * @brief Park current thread and run continuation repeatedly.
 * @param r Initialized BOUNCE_CORE.
 * @return True when succeeded continuation pumps.
 * @remarks The thread drives the GLib main context and only one parker is valid.
 */
bool bounce_park(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  if (r == NULL) {
    return false;
  }

  for (;;) {
    while (bounce_posix_glib_dispatch_next_ready(r, max_inline_depth)) {
    }

    (void)bounce_posix_glib_lock(&r->lock);
    if (bounce_posix_glib_should_exit_locked(r)) {
      (void)bounce_posix_glib_unlock(&r->lock);
      return true;
    }
    (void)bounce_posix_glib_unlock(&r->lock);

    if (r->main_context == NULL) {
      return false;
    }
    (void)g_main_context_iteration(r->main_context, TRUE);
  }
}

/**
 * @brief Pump current thread once without waiting for new completion work.
 * @param r Initialized BOUNCE_CORE.
 * @param max_inline_depth Maximum number of inline nested completion executions.
 * @return True when succeeded continuation pumps.
 * @remarks This performs at most one non-blocking main-context iteration after
 * draining already ready bounce items, then drains any bounce work that
 * iteration published into the ready queue.
 */
bool bounce_park_once(BOUNCE_CORE *r, unsigned int max_inline_depth) {
  if (r == NULL) {
    return false;
  }

  while (bounce_posix_glib_dispatch_next_ready(r, max_inline_depth)) {
  }
  if (r->main_context != NULL) {
    (void)g_main_context_iteration(r->main_context, FALSE);
  }
  while (bounce_posix_glib_dispatch_next_ready(r, max_inline_depth)) {
  }
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

  (void)bounce_posix_glib_lock(&r->lock);
  if (bounce_posix_glib_can_inline_locked(r)) {
    BOUNCE_POSIX_GLIB_DISPATCH_CONTEXT *context =
      bounce_posix_glib_get_dispatch_context();

    (void)bounce_posix_glib_unlock(&r->lock);
    context->depth += 1u;
    completion(BOUNCE_COMPLETION_COMPLETED, completion_state);
    context->depth -= 1u;
    return true;
  }
  (void)bounce_posix_glib_unlock(&r->lock);

  item = bounce_posix_glib_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;

  (void)bounce_posix_glib_lock(&r->lock);
  if (r->shutting_down != 0) {
    (void)bounce_posix_glib_unlock(&r->lock);
    bounce_posix_glib_recycle_completion_item(r, item);
    return false;
  }

  bounce_posix_glib_queue_ready_locked(
    r,
    item,
    BOUNCE_COMPLETION_COMPLETED);
  (void)bounce_posix_glib_unlock(&r->lock);

  bounce_posix_glib_signal_parker(r);
  return true;
}

/**
 * @brief Await GLib-integrated file-descriptor readiness through `GSource`.
 * @param r Initialized BOUNCE_CORE.
 * @param fd File descriptor watched by GLib main-context polling.
 * @param condition `GIOCondition` mask such as `G_IO_IN` or `G_IO_OUT`.
 * @param completion Completion callback entry point.
 * @param completion_state User provided completion callback state.
 * @param cancellation Cancellation when provided.
 */
void bounce_await_posix_glib_fd(
  BOUNCE_CORE *r,
  int fd,
  GIOCondition condition,
  BOUNCE_COMPLETION completion,
  void *completion_state,
  BOUNCE_CANCELLATION *cancellation) {
  __BOUNCE_COMPLETION_ITEM *item;
  __BOUNCE_POSIX_GLIB_START_RESULT start_result;

  if ((r == NULL) ||
      (fd < 0) ||
      (condition == 0) ||
      (completion == NULL)) {
    bounce_posix_glib_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item = bounce_posix_glib_allocate_completion_item(r);
  if (item == NULL) {
    bounce_posix_glib_complete_direct(
      completion,
      completion_state,
      BOUNCE_COMPLETION_ABORTED);
    return;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->fd = fd;
  item->condition = condition;

  (void)bounce_posix_glib_lock(&r->lock);
  start_result = bounce_posix_glib_activate_wait_item_locked(
    r,
    item,
    cancellation);
  if (start_result == __BOUNCE_POSIX_GLIB_START_RESULT_WAITING) {
    GSource *source = NULL;
    guint source_id = 0u;

    if ((r->main_context == NULL) ||
        (r->active_watch_count >= BOUNCE_MAX_POSIX_GLIB_WATCHES)) {
      (void)bounce_posix_glib_detach_wait_locked(r, item);
      start_result = bounce_posix_glib_finish_immediate_item_locked(
        r,
        item,
        BOUNCE_COMPLETION_ABORTED);
    } else {
      BOUNCE_POSIX_GLIB_FD_SOURCE *fd_source =
        (BOUNCE_POSIX_GLIB_FD_SOURCE *)g_source_new(
          &bounce_posix_glib_fd_source_funcs,
          sizeof *fd_source);

      source = (fd_source != NULL) ? &fd_source->source : NULL;
      if (source == NULL) {
        (void)bounce_posix_glib_detach_wait_locked(r, item);
        start_result = bounce_posix_glib_finish_immediate_item_locked(
          r,
          item,
          BOUNCE_COMPLETION_ABORTED);
      } else {
        fd_source->item = item;
        fd_source->tag = g_source_add_unix_fd(source, fd, condition);
        if (fd_source->tag == NULL) {
          g_source_unref(source);
          (void)bounce_posix_glib_detach_wait_locked(r, item);
          start_result = bounce_posix_glib_finish_immediate_item_locked(
            r,
            item,
            BOUNCE_COMPLETION_ABORTED);
        } else {
          item->source = source;
          r->active_watch_count += 1u;
          source_id = g_source_attach(source, r->main_context);
        }
        if ((start_result == __BOUNCE_POSIX_GLIB_START_RESULT_WAITING) &&
            (source_id == 0u)) {
          (void)bounce_posix_glib_take_source_locked(r, item);
          (void)bounce_posix_glib_detach_wait_locked(r, item);
          g_source_unref(source);
          start_result = bounce_posix_glib_finish_immediate_item_locked(
            r,
            item,
            BOUNCE_COMPLETION_ABORTED);
        } else {
          g_source_unref(source);
        }
      }
    }
  }
  (void)bounce_posix_glib_unlock(&r->lock);

  switch (start_result) {
    case __BOUNCE_POSIX_GLIB_START_RESULT_WAITING:
      return;
    case __BOUNCE_POSIX_GLIB_START_RESULT_READY_QUEUED:
      bounce_posix_glib_signal_parker(r);
      return;
    case __BOUNCE_POSIX_GLIB_START_RESULT_READY_INLINE:
      bounce_posix_glib_execute_claimed_item(r, item, 0u);
      return;
    case __BOUNCE_POSIX_GLIB_START_RESULT_FAILED:
    default:
      bounce_posix_glib_recycle_completion_item(r, item);
      bounce_posix_glib_complete_direct(
        completion,
        completion_state,
        BOUNCE_COMPLETION_ABORTED);
      return;
  }
}

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Shutdown parking threads.
 * @param r Initialized BOUNCE_CORE.
 */
void bounce_shutdown(BOUNCE_CORE *r, bool wait_for_idle) {
  if (r == NULL) {
    return;
  }

  (void)bounce_posix_glib_lock(&r->lock);
  r->shutting_down = 1;
  if (wait_for_idle) {
    r->shutdown_wait_for_idle = 1;
  } else {
    r->shutdown_wait_for_idle = 0;
  }
  (void)bounce_posix_glib_unlock(&r->lock);

  bounce_posix_glib_signal_parker(r);
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

  (void)bounce_posix_glib_lock(&r->lock);
  bounce_queue_init(&r->ready_queue);
  for (size_t index = 0u; index < BOUNCE_MAX_STATIC_COMPLETION_ITEMS; index++) {
    GSource *source = bounce_posix_glib_abort_pending_item_locked(
      r,
      &r->static_completion_items[index],
      &abort_queue);

    if (source != NULL) {
      (void)bounce_posix_glib_unlock(&r->lock);
      bounce_posix_glib_destroy_source(source);
      (void)bounce_posix_glib_lock(&r->lock);
    }
  }

  for (BOUNCE_DYNAMIC_BLOCK *block = r->dynamic_completion_blocks.head;
       block != NULL;
       block = block->next) {
    __BOUNCE_COMPLETION_ITEM *items =
      (__BOUNCE_COMPLETION_ITEM *)bounce_dynamic_block_const_items(block);

    for (size_t index = 0u; index < block->item_count; index++) {
      GSource *source = bounce_posix_glib_abort_pending_item_locked(
        r,
        &items[index],
        &abort_queue);

      if (source != NULL) {
        (void)bounce_posix_glib_unlock(&r->lock);
        bounce_posix_glib_destroy_source(source);
        (void)bounce_posix_glib_lock(&r->lock);
      }
    }
  }
  (void)bounce_posix_glib_unlock(&r->lock);

  while ((node = (BOUNCE_NODE_ITEM *)bounce_queue_dequeue(&abort_queue)) != NULL) {
    __BOUNCE_COMPLETION_ITEM *item =
      BOUNCE_POSIX_GLIB_CONTAINER_OF(
        node,
        __BOUNCE_COMPLETION_ITEM,
        ready_or_free_link);

    bounce_posix_glib_complete_direct(
      item->completion,
      item->completion_state,
      BOUNCE_COMPLETION_ABORTED);
  }

  if (r->ready_source != NULL) {
    g_source_destroy(r->ready_source);
    g_source_unref(r->ready_source);
    r->ready_source = NULL;
  }
  if (r->main_context != NULL) {
    g_main_context_unref(r->main_context);
    r->main_context = NULL;
  }

  bounce_dynamic_block_list_free_all(&r->dynamic_completion_blocks);
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
  __BOUNCE_POSIX_GLIB_START_RESULT start_result;

  if ((r == NULL) ||
      (cancellation == NULL) ||
      (registration == NULL) ||
      (completion == NULL)) {
    return false;
  }

  item = bounce_posix_glib_allocate_completion_item(r);
  if (item == NULL) {
    return false;
  }

  item->bounce = r;
  item->completion = completion;
  item->completion_state = completion_state;
  item->registration_owner = registration;

  (void)bounce_posix_glib_lock(&r->lock);
  start_result = bounce_posix_glib_activate_wait_item_locked(
    r,
    item,
    cancellation);
  (void)bounce_posix_glib_unlock(&r->lock);

  switch (start_result) {
    case __BOUNCE_POSIX_GLIB_START_RESULT_WAITING:
      return true;
    case __BOUNCE_POSIX_GLIB_START_RESULT_READY_QUEUED:
      bounce_posix_glib_signal_parker(r);
      return true;
    case __BOUNCE_POSIX_GLIB_START_RESULT_READY_INLINE:
      bounce_posix_glib_execute_claimed_item(r, item, 0u);
      return true;
    case __BOUNCE_POSIX_GLIB_START_RESULT_FAILED:
    default:
      bounce_posix_glib_recycle_completion_item(r, item);
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
  GSource *source;

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

  (void)bounce_posix_glib_lock(&bounce->lock);
  item = registration->item;
  if ((item == NULL) ||
      (item->registration_owner != registration) ||
      (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING)) {
    (void)bounce_posix_glib_unlock(&bounce->lock);
    return false;
  }

  source = bounce_posix_glib_detach_wait_locked(bounce, item);
  item->state = BOUNCE_COMPLETION_ITEM_STATE_CLAIMED;
  (void)bounce_posix_glib_unlock(&bounce->lock);

  bounce_posix_glib_destroy_source(source);
  bounce_posix_glib_recycle_completion_item(bounce, item);
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
  bool wake_parker = false;

  if ((r == NULL) ||
      (cancellation == NULL)) {
    return;
  }

  (void)bounce_posix_glib_lock(&r->lock);
  (void)bounce_posix_glib_lock(&cancellation->lock);
  if (cancellation->canceled != 0) {
    (void)bounce_posix_glib_unlock(&cancellation->lock);
    (void)bounce_posix_glib_unlock(&r->lock);
    return;
  }

  cancellation->canceled = 1;
  for (;;) {
    BOUNCE_LIST_ITEM *list_item =
      (BOUNCE_LIST_ITEM *)bounce_list_pop_head(&cancellation->listeners);
    __BOUNCE_COMPLETION_ITEM *item;
    GSource *source = NULL;

    if (list_item == NULL) {
      break;
    }

    item = BOUNCE_POSIX_GLIB_CONTAINER_OF(
      list_item,
      __BOUNCE_COMPLETION_ITEM,
      cancellation_link);
    item->cancellation = NULL;
    if (item->state != BOUNCE_COMPLETION_ITEM_STATE_WAITING) {
      continue;
    }

    source = bounce_posix_glib_take_source_locked(r, item);
    if (item->registration_owner != NULL) {
      if (item->registration_owner->item == item) {
        item->registration_owner->item = NULL;
      }
      item->registration_owner = NULL;
    }
    bounce_posix_glib_queue_ready_locked(
      r,
      item,
      BOUNCE_COMPLETION_CANCELED);
    wake_parker = true;

    (void)bounce_posix_glib_unlock(&cancellation->lock);
    (void)bounce_posix_glib_unlock(&r->lock);
    bounce_posix_glib_destroy_source(source);
    (void)bounce_posix_glib_lock(&r->lock);
    (void)bounce_posix_glib_lock(&cancellation->lock);
  }
  (void)bounce_posix_glib_unlock(&cancellation->lock);
  (void)bounce_posix_glib_unlock(&r->lock);

  if (wake_parker) {
    bounce_posix_glib_signal_parker(r);
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
