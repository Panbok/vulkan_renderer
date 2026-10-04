/* A sliding door, the sample of entity IO for scripts (ADR-084): its `open`,
 * `close`, `toggle`, `lock` and `unlock` inputs take connections from
 * triggers and other entities, and it fires `opened` and `closed` when it
 * comes to rest. Any root object can be a door; a brush door's collision
 * follows it once it rests. */
#include "fps_module.h"

#include <math.h>

VKR_COMPONENT_DEFINE(FpsDoor, door, "Door", FPS_DOOR_FIELDS)

/* Runtime state: where the door stands closed, and how far open it is. */
typedef struct FpsDoorState {
  VkrTRS closed;
  /* 0 closed to 1 open. */
  float32_t progress;
  bool8_t opening;
  /* The last rest it announced, so each one fires once. */
  bool8_t announced_open;
  bool8_t announced_closed;
} FpsDoorState;

VKR_OUTPUTS(door, VKR_OUTPUT(opened, "On opened", NONE)
                      VKR_OUTPUT(closed, "On closed", NONE))

static FpsDoorState *door_state(VkrCtx *ctx, VkrEntity self) {
  return vkr_state_get(ctx, self, VKR_STATE_TYPE(ctx, FpsDoorState));
}

static void door_start(VkrCtx *ctx, VkrEntity self, FpsDoor *door) {
  (void)door;
  Mat4 world;
  if (!vkr_world_matrix(ctx, self, &world)) {
    vkr_disable(ctx);
    return;
  }
  /* A root door's world matrix is its local transform. */
  const Vec3 axes[3] = {
      vec3_new(world.elements[0], world.elements[1], world.elements[2]),
      vec3_new(world.elements[4], world.elements[5], world.elements[6]),
      vec3_new(world.elements[8], world.elements[9], world.elements[10])};
  FpsDoorState state = {
      .closed = {.position = vec3_new(world.elements[12], world.elements[13],
                                      world.elements[14]),
                 .scale = vec3_new(vec3_length(axes[0]), vec3_length(axes[1]),
                                   vec3_length(axes[2]))},
      .announced_closed = true_v};
  Mat4 basis = mat4_identity();
  for (uint32_t i = 0; i < 3u; ++i) {
    const float32_t length = state.closed.scale.elements[i];
    const Vec3 axis =
        length > 0.0f ? vec3_scale(axes[i], 1.0f / length) : vec3_zero();
    basis.elements[i * 4u + 0u] = axis.x;
    basis.elements[i * 4u + 1u] = axis.y;
    basis.elements[i * 4u + 2u] = axis.z;
  }
  state.closed.rotation = vkr_quat_from_mat4(basis);
  (void)vkr_state_add(ctx, self, VKR_STATE_TYPE(ctx, FpsDoorState), &state);
}

/* Slides toward open or closed each tick and announces each rest. */
static void door_fixed_update(VkrCtx *ctx, VkrEntity self, FpsDoor *door) {
  FpsDoorState *state = door_state(ctx, self);
  if (!state) {
    return;
  }
  const float32_t target = state->opening ? 1.0f : 0.0f;
  if (state->progress != target) {
    const float32_t length = vec3_length(door->offset);
    const float32_t step =
        length > 0.0f ? door->speed * (float32_t)vkr_fixed_dt(ctx) / length
                      : 1.0f;
    state->progress = state->opening ? fminf(1.0f, state->progress + step)
                                     : fmaxf(0.0f, state->progress - step);
    VkrTRS pose = state->closed;
    pose.position =
        vec3_add(pose.position, vec3_scale(door->offset, state->progress));
    (void)vkr_set_transform(ctx, self, &pose);
  }
  if (state->progress == 1.0f && !state->announced_open) {
    state->announced_open = true_v;
    state->announced_closed = false_v;
    (void)VKR_FIRE(ctx, self, door, opened, NULL);
  } else if (state->progress == 0.0f && !state->announced_closed) {
    state->announced_closed = true_v;
    state->announced_open = false_v;
    (void)VKR_FIRE(ctx, self, door, closed, NULL);
  }
}

static void door_open(VkrCtx *ctx, VkrEntity self, FpsDoor *door,
                      const VkrIoValue *value) {
  (void)value;
  FpsDoorState *state = door_state(ctx, self);
  /* A locked door refuses the request; the input decides, not the
     connection. */
  if (state && !door->locked) {
    state->opening = true_v;
  }
}

static void door_close(VkrCtx *ctx, VkrEntity self, FpsDoor *door,
                       const VkrIoValue *value) {
  (void)door;
  (void)value;
  FpsDoorState *state = door_state(ctx, self);
  if (state) {
    state->opening = false_v;
  }
}

static void door_toggle(VkrCtx *ctx, VkrEntity self, FpsDoor *door,
                        const VkrIoValue *value) {
  FpsDoorState *state = door_state(ctx, self);
  if (state && state->opening) {
    door_close(ctx, self, door, value);
  } else {
    door_open(ctx, self, door, value);
  }
}

static void door_lock(VkrCtx *ctx, VkrEntity self, FpsDoor *door,
                      const VkrIoValue *value) {
  (void)value;
  FpsDoor locked = *door;
  locked.locked = true_v;
  (void)vkr_component_set(ctx, self, door_type(), &locked);
}

static void door_unlock(VkrCtx *ctx, VkrEntity self, FpsDoor *door,
                        const VkrIoValue *value) {
  (void)value;
  FpsDoor unlocked = *door;
  unlocked.locked = false_v;
  (void)vkr_component_set(ctx, self, door_type(), &unlocked);
}

VKR_INPUTS(door, VKR_INPUT(open, "Open", NONE, door_open)
                     VKR_INPUT(close, "Close", NONE, door_close)
                         VKR_INPUT(toggle, "Toggle", NONE, door_toggle)
                             VKR_INPUT(lock, "Lock", NONE, door_lock)
                                 VKR_INPUT(unlock, "Unlock", NONE, door_unlock))

VKR_BEHAVIOR(door, .start = door_start, .fixed_update = door_fixed_update,
             .outputs = door_outputs, .inputs = door_inputs)

const VkrBehaviorDesc *fps_door_behavior(void) { return door_behavior(); }
