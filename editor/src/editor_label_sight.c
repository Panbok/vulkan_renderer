/* Which object icons the camera sees: the occlusion ray that Scene icons
 * and view.capture marks share, and the per-icon visibility kept across
 * builds. It touches no other editor unit, so CPU tests link it alone. */
#include "editor_internal.h"
#include "renderer/systems/vkr_scene_edit.h"
#include "renderer/systems/vkr_scene_physics.h"

#include <math.h>

/* Seconds an icon takes to fade fully in or out. */
#define EDITOR_LABEL_FADE_SECONDS 0.12f
/* Opacity of an icon collision hides. */
#define EDITOR_LABEL_OCCLUDED_ALPHA 0.0f
/* Share of the icon distance, at its far end, over which icons fade out. */
#define EDITOR_LABEL_DISTANCE_FADE 0.2f
/* A quiet key still gets a sweep this often, in seconds. */
#define EDITOR_LABEL_REFRESH_SECONDS 1.0f
/* How far ahead in the last build's order an icon's entry is searched. */
#define EDITOR_LABEL_MATCH_WINDOW 32u

#define EDITOR_LABEL_SCENE_COUNT (2u + VKR_SCENE_ADDITIVE_MAX)

/* The open scene, the World and each added slot, NULL where empty. */
static void label_scenes(const VkrSampleUiFrame *frame,
                         const VkrScene *scenes[EDITOR_LABEL_SCENE_COUNT]) {
  scenes[0] = frame->scene;
  scenes[1] = frame->world;
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    scenes[2u + i] = frame->additive[i];
  }
}

float32_t vkr_editor_label_distance(float32_t metres) {
  if (!isfinite(metres)) {
    return VKR_EDITOR_LABEL_DISTANCE_DEFAULT;
  }
  return vkr_clamp_f32(metres, 0.0f, VKR_EDITOR_LABEL_DISTANCE_MAX);
}

static bool8_t label_physics_on(const VkrScene *scene) {
  return scene && scene->physics && !scene->physics_disabled;
}

bool8_t vkr_editor_label_occlusion_available(const VkrSampleUiFrame *frame) {
  const VkrScene *scenes[EDITOR_LABEL_SCENE_COUNT];
  label_scenes(frame, scenes);
  for (uint32_t i = 0; i < EDITOR_LABEL_SCENE_COUNT; ++i) {
    if (label_physics_on(scenes[i])) {
      return true_v;
    }
  }
  return false_v;
}

bool8_t vkr_editor_label_occluded(const VkrSampleUiFrame *frame,
                                  Mat4 view_projection,
                                  Mat4 inverse_view_projection, Vec3 point,
                                  const VkrScene *owner, VkrEntityId entity) {
  const Vec4 clip = mat4_mul_vec4(view_projection, vec3_to_vec4(point, 1.0f));
  if (!isfinite(clip.w) || clip.w <= 1.0e-6f) {
    return false_v;
  }
  const Vec4 near_point =
      mat4_mul_vec4(inverse_view_projection,
                    (Vec4){clip.x / clip.w, clip.y / clip.w, 0.0f, 1.0f});
  if (!isfinite(near_point.w) || fabsf(near_point.w) < 1.0e-9f) {
    return false_v;
  }
  const Vec3 origin =
      vec3_new(near_point.x / near_point.w, near_point.y / near_point.w,
               near_point.z / near_point.w);
  const Vec3 displacement = vec3_sub(point, origin);
  const float32_t length = vec3_length(displacement);
  if (!isfinite(length) || length < 1.0e-3f) {
    return false_v;
  }
  /* A point on a surface stays visible; the length term covers float
     error on the long rays of an orthographic view. */
  const float32_t slack = 0.05f + 1.0e-4f * length;

  /* Body user data is the entity id, which carries its container's world
     id, so one list serves every scene. */
  uint64_t ignored[2];
  uint32_t ignored_count = 0u;
  if (owner && entity.u64 != VKR_ENTITY_ID_INVALID.u64) {
    ignored[ignored_count++] = entity.u64;
    const SceneTransform *transform = vkr_entity_get_component_if_alive_const(
        owner->world, entity, owner->comp_transform);
    if (transform && transform->parent.u64 != VKR_ENTITY_ID_INVALID.u64) {
      ignored[ignored_count++] = transform->parent.u64;
    }
  }

  const VkrScene *scenes[EDITOR_LABEL_SCENE_COUNT];
  label_scenes(frame, scenes);
  const VkrScenePhysicsSet *queried[EDITOR_LABEL_SCENE_COUNT];
  uint32_t queried_count = 0u;
  for (uint32_t i = 0; i < EDITOR_LABEL_SCENE_COUNT; ++i) {
    const VkrScene *scene = scenes[i];
    if (!label_physics_on(scene)) {
      continue;
    }
    /* Members of one physics set share a native world; one query covers
       them all (ADR-076). */
    bool8_t shared = false_v;
    for (uint32_t q = 0; q < queried_count && scene->physics_set; ++q) {
      shared = shared || queried[q] == scene->physics_set;
    }
    if (shared) {
      continue;
    }
    if (scene->physics_set) {
      queried[queried_count++] = scene->physics_set;
    }
    VkrPhysicsQueryFilter filter = {
        .mask = UINT16_MAX,
        .ignored_entities = ignored_count ? ignored : NULL,
        .ignored_count = ignored_count,
    };
    VkrPhysicsRayHit hit = {0};
    /* Physics queries take a mutable scene but change none of its state. */
    if (vkr_scene_physics_raycast_query((VkrScene *)scene, origin, displacement,
                                        &filter, &hit) &&
        hit.fraction * length < length - slack) {
      return true_v;
    }
  }
  return false_v;
}

void vkr_editor_label_sights_begin(VkrEditorLabelSights *sights,
                                   const VkrSampleUiFrame *frame,
                                   bool8_t occlusion, float32_t max_distance,
                                   float32_t delta_seconds, bool8_t instant,
                                   VkrAllocator *scratch) {
  /* The last build's entries move to scratch, so this build can write its
     own in place while it matches them. */
  const uint32_t previous_count = sights->count;
  VkrEditorLabelSight *previous =
      previous_count && scratch
          ? vkr_allocator_alloc(scratch,
                                sizeof(*previous) * (uint64_t)previous_count,
                                VKR_ALLOCATOR_MEMORY_TAG_ARRAY)
          : NULL;
  if (previous) {
    MemCopy(previous, sights->entries, sizeof(*previous) * previous_count);
  }
  sights->previous = previous;
  sights->previous_count = previous ? previous_count : 0u;
  sights->match = 0u;
  sights->count = 0u;
  sights->occluded_count = 0u;

  sights->occlusion = occlusion && vkr_editor_label_occlusion_available(frame);
  sights->max_distance =
      isfinite(max_distance) && max_distance > 0.0f ? max_distance : 0.0f;
  sights->delta_seconds =
      isfinite(delta_seconds) && delta_seconds > 0.0f ? delta_seconds : 0.0f;
  sights->instant = instant;
  const Mat4 view_projection = frame->view_projection;
  sights->inverse_view_projection = mat4_inverse(view_projection);
  /* An orthographic projection leaves clip w at 1: its bottom row is
     (0, 0, 0, 1). */
  sights->perspective = view_projection.elements[3] != 0.0f ||
                        view_projection.elements[7] != 0.0f ||
                        view_projection.elements[11] != 0.0f;

  VkrEditorLabelSightKey key;
  MemZero(&key, sizeof(key));
  key.view_projection = view_projection;
  label_scenes(frame, key.scenes);
  const VkrSceneEditState *edits[EDITOR_LABEL_SCENE_COUNT] = {
      frame->edits, frame->world_edits};
  for (uint32_t i = 0; i < VKR_SCENE_ADDITIVE_MAX; ++i) {
    edits[2u + i] = frame->additive_edits[i];
  }
  for (uint32_t i = 0; i < EDITOR_LABEL_SCENE_COUNT; ++i) {
    key.revisions[i] = key.scenes[i] ? key.scenes[i]->structure_revision : 0u;
    key.edits[i] = key.scenes[i] && edits[i] ? edits[i]->revision : 0u;
  }
  key.generation = frame->scene_generation;
  key.occlusion = sights->occlusion;

  /* A running simulation moves bodies without edits, so it sweeps every
     build. */
  const bool8_t changed = MemCompare(&key, &sights->key, sizeof(key)) != 0 ||
                          frame->simulation_running;
  MemCopy(&sights->key, &key, sizeof(key));
  if (changed) {
    sights->sweep = sights->previous_count;
    sights->idle_seconds = 0.0f;
  } else if (!sights->sweep) {
    sights->idle_seconds += sights->delta_seconds;
    if (sights->idle_seconds >= EDITOR_LABEL_REFRESH_SECONDS) {
      sights->sweep = sights->previous_count;
      sights->idle_seconds = 0.0f;
    }
  }
  if (sights->cursor >= sights->previous_count) {
    sights->cursor = 0u;
  }
  sights->rays_left = sights->occlusion ? VKR_EDITOR_LABEL_RAYS_PER_FRAME : 0u;
}

float32_t vkr_editor_label_sight(VkrEditorLabelSights *sights,
                                 const VkrSampleUiFrame *frame,
                                 const VkrScene *scene, VkrEntityId entity,
                                 bool8_t placed, Vec3 position) {
  if (sights->count >= VKR_EDITOR_LABEL_SIGHT_MAX) {
    return 1.0f;
  }
  const uint32_t index = sights->count;

  /* Anchor order holds while the set of icons does; an icon added or
     removed shifts it, and the window finds the rest. A new icon starts
     transparent and fades in. */
  VkrEditorLabelSight sight = {.entity = entity};
  const uint32_t end =
      Min(sights->previous_count, sights->match + EDITOR_LABEL_MATCH_WINDOW);
  for (uint32_t i = sights->match; i < end; ++i) {
    if (sights->previous[i].entity.u64 == entity.u64) {
      sight = sights->previous[i];
      sights->match = i + 1u;
      break;
    }
  }

  const bool8_t selected = entity.u64 == frame->selected_entity.u64;
  Vec4 clip = {0};
  bool8_t on_screen = false_v;
  if (placed) {
    clip = mat4_mul_vec4(frame->view_projection, vec3_to_vec4(position, 1.0f));
    on_screen = isfinite(clip.w) && clip.w > 1.0e-6f &&
                fabsf(clip.x) <= clip.w && fabsf(clip.y) <= clip.w &&
                clip.z >= 0.0f && clip.z <= clip.w;
  }
  /* Clip w is the view depth of a perspective camera. */
  const float32_t depth = sights->perspective ? clip.w : 0.0f;
  const bool8_t distance_fade =
      sights->max_distance > 0.0f && sights->perspective && placed;
  const bool8_t far = distance_fade && depth >= sights->max_distance;

  /* The sweep visits icons in order from its cursor; an icon no ray has
     tested yet goes first while rays remain. Icons off the Scene or past
     the icon distance spend no ray. */
  const bool8_t visit =
      sights->sweep && index >= sights->cursor && sights->rays_left;
  const bool8_t testable = sights->occlusion && placed && on_screen && !far;
  if (testable && sights->rays_left && (visit || !sight.tested)) {
    sight.occluded = vkr_editor_label_occluded(frame, frame->view_projection,
                                               sights->inverse_view_projection,
                                               position, scene, entity);
    sight.tested = true_v;
    sights->rays_left--;
  }
  if (visit) {
    sights->sweep--;
    sights->cursor = index + 1u;
  }

  /* The selection always shows. An untested icon holds its opacity until
     a ray decides, so a hidden one never flashes in. */
  float32_t target = 1.0f;
  if (placed && !selected) {
    const float32_t fade = distance_fade
                               ? vkr_clamp_f32((sights->max_distance - depth) /
                                                   (EDITOR_LABEL_DISTANCE_FADE *
                                                    sights->max_distance),
                                               0.0f, 1.0f)
                               : 1.0f;
    if (sights->occlusion && !sight.tested) {
      target = Min(sight.alpha, fade);
    } else if (sights->occlusion && sight.occluded) {
      target = EDITOR_LABEL_OCCLUDED_ALPHA * fade;
    } else {
      target = fade;
    }
    if (sights->occlusion && sight.tested && sight.occluded) {
      sights->occluded_count++;
    }
  }
  if (sights->instant) {
    sight.alpha = target;
  } else {
    const float32_t step = sights->delta_seconds / EDITOR_LABEL_FADE_SECONDS;
    sight.alpha = target > sight.alpha ? Min(target, sight.alpha + step)
                                       : Max(target, sight.alpha - step);
  }
  sights->entries[sights->count++] = sight;
  return sight.alpha;
}

void vkr_editor_label_sights_end(VkrEditorLabelSights *sights) {
  /* A sweep that reached the last icon carries on from the first. */
  if (sights->cursor >= sights->count) {
    sights->cursor = 0u;
  }
  sights->previous = NULL;
  sights->previous_count = 0u;
  sights->match = 0u;
}
