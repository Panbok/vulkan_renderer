#pragma once

#include "editor_ui.h"

/*
 * The environment of a container in one place (ADR-098): the sky light,
 * atmosphere, clouds, height fog, volumetric fog, post process, time of day
 * and the atmosphere's sun and moon lights. The Environment window shows
 * where each comes from (the container or the World) and edits it, and an
 * environment preset (`.environment`, a content-root document) captures
 * them all. Applying a preset copies its values into the container, as
 * component presets do: World inheritance, not the preset, shares values
 * across levels.
 */

#define VKR_EDITOR_ENVIRONMENT_EXTENSION ".environment"
#define VKR_EDITOR_ENVIRONMENT_DIRECTORY "assets/environments"

typedef enum VkrEditorEnvironmentPart {
  VKR_EDITOR_ENVIRONMENT_SKY_LIGHT = 0,
  VKR_EDITOR_ENVIRONMENT_ATMOSPHERE,
  VKR_EDITOR_ENVIRONMENT_CLOUDS,
  VKR_EDITOR_ENVIRONMENT_FOG,
  VKR_EDITOR_ENVIRONMENT_VOLUMETRIC_FOG,
  VKR_EDITOR_ENVIRONMENT_POST_PROCESS,
  VKR_EDITOR_ENVIRONMENT_TIME_OF_DAY,
  VKR_EDITOR_ENVIRONMENT_SUN,
  VKR_EDITOR_ENVIRONMENT_MOON,
  VKR_EDITOR_ENVIRONMENT_PART_COUNT,
} VkrEditorEnvironmentPart;

/** A part's key in documents and operations: the component type's name,
    or "sun" and "moon". */
const char *vkr_editor_environment_key(VkrEditorEnvironmentPart part);
const char *vkr_editor_environment_label(VkrEditorEnvironmentPart part);
/** The part's value type; the sun and moon are directional lights. */
const VkrTypeDesc *vkr_editor_environment_type(VkrEditorEnvironmentPart part);

/** Where one part of a container's environment comes from. */
typedef struct VkrEditorEnvironmentSource {
  /** The entity holding the value, and the scene that owns it (the
      container, or the World it inherits); invalid when the part is
      unset and its defaults apply. */
  VkrEntityId entity;
  const VkrScene *owner;
  bool8_t from_world;
  _Alignas(16) uint8_t value[VKR_TYPE_VALUE_MAX];
} VkrEditorEnvironmentSource;

/** Resolves every part for `container` (0 the primary scene, 1 to
    VKR_SCENE_ADDITIVE_MAX an added scene, VKR_SCENE_WORLD_ROOT_ID the
    World) as the renderer does. False when the container is not loaded. */
bool8_t vkr_editor_environment_resolve(
    const VkrSampleUiFrame *frame, uint16_t container,
    VkrEditorEnvironmentSource out[VKR_EDITOR_ENVIRONMENT_PART_COUNT]);

/** An environment preset: the parts it holds (bit per part) and their
    values. */
typedef struct VkrEditorEnvironmentPreset {
  uint32_t parts;
  _Alignas(
      16) uint8_t values[VKR_EDITOR_ENVIRONMENT_PART_COUNT][VKR_TYPE_VALUE_MAX];
} VkrEditorEnvironmentPreset;

/** The preset of a container's resolved environment: every part that is
    set, whether in the container or the World. */
void vkr_editor_environment_capture(
    const VkrEditorEnvironmentSource sources[VKR_EDITOR_ENVIRONMENT_PART_COUNT],
    VkrEditorEnvironmentPreset *out);

/** `.environment` text: {"version": 1, "parts": {"<key>": {values}}}. */
bool8_t vkr_editor_environment_write(const VkrEditorEnvironmentPreset *preset,
                                     VkrAllocator *allocator, String8 *out);
/** Reads `.environment` text; an unknown key or an invalid value fails
    with `error`. */
bool8_t vkr_editor_environment_read(String8 text, Arena *arena,
                                    VkrEditorEnvironmentPreset *out,
                                    char *error, uint32_t capacity);

/** The edits that copy `preset` into `container`, as one batch: each part
    onto the container's own entity that holds it, else onto a new entity
    named for the part. The time of day applies to the World only, and a
    sky light, which loads with its scene, only where one exists. Returns
    the item count (at most `capacity`); `skipped` gets the parts left out. */
uint32_t vkr_editor_environment_apply_items(
    const VkrSampleUiFrame *frame, uint16_t container,
    const VkrEditorEnvironmentPreset *preset, VkrSampleEditBatchItem *items,
    uint32_t capacity, uint32_t *skipped);

/** The Environment window body. */
void vkr_editor_environment_window_build(VkrEditorUi *editor,
                                         const VkrSampleUiFrame *frame,
                                         VkrUiRect bounds);
