#pragma once
#include "defines.h"

#define FPS_INPUT_CAPACITY 256u

typedef enum FpsAction {
  FPS_ACTION_FORWARD,
  FPS_ACTION_BACKWARD,
  FPS_ACTION_LEFT,
  FPS_ACTION_RIGHT,
  FPS_ACTION_FIRE,
  FPS_ACTION_RELOAD,
  FPS_ACTION_JUMP,
  FPS_ACTION_CAMERA,
  FPS_ACTION_LOOK,
  FPS_ACTION_CROUCH,
  FPS_ACTION_WALK,
  FPS_ACTION_USE,
  FPS_ACTION_COUNT,
} FpsAction;

/* Serializable field values, not a wire-format struct. No pointers or wall
 * timestamps reach simulation. Sequence breaks ties inside one tick. */
typedef struct FpsCommand {
  uint64_t tick;
  uint64_t sequence;
  FpsAction action;
  bool8_t pressed;
  float32_t x;
  float32_t y;
} FpsCommand;

typedef enum FpsInputError {
  FPS_INPUT_ERROR_NONE,
  FPS_INPUT_ERROR_COMMAND,
  FPS_INPUT_ERROR_LATE,
  FPS_INPUT_ERROR_ORDER,
  FPS_INPUT_ERROR_CAPACITY,
  FPS_INPUT_ERROR_TICK,
  FPS_INPUT_ERROR_TIME,
} FpsInputError;

typedef struct FpsInput {
  FpsCommand commands[FPS_INPUT_CAPACITY];
  uint32_t head;
  uint32_t count;
  uint64_t last_sequence;
  uint64_t last_tick;
  uint64_t consumed_tick;
  bool8_t faulted;
  FpsInputError error;
} FpsInput;

/* Ordered admission. Invalid, late, reordered or capacity-exhausted input
 * faults the stream instead of silently losing releases/actions. Reset at a
 * paused/session boundary by assigning zero, clearing the consumer's held state
 * too. Adjacent LOOK commands for the same tick coalesce to the latest absolute
 * aim (x/y); discrete transitions and look on either side retain their
 * ordering. The producer and consumer share one owning thread. No allocations.
 */
bool8_t fps_input_push(FpsInput *input, FpsCommand command);
/* Tick boundaries are monotonically consecutive (first tick=1); commands for
 * future ticks remain queued. Call next repeatedly, then finish_tick once. */
bool8_t fps_input_next(FpsInput *input, uint64_t tick, FpsCommand *command);
bool8_t fps_input_finish_tick(FpsInput *input, uint64_t tick);
/* Event time in admitted simulation seconds maps to the interval containing
 * it: [0,dt) -> tick 1. Exact boundaries belong to the following tick. */
bool8_t fps_input_tick(float64_t elapsed, uint64_t *tick);

/* Static diagnostic text; NULL means no fault. */
const char *fps_input_error(const FpsInput *input);
