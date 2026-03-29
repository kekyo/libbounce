/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#pragma once

#ifndef _LIBBOUNCE_UTILS_H
#define _LIBBOUNCE_UTILS_H

#include <stddef.h>

#ifndef BOUNCE_UTILS_EXTERN
#define BOUNCE_UTILS_EXTERN extern
#endif

//////////////////////////////////////////////////////////////////////////////////

typedef struct BOUNCE_NODE_ITEM BOUNCE_NODE_ITEM;

/**
 * @brief Shared intrusive node for bounce stacks and queues.
 * @remarks Caller-owned payload may follow this field. Concurrent access must
 * be serialized by the caller.
 */
struct BOUNCE_NODE_ITEM {
  BOUNCE_NODE_ITEM *next;
  /* (And user provideed storage in this area) */
};

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Intrusive node for bounce lists.
 * @remarks Caller-owned payload may follow this field. Concurrent access must
 * be serialized by the caller.
 */
typedef struct BOUNCE_LIST_ITEM {
  struct BOUNCE_LIST_ITEM *previous;
  struct BOUNCE_LIST_ITEM *next;
} BOUNCE_LIST_ITEM;

/**
 * @brief Intrusive double-linked list state.
 * @remarks The caller provides storage for this structure and must externally
 * serialize mutations and traversals when concurrent access is possible.
 */
typedef struct BOUNCE_LIST {
  BOUNCE_LIST_ITEM *head;
  BOUNCE_LIST_ITEM *tail;
} BOUNCE_LIST;

/**
 * @brief Initialize a list.
 * @param list List state provided by the caller.
 */
BOUNCE_UTILS_EXTERN void bounce_list_init(BOUNCE_LIST *list);

/**
 * @brief Initialize a list item.
 * @param item List item storage provided by the caller.
 */
BOUNCE_UTILS_EXTERN void bounce_list_item_init(BOUNCE_LIST_ITEM *item);

/**
 * @brief Insert an item at the head of the list.
 * @param list Initialized list state.
 * @param item Caller-owned item (BOUNCE_LIST_ITEM) to insert.
 */
BOUNCE_UTILS_EXTERN void bounce_list_insert_head(
  BOUNCE_LIST *list,
  /* BOUNCE_LIST_ITEM */ void *item);

/**
 * @brief Insert an item at the tail of the list.
 * @param list Initialized list state.
 * @param item Caller-owned item (BOUNCE_LIST_ITEM) to insert.
 */
BOUNCE_UTILS_EXTERN void bounce_list_insert_tail(
  BOUNCE_LIST *list,
  /* BOUNCE_LIST_ITEM */ void *item);

/**
 * @brief Remove an item from the list.
 * @param list Initialized list state.
 * @param item Caller-owned item (BOUNCE_LIST_ITEM) to remove.
 */
BOUNCE_UTILS_EXTERN void bounce_list_remove(
  BOUNCE_LIST *list,
  /* BOUNCE_LIST_ITEM */ void *item);

/**
 * @brief Pop an item from the head of the list.
 * @param list Initialized list state.
 * @return Popped item (BOUNCE_LIST_ITEM), or NULL when the list is empty.
 */
BOUNCE_UTILS_EXTERN /* BOUNCE_LIST_ITEM */ void *bounce_list_pop_head(
  BOUNCE_LIST *list);

/**
 * @brief Pop an item from the tail of the list.
 * @param list Initialized list state.
 * @return Popped item (BOUNCE_LIST_ITEM), or NULL when the list is empty.
 */
BOUNCE_UTILS_EXTERN /* BOUNCE_LIST_ITEM */ void *bounce_list_pop_tail(
  BOUNCE_LIST *list);

/**
 * @brief Move all items from one list into another in O(1).
 * @param destination Destination list state.
 * @param source Source list state.
 * @remarks Destination contents are overwritten.
 */
BOUNCE_UTILS_EXTERN void bounce_list_take_all(
  BOUNCE_LIST *destination,
  BOUNCE_LIST *source);

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Intrusive node for bounce stacks.
 * @remarks Caller-owned payload may follow this field. Concurrent access must
 * be serialized by the caller.
 */
typedef BOUNCE_NODE_ITEM BOUNCE_STACK_ITEM;

/**
 * @brief Intrusive stack state.
 * @remarks The caller provides storage for this structure and must externally
 * serialize mutations when concurrent access is possible.
 */
typedef struct BOUNCE_STACK {
  BOUNCE_STACK_ITEM *top;
} BOUNCE_STACK;

/**
 * @brief Initialize a stack.
 * @param s Stack state provided by the caller.
 */
BOUNCE_UTILS_EXTERN void bounce_stack_init(BOUNCE_STACK *s);

/**
 * @brief Push an item onto the stack.
 * @param s Initialized stack state.
 * @param item Caller-owned item (BOUNCE_STACK_ITEM) to push.
 */
BOUNCE_UTILS_EXTERN void bounce_stack_push(
  BOUNCE_STACK *s,
  /* BOUNCE_STACK_ITEM */ void *item);

/**
 * @brief Pop an item from the stack.
 * @param s Initialized stack state.
 * @return Popped item (BOUNCE_STACK_ITEM), or NULL when the stack is empty.
 * @remarks The returned item's `next` field is internal state and should be
 * overwritten before the item is reused.
 */
BOUNCE_UTILS_EXTERN /* BOUNCE_STACK_ITEM */ void *bounce_stack_pop(
  BOUNCE_STACK *s);

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Intrusive node for bounce queues.
 * @remarks Caller-owned payload may follow this field. Concurrent access must
 * be serialized by the caller.
 */
typedef BOUNCE_NODE_ITEM BOUNCE_QUEUE_ITEM;

/**
 * @brief Intrusive queue state.
 * @remarks The caller provides storage for this structure and must externally
 * serialize mutations when concurrent access is possible.
 */
typedef struct BOUNCE_QUEUE {
  BOUNCE_QUEUE_ITEM *head;
  BOUNCE_QUEUE_ITEM *tail;
} BOUNCE_QUEUE;

/**
 * @brief Initialize a queue.
 * @param q Queue state provided by the caller.
 */
BOUNCE_UTILS_EXTERN void bounce_queue_init(BOUNCE_QUEUE *q);

/**
 * @brief Enqueue an item.
 * @param q Initialized queue state.
 * @param item Caller-owned item (BOUNCE_QUEUE_ITEM) to enqueue.
 */
BOUNCE_UTILS_EXTERN void bounce_queue_enqueue(
  BOUNCE_QUEUE *q,
  /* BOUNCE_QUEUE_ITEM */ void *item);

/**
 * @brief Dequeue an item.
 * @param q Initialized queue state.
 * @return Dequeued item (BOUNCE_QUEUE_ITEM), or NULL when the queue is empty.
 * @remarks The returned item's `next` field is internal state and should be
 * overwritten before the item is reused.
 */
BOUNCE_UTILS_EXTERN /* BOUNCE_QUEUE_ITEM */ void *bounce_queue_dequeue(
  BOUNCE_QUEUE *q);

//////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Header for dynamically allocated append-only blocks.
 * @remarks The caller owns synchronization for list mutations and traversals.
 * Blocks stay valid until released through `bounce_dynamic_block_list_free_all()`.
 */
typedef struct BOUNCE_DYNAMIC_BLOCK {
  struct BOUNCE_DYNAMIC_BLOCK *next;
  size_t item_count;
} BOUNCE_DYNAMIC_BLOCK;

/**
 * @brief Append-only list of dynamically allocated blocks.
 */
typedef struct BOUNCE_DYNAMIC_BLOCK_LIST {
  BOUNCE_DYNAMIC_BLOCK *head;
} BOUNCE_DYNAMIC_BLOCK_LIST;

/**
 * @brief Initialize a dynamic block list.
 * @param list Dynamic block list storage provided by the caller.
 */
BOUNCE_UTILS_EXTERN void bounce_dynamic_block_list_init(
  BOUNCE_DYNAMIC_BLOCK_LIST *list);

/**
 * @brief Allocate a detached dynamic block.
 * @param item_size Size of one payload item in bytes.
 * @param item_count Number of payload items stored in the block.
 * @return Allocated block header, or NULL on allocation failure or overflow.
 * @remarks Payload storage begins immediately after the returned header and can
 * be accessed with `bounce_dynamic_block_items()`. The caller links the block
 * into a list through `bounce_dynamic_block_list_prepend()` after
 * initialization.
 */
BOUNCE_UTILS_EXTERN BOUNCE_DYNAMIC_BLOCK *bounce_dynamic_block_allocate(
  size_t item_size,
  size_t item_count);

/**
 * @brief Prepend a dynamic block to a dynamic block list.
 * @param list Initialized dynamic block list.
 * @param block Dynamic block allocated by `bounce_dynamic_block_allocate()`.
 */
BOUNCE_UTILS_EXTERN void bounce_dynamic_block_list_prepend(
  BOUNCE_DYNAMIC_BLOCK_LIST *list,
  BOUNCE_DYNAMIC_BLOCK *block);

/**
 * @brief Get the payload storage for a dynamic block.
 * @param block Dynamic block header returned by allocation.
 * @return Mutable payload storage immediately following the header.
 */
BOUNCE_UTILS_EXTERN void *bounce_dynamic_block_items(
  BOUNCE_DYNAMIC_BLOCK *block);

/**
 * @brief Get the payload storage for a dynamic block.
 * @param block Dynamic block header returned by allocation.
 * @return Read-only payload storage immediately following the header.
 */
BOUNCE_UTILS_EXTERN const void *bounce_dynamic_block_const_items(
  const BOUNCE_DYNAMIC_BLOCK *block);

/**
 * @brief Free all blocks in a dynamic block list.
 * @param list Initialized dynamic block list.
 */
BOUNCE_UTILS_EXTERN void bounce_dynamic_block_list_free_all(
  BOUNCE_DYNAMIC_BLOCK_LIST *list);

//////////////////////////////////////////////////////////////////////////////////

#endif
