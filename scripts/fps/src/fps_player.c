#include "fps_player.h"
#include "fps_module.h"
#include <math.h>
#include <stdio.h>

#define PLAYER_PITCH_LIMIT 1.45f
#define PLAYER_LOOK_SCALE 0.0025f
/* How far the use key reaches from the eye, in metres, and how many parents
   up from the hit brush it looks for a button or mover. */
#define PLAYER_USE_REACH 2.0f
#define PLAYER_USE_DEPTH 8u
/* How far ahead of the chest a ladder is found, and how far down the view
   looks before forward climbs down. */
#define PLAYER_LADDER_REACH 0.6f
#define PLAYER_LADDER_DOWN_PITCH -0.5f
/* Speed a jump pushes off a ladder with, away and up. */
#define PLAYER_LADDER_PUSH 3.0f

FpsPlayerSettings fps_player_settings_default(void) {
  FpsPlayerSettings settings;
  fps_player_type()->defaults(&settings);
  return settings;
}

FpsPlayerState *fps_player_state(VkrCtx *ctx, const FpsPlayer *player) {
  return player->attached
             ? vkr_state_get(ctx, player->entity, player->state_type)
             : NULL;
}

static bool8_t player_fail(FpsPlayer *player, const char *message) {
  if (message != player->error) {
    snprintf(player->error, sizeof(player->error), "%s",
             message ? message : "Unknown player failure");
  }
  return false_v;
}

static bool8_t player_prepare_animation(VkrCtx *ctx, FpsPlayer *player,
                                        const char **error) {
  const uint64_t animation_id =
      vkr_entity_valid(player->visual) ? vkr_anim_id(ctx, player->visual) : 0u;
  player->animation = (FpsPlayerAnimation){0};
  player->locomotion = (FpsLocomotion){0};
  player->locomotion_active = false_v;
  player->pose_failed = false_v;
  player->weapon_reference_valid = false_v;
  player->animation_id = animation_id;
  if (player->visual_loading) {
    return true_v;
  }
  if (player->weapon_bone != UINT32_MAX &&
      player->weapon_bone >= vkr_anim_bone_count(ctx, player->visual)) {
    if (error) {
      *error = "The fps_weapon bone is outside the player's animation";
    }
    return false_v;
  }
  if (!animation_id) {
    return true_v;
  }
  if (vkr_anim_has_graph(ctx, player->visual)) {
    if (error) {
      *error = "Player animation already has a graph owner";
    }
    return false_v;
  }
  // A bank with the mannequin's clip names moves with speed-synchronized
  // locomotion; any other plays its named action clips.
  if (fps_locomotion_supported(ctx, player->visual)) {
    if (!fps_locomotion_initialize(ctx, &player->locomotion, player->visual,
                                   error)) {
      return false_v;
    }
    player->locomotion_active = true_v;
  } else if (!fps_player_animation_initialize(ctx, &player->animation,
                                              player->visual, vkr_fixed_dt(ctx),
                                              error)) {
    return false_v;
  }
  Mat4 bone;
  if (vkr_anim_bone_pose(ctx, player->visual, player->weapon_bone, &bone)) {
    player->weapon_reference_inverse = mat4_inverse(bone);
    player->weapon_reference_valid = true_v;
  }
  return true_v;
}

static FpsPlayerAnimationInput
player_animation_input(const FpsPlayerState *state) {
  return (FpsPlayerAnimationInput){
      .speed = sqrtf(state->velocity.x * state->velocity.x +
                     state->velocity.z * state->velocity.z),
      .shot_sequence = state->weapon.shot_sequence,
      .grounded = state->grounded,
      .crouched = state->crouched,
      .reloading = state->weapon.reloading};
}

static bool8_t player_reserve_shot(const FpsWeaponShot *shot, void *context) {
  FpsPlayer *player = context;
  if (player->shot_pending) {
    return false_v;
  }
  player->pending_shot = *shot;
  player->shot_pending = true_v;
  return true_v;
}

static void player_fire(VkrCtx *ctx, FpsPlayer *player, FpsPlayerState *state,
                        uint64_t tick) {
  FpsWeaponShot shot;
  if (fps_weapon_try_fire(&state->weapon, tick, player_reserve_shot, player,
                          &shot) != FPS_WEAPON_OK) {
    return;
  }
  const Vec3 eye = vec3_add(player->current_foot,
                            vec3_new(0, state->crouched ? .9f : 1.6f, 0));
  const Vec3 direction =
      vec3_new(cosf(state->pitch) * cosf(state->yaw), sinf(state->pitch),
               cosf(state->pitch) * sinf(state->yaw));
  const VkrQueryFilter filter = {
      .mask = UINT16_MAX, .ignored = &player->entity, .ignored_count = 1};
  player->hit_pending = vkr_raycast(ctx, eye, vec3_scale(direction, 100),
                                    &filter, &player->pending_hit);
}

/* The use key presses the button the eye looks at within reach, or uses
   the mover it belongs to: the brush hit or an object above it carries the
   `button` or `mover` component, as Source's func_button and func_door are
   brushes. The mover decides whether the use moves it. The press names the
   player as its activator. */
static void player_use(VkrCtx *ctx, FpsPlayer *player,
                       const FpsPlayerState *state) {
  const bool8_t buttons = player->button_type && player->press_input.id;
  const bool8_t movers = player->mover_type && player->use_input.id;
  if (!buttons && !movers) {
    return;
  }
  const Vec3 eye = vec3_add(player->current_foot,
                            vec3_new(0, state->crouched ? .9f : 1.6f, 0));
  const Vec3 direction =
      vec3_new(cosf(state->pitch) * cosf(state->yaw), sinf(state->pitch),
               cosf(state->pitch) * sinf(state->yaw));
  const VkrQueryFilter filter = {
      .mask = UINT16_MAX, .ignored = &player->entity, .ignored_count = 1};
  VkrRayHit hit;
  if (!vkr_raycast(ctx, eye, vec3_scale(direction, PLAYER_USE_REACH), &filter,
                   &hit)) {
    return;
  }
  /* The hit brush, or the nearest object above it, carries either. */
  VkrEntity target = vkr_entity_valid(hit.collider) ? hit.collider : hit.entity;
  for (uint32_t depth = 0; vkr_entity_valid(target); ++depth) {
    const VkrIoValue activator = {.kind = VKR_IO_ENTITY,
                                  .entity = player->entity};
    if (buttons && vkr_component_get(ctx, target, player->button_type)) {
      (void)vkr_io_send(ctx, target, player->press_input, &activator);
      return;
    }
    if (movers && vkr_component_get(ctx, target, player->mover_type)) {
      (void)vkr_io_send(ctx, target, player->use_input, &activator);
      return;
    }
    target =
        depth < PLAYER_USE_DEPTH ? vkr_parent(ctx, target) : VKR_ENTITY_NONE;
  }
}

/* The ladder ahead: a sensor with `fps_ladder` that a short level ray from
   the chest meets, or NULL. A ray starting inside it meets it at once. */
static const FpsLadder *player_ladder(VkrCtx *ctx, const FpsPlayer *player,
                                      const FpsPlayerState *state) {
  const Vec3 chest = vec3_add(player->current_foot,
                              vec3_new(0, state->crouched ? .6f : 1.0f, 0));
  const Vec3 ahead = vec3_new(cosf(state->yaw), 0.0f, sinf(state->yaw));
  const VkrQueryFilter filter = {.mask = UINT16_MAX,
                                 .include_sensors = true_v,
                                 .ignored = &player->entity,
                                 .ignored_count = 1};
  VkrRayHit hit;
  if (!vkr_raycast(ctx, chest, vec3_scale(ahead, PLAYER_LADDER_REACH), &filter,
                   &hit)) {
    return NULL;
  }
  const FpsLadder *ladder =
      vkr_component_get(ctx, hit.collider, fps_ladder_type());
  return ladder ? ladder
                : vkr_component_get(ctx, hit.entity, fps_ladder_type());
}

static bool8_t player_before(VkrCtx *ctx, FpsPlayer *player, uint64_t tick) {
  FpsPlayerState *state = fps_player_state(ctx, player);
  if (!state) {
    return player_fail(player, "Player entity or behavior was removed");
  }
  if (player->commands.faulted) {
    return player_fail(player, player->error[0]
                                   ? player->error
                                   : fps_input_error(&player->commands));
  }
  if (player->pose_failed) {
    return player_fail(player, player->error);
  }
  // A body that was loading binds its animation once it has arrived, or
  // stays without one when its model failed.
  if (player->visual_loading &&
      vkr_model_state(ctx, player->visual) != VKR_MODEL_LOADING) {
    player->visual_loading = false_v;
    const char *animation_error = NULL;
    if (!player_prepare_animation(ctx, player, &animation_error)) {
      return player_fail(player, animation_error);
    }
  }
  const uint64_t animation_id =
      vkr_entity_valid(player->visual) ? vkr_anim_id(ctx, player->visual) : 0u;
  if (animation_id != player->animation_id ||
      (animation_id && vkr_anim_has_graph(ctx, player->visual))) {
    return player_fail(
        player, "Player animation binding changed; reset before resuming");
  }
  player->previous_facing = player->facing;
  player->previous_foot = player->current_foot;
  if (state->weapon.reloading) {
    uint32_t transferred;
    (void)fps_weapon_reload_complete(&state->weapon, state->reload, tick,
                                     &state->reserve_rounds, &transferred);
  }
  bool8_t jump = false_v;
  FpsCommand command;
  while (fps_input_next(&player->commands, tick, &command)) {
    const uint32_t bit = 1u << command.action;
    if (command.action == FPS_ACTION_LOOK) {
      state->yaw = command.x;
      state->pitch = command.y;
      continue;
    }
    if (command.pressed) {
      state->held |= bit;
    } else {
      state->held &= ~bit;
    }
    if (!command.pressed) {
      continue;
    }
    switch (command.action) {
    case FPS_ACTION_FIRE:
      player_fire(ctx, player, state, tick);
      break;
    case FPS_ACTION_RELOAD:
      (void)fps_weapon_reload_start(&state->weapon, tick, state->reserve_rounds,
                                    &state->reload);
      break;
    case FPS_ACTION_JUMP:
      jump = true_v;
      break;
    case FPS_ACTION_USE:
      player_use(ctx, player, state);
      break;
    case FPS_ACTION_CAMERA:
      fps_camera_rig_set_mode(&player->camera,
                              (FpsCameraRigMode)((player->camera.mode + 1) % 3),
                              false_v);
      break;
    default:
      break;
    }
  }
  if (!fps_input_finish_tick(&player->commands, tick)) {
    return player_fail(player, fps_input_error(&player->commands));
  }
  if (state->held & (1u << FPS_ACTION_FIRE)) {
    player_fire(ctx, player, state, tick);
  }
  VkrCharacterState motor;
  if (!vkr_character_state(ctx, player->entity, &motor)) {
    return player_fail(player, vkr_last_error(ctx));
  }
  const float32_t forward = !!(state->held & (1u << FPS_ACTION_FORWARD)) -
                            !!(state->held & (1u << FPS_ACTION_BACKWARD));
  const float32_t right = !!(state->held & (1u << FPS_ACTION_RIGHT)) -
                          !!(state->held & (1u << FPS_ACTION_LEFT));
  const float32_t length = sqrtf(forward * forward + right * right);
  const bool8_t crouch_requested =
      (state->held & (1u << FPS_ACTION_CROUCH)) != 0;
  const bool8_t walk_requested = (state->held & (1u << FPS_ACTION_WALK)) != 0;
  const float32_t move_speed = motor.crouched || crouch_requested
                                   ? player->settings.crouch_speed
                               : walk_requested ? player->settings.walk_speed
                                                : player->settings.move_speed;
  const float32_t speed = length > 0 ? move_speed / length : 0;
  const float32_t dt = (float32_t)vkr_fixed_dt(ctx);
  const Vec2 desired =
      vec2_new((cosf(state->yaw) * forward - sinf(state->yaw) * right) * speed,
               (sinf(state->yaw) * forward + cosf(state->yaw) * right) * speed);
  // Third person turns the body towards its movement and eases speed
  // changes; other views strafe with the camera at once.
  const bool8_t orient = player->settings.orient_to_movement &&
                         player->camera.mode == FPS_CAMERA_RIG_THIRD_PERSON;
  if (orient && player->settings.acceleration > 0.0f) {
    const Vec2 delta = vec2_new(desired.x - player->move_velocity.x,
                                desired.y - player->move_velocity.y);
    const float32_t distance = sqrtf(delta.x * delta.x + delta.y * delta.y);
    /* Braking to a stop is a third quicker than speeding up. */
    const float32_t braking = length > 0 ? 1.0f : 4.0f / 3.0f;
    const float32_t step = player->settings.acceleration * braking * dt;
    const float32_t take = distance > step ? step / distance : 1.0f;
    player->move_velocity.x += delta.x * take;
    player->move_velocity.y += delta.y * take;
  } else {
    player->move_velocity = desired;
  }
  if (orient) {
    const Vec2 v = player->move_velocity;
    if (v.x * v.x + v.y * v.y > 0.04f) {
      const float32_t target = atan2f(v.y, v.x);
      const float32_t turn =
          remainderf(target - player->facing, 6.28318530718f);
      const float32_t limit = player->settings.turn_rate * dt;
      player->facing = remainderf(player->facing + Clamp(turn, -limit, limit),
                                  6.28318530718f);
    }
  } else {
    player->facing = state->yaw;
  }
  VkrCharacterMove move = {.velocity = vec3_new(player->move_velocity.x,
                                                motor.velocity.y,
                                                player->move_velocity.y),
                           .gravity = vkr_gravity(ctx),
                           .crouch = crouch_requested,
                           .dt = dt};
  /* On a ladder gravity waits: forward climbs, down while the view looks
     down, and a jump pushes off it, away and up. */
  const FpsLadder *ladder = player_ladder(ctx, player, state);
  if (ladder && jump) {
    move.velocity =
        vec3_new(-cosf(state->yaw) * PLAYER_LADDER_PUSH, PLAYER_LADDER_PUSH,
                 -sinf(state->yaw) * PLAYER_LADDER_PUSH);
  } else if (ladder) {
    move.gravity = vec3_zero();
    move.velocity.y =
        (state->pitch < PLAYER_LADDER_DOWN_PITCH ? -forward : forward) *
        ladder->climb_speed;
  } else if (motor.ground == VKR_GROUND_ON_GROUND) {
    /* The move is relative to the ground: what it stands on carries it
       (ADR-073), and a turning floor turns it, the view too: yaw runs from
       +X toward +Z, against a spin about +Y. */
    move.velocity.y = jump && !crouch_requested && !motor.crouched
                          ? player->settings.jump_speed
                          : 0.0f;
    const float32_t turn = -motor.ground_angular_velocity.y * dt;
    if (turn != 0.0f) {
      state->yaw = remainderf(state->yaw + turn, 6.28318530718f);
      player->render_yaw =
          remainderf(player->render_yaw + turn, 6.28318530718f);
      player->facing = remainderf(player->facing + turn, 6.28318530718f);
    }
    player->carried =
        vec2_new(motor.ground_velocity.x, motor.ground_velocity.z);
  } else {
    /* Off the ground it keeps the way what it last stood on carried it, as
       a jump from a moving tram lands where it took off. */
    move.velocity.x += player->carried.x;
    move.velocity.z += player->carried.y;
  }
  if (ladder) {
    player->carried = (Vec2){0};
  }
  if (!vkr_character_move(ctx, player->entity, &move, &motor)) {
    return player_fail(player, vkr_last_error(ctx));
  }
  player->current_foot = motor.foot;
  state->velocity = motor.velocity;
  state->grounded = motor.ground == VKR_GROUND_ON_GROUND;
  state->crouched = motor.crouched;
  player->camera.config.eye_height = state->crouched ? .9f : 1.6f;
  player->steps++;
  if (player->locomotion_active) {
    // Solved velocity in the body's frame: forward along the facing, right
    // along (-sin, cos) as the camera rig defines it.
    const float32_t c = cosf(player->facing);
    const float32_t s = sinf(player->facing);
    player->locomotion_input = (FpsLocomotionInput){
        .forward = state->velocity.x * c + state->velocity.z * s,
        .right = -state->velocity.x * s + state->velocity.z * c,
        .up = state->velocity.y,
        .grounded = state->grounded,
        .crouched = state->crouched};
  } else if (vkr_entity_valid(player->animation.entity)) {
    const FpsPlayerAnimationInput animation = player_animation_input(state);
    if (!fps_player_animation_update(ctx, &player->animation, &animation)) {
      return player_fail(player, "Player action animation failed");
    }
  }
  return true_v;
}

void fps_player_after_physics(VkrCtx *ctx, FpsPlayer *player) {
  if (player->shot_pending) {
    player->shots_fired++;
    if (player->hit_pending) {
      player->hits++;
      const Vec3 impulse =
          vec3_scale(vec3_negate(player->pending_hit.normal), 5.0f);
      // Static hits remain valid facts; only a dynamic target accepts impulse.
      (void)vkr_impulse(ctx, player->pending_hit.entity, impulse, NULL);
    }
    player->shot_pending = false_v;
    player->hit_pending = false_v;
  }
}

void fps_player_reset(VkrCtx *ctx, FpsPlayer *player) {
  FpsPlayerState *state = fps_player_state(ctx, player);
  if (!state || player->instance_id == UINT64_MAX) {
    player->commands.faulted = true_v;
    return;
  }
  const char *animation_error = NULL;
  if (!player_prepare_animation(ctx, player, &animation_error)) {
    player_fail(player, animation_error);
    player->commands.faulted = true_v;
    return;
  }
  const FpsWeaponConfig weapon = {
      .magazine_capacity = player->settings.magazine,
      .fire_interval_ticks = 6,
      .reload_ticks =
          player->animation.reload_ticks ? player->animation.reload_ticks : 60};
  *state = (FpsPlayerState){.reserve_rounds = player->settings.reserve,
                            .yaw = player->spawn_yaw};
  fps_weapon_initialize(&state->weapon, &weapon, player->settings.magazine,
                        ++player->instance_id);
  player->error[0] = 0;
  player->camera.config.eye_height = 1.6f;
  player->render_yaw = player->spawn_yaw;
  player->render_pitch = 0;
  player->facing = player->spawn_yaw;
  player->previous_facing = player->spawn_yaw;
  player->move_velocity = (Vec2){0};
  player->carried = (Vec2){0};
  player->commands = (FpsInput){0};
  player->shot_pending = false_v;
  player->hit_pending = false_v;
  player->clock_running = false_v;
  player->active = false_v;
  player->shots_fired = 0;
  player->hits = 0;
  VkrCharacterState motor;
  if (vkr_character_state(ctx, player->entity, &motor)) {
    player->current_foot = motor.foot;
    player->previous_foot = player->current_foot;
    state->grounded = motor.ground == VKR_GROUND_ON_GROUND;
    state->crouched = motor.crouched;
  }
  player->steps = 0;
  player->pose_time = 0.0;
  player->locomotion_input = (FpsLocomotionInput){.grounded = state->grounded,
                                                  .crouched = state->crouched};
  if (player->locomotion_active) {
    if (!fps_locomotion_reset(ctx, &player->locomotion)) {
      player_fail(player, "Player locomotion reset failed");
      player->commands.faulted = true_v;
    }
  } else if (vkr_entity_valid(player->animation.entity)) {
    const FpsPlayerAnimationInput animation = player_animation_input(state);
    if (!fps_player_animation_reset(ctx, &player->animation, &animation)) {
      player_fail(player, "Player animation reset failed");
      player->commands.faulted = true_v;
    }
  }
}

/* A modifier is held while any of its keys is. */
static bool8_t player_any_down(VkrCtx *ctx, VkrKey a, VkrKey b, VkrKey c) {
  return vkr_key_down(ctx, a) || vkr_key_down(ctx, b) || vkr_key_down(ctx, c);
}

void fps_player_observe(VkrCtx *ctx, FpsPlayer *player,
                        const VkrInputEvent *event) {
  if (!player->attached || !player->active || player->commands.faulted) {
    return;
  }
  FpsAction action;
  bool8_t pressed = event->pressed;
  if (event->kind == VKR_INPUT_LOOK) {
    action = FPS_ACTION_LOOK;
    player->render_yaw = remainderf(player->render_yaw + (float32_t)event->dx *
                                                             PLAYER_LOOK_SCALE,
                                    6.28318530718f);
    // Captured look reports upward motion as positive dy.
    player->render_pitch =
        Clamp(player->render_pitch + (float32_t)event->dy * PLAYER_LOOK_SCALE,
              -PLAYER_PITCH_LIMIT, PLAYER_PITCH_LIMIT);
  } else if (event->kind == VKR_INPUT_BUTTON && event->code == VKR_MOUSE_LEFT) {
    action = FPS_ACTION_FIRE;
  } else if (event->kind == VKR_INPUT_KEY) {
    switch (event->code) {
    case VKR_KEY_W:
      action = FPS_ACTION_FORWARD;
      break;
    case VKR_KEY_S:
      action = FPS_ACTION_BACKWARD;
      break;
    case VKR_KEY_A:
      action = FPS_ACTION_LEFT;
      break;
    case VKR_KEY_D:
      action = FPS_ACTION_RIGHT;
      break;
    case VKR_KEY_R:
      action = FPS_ACTION_RELOAD;
      break;
    case VKR_KEY_SPACE:
      action = FPS_ACTION_JUMP;
      break;
    case VKR_KEY_V:
      action = FPS_ACTION_CAMERA;
      break;
    case VKR_KEY_E:
      action = FPS_ACTION_USE;
      break;
    case VKR_KEY_CONTROL:
    case VKR_KEY_LCONTROL:
    case VKR_KEY_RCONTROL:
      action = FPS_ACTION_CROUCH;
      pressed = player_any_down(ctx, VKR_KEY_CONTROL, VKR_KEY_LCONTROL,
                                VKR_KEY_RCONTROL);
      break;
    case VKR_KEY_SHIFT:
    case VKR_KEY_LSHIFT:
    case VKR_KEY_RSHIFT:
      action = FPS_ACTION_WALK;
      pressed =
          player_any_down(ctx, VKR_KEY_SHIFT, VKR_KEY_LSHIFT, VKR_KEY_RSHIFT);
      break;
    default:
      return;
    }
  } else {
    return;
  }
  uint64_t tick;
  if (!fps_input_tick(event->time - player->epoch, &tick)) {
    player->commands.faulted = true_v;
    player->commands.error = FPS_INPUT_ERROR_TIME;
  } else if (player->commands.last_sequence == UINT64_MAX ||
             player->commands.consumed_tick == UINT64_MAX) {
    player->commands.faulted = true_v;
    player->commands.error = FPS_INPUT_ERROR_ORDER;
  } else {
    // Synchronous producer events arriving after the last admitted frame cannot
    // belong to its completed ticks. Clock subtraction or the tick epsilon can
    // place an exact-boundary event one interval behind; admit it next instead.
    // Explicit/replayed queue commands retain strict late-input rejection.
    if (tick <= player->commands.consumed_tick &&
        event->time >= player->last_frame_time) {
      tick = player->commands.consumed_tick + 1;
    }
    (void)fps_input_push(
        &player->commands,
        (FpsCommand){.tick = tick,
                     .sequence = player->commands.last_sequence + 1,
                     .action = action,
                     .pressed = pressed,
                     .x = player->render_yaw,
                     .y = player->render_pitch});
  }
  if (player->commands.faulted) {
    snprintf(player->error, sizeof(player->error),
             "%s (queued=%u, consumed=%llu, last=%llu)",
             fps_input_error(&player->commands), player->commands.count,
             (unsigned long long)player->commands.consumed_tick,
             (unsigned long long)player->commands.last_tick);
  }
}

bool8_t fps_player_before_physics(VkrCtx *ctx, FpsPlayer *player, uint64_t tick,
                                  const char **error) {
  if (!player_before(ctx, player, tick)) {
    *error = player->error;
    return false_v;
  }
  return true_v;
}

bool8_t fps_player_attach(VkrCtx *ctx, FpsPlayer *player,
                          const FpsPlayerConfig *config, const char **error) {
  if (!player || player->attached || !config || !config->instance_id ||
      config->instance_id == UINT64_MAX || !isfinite(config->yaw) ||
      !vkr_paused(ctx) || vkr_ticks(ctx) || vkr_sim_debt(ctx) ||
      !vkr_alive(ctx, config->entity)) {
    if (error)
      *error = "Player requires an unbound paused/reset scene";
    return false_v;
  }
  const VkrStateType state_type = VKR_STATE_TYPE(ctx, FpsPlayerState);
  if (!state_type.id || vkr_state_get(ctx, config->entity, state_type) ||
      vkr_character_state(ctx, config->entity, NULL)) {
    if (error)
      *error = "Player behavior/motor is already attached";
    return false_v;
  }
  *player = (FpsPlayer){.entity = config->entity,
                        .state_type = state_type,
                        .settings = config->settings,
                        .visual = config->visual,
                        .visual_loading = config->visual_loading,
                        .weapon_bone = config->weapon_bone,
                        .instance_id = config->instance_id - 1,
                        .spawn_yaw = config->yaw,
                        .render_yaw = config->yaw,
                        .attached = true_v};
  if (!vkr_state_add(ctx, config->entity, state_type, NULL)) {
    goto fail;
  }
  player->button_type = vkr_component_named(ctx, "button");
  if (player->button_type) {
    player->press_input = vkr_io_input(ctx, player->button_type, "press");
  }
  player->mover_type = vkr_component_named(ctx, "mover");
  if (player->mover_type) {
    player->use_input = vkr_io_input(ctx, player->mover_type, "use");
  }
  const VkrCharacterDesc motor = vkr_character_default(ctx);
  if (!vkr_character_create(ctx, config->entity, &motor,
                            config->has_spawn ? &config->spawn_foot : NULL)) {
    if (error) {
      *error = vkr_last_error(ctx);
    }
    goto remove_state;
  }
  const FpsCameraRigConfig camera = {.eye_height = 1.6f,
                                     .distance = 4,
                                     .shoulder_offset = 0.6f,
                                     .shoulder_height = 0,
                                     .sweep_radius = 0.2f,
                                     .pitch_limit = PLAYER_PITCH_LIMIT};
  if (!fps_camera_rig_initialize(&player->camera, &camera) ||
      !fps_camera_rig_set_mode(&player->camera,
                               (FpsCameraRigMode)config->settings.camera_mode,
                               false_v)) {
    if (error) {
      *error = "Player camera initialization failed";
    }
    goto remove_motor;
  }
  fps_player_reset(ctx, player);
  if (player->commands.faulted) {
    if (error) {
      *error = "Player animation initialization failed";
    }
    goto remove_motor;
  }
  return true_v;
remove_motor:
  vkr_character_destroy(ctx, config->entity);
remove_state:
  vkr_state_remove(ctx, config->entity, state_type);
fail:
  *player = (FpsPlayer){0};
  return false_v;
}

void fps_player_shutdown(VkrCtx *ctx, FpsPlayer *player) {
  if (!player || !player->attached) {
    return;
  }
  // The scene owner destroys the entity; teardown releases only what attach
  // created and resets no unrelated native state.
  vkr_character_destroy(ctx, player->entity);
  vkr_state_remove(ctx, player->entity, player->state_type);
  *player = (FpsPlayer){0};
}

float64_t fps_player_frame(VkrCtx *ctx, FpsPlayer *player, float64_t now,
                           bool8_t active) {
  if (!player || !player->attached || !isfinite(now)) {
    return 0;
  }
  const bool8_t running = vkr_simulating(ctx);
  const float64_t dt =
      running && player->clock_running ? now - player->last_frame_time : 0.0;
  if (dt < 0.0 || !isfinite(dt)) {
    player->commands.faulted = true_v;
    player->commands.error = FPS_INPUT_ERROR_TIME;
    return 0;
  }
  if (!running || !player->clock_running) {
    player->epoch = now - vkr_sim_time(ctx) - vkr_sim_debt(ctx);
  }
  player->last_frame_time = now;
  player->clock_running = running;
  active = active && running;
  if (!active || !player->active) {
    FpsPlayerState *state = fps_player_state(ctx, player);
    if (state) {
      if (player->active && !active) {
        player->render_yaw = state->yaw;
        player->render_pitch = state->pitch;
      }
      state->held = 0;
      fps_weapon_reload_cancel(&state->weapon, state->reload);
    }
    player->commands = (FpsInput){.consumed_tick = vkr_ticks(ctx)};
  }
  player->active = active;
  return dt;
}

typedef struct PlayerSweep {
  VkrCtx *ctx;
  const FpsPlayer *player;
} PlayerSweep;

static bool8_t player_camera_sweep(Vec3 origin, Vec3 displacement,
                                   float32_t radius, void *context,
                                   float32_t *fraction) {
  const PlayerSweep *sweep = context;
  const VkrQueryFilter filter = {.mask = UINT16_MAX,
                                 .ignored = &sweep->player->entity,
                                 .ignored_count = 1};
  VkrRayHit hit;
  if (vkr_sweep_sphere(sweep->ctx, origin, displacement, radius, &filter,
                       &hit)) {
    *fraction = hit.fraction;
    return true_v;
  }
  /* No hit, or a refused query: the camera keeps its full distance. */
  *fraction = 1.0f;
  return true_v;
}

bool8_t fps_player_camera(VkrCtx *ctx, FpsPlayer *player,
                          FpsCameraRigPose *pose) {
  if (!player || !player->attached) {
    return false_v;
  }
  const float32_t alpha = vkr_tick_alpha(ctx);
  const Vec3 foot = vec3_add(
      player->previous_foot,
      vec3_scale(vec3_sub(player->current_foot, player->previous_foot), alpha));
  PlayerSweep sweep = {.ctx = ctx, .player = player};
  return fps_camera_rig_evaluate(&player->camera, foot, player->render_yaw,
                                 player->render_pitch, player_camera_sweep,
                                 &sweep, pose);
}

float32_t fps_player_render_facing(const FpsPlayer *player, float32_t alpha) {
  const float32_t turn =
      remainderf(player->facing - player->previous_facing, 6.28318530718f);
  return player->previous_facing + turn * Clamp(alpha, 0.0f, 1.0f);
}

void fps_player_animate(VkrCtx *ctx, FpsPlayer *player, float32_t alpha) {
  if (!player->locomotion_active || !player->steps || player->pose_failed) {
    return;
  }
  // The root renders `alpha` of the way through the latest tick.
  const float64_t time =
      ((float64_t)player->steps - 1.0 + (float64_t)Clamp(alpha, 0.0f, 1.0f)) *
      vkr_fixed_dt(ctx);
  if (time <= player->pose_time) {
    return;
  }
  const float64_t dt = time - player->pose_time;
  player->pose_time = time;
  if (!fps_locomotion_update(ctx, &player->locomotion,
                             &player->locomotion_input, dt)) {
    player_fail(player, "Player locomotion animation failed");
    player->pose_failed = true_v;
  }
}
