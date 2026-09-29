#include "gameplay_input_test.h"

#include "fps_input.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_gameplay_input_order(void) {
  FpsInput input = {0};
  const FpsCommand commands[] = {
      {.tick = 1, .sequence = 1, .action = FPS_ACTION_FIRE, .pressed = true_v},
      {.tick = 1, .sequence = 2, .action = FPS_ACTION_FIRE, .pressed = false_v},
      {.tick = 1, .sequence = 3, .action = FPS_ACTION_FIRE, .pressed = true_v},
      {.tick = 3, .sequence = 4, .action = FPS_ACTION_LOOK, .x = 2, .y = -3},
      {.tick = 3, .sequence = 5, .action = FPS_ACTION_FIRE, .pressed = false_v},
  };
  for (uint32_t i = 0; i < ArrayCount(commands); ++i) {
    assert(fps_input_push(&input, commands[i]));
  }
  // A frame with zero simulation ticks does not consume queued transitions.
  assert(input.count == 5 && input.consumed_tick == 0);
  uint32_t command_index = 0;
  bool8_t held = false_v;
  uint32_t presses = 0;
  for (uint64_t tick = 1; tick <= 3; ++tick) {
    FpsCommand command;
    while (fps_input_next(&input, tick, &command)) {
      const FpsCommand *expected = &commands[command_index++];
      assert(command.tick == expected->tick);
      assert(command.sequence == expected->sequence);
      assert(command.action == expected->action);
      assert(command.pressed == expected->pressed);
      assert(command.x == expected->x && command.y == expected->y);
      if (command.action == FPS_ACTION_FIRE) {
        if (command.pressed && !held) {
          presses++;
        }
        held = command.pressed;
      }
    }
    assert(!input.faulted);
    assert(fps_input_finish_tick(&input, tick));
    if (tick < 3) {
      assert(held && presses == 2 && input.count == 2);
    }
  }
  assert(command_index == 5 && !held && presses == 2 && input.count == 0);

  // Repeated admission/draining crosses the physical ring boundary.
  for (uint64_t tick = 4; tick < FPS_INPUT_CAPACITY + 8; ++tick) {
    FpsCommand command = {.tick = tick,
                          .sequence = tick + 2,
                          .action = FPS_ACTION_JUMP,
                          .pressed = true_v};
    assert(fps_input_push(&input, command));
    FpsCommand result;
    assert(fps_input_next(&input, tick, &result));
    assert(result.tick == tick && result.sequence == tick + 2);
    assert(fps_input_finish_tick(&input, tick));
  }
}

static void test_gameplay_input_look_coalescing(void) {
  FpsInput input = {0};
  assert(!fps_input_error(&input));
  for (uint64_t sequence = 1; sequence <= 1024; ++sequence) {
    assert(fps_input_push(&input, (FpsCommand){.tick = 1,
                                               .sequence = sequence,
                                               .action = FPS_ACTION_LOOK,
                                               .x = (float32_t)sequence,
                                               .y = -0.25f}));
  }
  assert(input.count == 1 && !input.faulted);
  assert(fps_input_push(&input, (FpsCommand){.tick = 1,
                                             .sequence = 1025,
                                             .action = FPS_ACTION_FIRE,
                                             .pressed = true_v}));
  assert(fps_input_push(&input, (FpsCommand){.tick = 1,
                                             .sequence = 1026,
                                             .action = FPS_ACTION_LOOK,
                                             .x = 9}));
  assert(fps_input_push(&input, (FpsCommand){.tick = 1,
                                             .sequence = 1027,
                                             .action = FPS_ACTION_LOOK,
                                             .x = 10}));
  assert(fps_input_push(&input, (FpsCommand){.tick = 1,
                                             .sequence = 1028,
                                             .action = FPS_ACTION_FIRE,
                                             .pressed = false_v}));
  assert(fps_input_push(&input, (FpsCommand){.tick = 2,
                                             .sequence = 1029,
                                             .action = FPS_ACTION_LOOK,
                                             .x = 11}));
  assert(fps_input_push(&input, (FpsCommand){.tick = 3,
                                             .sequence = 1030,
                                             .action = FPS_ACTION_LOOK,
                                             .x = 12}));
  FpsCommand command;
  assert(fps_input_next(&input, 1, &command));
  assert(command.action == FPS_ACTION_LOOK && command.sequence == 1024 &&
         command.x == 1024 && command.y == -0.25f);
  assert(fps_input_next(&input, 1, &command));
  assert(command.action == FPS_ACTION_FIRE && command.pressed);
  assert(fps_input_next(&input, 1, &command));
  assert(command.action == FPS_ACTION_LOOK && command.sequence == 1027 &&
         command.x == 10);
  assert(fps_input_next(&input, 1, &command));
  assert(command.action == FPS_ACTION_FIRE && !command.pressed);
  assert(!fps_input_next(&input, 1, &command));
  assert(fps_input_finish_tick(&input, 1));
  assert(fps_input_next(&input, 2, &command));
  assert(command.action == FPS_ACTION_LOOK && command.x == 11);
  assert(fps_input_finish_tick(&input, 2));
  assert(fps_input_next(&input, 3, &command));
  assert(command.action == FPS_ACTION_LOOK && command.x == 12);
  assert(fps_input_finish_tick(&input, 3));

  // A full queue may replace its trailing look, including when its physical
  // tail wraps, but must still reject a discrete release without a free slot.
  input = (FpsInput){0};
  for (uint64_t tick = 1; tick <= 3; ++tick) {
    assert(fps_input_push(&input, (FpsCommand){.tick = tick,
                                               .sequence = tick,
                                               .action = FPS_ACTION_LOOK}));
    assert(fps_input_next(&input, tick, &command));
    assert(fps_input_finish_tick(&input, tick));
  }
  for (uint64_t i = 0; i < FPS_INPUT_CAPACITY; ++i) {
    assert(fps_input_push(&input, (FpsCommand){.tick = i + 4,
                                               .sequence = i + 4,
                                               .action = FPS_ACTION_LOOK}));
  }
  const uint64_t last_tick = FPS_INPUT_CAPACITY + 3;
  assert(fps_input_push(&input, (FpsCommand){.tick = last_tick,
                                             .sequence = last_tick + 1,
                                             .action = FPS_ACTION_LOOK,
                                             .x = 7}));
  assert(input.count == FPS_INPUT_CAPACITY && !input.faulted);
  assert(!fps_input_push(&input, (FpsCommand){.tick = last_tick,
                                              .sequence = last_tick + 2,
                                              .action = FPS_ACTION_FIRE,
                                              .pressed = false_v}));
  assert(input.error == FPS_INPUT_ERROR_CAPACITY);
  assert(fps_input_error(&input));
}

static void test_gameplay_input_faults(void) {
  const FpsCommand valid = {
      .tick = 1, .sequence = 1, .action = FPS_ACTION_FIRE, .pressed = true_v};
  FpsInput input = {0};
  assert(fps_input_push(&input, valid));
  assert(!fps_input_finish_tick(&input, 1));
  assert(input.faulted && input.count == 1 && input.consumed_tick == 0);
  assert(input.error == FPS_INPUT_ERROR_TICK);
  FpsCommand output = {.sequence = 1234};
  assert(!fps_input_next(&input, 1, &output));
  assert(output.sequence == 1234);

  input = (FpsInput){0};
  for (uint32_t i = 0; i < FPS_INPUT_CAPACITY; ++i) {
    FpsCommand command = valid;
    command.sequence = i + 1;
    assert(fps_input_push(&input, command));
  }
  FpsCommand release = valid;
  release.sequence = FPS_INPUT_CAPACITY + 1;
  release.pressed = false_v;
  assert(!fps_input_push(&input, release));
  assert(input.faulted && input.count == FPS_INPUT_CAPACITY);
  assert(!fps_input_next(&input, 1, &output));
  assert(!fps_input_finish_tick(&input, 1));

  const FpsCommand invalid[] = {
      {.tick = 0, .sequence = 1},
      {.tick = 1, .sequence = 0},
      {.tick = 1, .sequence = 1, .action = FPS_ACTION_COUNT},
      {.tick = 1, .sequence = 1, .action = (FpsAction)-1},
      {.tick = 1, .sequence = 1, .x = NAN},
      {.tick = 1, .sequence = 1, .y = INFINITY},
  };
  for (uint32_t i = 0; i < ArrayCount(invalid); ++i) {
    input = (FpsInput){0};
    assert(!fps_input_push(&input, invalid[i]));
    assert(input.faulted && input.count == 0);
  }
  input = (FpsInput){0};
  assert(fps_input_push(&input, valid));
  assert(!fps_input_push(&input, valid)); // Duplicate sequence.
  assert(input.faulted && input.count == 1);
  input = (FpsInput){0};
  FpsCommand future = valid;
  future.tick = 2;
  assert(fps_input_push(&input, future));
  release.sequence = 2;
  assert(!fps_input_push(&input, release)); // Reordered tick.
  assert(input.faulted && input.count == 1);
  input = (FpsInput){0};
  assert(fps_input_finish_tick(&input, 1));
  assert(!fps_input_push(&input, valid)); // Late event.
  assert(input.faulted && input.error == FPS_INPUT_ERROR_LATE);
  input = (FpsInput){0};
  assert(!fps_input_next(&input, 2, &output)); // Skipped tick.
  assert(input.faulted);
  input = (FpsInput){0};
  assert(fps_input_finish_tick(&input, 1));
  assert(!fps_input_finish_tick(&input, 1)); // Duplicate tick.
  assert(input.faulted);
}

static void test_gameplay_input_quantization(void) {
  uint64_t tick = 999;
  assert(fps_input_tick(0, &tick) && tick == 1);
  const uint64_t boundaries[] = {1, 23, 60};
  for (uint32_t i = 0; i < ArrayCount(boundaries); ++i) {
    const float64_t boundary = (float64_t)boundaries[i] / 60.0;
    assert(fps_input_tick(nextafter(boundary, 0), &tick));
    assert(tick == boundaries[i]);
    assert(fps_input_tick(boundary, &tick));
    assert(tick == boundaries[i] + 1);
    assert(fps_input_tick(nextafter(boundary, INFINITY), &tick));
    assert(tick == boundaries[i] + 1);
  }
  const float64_t invalid[] = {-1, NAN, INFINITY, 1.0e12};
  for (uint32_t i = 0; i < ArrayCount(invalid); ++i) {
    tick = 999;
    assert(!fps_input_tick(invalid[i], &tick));
    assert(tick == 999);
  }
  assert(!fps_input_tick(0, NULL));
}

bool32_t run_gameplay_input_tests(void) {
  test_gameplay_input_order();
  test_gameplay_input_look_coalescing();
  test_gameplay_input_faults();
  test_gameplay_input_quantization();
  printf("Gameplay input tests passed\n");
  return true_v;
}
