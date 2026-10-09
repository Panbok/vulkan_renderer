#pragma once

#include "editor_brush_grid.h"
#include "editor_ui.h"

#include "core/vkr_type_desc.h"

/*
 * Lighting workbench tools (ADR-100): the bake settings the project's Bake
 * lighting job sends to Bakery, the time-of-day scrubber, the light list
 * and the viewport outlines of lights, reflection probes and look volumes.
 */

/* `lightmap_settings` of a bake request; a zero leaves Bakery's default. */
typedef struct VkrEditorLightmapSettings {
  uint32_t samples;
  uint32_t max_depth;
  uint32_t seed;
  uint32_t page_size;
  float32_t texels_per_unit;
  /* 0 Bakery's default, 1 on, 2 off. */
  uint32_t denoise;
  uint32_t denoise_iterations;
  float32_t indirect_clamp;
} VkrEditorLightmapSettings;

/* `diffuse_settings` of a bake request; a zero leaves Bakery's default. */
typedef struct VkrEditorDiffuseSettings {
  float32_t voxel_size;
  float32_t face_size;
  uint32_t samples;
  uint32_t max_depth;
  uint32_t seed;
  uint32_t photons;
  float32_t photon_radius;
} VkrEditorDiffuseSettings;

typedef struct VkrEditorBakeSettings {
  VkrEditorLightmapSettings lightmap;
  VkrEditorDiffuseSettings diffuse;
} VkrEditorBakeSettings;

extern const VkrTypeDesc vkr_editor_lightmap_settings_type;
extern const VkrTypeDesc vkr_editor_diffuse_settings_type;

struct VkrJsonWriter;
/** Writes `lightmap_settings` and `diffuse_settings` members of a bake
    request, each holding only its nonzero values, and none for a group
    without any. `lightmap_samples` (nonzero) replaces the settings'
    sample count, as Cmd `scene.bake <samples>` asks. */
bool8_t vkr_editor_bake_settings_write(struct VkrJsonWriter *writer,
                                       const VkrEditorBakeSettings *settings,
                                       uint32_t lightmap_samples);

/** The Bake settings window body: both groups as Details rows, and Bake
    lighting. */
void vkr_editor_bake_settings_window_build(VkrEditorUi *editor,
                                           const VkrSampleUiFrame *frame,
                                           VkrUiRect bounds);

/** The Lighting palette's TIME rows: the hour the scene shows, a slider
    that moves the live clock, and Keep, which makes it the World's
    starting hour. */
void vkr_editor_lighting_time_rows(VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   float32_t width, float32_t *y);

/* Outline segments a frame: a point light's three circles and the most a
   selection usually needs besides. */
#define VKR_EDITOR_LIGHTING_LINE_MAX 192u

/** World-space outlines of the selected objects: a point light's range as
    three circles, a spot light's cones, a rectangle light's outline and
    emission arrow, a directional light's arrow, and the boxes of look
    volumes, decals and reflection probes with their blend margins. Writes
    at most `capacity` into `out` and returns the count; with a NULL `out`,
    the count the selection needs. None while scripts run. */
uint32_t vkr_editor_lighting_lines(const VkrEditorUi *editor,
                                   const VkrSampleUiFrame *frame,
                                   VkrEditorBrushGridLine *out,
                                   uint32_t capacity);

/** The Lights window body: light groups with live intensity sliders, the
    selected light's rows, and every light of the scene and the World with
    its enabled state, mobility, group and intensity. */
void vkr_editor_lights_window_build(VkrEditorUi *editor,
                                    const VkrSampleUiFrame *frame,
                                    VkrUiRect bounds);

typedef enum VkrEditorLightKind {
  VKR_EDITOR_LIGHT_DIRECTIONAL = 0,
  VKR_EDITOR_LIGHT_POINT,
  VKR_EDITOR_LIGHT_SPOT,
  VKR_EDITOR_LIGHT_RECT,
} VkrEditorLightKind;

/** A light of the primary scene or the World. */
typedef struct VkrEditorLight {
  const VkrScene *scene;
  VkrEntityId entity;
  VkrEditorLightKind kind;
} VkrEditorLight;

/** The component type a light kind keeps its values in. */
const VkrTypeDesc *vkr_editor_light_type(VkrEditorLightKind kind);

/** The light's component value, or NULL once it is gone. */
const void *vkr_editor_light_value(const VkrEditorLight *light);

/** The primary scene's lights, then the World's, in entity order.
    `*out` comes from `allocator`; zero without lights or storage. */
uint32_t vkr_editor_lighting_list(const VkrSampleUiFrame *frame,
                                  VkrAllocator *allocator,
                                  VkrEditorLight **out);
