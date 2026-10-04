/* The FPS sample game as a C script module (ADR-079): an `fps_player` entity
 * or the resolved Player Start spawns a character with a hitscan rifle, and
 * an optional `fps_weapon` entity follows the player's animated hand.
 *
 * Everything this module spawns, the character, its runtime state and the
 * render poses it publishes are in the instance's ledger, so the host
 * releases them when Play stops or a start fails part way. */
#include "fps_module.h"
#include "fps_mannequin.h"
#include "fps_player.h"
#include <math.h>
#include <stdio.h>

#define FPS_DEGREES_PER_RADIAN 57.2957795131f

const char *const fps_camera_mode_names[] = {"first_person", "third_person",
                                             "shoulder", NULL};

VKR_COMPONENT_DEFINE(FpsPlayerSettings, fps_player, "FPS player",
                     FPS_PLAYER_FIELDS)
VKR_COMPONENT_DEFINE(FpsWeaponBinding, fps_weapon, "FPS weapon",
                     FPS_WEAPON_FIELDS)

// =============================================================================
// Module data
// =============================================================================

typedef struct FpsModule {
  FpsPlayer player;
  /* Hidden in first person: the authored player's own model, or the body
   * this instance spawned (the mannequin, or a capsule-sized box when its
   * content is missing). */
  VkrEntity visual;
  VkrEntity weapon;
  /* The authored player entity is its own visual; its visibility returns
   * at stop. */
  bool8_t visual_authored;
  bool8_t visual_visible;
  /* The spawned mannequin is loading; a failed load becomes the box. */
  bool8_t body_loading;
} FpsModule;

static Vec3 fps_matrix_position(const Mat4 *world) {
  return vec3_new(world->elements[12], world->elements[13],
                  world->elements[14]);
}

/* Player yaw of a pose's -Z axis: yaw zero faces +X and -pi/2 faces -Z. */
static float32_t fps_matrix_yaw(const Mat4 *world) {
  const float32_t x = -world->elements[8];
  const float32_t z = -world->elements[10];
  return x * x + z * z > 1e-12f ? atan2f(z, x) : -1.57079632679f;
}

/* An entity in the played container, released with the instance. */
static VkrEntity fps_spawn(VkrCtx *ctx, const char *name, Vec3 position,
                           VkrEntity parent) {
  return vkr_spawn(ctx, &(VkrSpawnDesc){
                            .name = name,
                            .parent = parent,
                            .transform = {.position = position},
                            .container = vkr_container_active(ctx),
                        });
}

/* A small training platform above the loaded Bistro world, with a Player
 * Start on it. Its collision belongs to these explicit bodies; the decorative
 * city is not implicitly promoted to collision geometry. */
static bool8_t fps_create_training_platform(VkrCtx *ctx) {
  for (uint32_t i = 0; i < 4; ++i) {
    Vec3 position;
    VkrShapeDesc shape;
    if (i == 0) {
      position = vec3_new(-28, 19.5f, 0);
      shape = (VkrShapeDesc){.size = vec3_new(16, 1, 16),
                             .color = vec4_new(.25f, .3f, .32f, 1)};
    } else if (i == 3) {
      position = vec3_new(-24, 20.15f, 0);
      shape = (VkrShapeDesc){.size = vec3_new(2, .3f, 2),
                             .color = vec4_new(.55f, .6f, .65f, 1)};
    } else {
      position = vec3_new(-28 + ((int32_t)i - 2) * 2.0f, 20.6f, -2);
      shape = (VkrShapeDesc){.size = vec3_new(1, 1.2f, 1),
                             .color = vec4_new(.85f, .3f, .12f, 1)};
    }
    const VkrEntity entity = fps_spawn(
        ctx, i == 0 ? "TrainingPlatform" : "Target", position, VKR_ENTITY_NONE);
    VkrBodyDesc body = vkr_body_default(ctx);
    body.motion = i == 0 || i == 3 ? VKR_MOTION_STATIC : VKR_MOTION_DYNAMIC;
    body.half_extent = vec3_scale(shape.size, .5f);
    if (!vkr_entity_valid(entity) || !vkr_set_shape(ctx, entity, &shape) ||
        !vkr_set_body(ctx, entity, &body)) {
      vkr_fail(ctx, "Training platform allocation failed: %s",
               vkr_last_error(ctx));
      return false_v;
    }
  }
  const VkrComponentDesc *player_start =
      vkr_component_named(ctx, "player_start");
  const VkrEntity start =
      fps_spawn(ctx, "PlayerStart", vec3_new(-28, 20, 3), VKR_ENTITY_NONE);
  if (!player_start || !vkr_entity_valid(start) ||
      !vkr_component_set(ctx, start, player_start, NULL)) {
    vkr_fail(ctx, "Training Player Start allocation failed");
    return false_v;
  }
  return true_v;
}

/* A capsule-sized box standing in for the mannequin. */
static bool8_t fps_body_box(VkrCtx *ctx, VkrEntity body) {
  const VkrShapeDesc shape = {.size = vec3_new(.6f, 1.8f, .6f),
                              .color = vec4_new(.15f, .45f, .85f, 1)};
  return vkr_set_transform(ctx, body,
                           &(VkrTRS){.position = vec3_new(0, .9f, 0)}) &&
         vkr_set_shape(ctx, body, &shape);
}

/* The default character's body under `parent`: the engine mannequin with
 * its animation bank, which loads in the background (`*loading`), or a
 * capsule-sized box when that content is missing. Its animation, when it
 * has one, drives locomotion. */
static VkrEntity fps_spawn_body(VkrCtx *ctx, VkrEntity parent,
                                bool8_t *loading) {
  *loading = false_v;
  const VkrEntity body = fps_spawn(ctx, "PlayerBody", vec3_zero(), parent);
  if (!vkr_entity_valid(body)) {
    return VKR_ENTITY_NONE;
  }
  if (vkr_spawn_model(ctx, body, FPS_MANNEQUIN_MESH, FPS_MANNEQUIN_ANIMATION)) {
    *loading = true_v;
    return body;
  }
  vkr_log(ctx, VKR_LOG_WARN,
          "Default mannequin unavailable (%s); the player uses a box",
          vkr_last_error(ctx));
  return fps_body_box(ctx, body) ? body : VKR_ENTITY_NONE;
}

// =============================================================================
// Hooks
// =============================================================================

static void fps_stop(VkrCtx *ctx, FpsModule *module) {
  fps_player_shutdown(ctx, &module->player);
  if (module->visual_authored) {
    vkr_set_visible(ctx, module->visual, module->visual_visible);
  }
}

static void fps_start(VkrCtx *ctx, FpsModule *module) {
  const VkrContainer active = vkr_container_active(ctx);
  VkrEntity players[2];
  VkrEntity weapons[2];
  const uint32_t player_count =
      vkr_find_in(ctx, active, fps_player_type(), players, ArrayCount(players));
  const uint32_t weapon_count =
      vkr_find_in(ctx, active, fps_weapon_type(), weapons, ArrayCount(weapons));
  if (player_count > 1 || weapon_count > 1) {
    vkr_fail(ctx, "A scene holds at most one fps_player and one fps_weapon");
    return;
  }
  if (weapon_count && !player_count) {
    vkr_fail(ctx, "An fps_weapon needs an fps_player entity");
    return;
  }
  if (!player_count && vkr_option(ctx, "gameplay") &&
      !fps_create_training_platform(ctx)) {
    return;
  }
  Mat4 start;
  const bool8_t has_start = vkr_player_start(ctx, &start);
  if (!player_count && !has_start) {
    vkr_disable(ctx);
    return;
  }

  FpsPlayerConfig config = {.settings = fps_player_settings_default(),
                            .weapon_bone = UINT32_MAX,
                            .instance_id = 1u};
  if (player_count) {
    Mat4 authored;
    config.entity = players[0];
    config.settings = *fps_player_get(ctx, players[0]);
    config.yaw = vkr_world_matrix(ctx, players[0], &authored)
                     ? fps_matrix_yaw(&authored)
                     : -1.57079632679f;
    if (has_start) {
      config.spawn_foot = fps_matrix_position(&start);
      config.has_spawn = true_v;
      config.yaw = fps_matrix_yaw(&start);
    }
    if (vkr_anim_id(ctx, players[0]) || vkr_has_visual(ctx, players[0])) {
      // The authored model is the player's body.
      module->visual = players[0];
      module->visual_authored = true_v;
      module->visual_visible = vkr_visible(ctx, players[0]);
    } else {
      module->visual = fps_spawn_body(ctx, players[0], &module->body_loading);
      if (!vkr_entity_valid(module->visual)) {
        vkr_fail(ctx, "Player body allocation failed");
        return;
      }
    }
  } else {
    /* No entity carries `fps_player`: a root the motor moves, at the Player
       Start, with the default body. */
    config.entity =
        fps_spawn(ctx, "Player", fps_matrix_position(&start), VKR_ENTITY_NONE);
    module->visual =
        vkr_entity_valid(config.entity)
            ? fps_spawn_body(ctx, config.entity, &module->body_loading)
            : VKR_ENTITY_NONE;
    config.yaw = fps_matrix_yaw(&start);
    if (!vkr_entity_valid(module->visual)) {
      vkr_fail(ctx, "Player spawn allocation failed");
      return;
    }
  }
  config.visual = module->visual;
  config.visual_loading = module->body_loading;
  if (weapon_count) {
    /* A body still loading has its bone checked when it arrives. */
    config.weapon_bone = fps_weapon_get(ctx, weapons[0])->bone;
    if (!module->body_loading &&
        config.weapon_bone >= vkr_anim_bone_count(ctx, config.visual)) {
      vkr_fail(ctx, "The fps_weapon bone is outside the player's animation");
      return;
    }
    module->weapon = weapons[0];
  }

  // Render poses the late update refreshes; the authored transforms stay.
  Mat4 pose;
  if (!vkr_world_matrix(ctx, config.entity, &pose) ||
      !vkr_set_render_pose(ctx, config.entity, &pose) ||
      (vkr_entity_valid(module->weapon) &&
       !vkr_set_render_pose(ctx, module->weapon, &pose))) {
    vkr_fail(ctx, "Player render pose failed");
    return;
  }
  const char *failure = NULL;
  if (!fps_player_attach(ctx, &module->player, &config, &failure)) {
    vkr_fail(ctx, "%s", failure ? failure : "Player attach failed");
    return;
  }
  vkr_log(ctx, VKR_LOG_INFO,
          "Gameplay ready: WASD move, Shift walk, mouse look, left click "
          "fire, R reload, Space jump, Ctrl crouch, V camera mode, "
          "Backspace reset, Tab/Escape release or capture mouse.");
}

static void fps_fixed_update(VkrCtx *ctx, FpsModule *module) {
  const char *error = NULL;
  if (!fps_player_before_physics(ctx, &module->player, vkr_ticks(ctx) + 1u,
                                 &error)) {
    vkr_fail(ctx, "%s", error);
  }
}

static void fps_late_fixed_update(VkrCtx *ctx, FpsModule *module) {
  fps_player_after_physics(ctx, &module->player);
}

static void fps_input(VkrCtx *ctx, FpsModule *module,
                      const VkrInputEvent *event) {
  fps_player_observe(ctx, &module->player, event);
}

/* The player owns the input clock: the scene advances by the elapsed time
 * its input admitted. */
static void fps_update(VkrCtx *ctx, FpsModule *module, float32_t dt) {
  (void)dt;
  if (module->body_loading &&
      vkr_model_state(ctx, module->visual) != VKR_MODEL_LOADING) {
    module->body_loading = false_v;
    if (vkr_model_state(ctx, module->visual) == VKR_MODEL_FAILED) {
      vkr_log(ctx, VKR_LOG_WARN,
              "Default mannequin failed to load; the player uses a box");
      if (!fps_body_box(ctx, module->visual)) {
        vkr_fail(ctx, "Player body box failed: %s", vkr_last_error(ctx));
        return;
      }
    }
  }
  vkr_set_time_step(ctx, fps_player_frame(ctx, &module->player, vkr_time(ctx),
                                          vkr_input_focused(ctx)));
}

/* Render poses never modify the authored spawn or weapon transform. */
static void fps_visuals(VkrCtx *ctx, FpsModule *module,
                        const FpsCameraRigPose *camera) {
  FpsPlayer *player = &module->player;
  const float32_t alpha = vkr_tick_alpha(ctx);
  fps_player_animate(ctx, player, alpha);
  const Vec3 foot = vec3_add(
      player->previous_foot,
      vec3_scale(vec3_sub(player->current_foot, player->previous_foot), alpha));
  // A body orienting to movement turns on its own; otherwise it follows the
  // latest look.
  const float32_t yaw =
      player->settings.orient_to_movement &&
              player->camera.mode == FPS_CAMERA_RIG_THIRD_PERSON
          ? fps_player_render_facing(player, alpha)
          : player->render_yaw;
  const VkrQuat rotation =
      vkr_quat_from_axis_angle(vec3_new(0, 1, 0), -yaw - 1.57079632679f);
  const Mat4 root = mat4_mul(mat4_translate(foot), vkr_quat_to_mat4(rotation));
  vkr_set_render_pose(ctx, player->entity, &root);
  Mat4 bone;
  if (!vkr_entity_valid(module->weapon) ||
      !vkr_anim_bone_pose(ctx, player->visual, player->weapon_bone, &bone)) {
    return;
  }
  Mat4 weapon;
  if (camera && player->camera.mode == FPS_CAMERA_RIG_FIRST_PERSON) {
    const Vec3 right = vec3_cross(camera->forward, camera->up);
    const Vec3 position =
        vec3_add(camera->position,
                 vec3_add(vec3_scale(right, .22f),
                          vec3_add(vec3_scale(camera->up, -.25f),
                                   vec3_scale(camera->forward, .35f))));
    weapon = mat4_identity();
    weapon.elements[0] = right.x;
    weapon.elements[1] = right.y;
    weapon.elements[2] = right.z;
    weapon.elements[4] = camera->up.x;
    weapon.elements[5] = camera->up.y;
    weapon.elements[6] = camera->up.z;
    weapon.elements[8] = -camera->forward.x;
    weapon.elements[9] = -camera->forward.y;
    weapon.elements[10] = -camera->forward.z;
    weapon.elements[12] = position.x;
    weapon.elements[13] = position.y;
    weapon.elements[14] = position.z;
    if (player->weapon_reference_valid) {
      weapon =
          mat4_mul(weapon, mat4_mul(player->weapon_reference_inverse, bone));
    }
  } else {
    weapon = mat4_mul(root, bone);
  }
  vkr_set_render_pose(ctx, module->weapon, &weapon);
}

static void fps_late_update(VkrCtx *ctx, FpsModule *module, float32_t dt) {
  (void)dt;
  FpsPlayer *player = &module->player;
  const bool8_t playing = vkr_playing(ctx) && vkr_camera_available(ctx);
  vkr_set_visible(ctx, module->visual,
                  !playing ||
                      player->camera.mode != FPS_CAMERA_RIG_FIRST_PERSON);
  FpsCameraRigPose pose;
  const bool8_t have_pose = fps_player_camera(ctx, player, &pose);
  if (playing && have_pose) {
    vkr_set_camera(ctx, pose.position,
                   player->render_yaw * FPS_DEGREES_PER_RADIAN,
                   pose.pitch * FPS_DEGREES_PER_RADIAN);
  }
  fps_visuals(ctx, module, playing && have_pose ? &pose : NULL);
  const FpsPlayerState *state = fps_player_state(ctx, player);
  if (state) {
    vkr_hud(ctx,
            "Ammo %u / %u%s  Hits %llu\nWASD move | Shift walk | Mouse "
            "fire/look\nR reload | Space jump | Ctrl crouch | V camera\nTab "
            "mouse | Backspace reset",
            state->weapon.magazine_rounds, state->reserve_rounds,
            state->weapon.reloading ? "  Reloading" : "",
            (unsigned long long)player->hits);
  }
}

VKR_MODULE(fps, FpsModule,
           VKR_EXPORT_COMPONENT(fps_player) VKR_EXPORT_COMPONENT(fps_weapon)
               VKR_EXPORT_BEHAVIOR(fps_door),
           .scope = VKR_SCOPE_WORLD, .data_version = 3, .start = fps_start,
           .stop = fps_stop, .update = fps_update,
           .late_update = fps_late_update, .fixed_update = fps_fixed_update,
           .late_fixed_update = fps_late_fixed_update, .input = fps_input)
