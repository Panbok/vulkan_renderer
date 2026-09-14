#pragma once
#include "defines.h"

#define VKR_GAMEPLAY_INPUT_CAPACITY 256u

typedef enum VkrGameplayAction {
  VKR_GAMEPLAY_FORWARD,
  VKR_GAMEPLAY_BACKWARD,
  VKR_GAMEPLAY_LEFT,
  VKR_GAMEPLAY_RIGHT,
  VKR_GAMEPLAY_FIRE,
  VKR_GAMEPLAY_RELOAD,
  VKR_GAMEPLAY_JUMP,
  VKR_GAMEPLAY_CAMERA,
  VKR_GAMEPLAY_LOOK,
  VKR_GAMEPLAY_CROUCH,
  VKR_GAMEPLAY_ACTION_COUNT,
} VkrGameplayAction;

/* Serializable field values, not a wire-format struct. No pointers or wall
 * timestamps reach simulation. Sequence breaks ties inside one tick. */
typedef struct VkrGameplayCommand {
  uint64_t tick;
  uint64_t sequence;
  VkrGameplayAction action;
  bool8_t pressed;
  float32_t x;
  float32_t y;
} VkrGameplayCommand;

typedef enum VkrGameplayInputError {
  VKR_GAMEPLAY_INPUT_ERROR_NONE,
  VKR_GAMEPLAY_INPUT_ERROR_COMMAND,
  VKR_GAMEPLAY_INPUT_ERROR_LATE,
  VKR_GAMEPLAY_INPUT_ERROR_ORDER,
  VKR_GAMEPLAY_INPUT_ERROR_CAPACITY,
  VKR_GAMEPLAY_INPUT_ERROR_TICK,
  VKR_GAMEPLAY_INPUT_ERROR_TIME,
} VkrGameplayInputError;

typedef struct VkrGameplayInput {
  VkrGameplayCommand commands[VKR_GAMEPLAY_INPUT_CAPACITY];
  uint32_t head;
  uint32_t count;
  uint64_t last_sequence;
  uint64_t last_tick;
  uint64_t consumed_tick;
  bool8_t faulted;
  VkrGameplayInputError error;
} VkrGameplayInput;

/* Ordered admission. Invalid, late, reordered or capacity-exhausted input
 * faults the stream instead of silently losing releases/actions. Reset at a
 * paused/session boundary by assigning zero, clearing the consumer's held state
 * too. Adjacent LOOK commands for the same tick coalesce to the latest absolute
 * aim (x/y); discrete transitions and look on either side retain their
 * ordering. The producer and consumer share one owning thread. No allocations.
 */
bool8_t vkr_gameplay_input_push(VkrGameplayInput *input,
                                VkrGameplayCommand command);
/* Tick boundaries are monotonically consecutive (first tick=1); commands for
 * future ticks remain queued. Call next repeatedly, then finish_tick once. */
bool8_t vkr_gameplay_input_next(VkrGameplayInput *input, uint64_t tick,
                                VkrGameplayCommand *command);
bool8_t vkr_gameplay_input_finish_tick(VkrGameplayInput *input, uint64_t tick);
/* Event time in admitted simulation seconds maps to the interval containing
 * it: [0,dt) -> tick 1. Exact boundaries belong to the following tick. */
bool8_t vkr_gameplay_input_tick(float64_t elapsed, uint64_t *tick);

/* Static diagnostic text; NULL means no fault. */
const char *vkr_gameplay_input_error(const VkrGameplayInput *input);
