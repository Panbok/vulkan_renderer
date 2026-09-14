#include "gameplay/vkr_gameplay_player.h"
#include "renderer/systems/vkr_scene_animation.h"
#include <math.h>
#include <stdio.h>

#define PLAYER_PITCH_LIMIT 1.45f
#define PLAYER_LOOK_SCALE 0.0025f

static VkrPlayerState *player_state(VkrGameplayPlayer *player) {
  return vkr_entity_get_component_if_alive(player->scene->world, player->entity,
                                           player->component);
}

static bool8_t player_fail(VkrGameplayPlayer *player, const char *message) {
  if (message != player->error) {
    snprintf(player->error, sizeof(player->error), "%s",
             message ? message : "Unknown player failure");
  }
  player->scene->simulation.error = player->error;
  return false_v;
}

static bool8_t player_prepare_animation(VkrGameplayPlayer *player,
                                        const char **error) {
  VkrScene *scene = player->scene;
  VkrAnimationPlayer *current =
      vkr_scene_animation_get_player(scene, player->entity);
  player->animation = (VkrPlayerAnimation){0};
  player->weapon_reference_valid = false_v;
  if (!current) {
    return true_v;
  }
  if (vkr_scene_animation_get_graph(scene, player->entity)) {
    if (error) {
      *error = "Player animation already has a graph owner";
    }
    return false_v;
  }
  if (!vkr_player_animation_initialize(&player->animation, current,
                                       VKR_SCENE_SIMULATION_FIXED_DT, error)) {
    return false_v;
  }
  const VkrAnimationAsset *asset = vkr_animation_player_asset(current);
  if (scene->player_entity.u64 == player->entity.u64 &&
      scene->player_weapon_entity.u64 &&
      scene->player_weapon_bone < asset->node_count) {
    player->weapon_reference_inverse = mat4_inverse(
        vkr_animation_player_global_pose(current)[scene->player_weapon_bone]);
    player->weapon_reference_valid = true_v;
  }
  return true_v;
}

static VkrPlayerAnimationInput
player_animation_input(const VkrPlayerState *state) {
  return (VkrPlayerAnimationInput){
      .speed = sqrtf(state->velocity.x * state->velocity.x +
                     state->velocity.z * state->velocity.z),
      .shot_sequence = state->weapon.shot_sequence,
      .grounded = state->grounded,
      .crouched = state->crouched,
      .reloading = state->weapon.reloading};
}

static bool8_t player_reserve_shot(const VkrWeaponShot *shot, void *context) {
  VkrGameplayPlayer *player = context;
  if (player->shot_pending) {
    return false_v;
  }
  player->pending_shot = *shot;
  player->shot_pending = true_v;
  return true_v;
}

static void player_fire(VkrGameplayPlayer *player, VkrPlayerState *state,
                        uint64_t tick) {
  VkrWeaponShot shot;
  if (vkr_weapon_try_fire(&state->weapon, tick, player_reserve_shot, player,
                          &shot) != VKR_WEAPON_OK) {
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
  player->hit_pending = vkr_scene_physics_raycast_query(
      player->scene, eye, vec3_scale(direction, 100), &filter,
      &player->pending_hit);
}

static bool8_t player_before(VkrScene *scene, uint64_t tick, void *context) {
  VkrGameplayPlayer *player = context;
  VkrPlayerState *state = player_state(player);
  if (!state) {
    return player_fail(player, "Player entity or behavior was removed");
  }
  if (player->commands.faulted) {
    return player_fail(
        player, player->error[0] ? player->error
                                 : vkr_gameplay_input_error(&player->commands));
  }
  if (vkr_scene_animation_get_player(scene, player->entity) !=
          player->animation.player ||
      vkr_scene_animation_get_graph(scene, player->entity)) {
    return player_fail(
        player, "Player animation binding changed; reset before resuming");
  }
  player->previous_foot = player->current_foot;
  if (state->weapon.reloading) {
    uint32_t transferred;
    (void)vkr_weapon_reload_complete(&state->weapon, state->reload, tick,
                                     &state->reserve_rounds, &transferred);
  }
  bool8_t jump = false_v;
  VkrGameplayCommand command;
  while (vkr_gameplay_input_next(&player->commands, tick, &command)) {
    const uint32_t bit = 1u << command.action;
    if (command.action == VKR_GAMEPLAY_LOOK) {
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
    case VKR_GAMEPLAY_FIRE:
      player_fire(player, state, tick);
      break;
    case VKR_GAMEPLAY_RELOAD:
      (void)vkr_weapon_reload_start(&state->weapon, tick, state->reserve_rounds,
                                    &state->reload);
      break;
    case VKR_GAMEPLAY_JUMP:
      jump = true_v;
      break;
    case VKR_GAMEPLAY_CAMERA:
      vkr_camera_rig_set_mode(&player->camera,
                              (VkrCameraRigMode)((player->camera.mode + 1) % 3),
                              false_v);
      break;
    default:
      break;
    }
  }
  if (!vkr_gameplay_input_finish_tick(&player->commands, tick)) {
    return player_fail(player, vkr_gameplay_input_error(&player->commands));
  }
  if (state->held & (1u << VKR_GAMEPLAY_FIRE)) {
    player_fire(player, state, tick);
  }
  VkrPhysicsCharacterState motor;
  const char *error = NULL;
  if (!vkr_scene_character_get_state(scene, player->entity, &motor, &error)) {
    return player_fail(player, error);
  }
  const float32_t forward = !!(state->held & (1u << VKR_GAMEPLAY_FORWARD)) -
                            !!(state->held & (1u << VKR_GAMEPLAY_BACKWARD));
  const float32_t right = !!(state->held & (1u << VKR_GAMEPLAY_RIGHT)) -
                          !!(state->held & (1u << VKR_GAMEPLAY_LEFT));
  const float32_t length = sqrtf(forward * forward + right * right);
  const bool8_t crouch_requested =
      (state->held & (1u << VKR_GAMEPLAY_CROUCH)) != 0;
  const float32_t move_speed = motor.crouched || crouch_requested ? .6f : 5.0f;
  const float32_t speed = length > 0 ? move_speed / length : 0;
  VkrPhysicsCharacterInput input = {
      .velocity = {(cosf(state->yaw) * forward - sinf(state->yaw) * right) *
                       speed,
                   motor.velocity[1],
                   (sinf(state->yaw) * forward + cosf(state->yaw) * right) *
                       speed},
      .gravity = {0, -9.81f, 0},
      .crouch = crouch_requested,
      .dt = (float32_t)VKR_SCENE_SIMULATION_FIXED_DT};
  if (motor.ground == VKR_PHYSICS_CHARACTER_ON_GROUND) {
    input.velocity[1] = jump && !crouch_requested && !motor.crouched
                            ? 5.0f
                            : motor.ground_velocity[1];
  }
  if (!vkr_scene_character_step(scene, player->entity, &input, &motor,
                                &error)) {
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
    const VkrPlayerAnimationInput animation = player_animation_input(state);
    if (!vkr_player_animation_update(&player->animation, &animation)) {
      return player_fail(player, "Player action animation failed");
    }
  }
  return true_v;
}

static bool8_t player_after(VkrScene *scene, uint64_t tick, void *context) {
  (void)tick;
  VkrGameplayPlayer *player = context;
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
      (void)vkr_scene_physics_impulse(scene, target, impulse, NULL, NULL);
    }
    player->shot_pending = false_v;
    player->hit_pending = false_v;
  }
  return true_v;
}

static void player_reset(VkrScene *scene, void *context) {
  VkrGameplayPlayer *player = context;
  VkrPlayerState *state = player_state(player);
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
  const VkrWeaponConfig weapon = {
      .magazine_capacity = 12,
      .fire_interval_ticks = 6,
      .reload_ticks =
          player->animation.reload_ticks ? player->animation.reload_ticks : 60};
  *state = (VkrPlayerState){.reserve_rounds = 120, .yaw = player->spawn_yaw};
  vkr_weapon_initialize(&state->weapon, &weapon, 12, ++player->instance_id);
  player->error[0] = 0;
  player->camera.config.eye_height = 1.6f;
  player->render_yaw = player->spawn_yaw;
  player->render_pitch = 0;
  player->commands = (VkrGameplayInput){0};
  player->shot_pending = false_v;
  player->hit_pending = false_v;
  player->active = false_v;
  player->shots_fired = 0;
  player->hits = 0;
  VkrPhysicsCharacterState motor;
  if (vkr_scene_character_get_state(scene, player->entity, &motor, NULL)) {
    player->current_foot = vec3_new(
        motor.foot_position[0], motor.foot_position[1], motor.foot_position[2]);
    player->previous_foot = player->current_foot;
    state->grounded = motor.ground == VKR_PHYSICS_CHARACTER_ON_GROUND;
    state->crouched = motor.crouched;
  }
  if (player->animation.player) {
    const VkrPlayerAnimationInput animation = player_animation_input(state);
    if (!vkr_player_animation_reset(&player->animation, &animation)) {
      player_fail(player, "Player animation reset failed");
      player->commands.faulted = true_v;
    }
  }
}

static void player_observe(const VkrInputTransition *event, void *context) {
  VkrGameplayPlayer *player = context;
  if (!player->active || player->commands.faulted) {
    return;
  }
  VkrGameplayAction action;
  bool8_t pressed = event->pressed;
  if (event->kind == VKR_INPUT_TRANSITION_LOOK) {
    action = VKR_GAMEPLAY_LOOK;
    player->render_yaw = remainderf(
        player->render_yaw + (float32_t)event->delta_x * PLAYER_LOOK_SCALE,
        6.28318530718f);
    player->render_pitch = Clamp(
        player->render_pitch - (float32_t)event->delta_y * PLAYER_LOOK_SCALE,
        -PLAYER_PITCH_LIMIT, PLAYER_PITCH_LIMIT);
  } else if (event->kind == VKR_INPUT_TRANSITION_BUTTON &&
             event->code == BUTTON_LEFT) {
    action = VKR_GAMEPLAY_FIRE;
  } else if (event->kind == VKR_INPUT_TRANSITION_KEY) {
    switch (event->code) {
    case KEY_W:
      action = VKR_GAMEPLAY_FORWARD;
      break;
    case KEY_S:
      action = VKR_GAMEPLAY_BACKWARD;
      break;
    case KEY_A:
      action = VKR_GAMEPLAY_LEFT;
      break;
    case KEY_D:
      action = VKR_GAMEPLAY_RIGHT;
      break;
    case KEY_R:
      action = VKR_GAMEPLAY_RELOAD;
      break;
    case KEY_SPACE:
      action = VKR_GAMEPLAY_JUMP;
      break;
    case KEY_V:
      action = VKR_GAMEPLAY_CAMERA;
      break;
    case KEY_CONTROL:
    case KEY_LCONTROL:
    case KEY_RCONTROL:
      action = VKR_GAMEPLAY_CROUCH;
      pressed = input_is_key_down(player->input, KEY_CONTROL) ||
                input_is_key_down(player->input, KEY_LCONTROL) ||
                input_is_key_down(player->input, KEY_RCONTROL);
      break;
    default:
      return;
    }
  } else {
    return;
  }
  uint64_t tick;
  if (!vkr_gameplay_input_tick(event->time_seconds - player->epoch, &tick)) {
    player->commands.faulted = true_v;
    player->commands.error = VKR_GAMEPLAY_INPUT_ERROR_TIME;
  } else if (player->commands.last_sequence == UINT64_MAX ||
             player->commands.consumed_tick == UINT64_MAX) {
    player->commands.faulted = true_v;
    player->commands.error = VKR_GAMEPLAY_INPUT_ERROR_ORDER;
  } else {
    // Synchronous producer events arriving after the last admitted frame cannot
    // belong to its completed ticks. Clock subtraction or the tick epsilon can
    // place an exact-boundary event one interval behind; admit it next instead.
    // Explicit/replayed queue commands retain strict late-input rejection.
    if (tick <= player->commands.consumed_tick &&
        event->time_seconds >= player->last_frame_time) {
      tick = player->commands.consumed_tick + 1;
    }
    (void)vkr_gameplay_input_push(
        &player->commands,
        (VkrGameplayCommand){.tick = tick,
                             .sequence = player->commands.last_sequence + 1,
                             .action = action,
                             .pressed = pressed,
                             .x = player->render_yaw,
                             .y = player->render_pitch});
  }
  if (player->commands.faulted) {
    snprintf(player->error, sizeof(player->error),
             "%s (queued=%u, consumed=%llu, last=%llu)",
             vkr_gameplay_input_error(&player->commands),
             player->commands.count,
             (unsigned long long)player->commands.consumed_tick,
             (unsigned long long)player->commands.last_tick);
  }
}

bool8_t vkr_gameplay_player_attach(VkrGameplayPlayer *player, VkrScene *scene,
                                   InputState *input, VkrEntityId entity,
                                   uint64_t instance_id, float32_t yaw,
                                   const char **error) {
  if (!player || player->scene || !scene || !input || !instance_id ||
      instance_id == UINT64_MAX || !isfinite(yaw) || !scene->physics_paused ||
      scene->simulation.enabled || scene->simulation.completed_ticks ||
      scene->simulation.accumulator || input->observer ||
      !vkr_scene_entity_alive(scene, entity)) {
    if (error)
      *error = "Player requires an unbound paused/reset scene and input";
    return false_v;
  }
  const VkrComponentTypeId existing =
      vkr_entity_find_component(scene->world, "VkrPlayerState");
  VkrPhysicsCharacterState existing_motor;
  if ((existing != VKR_COMPONENT_TYPE_INVALID &&
       vkr_entity_has_component(scene->world, entity, existing)) ||
      vkr_scene_character_get_state(scene, entity, &existing_motor, NULL)) {
    if (error)
      *error = "Player behavior/motor is already attached";
    return false_v;
  }
  *player = (VkrGameplayPlayer){.scene = scene,
                                .input = input,
                                .entity = entity,
                                .instance_id = instance_id - 1,
                                .spawn_yaw = yaw,
                                .render_yaw = yaw};
  player->component = vkr_entity_register_component_once(
      scene->world, "VkrPlayerState", sizeof(VkrPlayerState),
      AlignOf(VkrPlayerState));
  VkrPlayerState state = {0};
  if (player->component == VKR_COMPONENT_TYPE_INVALID ||
      !vkr_entity_add_component(scene->world, entity, player->component,
                                &state)) {
    goto fail;
  }
  VkrPhysicsCharacterDesc motor = vkr_physics_character_default();
  if (!vkr_scene_character_create(scene, entity, &motor, error)) {
    goto remove_component;
  }
  const VkrCameraRigConfig camera = {.eye_height = 1.6f,
                                     .distance = 4,
                                     .shoulder_offset = 0.6f,
                                     .shoulder_height = 0,
                                     .sweep_radius = 0.2f,
                                     .pitch_limit = PLAYER_PITCH_LIMIT};
  vkr_camera_rig_initialize(&player->camera, &camera);
  const VkrSceneSimulationCallbacks callbacks = {.before_physics =
                                                     player_before,
                                                 .after_physics = player_after,
                                                 .reset = player_reset,
                                                 .context = player};
  if (!vkr_scene_simulation_configure(scene, &callbacks, error)) {
    goto remove_motor;
  }
  player_reset(scene, player);
  if (player->commands.faulted) {
    if (error) {
      *error = "Player animation initialization failed";
    }
    vkr_scene_simulation_configure(scene, NULL, NULL);
    goto remove_motor;
  }
  if (!input_observe(input, player_observe, player)) {
    vkr_scene_simulation_configure(scene, NULL, NULL);
    goto remove_motor;
  }
  return true_v;
remove_motor:
  vkr_scene_character_destroy(scene, entity, NULL);
remove_component:
  vkr_entity_remove_component(scene->world, entity, player->component);
fail:
  *player = (VkrGameplayPlayer){0};
  return false_v;
}

void vkr_gameplay_player_shutdown(VkrGameplayPlayer *player) {
  if (!player || !player->scene || player->scene->simulation.active) {
    return;
  }
  input_unobserve(player->input, player);
  VkrScene *scene = player->scene;
  vkr_scene_physics_set_paused(scene, true_v);
  // Detach only this client's borrowed callback context. Teardown does not seek
  // or reset unrelated native state, and the scene owner destroys the entity.
  vkr_scene_simulation_detach(scene, player);
  vkr_scene_character_destroy(scene, player->entity, NULL);
  vkr_entity_remove_component(scene->world, player->entity, player->component);
  *player = (VkrGameplayPlayer){0};
}

float64_t vkr_gameplay_player_frame(VkrGameplayPlayer *player, float64_t now,
                                    bool8_t active) {
  if (!player || !player->scene || !isfinite(now)) {
    return 0;
  }
  active = active && !player->scene->physics_paused &&
           !player->scene->physics_disabled &&
           !player->scene->simulation.faulted;
  if (!active || !player->active) {
    VkrPlayerState *state = player_state(player);
    if (state) {
      if (player->active && !active) {
        player->render_yaw = state->yaw;
        player->render_pitch = state->pitch;
      }
      state->held = 0;
      vkr_weapon_reload_cancel(&state->weapon, state->reload);
    }
    player->commands = (VkrGameplayInput){
        .consumed_tick = vkr_scene_simulation_completed_ticks(player->scene)};
    player->epoch = now - vkr_scene_physics_time(player->scene) -
                    vkr_scene_physics_debt(player->scene);
    player->last_frame_time = now;
    player->active = active;
    return 0;
  }
  const float64_t dt = now - player->last_frame_time;
  player->last_frame_time = now;
  if (dt < 0 || !isfinite(dt)) {
    player->commands.faulted = true_v;
    return 0;
  }
  return dt;
}

static bool8_t player_camera_sweep(Vec3 origin, Vec3 displacement,
                                   float32_t radius, void *context,
                                   float32_t *fraction) {
  VkrGameplayPlayer *player = context;
  const uint64_t ignored = player->entity.u64;
  const VkrPhysicsQueryFilter filter = {
      .mask = UINT16_MAX, .ignored_entities = &ignored, .ignored_count = 1};
  VkrPhysicsRayHit hit;
  bool8_t has_hit = false_v;
  if (!vkr_scene_physics_sweep_sphere(player->scene, origin, displacement,
                                      radius, &filter, &hit, &has_hit)) {
    return false_v;
  }
  *fraction = has_hit ? hit.fraction : 1.0f;
  return true_v;
}

bool8_t vkr_gameplay_player_camera(VkrGameplayPlayer *player,
                                   VkrCameraRigPose *pose) {
  if (!player || !player->scene) {
    return false_v;
  }
  const float32_t alpha =
      player->scene->physics_paused
          ? 1
          : (float32_t)Min(1.0, vkr_scene_physics_debt(player->scene) /
                                    VKR_SCENE_SIMULATION_FIXED_DT);
  const Vec3 foot = vec3_add(
      player->previous_foot,
      vec3_scale(vec3_sub(player->current_foot, player->previous_foot), alpha));
  return vkr_camera_rig_evaluate(&player->camera, foot, player->render_yaw,
                                 player->render_pitch, player_camera_sweep,
                                 player, pose);
}
