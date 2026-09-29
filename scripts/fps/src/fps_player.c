#include "fps_player.h"
#include <math.h>
#include <stdio.h>

#define PLAYER_PITCH_LIMIT 1.45f
#define PLAYER_LOOK_SCALE 0.0025f

FpsPlayerSettings fps_player_settings_default(void) {
  return (FpsPlayerSettings){.move_speed = 5.0f,
                             .crouch_speed = 0.6f,
                             .jump_speed = 5.0f,
                             .magazine = 12,
                             .reserve = 120};
}

static FpsPlayerState *player_state(FpsPlayer *player) {
  return player->api->get_state(player->scene, player->entity,
                                player->component);
}

static bool8_t player_fail(FpsPlayer *player, const char *message) {
  if (message != player->error) {
    snprintf(player->error, sizeof(player->error), "%s",
             message ? message : "Unknown player failure");
  }
  return false_v;
}

static bool8_t player_prepare_animation(FpsPlayer *player, const char **error) {
  const VkrScriptApi *api = player->api;
  VkrAnimationPlayer *current =
      api->animation_player(player->scene, player->entity);
  player->animation = (FpsPlayerAnimation){0};
  player->weapon_reference_valid = false_v;
  if (!current) {
    return true_v;
  }
  if (api->animation_graph(player->scene, player->entity)) {
    if (error) {
      *error = "Player animation already has a graph owner";
    }
    return false_v;
  }
  if (!fps_player_animation_initialize(&player->animation, api, current,
                                       VKR_SCENE_SIMULATION_FIXED_DT, error)) {
    return false_v;
  }
  const VkrAnimationAsset *asset = api->animation_asset(current);
  if (player->weapon_bone < asset->node_count) {
    player->weapon_reference_inverse =
        mat4_inverse(api->animation_global_pose(current)[player->weapon_bone]);
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

static void player_fire(FpsPlayer *player, FpsPlayerState *state,
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
  const uint64_t ignored = player->entity.u64;
  const VkrPhysicsQueryFilter filter = {
      .mask = UINT16_MAX, .ignored_entities = &ignored, .ignored_count = 1};
  player->hit_pending =
      player->api->raycast(player->scene, eye, vec3_scale(direction, 100),
                           &filter, &player->pending_hit);
}

static bool8_t player_before(FpsPlayer *player, uint64_t tick) {
  const VkrScriptApi *api = player->api;
  VkrScene *scene = player->scene;
  FpsPlayerState *state = player_state(player);
  if (!state) {
    return player_fail(player, "Player entity or behavior was removed");
  }
  if (player->commands.faulted) {
    return player_fail(player, player->error[0]
                                   ? player->error
                                   : fps_input_error(&player->commands));
  }
  if (api->animation_player(scene, player->entity) !=
          player->animation.player ||
      api->animation_graph(scene, player->entity)) {
    return player_fail(
        player, "Player animation binding changed; reset before resuming");
  }
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
      player_fire(player, state, tick);
      break;
    case FPS_ACTION_RELOAD:
      (void)fps_weapon_reload_start(&state->weapon, tick, state->reserve_rounds,
                                    &state->reload);
      break;
    case FPS_ACTION_JUMP:
      jump = true_v;
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
    player_fire(player, state, tick);
  }
  VkrPhysicsCharacterState motor;
  const char *error = NULL;
  if (!api->character_get_state(scene, player->entity, &motor, &error)) {
    return player_fail(player, error);
  }
  const float32_t forward = !!(state->held & (1u << FPS_ACTION_FORWARD)) -
                            !!(state->held & (1u << FPS_ACTION_BACKWARD));
  const float32_t right = !!(state->held & (1u << FPS_ACTION_RIGHT)) -
                          !!(state->held & (1u << FPS_ACTION_LEFT));
  const float32_t length = sqrtf(forward * forward + right * right);
  const bool8_t crouch_requested =
      (state->held & (1u << FPS_ACTION_CROUCH)) != 0;
  const float32_t move_speed = motor.crouched || crouch_requested
                                   ? player->settings.crouch_speed
                                   : player->settings.move_speed;
  const float32_t speed = length > 0 ? move_speed / length : 0;
  const Vec3 gravity = api->gravity(scene);
  VkrPhysicsCharacterInput input = {
      .velocity = {(cosf(state->yaw) * forward - sinf(state->yaw) * right) *
                       speed,
                   motor.velocity[1],
                   (sinf(state->yaw) * forward + cosf(state->yaw) * right) *
                       speed},
      .gravity = {gravity.x, gravity.y, gravity.z},
      .crouch = crouch_requested,
      .dt = (float32_t)VKR_SCENE_SIMULATION_FIXED_DT};
  if (motor.ground == VKR_PHYSICS_CHARACTER_ON_GROUND) {
    input.velocity[1] = jump && !crouch_requested && !motor.crouched
                            ? player->settings.jump_speed
                            : motor.ground_velocity[1];
  }
  if (!api->character_step(scene, player->entity, &input, &motor, &error)) {
    return player_fail(player, error);
  }
  player->current_foot = vec3_new(
      motor.foot_position[0], motor.foot_position[1], motor.foot_position[2]);
  state->velocity =
      vec3_new(motor.velocity[0], motor.velocity[1], motor.velocity[2]);
  state->grounded = motor.ground == VKR_PHYSICS_CHARACTER_ON_GROUND;
  state->crouched = motor.crouched;
  player->camera.config.eye_height = state->crouched ? .9f : 1.6f;
  if (player->animation.player) {
    const FpsPlayerAnimationInput animation = player_animation_input(state);
    if (!fps_player_animation_update(&player->animation, &animation)) {
      return player_fail(player, "Player action animation failed");
    }
  }
  return true_v;
}

void fps_player_after_physics(FpsPlayer *player) {
  if (player->shot_pending) {
    player->shots_fired++;
    if (player->hit_pending) {
      player->hits++;
      const Vec3 impulse = vec3_scale(vec3_new(-player->pending_hit.normal[0],
                                               -player->pending_hit.normal[1],
                                               -player->pending_hit.normal[2]),
                                      5.0f);
      const VkrEntityId target = {.u64 = player->pending_hit.entity_id};
      // Static hits remain valid facts; only a dynamic target accepts impulse.
      (void)player->api->physics_impulse(player->scene, target, impulse, NULL,
                                         NULL);
    }
    player->shot_pending = false_v;
    player->hit_pending = false_v;
  }
}

void fps_player_reset(FpsPlayer *player) {
  FpsPlayerState *state = player_state(player);
  if (!state || player->instance_id == UINT64_MAX) {
    player->commands.faulted = true_v;
    return;
  }
  const char *animation_error = NULL;
  if (!player_prepare_animation(player, &animation_error)) {
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
  player->commands = (FpsInput){0};
  player->shot_pending = false_v;
  player->hit_pending = false_v;
  player->clock_running = false_v;
  player->active = false_v;
  player->shots_fired = 0;
  player->hits = 0;
  VkrPhysicsCharacterState motor;
  if (player->api->character_get_state(player->scene, player->entity, &motor,
                                       NULL)) {
    player->current_foot = vec3_new(
        motor.foot_position[0], motor.foot_position[1], motor.foot_position[2]);
    player->previous_foot = player->current_foot;
    state->grounded = motor.ground == VKR_PHYSICS_CHARACTER_ON_GROUND;
    state->crouched = motor.crouched;
  }
  if (player->animation.player) {
    const FpsPlayerAnimationInput animation = player_animation_input(state);
    if (!fps_player_animation_reset(&player->animation, &animation)) {
      player_fail(player, "Player animation reset failed");
      player->commands.faulted = true_v;
    }
  }
}

void fps_player_observe(FpsPlayer *player, const VkrInputTransition *event) {
  if (!player->scene || !player->active || player->commands.faulted) {
    return;
  }
  FpsAction action;
  bool8_t pressed = event->pressed;
  if (event->kind == VKR_INPUT_TRANSITION_LOOK) {
    action = FPS_ACTION_LOOK;
    player->render_yaw = remainderf(
        player->render_yaw + (float32_t)event->delta_x * PLAYER_LOOK_SCALE,
        6.28318530718f);
    player->render_pitch = Clamp(
        player->render_pitch - (float32_t)event->delta_y * PLAYER_LOOK_SCALE,
        -PLAYER_PITCH_LIMIT, PLAYER_PITCH_LIMIT);
  } else if (event->kind == VKR_INPUT_TRANSITION_BUTTON &&
             event->code == BUTTON_LEFT) {
    action = FPS_ACTION_FIRE;
  } else if (event->kind == VKR_INPUT_TRANSITION_KEY) {
    switch (event->code) {
    case KEY_W:
      action = FPS_ACTION_FORWARD;
      break;
    case KEY_S:
      action = FPS_ACTION_BACKWARD;
      break;
    case KEY_A:
      action = FPS_ACTION_LEFT;
      break;
    case KEY_D:
      action = FPS_ACTION_RIGHT;
      break;
    case KEY_R:
      action = FPS_ACTION_RELOAD;
      break;
    case KEY_SPACE:
      action = FPS_ACTION_JUMP;
      break;
    case KEY_V:
      action = FPS_ACTION_CAMERA;
      break;
    case KEY_CONTROL:
    case KEY_LCONTROL:
    case KEY_RCONTROL:
      action = FPS_ACTION_CROUCH;
      pressed = player->api->input_key_down(player->input, KEY_CONTROL) ||
                player->api->input_key_down(player->input, KEY_LCONTROL) ||
                player->api->input_key_down(player->input, KEY_RCONTROL);
      break;
    default:
      return;
    }
  } else {
    return;
  }
  uint64_t tick;
  if (!fps_input_tick(event->time_seconds - player->epoch, &tick)) {
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
        event->time_seconds >= player->last_frame_time) {
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

bool8_t fps_player_before_physics(FpsPlayer *player, uint64_t tick,
                                  const char **error) {
  if (!player_before(player, tick)) {
    *error = player->error;
    return false_v;
  }
  return true_v;
}

bool8_t fps_player_attach(FpsPlayer *player, const FpsPlayerConfig *config,
                          const char **error) {
  const VkrScriptApi *api = config ? config->api : NULL;
  VkrScene *scene = config ? config->scene : NULL;
  if (!player || player->scene || !api || !scene || !config->input ||
      !config->instance_id || config->instance_id == UINT64_MAX ||
      !isfinite(config->yaw) || !api->physics_paused(scene) ||
      api->simulation_completed_ticks(scene) || api->physics_debt(scene) ||
      !api->entity_alive(scene, config->entity)) {
    if (error)
      *error = "Player requires an unbound paused/reset scene and input";
    return false_v;
  }
  const VkrComponentTypeId component = api->register_state(
      scene, "FpsPlayerState", sizeof(FpsPlayerState), AlignOf(FpsPlayerState));
  VkrPhysicsCharacterState existing_motor;
  if (component == VKR_COMPONENT_TYPE_INVALID ||
      api->has_state(scene, config->entity, component) ||
      api->character_get_state(scene, config->entity, &existing_motor, NULL)) {
    if (error)
      *error = "Player behavior/motor is already attached";
    return false_v;
  }
  *player = (FpsPlayer){.api = api,
                        .scene = scene,
                        .input = config->input,
                        .entity = config->entity,
                        .component = component,
                        .settings = config->settings,
                        .weapon_bone = config->weapon_bone,
                        .instance_id = config->instance_id - 1,
                        .spawn_yaw = config->yaw,
                        .render_yaw = config->yaw};
  FpsPlayerState state = {0};
  if (!api->add_state(scene, config->entity, component, &state)) {
    goto fail;
  }
  VkrPhysicsCharacterDesc motor = api->character_default();
  if (!api->character_create(scene, config->entity, &motor,
                             config->has_spawn ? &config->spawn_foot : NULL,
                             error)) {
    goto remove_component;
  }
  const FpsCameraRigConfig camera = {.eye_height = 1.6f,
                                     .distance = 4,
                                     .shoulder_offset = 0.6f,
                                     .shoulder_height = 0,
                                     .sweep_radius = 0.2f,
                                     .pitch_limit = PLAYER_PITCH_LIMIT};
  fps_camera_rig_initialize(&player->camera, &camera);
  fps_player_reset(player);
  if (player->commands.faulted) {
    if (error) {
      *error = "Player animation initialization failed";
    }
    goto remove_motor;
  }
  return true_v;
remove_motor:
  api->character_destroy(scene, config->entity, NULL);
remove_component:
  api->remove_state(scene, config->entity, component);
fail:
  *player = (FpsPlayer){0};
  return false_v;
}

void fps_player_shutdown(FpsPlayer *player) {
  if (!player || !player->scene) {
    return;
  }
  const VkrScriptApi *api = player->api;
  // The scene owner destroys the entity; teardown releases only what attach
  // created and resets no unrelated native state.
  api->character_destroy(player->scene, player->entity, NULL);
  api->remove_state(player->scene, player->entity, player->component);
  *player = (FpsPlayer){0};
}

float64_t fps_player_frame(FpsPlayer *player, float64_t now, bool8_t active) {
  if (!player || !player->scene || !isfinite(now)) {
    return 0;
  }
  const VkrScriptApi *api = player->api;
  const bool8_t running = api->simulation_running(player->scene);
  const float64_t dt =
      running && player->clock_running ? now - player->last_frame_time : 0.0;
  if (dt < 0.0 || !isfinite(dt)) {
    player->commands.faulted = true_v;
    player->commands.error = FPS_INPUT_ERROR_TIME;
    return 0;
  }
  if (!running || !player->clock_running) {
    player->epoch = now - api->physics_time(player->scene) -
                    api->physics_debt(player->scene);
  }
  player->last_frame_time = now;
  player->clock_running = running;
  active = active && running;
  if (!active || !player->active) {
    FpsPlayerState *state = player_state(player);
    if (state) {
      if (player->active && !active) {
        player->render_yaw = state->yaw;
        player->render_pitch = state->pitch;
      }
      state->held = 0;
      fps_weapon_reload_cancel(&state->weapon, state->reload);
    }
    player->commands = (FpsInput){
        .consumed_tick = api->simulation_completed_ticks(player->scene)};
  }
  player->active = active;
  return dt;
}

static bool8_t player_camera_sweep(Vec3 origin, Vec3 displacement,
                                   float32_t radius, void *context,
                                   float32_t *fraction) {
  FpsPlayer *player = context;
  const uint64_t ignored = player->entity.u64;
  const VkrPhysicsQueryFilter filter = {
      .mask = UINT16_MAX, .ignored_entities = &ignored, .ignored_count = 1};
  VkrPhysicsRayHit hit;
  bool8_t has_hit = false_v;
  if (!player->api->sweep_sphere(player->scene, origin, displacement, radius,
                                 &filter, &hit, &has_hit)) {
    return false_v;
  }
  *fraction = has_hit ? hit.fraction : 1.0f;
  return true_v;
}

bool8_t fps_player_camera(FpsPlayer *player, FpsCameraRigPose *pose) {
  if (!player || !player->scene) {
    return false_v;
  }
  const float32_t alpha =
      player->api->physics_paused(player->scene)
          ? 1
          : (float32_t)Min(1.0, player->api->physics_debt(player->scene) /
                                    VKR_SCENE_SIMULATION_FIXED_DT);
  const Vec3 foot = vec3_add(
      player->previous_foot,
      vec3_scale(vec3_sub(player->current_foot, player->previous_foot), alpha));
  return fps_camera_rig_evaluate(&player->camera, foot, player->render_yaw,
                                 player->render_pitch, player_camera_sweep,
                                 player, pose);
}
