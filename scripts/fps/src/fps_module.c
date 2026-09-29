/* The FPS sample game as a C script module (ADR-079): an `fps_player` entity
 * or the resolved Player Start spawns a character with a hitscan rifle, and
 * an optional `fps_weapon` entity follows the player's animated hand. */
#include "fps_module.h"
#include "fps_player.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define FPS_TYPE_OFFSET(type, field) (uint32_t)offsetof(type, field)
#define FPS_DEGREES_PER_RADIAN 57.2957795131f
#define FPS_CREATED_MAX 8u

// =============================================================================
// Script components
// =============================================================================

static const VkrPropertyDesc s_player_properties[] = {
    {.name = "move_speed",
     .label = "Move speed",
     .unit = "m/s",
     .offset = FPS_TYPE_OFFSET(FpsPlayerSettings, move_speed),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 50.0f,
     .step = 0.05f},
    {.name = "crouch_speed",
     .label = "Crouch speed",
     .unit = "m/s",
     .offset = FPS_TYPE_OFFSET(FpsPlayerSettings, crouch_speed),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 50.0f,
     .step = 0.05f},
    {.name = "jump_speed",
     .label = "Jump speed",
     .tooltip = "Vertical takeoff speed",
     .unit = "m/s",
     .offset = FPS_TYPE_OFFSET(FpsPlayerSettings, jump_speed),
     .kind = VKR_PROPERTY_F32,
     .min = 0.0f,
     .max = 50.0f,
     .step = 0.05f},
    {.name = "magazine",
     .label = "Magazine",
     .tooltip = "Rounds per magazine",
     .offset = FPS_TYPE_OFFSET(FpsPlayerSettings, magazine),
     .kind = VKR_PROPERTY_U32,
     .min = 1.0f,
     .max = 1000.0f},
    {.name = "reserve",
     .label = "Reserve",
     .tooltip = "Reserve rounds at spawn and reset",
     .offset = FPS_TYPE_OFFSET(FpsPlayerSettings, reserve),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 100000.0f},
};

static void fps_player_defaults(void *value) {
  *(FpsPlayerSettings *)value = fps_player_settings_default();
}

const VkrTypeDesc fps_player_type = {
    .name = "fps_player",
    .label = "FPS player",
    .category = "Scripts",
    .properties = s_player_properties,
    .property_count = ArrayCount(s_player_properties),
    .size = sizeof(FpsPlayerSettings),
    .align = AlignOf(FpsPlayerSettings),
    .defaults = fps_player_defaults,
};

static const VkrPropertyDesc s_weapon_properties[] = {
    {.name = "bone",
     .label = "Bone",
     .tooltip = "Node of the player's animation skeleton that holds the "
                "weapon outside first person",
     .offset = FPS_TYPE_OFFSET(FpsWeaponBinding, bone),
     .kind = VKR_PROPERTY_U32,
     .min = 0.0f,
     .max = 65535.0f},
};

const VkrTypeDesc fps_weapon_type = {
    .name = "fps_weapon",
    .label = "FPS weapon",
    .category = "Scripts",
    .properties = s_weapon_properties,
    .property_count = ArrayCount(s_weapon_properties),
    .size = sizeof(FpsWeaponBinding),
    .align = AlignOf(FpsWeaponBinding),
};

static const VkrTypeDesc *const s_types[] = {&fps_player_type,
                                             &fps_weapon_type};

// =============================================================================
// Session state
// =============================================================================

typedef struct FpsModule {
  FpsPlayer player;
  /* Hidden in first person: the authored player or the spawned capsule. */
  VkrEntityId visual;
  VkrEntityId weapon;
  /* The authored player's pose follows the motor and look through an
   * evaluated transform; a spawned player's motor moves its root itself. */
  bool8_t authored;
  bool8_t visual_visible;
  /* Entities this session created, destroyed at stop in reverse order. */
  VkrEntityId created[FPS_CREATED_MAX];
  uint32_t created_count;
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

static VkrEntityId fps_create(const VkrScriptSession *session,
                              FpsModule *module, const char *name,
                              Vec3 position) {
  const VkrScriptApi *api = session->api;
  if (module->created_count == FPS_CREATED_MAX) {
    return VKR_ENTITY_ID_INVALID;
  }
  const VkrEntityId entity = api->create_entity(session->scene, NULL);
  if (!entity.u64) {
    return VKR_ENTITY_ID_INVALID;
  }
  module->created[module->created_count++] = entity;
  const String8 label = {.str = (uint8_t *)name, .length = strlen(name)};
  if (!api->set_name(session->scene, entity, label) ||
      !api->set_transform(session->scene, entity, position, vkr_quat_identity(),
                          vec3_one())) {
    return VKR_ENTITY_ID_INVALID;
  }
  return entity;
}

static void fps_destroy_created(const VkrScriptSession *session,
                                FpsModule *module) {
  while (module->created_count) {
    session->api->destroy_entity(session->scene,
                                 module->created[--module->created_count]);
  }
}

/* A small training platform above the loaded Bistro world, with a Player
 * Start on it. Its collision belongs to these explicit bodies; the decorative
 * city is not implicitly promoted to collision geometry. */
static bool8_t fps_create_training_platform(const VkrScriptSession *session,
                                            FpsModule *module,
                                            const char **error) {
  const VkrScriptApi *api = session->api;
  VkrScene *scene = session->scene;
  for (uint32_t i = 0; i < 4; ++i) {
    Vec3 position;
    VkrSceneShapeConfig shape = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
    if (i == 0) {
      position = vec3_new(-28, 19.5f, 0);
      shape.dimensions = vec3_new(16, 1, 16);
      shape.color = vec4_new(.25f, .3f, .32f, 1);
    } else if (i == 3) {
      position = vec3_new(-24, 20.15f, 0);
      shape.dimensions = vec3_new(2, .3f, 2);
      shape.color = vec4_new(.55f, .6f, .65f, 1);
    } else {
      position = vec3_new(-28 + ((int32_t)i - 2) * 2.0f, 20.6f, -2);
      shape.dimensions = vec3_new(1, 1.2f, 1);
      shape.color = vec4_new(.85f, .3f, .12f, 1);
    }
    const VkrEntityId entity = fps_create(
        session, module, i == 0 ? "TrainingPlatform" : "Target", position);
    if (!entity.u64 ||
        !api->set_shape(scene, session->assets, entity, &shape, NULL)) {
      *error = "Training platform allocation failed";
      return false_v;
    }
    VkrScenePhysicsSnapshot body = api->physics_default();
    body.body.motion =
        i == 0 || i == 3 ? VKR_PHYSICS_STATIC : VKR_PHYSICS_DYNAMIC;
    body.colliders[0].half_extent = vec3_scale(shape.dimensions, .5f);
    if (!api->physics_apply(scene, entity, &body, error)) {
      return false_v;
    }
  }
  const ScenePlayerStart start = {.enabled = true_v};
  const VkrEntityId entity =
      fps_create(session, module, "PlayerStart", vec3_new(-28, 20, 3));
  if (!entity.u64 ||
      !api->set_typed(scene, entity, api->player_start_type, &start)) {
    *error = "Training Player Start allocation failed";
    return false_v;
  }
  api->update_transforms(scene);
  return true_v;
}

/* A capsule-sized player spawned at the Player Start when no entity carries
 * `fps_player`. */
static VkrEntityId fps_spawn_player(const VkrScriptSession *session,
                                    FpsModule *module, Vec3 foot) {
  const VkrScriptApi *api = session->api;
  VkrScene *scene = session->scene;
  const VkrEntityId root = fps_create(session, module, "Player", foot);
  const VkrEntityId visual =
      root.u64 ? fps_create(session, module, "PlayerBody", vec3_new(0, .9f, 0))
               : VKR_ENTITY_ID_INVALID;
  VkrSceneShapeConfig shape = VKR_SCENE_SHAPE_CONFIG_DEFAULT;
  shape.dimensions = vec3_new(.6f, 1.8f, .6f);
  shape.color = vec4_new(.15f, .45f, .85f, 1);
  if (!visual.u64 ||
      !api->set_shape(scene, session->assets, visual, &shape, NULL)) {
    return VKR_ENTITY_ID_INVALID;
  }
  api->set_parent(scene, visual, root);
  api->update_transforms(scene);
  module->visual = visual;
  return root;
}

// =============================================================================
// Hooks
// =============================================================================

static void fps_stop(const VkrScriptSession *session, void *state) {
  FpsModule *module = state;
  const VkrScriptApi *api = session->api;
  fps_player_shutdown(&module->player);
  if (module->authored) {
    api->set_evaluated_transform(session->scene, module->visual, NULL);
    api->set_visibility(session->scene, module->visual, module->visual_visible,
                        true_v);
  }
  if (module->weapon.u64) {
    api->set_evaluated_transform(session->scene, module->weapon, NULL);
  }
  fps_destroy_created(session, module);
  api->update_transforms(session->scene);
  *module = (FpsModule){0};
}

static VkrScriptStart fps_fail(const VkrScriptSession *session,
                               FpsModule *module, const char *error,
                               const char **out_error) {
  fps_stop(session, module);
  *out_error = error;
  return VKR_SCRIPT_START_FAILED;
}

static VkrScriptStart fps_start(const VkrScriptSession *session, void *state,
                                const char **error) {
  FpsModule *module = state;
  const VkrScriptApi *api = session->api;
  VkrScene *scene = session->scene;
  VkrEntityId players[2];
  VkrEntityId weapons[2];
  const uint32_t player_count =
      api->find_typed(scene, &fps_player_type, players, ArrayCount(players));
  const uint32_t weapon_count =
      api->find_typed(scene, &fps_weapon_type, weapons, ArrayCount(weapons));
  if (player_count > 1 || weapon_count > 1) {
    *error = "A scene holds at most one fps_player and one fps_weapon";
    return VKR_SCRIPT_START_FAILED;
  }
  if (weapon_count && !player_count) {
    *error = "An fps_weapon needs an fps_player entity";
    return VKR_SCRIPT_START_FAILED;
  }
  const char *failure = NULL;
  if (!player_count && (session->flags & VKR_SCRIPT_SESSION_SAMPLE_CONTENT) &&
      !fps_create_training_platform(session, module, &failure)) {
    return fps_fail(session, module, failure, error);
  }
  Mat4 start;
  const bool8_t has_start = api->player_start(scene, &start);
  if (!player_count && !has_start) {
    return VKR_SCRIPT_START_IDLE;
  }

  FpsPlayerConfig config = {.api = api,
                            .scene = scene,
                            .input = session->input,
                            .settings = fps_player_settings_default(),
                            .weapon_bone = UINT32_MAX,
                            .instance_id = session->instance_id};
  if (player_count) {
    Mat4 authored;
    config.entity = players[0];
    config.settings = *(const FpsPlayerSettings *)api->get_typed(
        scene, players[0], &fps_player_type);
    config.yaw = api->world_matrix(scene, players[0], &authored)
                     ? fps_matrix_yaw(&authored)
                     : -1.57079632679f;
    if (has_start) {
      config.spawn_foot = fps_matrix_position(&start);
      config.has_spawn = true_v;
      config.yaw = fps_matrix_yaw(&start);
    }
    module->authored = true_v;
    module->visual = players[0];
    module->visual_visible = api->entity_visible(scene, players[0]);
  } else {
    config.entity =
        fps_spawn_player(session, module, fps_matrix_position(&start));
    config.yaw = fps_matrix_yaw(&start);
    if (!config.entity.u64) {
      return fps_fail(session, module, "Player spawn allocation failed", error);
    }
  }
  if (weapon_count) {
    const FpsWeaponBinding *binding =
        api->get_typed(scene, weapons[0], &fps_weapon_type);
    const VkrAnimationAsset *asset =
        api->animation_asset(api->animation_player(scene, config.entity));
    if (!asset || binding->bone >= asset->node_count) {
      return fps_fail(session, module,
                      "The fps_weapon bone is outside the player's animation",
                      error);
    }
    config.weapon_bone = binding->bone;
    module->weapon = weapons[0];
  }

  // Attachment acquires the presentation overrides the present hook updates.
  Mat4 pose;
  if (module->authored &&
      (!api->world_matrix(scene, config.entity, &pose) ||
       !api->set_evaluated_transform(scene, config.entity, &pose) ||
       (module->weapon.u64 &&
        !api->set_evaluated_transform(scene, module->weapon, &pose)))) {
    return fps_fail(session, module, "Player presentation override failed",
                    error);
  }
  if (!fps_player_attach(&module->player, &config, &failure)) {
    return fps_fail(session, module, failure, error);
  }
  api->log(LOG_LEVEL_INFO,
           "Gameplay ready: WASD move, mouse look, left click fire, R reload, "
           "Space jump, Ctrl crouch, V camera mode, Backspace reset, "
           "Tab/Escape release or capture mouse.");
  return VKR_SCRIPT_START_ACTIVE;
}

static bool8_t fps_before_physics(const VkrScriptSession *session, void *state,
                                  uint64_t tick, const char **error) {
  (void)session;
  FpsModule *module = state;
  return fps_player_before_physics(&module->player, tick, error);
}

static bool8_t fps_after_physics(const VkrScriptSession *session, void *state,
                                 uint64_t tick, const char **error) {
  (void)session;
  (void)tick;
  (void)error;
  FpsModule *module = state;
  fps_player_after_physics(&module->player);
  return true_v;
}

static void fps_reset(const VkrScriptSession *session, void *state) {
  (void)session;
  FpsModule *module = state;
  fps_player_reset(&module->player);
}

static void fps_frame(const VkrScriptSession *session, void *state,
                      VkrScriptFrame *frame) {
  (void)session;
  FpsModule *module = state;
  frame->scene_delta =
      fps_player_frame(&module->player, frame->now, frame->input_focused);
}

/* Presentation overrides never modify the authored spawn or weapon transform;
 * both components were acquired at start and updates reuse their storage. */
static void fps_visuals(const VkrScriptSession *session, FpsModule *module,
                        const FpsCameraRigPose *camera) {
  const VkrScriptApi *api = session->api;
  VkrScene *scene = session->scene;
  FpsPlayer *player = &module->player;
  const float32_t alpha =
      api->physics_paused(scene)
          ? 1.0f
          : (float32_t)Min(1.0, api->physics_debt(scene) /
                                    VKR_SCENE_SIMULATION_FIXED_DT);
  const Vec3 foot = vec3_add(
      player->previous_foot,
      vec3_scale(vec3_sub(player->current_foot, player->previous_foot), alpha));
  const VkrQuat rotation = vkr_quat_from_axis_angle(
      vec3_new(0, 1, 0), -player->render_yaw - 1.57079632679f);
  const Mat4 root = mat4_mul(mat4_translate(foot), vkr_quat_to_mat4(rotation));
  api->set_evaluated_transform(scene, player->entity, &root);
  if (!module->weapon.u64) {
    api->update_transforms(scene);
    return;
  }
  VkrAnimationPlayer *animation = api->animation_player(scene, player->entity);
  const VkrAnimationAsset *asset = api->animation_asset(animation);
  if (!asset || player->weapon_bone >= asset->node_count) {
    api->update_transforms(scene);
    return;
  }
  const Mat4 bone = api->animation_global_pose(animation)[player->weapon_bone];
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
  api->set_evaluated_transform(scene, module->weapon, &weapon);
  api->update_transforms(scene);
}

static void fps_present(const VkrScriptSession *session, void *state,
                        const VkrScriptFrame *frame, VkrScriptView *view) {
  FpsModule *module = state;
  FpsPlayer *player = &module->player;
  const VkrScriptApi *api = session->api;
  const bool8_t playing = frame->simulation_running && frame->camera_available;
  api->set_visibility(
      session->scene, module->visual,
      !playing || player->camera.mode != FPS_CAMERA_RIG_FIRST_PERSON, true_v);
  FpsCameraRigPose pose;
  const bool8_t have_pose = fps_player_camera(player, &pose);
  if (playing && have_pose) {
    view->camera_valid = true_v;
    view->camera_position = pose.position;
    view->camera_yaw_degrees = player->render_yaw * FPS_DEGREES_PER_RADIAN;
    view->camera_pitch_degrees = pose.pitch * FPS_DEGREES_PER_RADIAN;
  }
  if (module->authored) {
    fps_visuals(session, module, playing && have_pose ? &pose : NULL);
  }
  const FpsPlayerState *player_state =
      api->get_state(session->scene, player->entity, player->component);
  if (player_state) {
    snprintf(view->hud, sizeof(view->hud),
             "Ammo %u / %u%s  Hits %llu\nWASD move | Mouse fire/look | R "
             "reload\nSpace jump | Ctrl crouch | V camera\nTab mouse | "
             "Backspace reset",
             player_state->weapon.magazine_rounds, player_state->reserve_rounds,
             player_state->weapon.reloading ? "  Reloading" : "",
             (unsigned long long)player->hits);
  }
}

static const VkrScriptModuleDesc s_module = {
    .abi_version = VKR_SCRIPT_ABI_VERSION,
    .size = sizeof(VkrScriptModuleDesc),
    .name = "fps",
    .types = s_types,
    .type_count = ArrayCount(s_types),
    .state_size = sizeof(FpsModule),
    .state_align = AlignOf(FpsModule),
    .start = fps_start,
    .stop = fps_stop,
    .before_physics = fps_before_physics,
    .after_physics = fps_after_physics,
    .reset = fps_reset,
    .frame = fps_frame,
    .present = fps_present,
};

VKR_SCRIPT_EXPORT const VkrScriptModuleDesc *
vkr_script_module_fps(const VkrScriptApi *api) {
  if (!api || api->version != VKR_SCRIPT_ABI_VERSION ||
      api->size < sizeof(VkrScriptApi)) {
    return NULL;
  }
  return &s_module;
}
