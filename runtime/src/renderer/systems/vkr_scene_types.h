/**
 * @file vkr_scene_types.h
 * @brief Type descriptors for scene component types (ADR-076).
 *
 * Each descriptor owns its component's validation, serialization and editor
 * presentation. Editing, loading and publication validate through these
 * tables so every consumer applies the same rules.
 */
#pragma once

#include "core/vkr_type_desc.h"
#include "renderer/systems/vkr_scene_system.h"

/** Authored translation, rotation and scale; cached matrices, hierarchy and
 * dirty state are runtime data outside the property table. */
extern const VkrTypeDesc vkr_scene_transform_type;
extern const VkrTypeDesc vkr_scene_visibility_type;
extern const VkrTypeDesc vkr_scene_point_light_type;
extern const VkrTypeDesc vkr_scene_directional_light_type;
extern const VkrTypeDesc vkr_scene_rectangle_light_type;

/* World components (ADR-076): authored settings stored as ECS components. */
extern const VkrTypeDesc vkr_scene_environment_type;
extern const VkrTypeDesc vkr_scene_atmosphere_type;
extern const VkrTypeDesc vkr_scene_clouds_type;
extern const VkrTypeDesc vkr_scene_fog_type;
extern const VkrTypeDesc vkr_scene_froxel_fog_type;
extern const VkrTypeDesc vkr_scene_fog_box_type;
extern const VkrTypeDesc vkr_scene_post_process_type;
extern const VkrTypeDesc vkr_scene_reflection_probe_type;
extern const VkrTypeDesc vkr_scene_diffuse_volume_type;
extern const VkrTypeDesc vkr_scene_subsurface_type;
extern const VkrTypeDesc vkr_scene_physics_settings_type;
extern const VkrTypeDesc vkr_scene_animation_settings_type;
/** World time of day (ADR-090). */
extern const VkrTypeDesc vkr_scene_time_of_day_type;
extern const VkrTypeDesc vkr_scene_shape_type;
extern const VkrTypeDesc vkr_scene_text_type;
extern const VkrTypeDesc vkr_scene_animation_type;
/** Player Start (ADR-079): the spawn pose a game script reads. */
extern const VkrTypeDesc vkr_scene_player_start_type;
extern const VkrTypeDesc vkr_scene_decal_type;
/* Brushes and their faces (ADR-084). */
extern const VkrTypeDesc vkr_scene_brush_type;
extern const VkrTypeDesc vkr_scene_brush_face_type;
extern const VkrTypeDesc vkr_scene_terrain_type;
/* Population (ADR-084). */
extern const VkrTypeDesc vkr_scene_spline_type;
extern const VkrTypeDesc vkr_scene_spline_point_type;
extern const VkrTypeDesc vkr_scene_spline_mesh_type;
extern const VkrTypeDesc vkr_scene_scatter_type;
/* World partition (ADR-086). */
extern const VkrTypeDesc vkr_scene_world_partition_type;
extern const VkrTypeDesc vkr_scene_always_loaded_type;
extern const VkrTypeDesc vkr_scene_free_placement_type;
extern const VkrTypeDesc vkr_scene_blockout_type;
/* Entity IO (ADR-084). */
extern const VkrTypeDesc vkr_scene_trigger_type;
extern const VkrTypeDesc vkr_scene_relay_type;
extern const VkrTypeDesc vkr_scene_button_type;
extern const VkrTypeDesc vkr_scene_timer_type;
extern const VkrTypeDesc vkr_scene_counter_type;
extern const VkrTypeDesc vkr_scene_mover_type;
extern const VkrTypeDesc vkr_scene_io_connection_type;
/** Tags (ADR-084): categories an object is found by; validation accepts
    only canonical text (SceneTags). */
extern const VkrTypeDesc vkr_scene_tags_type;
/** Read-only SceneMeshInfo rows; not a component type. */
extern const VkrTypeDesc vkr_scene_mesh_info_type;

/* Name of engine text font `font` (SceneTextSettings.font), or NULL past
   the last. */
const char *vkr_scene_text_font_name(uint32_t font);

ScenePostProcess vkr_scene_post_process_defaults(void);

/** Canonical tags from typed text: words split at spaces, tabs, line breaks
 * or commas, each lowercased and given a '#' when it lacks one, repeats
 * dropped, in first-seen order; empty text gives no tags. Fails, writing why
 * to `error` when set and leaving `out` unchanged, for a word with a
 * character outside [a-z0-9_-] after its '#', a '#' alone, a tag longer than
 * SCENE_TAG_LENGTH_MAX or more than SCENE_TAG_COUNT_MAX tags. */
bool8_t vkr_scene_tags_parse(String8 text, SceneTags *out, char *error,
                             uint32_t capacity);
/** Whether canonical `tags` hold the canonical tag `tag`, '#' included. */
bool8_t vkr_scene_tags_has(const SceneTags *tags, String8 tag);
/** The tag after `*cursor` (start at zero), borrowed from `tags`; false
 * past the last. */
bool8_t vkr_scene_tags_next(const SceneTags *tags, uint32_t *cursor,
                            String8 *out);

/** World component types by index, stored and edited generically; NULL past
 * the end. */
const VkrTypeDesc *vkr_scene_world_type(uint32_t index);
/** World component type by its document name, or NULL. */
const VkrTypeDesc *vkr_scene_world_type_named(String8 name);
/** Types that take effect when added to a loaded scene, so documents,
 * overlays and the editor may add them to any entity. Load-baked types
 * (environment, reflection probes, diffuse volume, subsurface) keep their
 * top-level document blocks. */
bool8_t vkr_scene_world_type_live(const VkrTypeDesc *type);

/** Registration boundary for component types owned outside the renderer,
 * such as script or gameplay modules (ADR-076). A registered type joins the
 * world types: documents, overlays, Details, Add component, presets and Cmd
 * paths accept it, and scenes store it generically. Register before any
 * scene initializes and keep the descriptor alive for the process. Fails
 * for a NULL or oversized type, a taken name or a full table
 * (VKR_SCENE_REGISTERED_TYPE_MAX). */
#define VKR_SCENE_REGISTERED_TYPE_MAX 32u
bool8_t vkr_scene_register_world_type(const VkrTypeDesc *type);
/** Registered types in registration order; NULL past the end. */
const VkrTypeDesc *vkr_scene_registered_type(uint32_t index);
bool8_t vkr_scene_world_type_registered(const VkrTypeDesc *type);
