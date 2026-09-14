#include "gameplay_input_test.h"

#include "gameplay/vkr_gameplay_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_gameplay_input_order(void) {
  VkrGameplayInput input = {0};
  const VkrGameplayCommand commands[] = {
      {.tick = 1,
       .sequence = 1,
       .action = VKR_GAMEPLAY_FIRE,
       .pressed = true_v},
      {.tick = 1,
       .sequence = 2,
       .action = VKR_GAMEPLAY_FIRE,
       .pressed = false_v},
      {.tick = 1,
       .sequence = 3,
       .action = VKR_GAMEPLAY_FIRE,
       .pressed = true_v},
      {.tick = 3, .sequence = 4, .action = VKR_GAMEPLAY_LOOK, .x = 2, .y = -3},
      {.tick = 3,
       .sequence = 5,
       .action = VKR_GAMEPLAY_FIRE,
       .pressed = false_v},
  };
  for (uint32_t i = 0; i < ArrayCount(commands); ++i) {
    assert(vkr_gameplay_input_push(&input, commands[i]));
  }
  // A frame with zero simulation ticks does not consume queued transitions.
  assert(input.count == 5 && input.consumed_tick == 0);
  uint32_t command_index = 0;
  bool8_t held = false_v;
  uint32_t presses = 0;
  for (uint64_t tick = 1; tick <= 3; ++tick) {
    VkrGameplayCommand command;
    while (vkr_gameplay_input_next(&input, tick, &command)) {
      const VkrGameplayCommand *expected = &commands[command_index++];
      assert(command.tick == expected->tick);
      assert(command.sequence == expected->sequence);
      assert(command.action == expected->action);
      assert(command.pressed == expected->pressed);
      assert(command.x == expected->x && command.y == expected->y);
      if (command.action == VKR_GAMEPLAY_FIRE) {
        if (command.pressed && !held) {
          presses++;
        }
        held = command.pressed;
      }
    }
    assert(!input.faulted);
    assert(vkr_gameplay_input_finish_tick(&input, tick));
    if (tick < 3) {
      assert(held && presses == 2 && input.count == 2);
    }
  }
  assert(command_index == 5 && !held && presses == 2 && input.count == 0);

  // Repeated admission/draining crosses the physical ring boundary.
  for (uint64_t tick = 4; tick < VKR_GAMEPLAY_INPUT_CAPACITY + 8; ++tick) {
    VkrGameplayCommand command = {.tick = tick,
                                  .sequence = tick + 2,
                                  .action = VKR_GAMEPLAY_JUMP,
                                  .pressed = true_v};
    assert(vkr_gameplay_input_push(&input, command));
    VkrGameplayCommand result;
    assert(vkr_gameplay_input_next(&input, tick, &result));
    assert(result.tick == tick && result.sequence == tick + 2);
    assert(vkr_gameplay_input_finish_tick(&input, tick));
  }
}

static void test_gameplay_input_look_coalescing(void) {
  VkrGameplayInput input = {0};
  assert(!vkr_gameplay_input_error(&input));
  for (uint64_t sequence = 1; sequence <= 1024; ++sequence) {
    assert(vkr_gameplay_input_push(
        &input, (VkrGameplayCommand){.tick = 1,
                                     .sequence = sequence,
                                     .action = VKR_GAMEPLAY_LOOK,
                                     .x = (float32_t)sequence,
                                     .y = -0.25f}));
  }
  assert(input.count == 1 && !input.faulted);
  assert(vkr_gameplay_input_push(
      &input, (VkrGameplayCommand){.tick = 1,
                                   .sequence = 1025,
                                   .action = VKR_GAMEPLAY_FIRE,
                                   .pressed = true_v}));
  assert(vkr_gameplay_input_push(
      &input,
      (VkrGameplayCommand){
          .tick = 1, .sequence = 1026, .action = VKR_GAMEPLAY_LOOK, .x = 9}));
  assert(vkr_gameplay_input_push(
      &input,
      (VkrGameplayCommand){
          .tick = 1, .sequence = 1027, .action = VKR_GAMEPLAY_LOOK, .x = 10}));
  assert(vkr_gameplay_input_push(
      &input, (VkrGameplayCommand){.tick = 1,
                                   .sequence = 1028,
                                   .action = VKR_GAMEPLAY_FIRE,
                                   .pressed = false_v}));
  assert(vkr_gameplay_input_push(
      &input,
      (VkrGameplayCommand){
          .tick = 2, .sequence = 1029, .action = VKR_GAMEPLAY_LOOK, .x = 11}));
  assert(vkr_gameplay_input_push(
      &input,
      (VkrGameplayCommand){
          .tick = 3, .sequence = 1030, .action = VKR_GAMEPLAY_LOOK, .x = 12}));
  VkrGameplayCommand command;
  assert(vkr_gameplay_input_next(&input, 1, &command));
  assert(command.action == VKR_GAMEPLAY_LOOK && command.sequence == 1024 &&
         command.x == 1024 && command.y == -0.25f);
  assert(vkr_gameplay_input_next(&input, 1, &command));
  assert(command.action == VKR_GAMEPLAY_FIRE && command.pressed);
  assert(vkr_gameplay_input_next(&input, 1, &command));
  assert(command.action == VKR_GAMEPLAY_LOOK && command.sequence == 1027 &&
         command.x == 10);
  assert(vkr_gameplay_input_next(&input, 1, &command));
  assert(command.action == VKR_GAMEPLAY_FIRE && !command.pressed);
  assert(!vkr_gameplay_input_next(&input, 1, &command));
  assert(vkr_gameplay_input_finish_tick(&input, 1));
  assert(vkr_gameplay_input_next(&input, 2, &command));
  assert(command.action == VKR_GAMEPLAY_LOOK && command.x == 11);
  assert(vkr_gameplay_input_finish_tick(&input, 2));
  assert(vkr_gameplay_input_next(&input, 3, &command));
  assert(command.action == VKR_GAMEPLAY_LOOK && command.x == 12);
  assert(vkr_gameplay_input_finish_tick(&input, 3));

  // A full queue may replace its trailing look, including when its physical
  // tail wraps, but must still reject a discrete release without a free slot.
  input = (VkrGameplayInput){0};
  for (uint64_t tick = 1; tick <= 3; ++tick) {
    assert(vkr_gameplay_input_push(
        &input, (VkrGameplayCommand){.tick = tick,
                                     .sequence = tick,
                                     .action = VKR_GAMEPLAY_LOOK}));
    assert(vkr_gameplay_input_next(&input, tick, &command));
    assert(vkr_gameplay_input_finish_tick(&input, tick));
  }
  for (uint64_t i = 0; i < VKR_GAMEPLAY_INPUT_CAPACITY; ++i) {
    assert(vkr_gameplay_input_push(
        &input, (VkrGameplayCommand){.tick = i + 4,
                                     .sequence = i + 4,
                                     .action = VKR_GAMEPLAY_LOOK}));
  }
  const uint64_t last_tick = VKR_GAMEPLAY_INPUT_CAPACITY + 3;
  assert(vkr_gameplay_input_push(
      &input, (VkrGameplayCommand){.tick = last_tick,
                                   .sequence = last_tick + 1,
                                   .action = VKR_GAMEPLAY_LOOK,
                                   .x = 7}));
  assert(input.count == VKR_GAMEPLAY_INPUT_CAPACITY && !input.faulted);
  assert(!vkr_gameplay_input_push(
      &input, (VkrGameplayCommand){.tick = last_tick,
                                   .sequence = last_tick + 2,
                                   .action = VKR_GAMEPLAY_FIRE,
                                   .pressed = false_v}));
  assert(input.error == VKR_GAMEPLAY_INPUT_ERROR_CAPACITY);
  assert(vkr_gameplay_input_error(&input));
}

static void test_gameplay_input_faults(void) {
  const VkrGameplayCommand valid = {
      .tick = 1, .sequence = 1, .action = VKR_GAMEPLAY_FIRE, .pressed = true_v};
  VkrGameplayInput input = {0};
  assert(vkr_gameplay_input_push(&input, valid));
  assert(!vkr_gameplay_input_finish_tick(&input, 1));
  assert(input.faulted && input.count == 1 && input.consumed_tick == 0);
  assert(input.error == VKR_GAMEPLAY_INPUT_ERROR_TICK);
  VkrGameplayCommand output = {.sequence = 1234};
  assert(!vkr_gameplay_input_next(&input, 1, &output));
  assert(output.sequence == 1234);

  input = (VkrGameplayInput){0};
  for (uint32_t i = 0; i < VKR_GAMEPLAY_INPUT_CAPACITY; ++i) {
    VkrGameplayCommand command = valid;
    command.sequence = i + 1;
    assert(vkr_gameplay_input_push(&input, command));
  }
  VkrGameplayCommand release = valid;
  release.sequence = VKR_GAMEPLAY_INPUT_CAPACITY + 1;
  release.pressed = false_v;
  assert(!vkr_gameplay_input_push(&input, release));
  assert(input.faulted && input.count == VKR_GAMEPLAY_INPUT_CAPACITY);
  assert(!vkr_gameplay_input_next(&input, 1, &output));
  assert(!vkr_gameplay_input_finish_tick(&input, 1));

  const VkrGameplayCommand invalid[] = {
      {.tick = 0, .sequence = 1},
      {.tick = 1, .sequence = 0},
      {.tick = 1, .sequence = 1, .action = VKR_GAMEPLAY_ACTION_COUNT},
      {.tick = 1, .sequence = 1, .action = (VkrGameplayAction)-1},
      {.tick = 1, .sequence = 1, .x = NAN},
      {.tick = 1, .sequence = 1, .y = INFINITY},
  };
  for (uint32_t i = 0; i < ArrayCount(invalid); ++i) {
    input = (VkrGameplayInput){0};
    assert(!vkr_gameplay_input_push(&input, invalid[i]));
    assert(input.faulted && input.count == 0);
  }
  input = (VkrGameplayInput){0};
  assert(vkr_gameplay_input_push(&input, valid));
  assert(!vkr_gameplay_input_push(&input, valid)); // Duplicate sequence.
  assert(input.faulted && input.count == 1);
  input = (VkrGameplayInput){0};
  VkrGameplayCommand future = valid;
  future.tick = 2;
  assert(vkr_gameplay_input_push(&input, future));
  release.sequence = 2;
  assert(!vkr_gameplay_input_push(&input, release)); // Reordered tick.
  assert(input.faulted && input.count == 1);
  input = (VkrGameplayInput){0};
  assert(vkr_gameplay_input_finish_tick(&input, 1));
  assert(!vkr_gameplay_input_push(&input, valid)); // Late event.
  assert(input.faulted && input.error == VKR_GAMEPLAY_INPUT_ERROR_LATE);
  input = (VkrGameplayInput){0};
  assert(!vkr_gameplay_input_next(&input, 2, &output)); // Skipped tick.
  assert(input.faulted);
  input = (VkrGameplayInput){0};
  assert(vkr_gameplay_input_finish_tick(&input, 1));
  assert(!vkr_gameplay_input_finish_tick(&input, 1)); // Duplicate tick.
  assert(input.faulted);
}

static void test_gameplay_input_quantization(void) {
  uint64_t tick = 999;
  assert(vkr_gameplay_input_tick(0, &tick) && tick == 1);
  const uint64_t boundaries[] = {1, 23, 60};
  for (uint32_t i = 0; i < ArrayCount(boundaries); ++i) {
    const float64_t boundary = (float64_t)boundaries[i] / 60.0;
    assert(vkr_gameplay_input_tick(nextafter(boundary, 0), &tick));
    assert(tick == boundaries[i]);
    assert(vkr_gameplay_input_tick(boundary, &tick));
    assert(tick == boundaries[i] + 1);
    assert(vkr_gameplay_input_tick(nextafter(boundary, INFINITY), &tick));
    assert(tick == boundaries[i] + 1);
  }
  const float64_t invalid[] = {-1, NAN, INFINITY, 1.0e12};
  for (uint32_t i = 0; i < ArrayCount(invalid); ++i) {
    tick = 999;
    assert(!vkr_gameplay_input_tick(invalid[i], &tick));
    assert(tick == 999);
  }
  assert(!vkr_gameplay_input_tick(0, NULL));
}

bool32_t run_gameplay_input_tests(void) {
  test_gameplay_input_order();
  test_gameplay_input_look_coalescing();
  test_gameplay_input_faults();
  test_gameplay_input_quantization();
  printf("Gameplay input tests passed\n");
  return true_v;
}
