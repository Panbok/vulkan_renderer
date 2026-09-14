#include "gameplay/vkr_gameplay_input.h"
#include <math.h>

bool8_t vkr_gameplay_input_tick(float64_t elapsed, uint64_t *tick) {
  if (!tick || !isfinite(elapsed) || elapsed < 0.0 || elapsed >= 1.0e12) {
    return false_v;
  }
  uint64_t interval = (uint64_t)floor(elapsed * 60.0);
  /* Multiplication can round a predecessor onto a boundary. Compare against
   * the representable boundary itself before assigning its interval. */
  if (interval && elapsed < (float64_t)interval / 60.0) {
    interval--;
  } else if (elapsed >= (float64_t)(interval + 1) / 60.0) {
    interval++;
  }
  *tick = interval + 1;
  return true_v;
}

static bool8_t gameplay_input_fail(VkrGameplayInput *input,
                                   VkrGameplayInputError error) {
  input->faulted = true_v;
  input->error = error;
  return false_v;
}

const char *vkr_gameplay_input_error(const VkrGameplayInput *input) {
  if (!input || !input->faulted) {
    return NULL;
  }
  switch (input->error) {
  case VKR_GAMEPLAY_INPUT_ERROR_COMMAND:
    return "Invalid gameplay input command";
  case VKR_GAMEPLAY_INPUT_ERROR_LATE:
    return "Gameplay input targets an already consumed tick";
  case VKR_GAMEPLAY_INPUT_ERROR_ORDER:
    return "Gameplay input tick or sequence is out of order";
  case VKR_GAMEPLAY_INPUT_ERROR_CAPACITY:
    return "Gameplay input queue capacity exhausted";
  case VKR_GAMEPLAY_INPUT_ERROR_TICK:
    return "Gameplay input tick was skipped, repeated, or left undrained";
  case VKR_GAMEPLAY_INPUT_ERROR_TIME:
    return "Gameplay input timestamp is invalid";
  default:
    return "Gameplay input stream faulted";
  }
}

bool8_t vkr_gameplay_input_push(VkrGameplayInput *input,
                                VkrGameplayCommand command) {
  if (!input || input->faulted) {
    return false_v;
  }
  if (!command.tick || !command.sequence ||
      command.action >= VKR_GAMEPLAY_ACTION_COUNT || command.action < 0 ||
      !isfinite(command.x) || !isfinite(command.y)) {
    return gameplay_input_fail(input, VKR_GAMEPLAY_INPUT_ERROR_COMMAND);
  }
  if (command.tick <= input->consumed_tick) {
    return gameplay_input_fail(input, VKR_GAMEPLAY_INPUT_ERROR_LATE);
  }
  if (command.tick < input->last_tick ||
      command.sequence <= input->last_sequence) {
    return gameplay_input_fail(input, VKR_GAMEPLAY_INPUT_ERROR_ORDER);
  }
  if (command.action == VKR_GAMEPLAY_LOOK && input->count) {
    const uint32_t tail =
        (input->head + input->count - 1) % VKR_GAMEPLAY_INPUT_CAPACITY;
    if (input->commands[tail].action == VKR_GAMEPLAY_LOOK &&
        input->commands[tail].tick == command.tick) {
      input->commands[tail] = command;
      input->last_sequence = command.sequence;
      return true_v;
    }
  }
  if (input->count == VKR_GAMEPLAY_INPUT_CAPACITY) {
    return gameplay_input_fail(input, VKR_GAMEPLAY_INPUT_ERROR_CAPACITY);
  }
  input->commands[(input->head + input->count) % VKR_GAMEPLAY_INPUT_CAPACITY] =
      command;
  input->count++;
  input->last_tick = command.tick;
  input->last_sequence = command.sequence;
  return true_v;
}

bool8_t vkr_gameplay_input_next(VkrGameplayInput *input, uint64_t tick,
                                VkrGameplayCommand *command) {
  if (!input || !command || input->faulted) {
    return false_v;
  }
  if (input->consumed_tick == UINT64_MAX || tick != input->consumed_tick + 1) {
    return gameplay_input_fail(input, VKR_GAMEPLAY_INPUT_ERROR_TICK);
  }
  if (!input->count || input->commands[input->head].tick > tick) {
    return false_v;
  }
  *command = input->commands[input->head];
  input->head = (input->head + 1) % VKR_GAMEPLAY_INPUT_CAPACITY;
  input->count--;
  return true_v;
}

bool8_t vkr_gameplay_input_finish_tick(VkrGameplayInput *input, uint64_t tick) {
  if (!input || input->faulted) {
    return false_v;
  }
  if (input->consumed_tick == UINT64_MAX || tick != input->consumed_tick + 1 ||
      (input->count && input->commands[input->head].tick <= tick)) {
    return gameplay_input_fail(input, VKR_GAMEPLAY_INPUT_ERROR_TICK);
  }
  input->consumed_tick = tick;
  return true_v;
}
