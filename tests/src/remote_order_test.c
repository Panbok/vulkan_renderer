#include "remote_order_test.h"

#include "../../editor/src/editor_remote_order.h"

#include <assert.h>
#include <stdio.h>

/* The client's window and re-ask period (editor_remote_view.c). */
#define ORDER_TEST_WINDOW_US 33333u
#define ORDER_TEST_REFRESH_US 500000u

static VkrEditorRemoteOrderFrame order_test_frame(uint64_t number, bool8_t key,
                                                  uint64_t received_us) {
  return (VkrEditorRemoteOrderFrame){
      .number = number,
      .key_frame = key,
      .received_us = received_us,
  };
}

/* Pushes a frame the order must hold. */
static void order_test_push(VkrEditorRemoteOrder *order, uint64_t number,
                            bool8_t key, uint64_t received_us) {
  const VkrEditorRemoteOrderFrame frame =
      order_test_frame(number, key, received_us);
  assert(vkr_editor_remote_order_push(order, &frame));
}

/* The next step must be `step` with frame `number`. */
static void order_test_expect(VkrEditorRemoteOrder *order, uint64_t now_us,
                              VkrEditorRemoteOrderStep step, uint64_t number) {
  VkrEditorRemoteOrderFrame frame = {0};
  assert(vkr_editor_remote_order_next(order, now_us, &frame) == step);
  assert(frame.number == number);
}

static void order_test_expect_wait(VkrEditorRemoteOrder *order,
                                   uint64_t now_us) {
  VkrEditorRemoteOrderFrame frame = {0};
  assert(vkr_editor_remote_order_next(order, now_us, &frame) ==
         VKR_EDITOR_REMOTE_ORDER_WAIT);
}

/* A frame that overtakes the one before it waits for it inside the window,
   and both decode in order with no key frame asked. Without the window,
   every resent packet would cost a key frame. */
static void test_remote_order_reorder_window(void) {
  printf("  Running test_remote_order_reorder_window...\n");
  VkrEditorRemoteOrder order;
  vkr_editor_remote_order_init(&order, 0u, ORDER_TEST_WINDOW_US,
                               ORDER_TEST_REFRESH_US);
  order_test_push(&order, 0u, true_v, 1000u);
  order_test_expect(&order, 1000u, VKR_EDITOR_REMOTE_ORDER_DECODE, 0u);
  order_test_push(&order, 2u, false_v, 20000u);
  order_test_expect_wait(&order, 30000u);
  order_test_push(&order, 1u, false_v, 31000u);
  order_test_expect(&order, 31000u, VKR_EDITOR_REMOTE_ORDER_DECODE, 1u);
  order_test_expect(&order, 31000u, VKR_EDITOR_REMOTE_ORDER_DECODE, 2u);
  order_test_expect_wait(&order, 31000u);
  assert(!vkr_editor_remote_order_want_key(&order, 31000u));
  assert(!vkr_editor_remote_order_want_key(&order, 2000000u));
  assert(order.frames_reordered == 1u);
  assert(order.frame_gaps == 0u);
  assert(order.frames_dropped == 0u);
  printf("  test_remote_order_reorder_window PASSED\n");
}

/* A frame that never comes leaves a gap once the window passes: the
   frames after it drop, or are refused, and a key frame is asked for at
   once; the next key frame starts the stream again. */
static void test_remote_order_gap_asks_key(void) {
  printf("  Running test_remote_order_gap_asks_key...\n");
  VkrEditorRemoteOrder order;
  vkr_editor_remote_order_init(&order, 0u, ORDER_TEST_WINDOW_US,
                               ORDER_TEST_REFRESH_US);
  order_test_push(&order, 0u, true_v, 1000u);
  order_test_expect(&order, 1000u, VKR_EDITOR_REMOTE_ORDER_DECODE, 0u);
  order_test_push(&order, 2u, false_v, 20000u);
  order_test_expect_wait(&order, 20000u + ORDER_TEST_WINDOW_US - 1u);
  assert(!vkr_editor_remote_order_want_key(&order, 40000u));
  order_test_expect(&order, 20000u + ORDER_TEST_WINDOW_US,
                    VKR_EDITOR_REMOTE_ORDER_DROP, 2u);
  assert(vkr_editor_remote_order_want_key(&order, 60000u));
  assert(!vkr_editor_remote_order_want_key(&order, 61000u));
  const VkrEditorRemoteOrderFrame waiting =
      order_test_frame(3u, false_v, 62000u);
  assert(!vkr_editor_remote_order_push(&order, &waiting));
  order_test_push(&order, 4u, true_v, 80000u);
  order_test_expect(&order, 80000u, VKR_EDITOR_REMOTE_ORDER_DECODE, 4u);
  assert(!vkr_editor_remote_order_want_key(&order, 2000000u));
  assert(order.frame_gaps == 1u);
  assert(order.frames_dropped == 2u);
  printf("  test_remote_order_gap_asks_key PASSED\n");
}

/* While no key frame arrives the order asks again every refresh period,
   from the stream's start on: a lost key frame stalled the stream for good
   when the client asked only once. */
static void test_remote_order_reasks_lost_key(void) {
  printf("  Running test_remote_order_reasks_lost_key...\n");
  VkrEditorRemoteOrder order;
  const uint64_t opened = 5000000u;
  vkr_editor_remote_order_init(&order, opened, ORDER_TEST_WINDOW_US,
                               ORDER_TEST_REFRESH_US);
  assert(!vkr_editor_remote_order_want_key(&order, opened + 100000u));
  const VkrEditorRemoteOrderFrame before_key =
      order_test_frame(7u, false_v, opened + 200000u);
  assert(!vkr_editor_remote_order_push(&order, &before_key));
  assert(
      vkr_editor_remote_order_want_key(&order, opened + ORDER_TEST_REFRESH_US));
  assert(!vkr_editor_remote_order_want_key(
      &order, opened + ORDER_TEST_REFRESH_US + 400000u));
  assert(vkr_editor_remote_order_want_key(&order,
                                          opened + 2u * ORDER_TEST_REFRESH_US));
  order_test_push(&order, 9u, true_v, opened + 1100000u);
  order_test_expect(&order, opened + 1100000u, VKR_EDITOR_REMOTE_ORDER_DECODE,
                    9u);
  assert(!vkr_editor_remote_order_want_key(&order, opened + 9000000u));
  printf("  test_remote_order_reasks_lost_key PASSED\n");
}

/* Late and duplicate frames are refused; a refused decode asks a key frame
   at once; a full order gives up on the frames it holds and asks once. */
static void test_remote_order_refusals(void) {
  printf("  Running test_remote_order_refusals...\n");
  VkrEditorRemoteOrder order;
  vkr_editor_remote_order_init(&order, 0u, ORDER_TEST_WINDOW_US,
                               ORDER_TEST_REFRESH_US);
  order_test_push(&order, 0u, true_v, 0u);
  order_test_expect(&order, 0u, VKR_EDITOR_REMOTE_ORDER_DECODE, 0u);
  order_test_push(&order, 1u, false_v, 0u);
  order_test_expect(&order, 0u, VKR_EDITOR_REMOTE_ORDER_DECODE, 1u);
  const VkrEditorRemoteOrderFrame late = order_test_frame(1u, false_v, 0u);
  assert(!vkr_editor_remote_order_push(&order, &late));
  order_test_push(&order, 3u, false_v, 0u);
  const VkrEditorRemoteOrderFrame twice = order_test_frame(3u, false_v, 0u);
  assert(!vkr_editor_remote_order_push(&order, &twice));
  assert(order.frames_late == 2u);

  vkr_editor_remote_order_decode_failed(&order);
  assert(vkr_editor_remote_order_want_key(&order, 1u));
  order_test_expect(&order, 1u, VKR_EDITOR_REMOTE_ORDER_DROP, 3u);

  order_test_push(&order, 10u, true_v, 2u);
  order_test_expect(&order, 2u, VKR_EDITOR_REMOTE_ORDER_DECODE, 10u);
  for (uint64_t i = 0u; i < VKR_EDITOR_REMOTE_ORDER_CAPACITY; ++i) {
    order_test_push(&order, 12u + i, false_v, 3u);
  }
  const VkrEditorRemoteOrderFrame extra =
      order_test_frame(12u + VKR_EDITOR_REMOTE_ORDER_CAPACITY, false_v, 3u);
  assert(!vkr_editor_remote_order_push(&order, &extra));
  assert(vkr_editor_remote_order_want_key(&order, 4u));
  /* More frames refused while it waits ask nothing more before the
     refresh period. */
  assert(!vkr_editor_remote_order_push(&order, &extra));
  assert(!vkr_editor_remote_order_want_key(&order, 5u));
  for (uint64_t i = 0u; i < VKR_EDITOR_REMOTE_ORDER_CAPACITY; ++i) {
    order_test_expect(&order, 4u, VKR_EDITOR_REMOTE_ORDER_DROP, 12u + i);
  }
  order_test_expect_wait(&order, 4u);
  printf("  test_remote_order_refusals PASSED\n");
}

bool32_t run_remote_order_tests(void) {
  printf("--- Starting Remote Order Tests ---\n");
  test_remote_order_reorder_window();
  test_remote_order_gap_asks_key();
  test_remote_order_reasks_lost_key();
  test_remote_order_refusals();
  printf("--- Remote Order Tests Completed ---\n");
  return true_v;
}
