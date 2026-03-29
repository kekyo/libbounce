/*
 * libbounce - A small thread dispatch library that handling asynchronous I/O.
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/libbounce
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "libbounce/utils.h"

#define TEST_ASSERT(condition)                                                   \
  do {                                                                           \
    if (!(condition)) {                                                          \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
      abort();                                                                   \
    }                                                                            \
  } while (0)

typedef struct TEST_STACK_NODE {
  BOUNCE_STACK_ITEM item;
  uint32_t value;
} TEST_STACK_NODE;

typedef struct TEST_QUEUE_NODE {
  BOUNCE_QUEUE_ITEM item;
  uint32_t value;
} TEST_QUEUE_NODE;

typedef struct TEST_LIST_NODE {
  BOUNCE_LIST_ITEM item;
  uint32_t value;
} TEST_LIST_NODE;

static TEST_STACK_NODE *test_stack_node_from_item(BOUNCE_STACK_ITEM *item) {
  return (TEST_STACK_NODE *)((char *)item - offsetof(TEST_STACK_NODE, item));
}

static TEST_QUEUE_NODE *test_queue_node_from_item(BOUNCE_QUEUE_ITEM *item) {
  return (TEST_QUEUE_NODE *)((char *)item - offsetof(TEST_QUEUE_NODE, item));
}

static TEST_LIST_NODE *test_list_node_from_item(BOUNCE_LIST_ITEM *item) {
  return (TEST_LIST_NODE *)((char *)item - offsetof(TEST_LIST_NODE, item));
}

static void test_stack_basic(void) {
  BOUNCE_STACK stack;
  TEST_STACK_NODE nodes[3];

  bounce_stack_init(&stack);

  for (uint32_t index = 0; index < 3u; index++) {
    nodes[index].value = index + 1u;
    bounce_stack_push(&stack, &nodes[index].item);
  }

  for (uint32_t expected = 3u; expected >= 1u; expected--) {
    BOUNCE_STACK_ITEM *item = bounce_stack_pop(&stack);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT(test_stack_node_from_item(item)->value == expected);
    if (expected == 1u) {
      break;
    }
  }

  TEST_ASSERT(bounce_stack_pop(&stack) == NULL);
}

static void test_queue_basic(void) {
  BOUNCE_QUEUE queue;
  TEST_QUEUE_NODE nodes[3];

  bounce_queue_init(&queue);

  for (uint32_t index = 0; index < 3u; index++) {
    nodes[index].value = index + 1u;
    bounce_queue_enqueue(&queue, &nodes[index].item);
  }

  for (uint32_t expected = 1u; expected <= 3u; expected++) {
    BOUNCE_QUEUE_ITEM *item = bounce_queue_dequeue(&queue);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT(test_queue_node_from_item(item)->value == expected);
  }

  TEST_ASSERT(bounce_queue_dequeue(&queue) == NULL);
}

static void test_dynamic_block_list_basic(void) {
  BOUNCE_DYNAMIC_BLOCK_LIST list;
  BOUNCE_DYNAMIC_BLOCK *first_block;
  BOUNCE_DYNAMIC_BLOCK *second_block;
  uint32_t *first_values;
  const uint32_t *second_values;

  bounce_dynamic_block_list_init(&list);
  TEST_ASSERT(list.head == NULL);

  first_block = bounce_dynamic_block_allocate(sizeof(uint32_t), 2u);
  TEST_ASSERT(first_block != NULL);
  TEST_ASSERT(first_block->item_count == 2u);
  first_values = (uint32_t *)bounce_dynamic_block_items(first_block);
  first_values[0] = 11u;
  first_values[1] = 12u;
  bounce_dynamic_block_list_prepend(&list, first_block);

  second_block = bounce_dynamic_block_allocate(sizeof(uint32_t), 3u);
  TEST_ASSERT(second_block != NULL);
  ((uint32_t *)bounce_dynamic_block_items(second_block))[0] = 21u;
  ((uint32_t *)bounce_dynamic_block_items(second_block))[1] = 22u;
  ((uint32_t *)bounce_dynamic_block_items(second_block))[2] = 23u;
  bounce_dynamic_block_list_prepend(&list, second_block);
  TEST_ASSERT(list.head == second_block);
  TEST_ASSERT(second_block->next == first_block);
  second_values = (const uint32_t *)bounce_dynamic_block_const_items(second_block);

  TEST_ASSERT(first_values[0] == 11u);
  TEST_ASSERT(first_values[1] == 12u);
  TEST_ASSERT(second_values[0] == 21u);
  TEST_ASSERT(second_values[1] == 22u);
  TEST_ASSERT(second_values[2] == 23u);

  bounce_dynamic_block_list_free_all(&list);
  TEST_ASSERT(list.head == NULL);
}

static void test_list_basic(void) {
  BOUNCE_LIST list;
  TEST_LIST_NODE nodes[3];

  bounce_list_init(&list);

  for (uint32_t index = 0; index < 3u; index++) {
    bounce_list_item_init(&nodes[index].item);
    nodes[index].value = index + 1u;
    bounce_list_insert_tail(&list, &nodes[index].item);
  }

  for (uint32_t expected = 1u; expected <= 3u; expected++) {
    BOUNCE_LIST_ITEM *item = bounce_list_pop_head(&list);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT(test_list_node_from_item(item)->value == expected);
  }

  TEST_ASSERT(bounce_list_pop_head(&list) == NULL);
  TEST_ASSERT(bounce_list_pop_tail(&list) == NULL);
}

static void test_list_remove_integrity(void) {
  BOUNCE_LIST list;
  TEST_LIST_NODE nodes[4];

  bounce_list_init(&list);

  for (uint32_t index = 0; index < 4u; index++) {
    bounce_list_item_init(&nodes[index].item);
    nodes[index].value = index + 1u;
    bounce_list_insert_tail(&list, &nodes[index].item);
  }

  bounce_list_remove(&list, &nodes[1].item);
  bounce_list_remove(&list, &nodes[0].item);
  bounce_list_remove(&list, &nodes[3].item);

  BOUNCE_LIST_ITEM *item = bounce_list_pop_head(&list);
  TEST_ASSERT(item != NULL);
  TEST_ASSERT(test_list_node_from_item(item)->value == 3u);
  TEST_ASSERT(bounce_list_pop_head(&list) == NULL);
}

static void test_list_take_all(void) {
  BOUNCE_LIST source;
  BOUNCE_LIST destination;
  TEST_LIST_NODE nodes[3];

  bounce_list_init(&source);
  bounce_list_init(&destination);

  for (uint32_t index = 0; index < 3u; index++) {
    bounce_list_item_init(&nodes[index].item);
    nodes[index].value = index + 1u;
    bounce_list_insert_tail(&source, &nodes[index].item);
  }

  bounce_list_take_all(&destination, &source);
  TEST_ASSERT(bounce_list_pop_head(&source) == NULL);

  for (uint32_t expected = 1u; expected <= 3u; expected++) {
    BOUNCE_LIST_ITEM *item = bounce_list_pop_head(&destination);
    TEST_ASSERT(item != NULL);
    TEST_ASSERT(test_list_node_from_item(item)->value == expected);
  }

  TEST_ASSERT(bounce_list_pop_head(&destination) == NULL);
}

int main(void) {
  test_stack_basic();
  test_list_basic();
  test_list_remove_integrity();
  test_list_take_all();
  test_queue_basic();
  test_dynamic_block_list_basic();
  puts("utils tests passed");
  return 0;
}
