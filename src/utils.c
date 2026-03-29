/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "libbounce/utils.h"

static void *bounce_utils_allocate(size_t size) {
  return malloc(size);
}

static void bounce_utils_release(void *ptr) {
  free(ptr);
}

BOUNCE_UTILS_EXTERN void bounce_list_init(BOUNCE_LIST *list) {
  memset(list, 0, sizeof *list);
}

BOUNCE_UTILS_EXTERN void bounce_list_item_init(BOUNCE_LIST_ITEM *item) {
  item->previous = NULL;
  item->next = NULL;
}

BOUNCE_UTILS_EXTERN void bounce_list_insert_head(
  BOUNCE_LIST *list,
  /* BOUNCE_LIST_ITEM */ void *item_) {
  BOUNCE_LIST_ITEM *item = (BOUNCE_LIST_ITEM *)item_;

  item->previous = NULL;
  item->next = list->head;
  if (list->head != NULL) {
    list->head->previous = item;
  } else {
    list->tail = item;
  }
  list->head = item;
}

BOUNCE_UTILS_EXTERN void bounce_list_insert_tail(
  BOUNCE_LIST *list,
  /* BOUNCE_LIST_ITEM */ void *item_) {
  BOUNCE_LIST_ITEM *item = (BOUNCE_LIST_ITEM *)item_;

  item->previous = list->tail;
  item->next = NULL;
  if (list->tail != NULL) {
    list->tail->next = item;
  } else {
    list->head = item;
  }
  list->tail = item;
}

BOUNCE_UTILS_EXTERN void bounce_list_remove(
  BOUNCE_LIST *list,
  /* BOUNCE_LIST_ITEM */ void *item_) {
  BOUNCE_LIST_ITEM *item = (BOUNCE_LIST_ITEM *)item_;

  if (item->previous != NULL) {
    item->previous->next = item->next;
  } else if (list->head == item) {
    list->head = item->next;
  }

  if (item->next != NULL) {
    item->next->previous = item->previous;
  } else if (list->tail == item) {
    list->tail = item->previous;
  }

  item->previous = NULL;
  item->next = NULL;
}

BOUNCE_UTILS_EXTERN /* BOUNCE_LIST_ITEM */ void *bounce_list_pop_head(
  BOUNCE_LIST *list) {
  BOUNCE_LIST_ITEM *item = list->head;

  if (item != NULL) {
    bounce_list_remove(list, item);
  }
  return item;
}

BOUNCE_UTILS_EXTERN /* BOUNCE_LIST_ITEM */ void *bounce_list_pop_tail(
  BOUNCE_LIST *list) {
  BOUNCE_LIST_ITEM *item = list->tail;

  if (item != NULL) {
    bounce_list_remove(list, item);
  }
  return item;
}

BOUNCE_UTILS_EXTERN void bounce_list_take_all(
  BOUNCE_LIST *destination,
  BOUNCE_LIST *source) {
  *destination = *source;
  bounce_list_init(source);
}

//////////////////////////////////////////////////////////////////////////////////

BOUNCE_UTILS_EXTERN void bounce_stack_init(BOUNCE_STACK *s) {
  memset(s, 0, sizeof *s);
}

BOUNCE_UTILS_EXTERN void bounce_stack_push(BOUNCE_STACK *s, /* BOUNCE_STACK_ITEM */ void *item_) {
  BOUNCE_STACK_ITEM *item = (BOUNCE_STACK_ITEM *)item_;

  item->next = s->top;
  s->top = item;
}

BOUNCE_UTILS_EXTERN /* BOUNCE_STACK_ITEM */ void *bounce_stack_pop(BOUNCE_STACK *s) {
  BOUNCE_STACK_ITEM *item = s->top;

  if (item != NULL) {
    s->top = item->next;
    item->next = NULL;
  }
  return item;
}

//////////////////////////////////////////////////////////////////////////////////

BOUNCE_UTILS_EXTERN void bounce_queue_init(BOUNCE_QUEUE *q) {
  memset(q, 0, sizeof *q);
}

BOUNCE_UTILS_EXTERN void bounce_queue_enqueue(BOUNCE_QUEUE *q, /* BOUNCE_QUEUE_ITEM */ void *item_) {
  BOUNCE_QUEUE_ITEM *item = (BOUNCE_QUEUE_ITEM *)item_;

  item->next = NULL;
  if (q->tail != NULL) {
    q->tail->next = item;
  } else {
    q->head = item;
  }
  q->tail = item;
}

BOUNCE_UTILS_EXTERN /* BOUNCE_QUEUE_ITEM */ void *bounce_queue_dequeue(BOUNCE_QUEUE *q) {
  BOUNCE_QUEUE_ITEM *item = q->head;

  if (item != NULL) {
    q->head = item->next;
    if (q->head == NULL) {
      q->tail = NULL;
    }
    item->next = NULL;
  }
  return item;
}

//////////////////////////////////////////////////////////////////////////////////

BOUNCE_UTILS_EXTERN void bounce_dynamic_block_list_init(
  BOUNCE_DYNAMIC_BLOCK_LIST *list) {
  list->head = NULL;
}

BOUNCE_UTILS_EXTERN BOUNCE_DYNAMIC_BLOCK *bounce_dynamic_block_allocate(
  size_t item_size,
  size_t item_count) {
  BOUNCE_DYNAMIC_BLOCK *block;
  size_t payload_size;
  size_t total_size;

  if ((item_size == 0u) || (item_count == 0u)) {
    return NULL;
  }
  if (item_count > ((SIZE_MAX - sizeof *block) / item_size)) {
    return NULL;
  }

  payload_size = item_size * item_count;
  total_size = sizeof *block + payload_size;
  block = (BOUNCE_DYNAMIC_BLOCK *)bounce_utils_allocate(total_size);
  if (block == NULL) {
    return NULL;
  }

  block->next = NULL;
  block->item_count = item_count;
  return block;
}

BOUNCE_UTILS_EXTERN void bounce_dynamic_block_list_prepend(
  BOUNCE_DYNAMIC_BLOCK_LIST *list,
  BOUNCE_DYNAMIC_BLOCK *block) {
  block->next = list->head;
  list->head = block;
}

BOUNCE_UTILS_EXTERN void *bounce_dynamic_block_items(
  BOUNCE_DYNAMIC_BLOCK *block) {
  return (void *)(block + 1);
}

BOUNCE_UTILS_EXTERN const void *bounce_dynamic_block_const_items(
  const BOUNCE_DYNAMIC_BLOCK *block) {
  return (const void *)(block + 1);
}

BOUNCE_UTILS_EXTERN void bounce_dynamic_block_list_free_all(
  BOUNCE_DYNAMIC_BLOCK_LIST *list) {
  BOUNCE_DYNAMIC_BLOCK *block = list->head;

  while (block != NULL) {
    BOUNCE_DYNAMIC_BLOCK *next = block->next;

    bounce_utils_release(block);
    block = next;
  }
  list->head = NULL;
}
