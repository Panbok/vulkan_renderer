#include "vkr_lighting_system.h"

#include "containers/vkr_sort.h"
#include "math/mat.h"
#include "math/vkr_quat.h"
#include "memory/vkr_heap.h"
#include "vkr_temporal.h"

// ============================================================================
// Internal Types
// ============================================================================

/**
 * @brief Context for syncing point lights from scene.
 */
typedef struct PointLightSyncContext {
  VkrLightingSystem *system;
  const VkrScene *scene;
} PointLightSyncContext;

typedef struct RectangleLightSyncContext {
  VkrLightingSystem *system;
  const VkrScene *scene;
} RectangleLightSyncContext;

vkr_internal bool8_t point_light_stable_precedes(const VkrPointLight *light,
                                                 const VkrPointLight *other) {
  const uint32_t stable_id = light->render_id ? light->render_id : UINT32_MAX;
  const uint32_t other_id = other->render_id ? other->render_id : UINT32_MAX;
  if (stable_id != other_id) {
    return stable_id < other_id;
  }
  if (light->position.x != other->position.x) {
    return light->position.x < other->position.x;
  }
  if (light->position.y != other->position.y) {
    return light->position.y < other->position.y;
  }
  return light->position.z < other->position.z;
}

/* Compares two sort records in stable order, ties in index order. */
vkr_internal int32_t point_light_record_compare_stable(const void *lhs,
                                                       const void *rhs) {
  const VkrPointLightSortRecord *a = lhs;
  const VkrPointLightSortRecord *b = rhs;
  if (a->stable_id != b->stable_id) {
    return a->stable_id < b->stable_id ? -1 : 1;
  }
  if (a->position.x != b->position.x) {
    return a->position.x < b->position.x ? -1 : 1;
  }
  if (a->position.y != b->position.y) {
    return a->position.y < b->position.y ? -1 : 1;
  }
  if (a->position.z != b->position.z) {
    return a->position.z < b->position.z ? -1 : 1;
  }
  if (a->index != b->index) {
    return a->index < b->index ? -1 : 1;
  }
  return 0;
}

/* Compares two sort records by key, then in stable order. */
vkr_internal int32_t point_light_record_compare_rank(const void *lhs,
                                                     const void *rhs) {
  const VkrPointLightSortRecord *a = lhs;
  const VkrPointLightSortRecord *b = rhs;
  if (a->key.range_distance != b->key.range_distance) {
    return a->key.range_distance < b->key.range_distance ? -1 : 1;
  }
  if (a->key.center_distance != b->key.center_distance) {
    return a->key.center_distance < b->key.center_distance ? -1 : 1;
  }
  return point_light_record_compare_stable(lhs, rhs);
}

/* Writes to `order` the indices of `count` lights, `indices` or the first
   `count` when NULL, sorted in stable order or, with `keys`, by key then in
   stable order; ties go in index order. `records` holds `count` records. */
vkr_internal void point_light_order(const VkrPointLight *lights,
                                    const VkrPointLightRankKey *keys,
                                    const uint32_t *indices, uint32_t count,
                                    VkrPointLightSortRecord *records,
                                    uint32_t *order) {
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t index = indices ? indices[i] : i;
    const VkrPointLight *light = &lights[index];
    records[i] = (VkrPointLightSortRecord){
        .key = keys ? keys[index] : (VkrPointLightRankKey){0},
        .stable_id = light->render_id ? light->render_id : UINT32_MAX,
        .position = light->position,
        .index = index,
    };
  }
  vkr_sort(records, count, sizeof(*records),
           keys ? point_light_record_compare_rank
                : point_light_record_compare_stable);
  for (uint32_t i = 0u; i < count; ++i) {
    order[i] = records[i].index;
  }
}

vkr_internal bool8_t rectangle_light_stable_precedes(
    const VkrRectangleLight *light, const VkrRectangleLight *other) {
  return light->render_id < other->render_id;
}

vkr_internal void rectangle_light_insert_stable(RectangleLightSyncContext *ctx,
                                                VkrRectangleLight candidate) {
  uint32_t insert = 0u;
  while (insert < ctx->system->rectangle_light_count &&
         !rectangle_light_stable_precedes(
             &candidate, &ctx->system->rectangle_lights[insert]))
    insert++;
  for (uint32_t i = ctx->system->rectangle_light_count; i > insert; --i)
    ctx->system->rectangle_lights[i] = ctx->system->rectangle_lights[i - 1u];
  ctx->system->rectangle_lights[insert] = candidate;
  ctx->system->rectangle_light_count++;
}

/* An incumbent ranks as if the camera were the bonus factor closer, so it
   also counts as inside its range until the camera is that factor past it. */
vkr_internal VkrPointLightRankKey point_light_rank_key(
    const VkrPointLight *light, Vec3 camera_position, bool8_t incumbent) {
  float32_t center_distance =
      vec3_length(vec3_sub(light->position, camera_position));
  if (!vkr_is_finite_f32(center_distance)) {
    return (VkrPointLightRankKey){
        .range_distance = VKR_FLOAT_MAX,
        .center_distance = VKR_FLOAT_MAX,
    };
  }
  if (incumbent) {
    center_distance /= VKR_POINT_LIGHT_LIMIT_INCUMBENT_BONUS;
  }

  const bool8_t unbounded =
      light->kind == VKR_POINT_LIGHT_KIND_POLYNOMIAL || light->range <= 0.0f;
  return (VkrPointLightRankKey){
      .range_distance =
          unbounded ? 0.0f : Max(center_distance - light->range, 0.0f),
      .center_distance = center_distance,
  };
}

/* Whether `light` ranks before `other`: by key, then in stable order. */
vkr_internal bool8_t point_light_key_precedes(const VkrPointLight *light,
                                              VkrPointLightRankKey key,
                                              const VkrPointLight *other,
                                              VkrPointLightRankKey other_key) {
  if (key.range_distance != other_key.range_distance) {
    return key.range_distance < other_key.range_distance;
  }
  if (key.center_distance != other_key.center_distance) {
    return key.center_distance < other_key.center_distance;
  }
  return point_light_stable_precedes(light, other);
}

/* Resizes `array` to `capacity` elements, keeping its leading ones; on
   failure leaves it as it was and clears `ok`. */
#define POINT_LIGHT_RESIZE(array, capacity, ok)                                \
  do {                                                                         \
    void *resized_ =                                                           \
        vkr_heap_realloc((array), (uint64_t)(capacity) * sizeof(*(array)));    \
    if (resized_) {                                                            \
      (array) = resized_;                                                      \
    } else {                                                                   \
      (ok) = false_v;                                                          \
    }                                                                          \
  } while (0)

/* Capacity for `count` lights: at least double `capacity`, so storage grows
   a few times per scene, and at least 64, within `limit`. */
vkr_internal uint32_t point_light_grown_capacity(uint32_t capacity,
                                                 uint32_t count,
                                                 uint32_t limit) {
  const uint64_t doubled = Max((uint64_t)capacity * 2u, 64u);
  return (uint32_t)Min(Max((uint64_t)count, doubled), (uint64_t)limit);
}

/* Slots of an open-addressed index over `capacity` lights: a power of two
   at least twice the lights, so a probe always ends at a free slot. */
vkr_internal uint32_t point_light_index_slots(uint32_t capacity) {
  uint32_t slots = 16u;
  while (slots < 2u * capacity) {
    slots *= 2u;
  }
  return slots;
}

vkr_internal uint32_t point_light_hash(uint32_t render_id) {
  return render_id * 2654435761u;
}

/* The previous frame's state of the light with `render_id`, or zero. */
vkr_internal VkrPointLightLimitEntry point_light_selection_find(
    const VkrPointLightSelection *state, uint32_t render_id) {
  if (state->entry_count == 0u) {
    return (VkrPointLightLimitEntry){0};
  }
  for (uint32_t slot = point_light_hash(render_id) & state->index_mask;;
       slot = (slot + 1u) & state->index_mask) {
    const uint32_t entry = state->index[slot];
    if (entry == 0u) {
      return (VkrPointLightLimitEntry){0};
    }
    if (state->entries[entry - 1u].render_id == render_id) {
      return state->entries[entry - 1u];
    }
  }
}

/* Grows `state` to record `count` lights. */
vkr_internal bool8_t
point_light_selection_reserve(VkrPointLightSelection *state, uint32_t count) {
  if (count <= state->entry_capacity) {
    return true_v;
  }
  const uint32_t capacity = point_light_grown_capacity(
      state->entry_capacity, count, VKR_POINT_LIGHT_SELECTION_ENTRY_MAX);
  const uint32_t slots = point_light_index_slots(capacity);
  bool8_t ok = count <= capacity;
  POINT_LIGHT_RESIZE(state->entries, capacity, ok);
  POINT_LIGHT_RESIZE(state->index, slots, ok);
  if (!ok) {
    return false_v;
  }
  state->entry_capacity = capacity;
  state->index_mask = slots - 1u;
  return true_v;
}

vkr_internal void point_light_selection_release(VkrPointLightSelection *state) {
  vkr_heap_free(state->entries);
  vkr_heap_free(state->index);
  MemZero(state, sizeof(*state));
}

/* Grows the ranking scratch to rank `count` lights. */
vkr_internal bool8_t point_light_ranking_reserve(VkrPointLightRanking *ranking,
                                                 uint32_t count) {
  if (count <= ranking->capacity) {
    return true_v;
  }
  const uint32_t capacity = point_light_grown_capacity(
      ranking->capacity, count, VKR_POINT_LIGHT_CANDIDATE_MAX);
  const uint32_t slots = point_light_index_slots(capacity);
  bool8_t ok = count <= capacity;
  POINT_LIGHT_RESIZE(ranking->previous, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->identified, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->keys, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->selected, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->head, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->rows, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->weights, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->order, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->drawn, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->ranked, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->occupant, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->records, capacity, ok);
  POINT_LIGHT_RESIZE(ranking->ids, slots, ok);
  if (!ok) {
    return false_v;
  }
  ranking->capacity = capacity;
  ranking->ids_mask = slots - 1u;
  return true_v;
}

vkr_internal void point_light_ranking_release(VkrPointLightRanking *ranking) {
  vkr_heap_free(ranking->previous);
  vkr_heap_free(ranking->identified);
  vkr_heap_free(ranking->keys);
  vkr_heap_free(ranking->selected);
  vkr_heap_free(ranking->head);
  vkr_heap_free(ranking->rows);
  vkr_heap_free(ranking->weights);
  vkr_heap_free(ranking->order);
  vkr_heap_free(ranking->drawn);
  vkr_heap_free(ranking->ranked);
  vkr_heap_free(ranking->occupant);
  vkr_heap_free(ranking->records);
  vkr_heap_free(ranking->ids);
  MemZero(ranking, sizeof(*ranking));
}

/* Whether light `a` ranks before light `b`: by key, then in stable order,
   then in index order, a total order. */
vkr_internal bool8_t point_light_ranks_before(const VkrPointLight *lights,
                                              const VkrPointLightRankKey *keys,
                                              uint32_t a, uint32_t b) {
  if (point_light_key_precedes(&lights[a], keys[a], &lights[b], keys[b])) {
    return true_v;
  }
  if (point_light_key_precedes(&lights[b], keys[b], &lights[a], keys[a])) {
    return false_v;
  }
  return a < b;
}

/* Restores the heap of light indices below `at`; its root ranks worst. */
vkr_internal void point_light_heap_sift_down(uint32_t *heap, uint32_t count,
                                             uint32_t at,
                                             const VkrPointLight *lights,
                                             const VkrPointLightRankKey *keys) {
  for (;;) {
    uint32_t worst = at;
    const uint32_t left = 2u * at + 1u;
    const uint32_t right = left + 1u;
    if (left < count &&
        point_light_ranks_before(lights, keys, heap[worst], heap[left])) {
      worst = left;
    }
    if (right < count &&
        point_light_ranks_before(lights, keys, heap[worst], heap[right])) {
      worst = right;
    }
    if (worst == at) {
      return;
    }
    const uint32_t moved = heap[at];
    heap[at] = heap[worst];
    heap[worst] = moved;
    at = worst;
  }
}

/* Restores the heap above `at` after `heap[at]` was placed. */
vkr_internal void point_light_heap_sift_up(uint32_t *heap, uint32_t at,
                                           const VkrPointLight *lights,
                                           const VkrPointLightRankKey *keys) {
  while (at > 0u) {
    const uint32_t parent = (at - 1u) / 2u;
    if (!point_light_ranks_before(lights, keys, heap[parent], heap[at])) {
      return;
    }
    const uint32_t moved = heap[at];
    heap[at] = heap[parent];
    heap[parent] = moved;
    at = parent;
  }
}

/* Writes to `best` the `k` of `count` lights, `indices` or the first `count`
   when NULL, that rank best by `keys` (point_light_ranks_before), in no
   order, and returns how many: a bounded heap, so ranking a few among many
   lights costs no sort. */
vkr_internal uint32_t point_light_top(const VkrPointLight *lights,
                                      const VkrPointLightRankKey *keys,
                                      const uint32_t *indices, uint32_t count,
                                      uint32_t k, uint32_t *best) {
  uint32_t best_count = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t index = indices ? indices[i] : i;
    if (best_count < k) {
      best[best_count] = index;
      point_light_heap_sift_up(best, best_count, lights, keys);
      best_count++;
    } else if (k > 0u &&
               point_light_ranks_before(lights, keys, index, best[0])) {
      best[0] = index;
      point_light_heap_sift_down(best, best_count, 0u, lights, keys);
    }
  }
  return best_count;
}

/* A first frame, a camera cut or an unusable step has no state worth
   following, so the selected lights show at once. Lights do not depend on
   the view direction: only the position half of the cut test applies. */
vkr_internal bool8_t
point_light_selection_snaps(const VkrPointLightSelection *state,
                            Vec3 camera_position, float32_t delta_seconds) {
  return !state->valid || !vkr_is_finite_f32(delta_seconds) ||
         delta_seconds < 0.0f ||
         vkr_temporal_is_camera_cut(state->camera, mat4_identity(),
                                    camera_position, mat4_identity());
}

/* Marks the identified lights among `count`: those with a nonzero render
   id no other of them shares. */
vkr_internal void point_light_identify(const VkrPointLight *lights,
                                       uint32_t count,
                                       VkrPointLightRanking *ranking) {
  if (count == 0u) {
    return;
  }
  MemZero(ranking->ids,
          ((uint64_t)ranking->ids_mask + 1u) * sizeof(*ranking->ids));
  for (uint32_t i = 0u; i < count; ++i) {
    const uint32_t render_id = lights[i].render_id;
    ranking->identified[i] = render_id != 0u;
    if (render_id == 0u) {
      continue;
    }
    uint32_t slot = point_light_hash(render_id) & ranking->ids_mask;
    while (ranking->ids[slot] != 0u) {
      const uint32_t other = ranking->ids[slot] - 1u;
      if (lights[other].render_id == render_id) {
        ranking->identified[other] = false_v;
        ranking->identified[i] = false_v;
        break;
      }
      slot = (slot + 1u) & ranking->ids_mask;
    }
    if (ranking->identified[i]) {
      ranking->ids[slot] = i + 1u;
    }
  }
}

/* Ranks `count` lights for `camera_position` against `state`, the previous
   frame's, and selects the `light_max` best, a light selected in that frame
   with the incumbent bonus; `selected`, when not NULL, holds the selection
   the caller already ranked. Lights still showing keep their slots, in rank
   order, and fade toward their selection by `step`; newly selected lights
   then take the free slots, from `step` when `fade_in` and at full intensity
   otherwise, so at most `light_max` lights draw. Rank order matters only
   when more lights select or show than can draw. A light without identity,
   a nonzero render id no other of the lights shares, shows at once and
   leaves at once. Fills `ranking`, which holds `count` lights, and returns
   the drawn count. */
vkr_internal uint32_t point_light_rank_and_fade(
    const VkrPointLightSelection *state, const VkrPointLight *lights,
    uint32_t count, Vec3 camera_position, float32_t step, bool8_t snap,
    bool8_t fade_in, uint32_t light_max, const bool8_t *selected,
    VkrPointLightRanking *ranking) {
  light_max = Min(light_max, VKR_MAX_SCENE_POINT_LIGHTS);
  point_light_identify(lights, count, ranking);
  for (uint32_t i = 0u; i < count; ++i) {
    ranking->previous[i] =
        ranking->identified[i] && !snap
            ? point_light_selection_find(state, lights[i].render_id)
            : (VkrPointLightLimitEntry){0};
    ranking->keys[i] = point_light_rank_key(&lights[i], camera_position,
                                            ranking->previous[i].selected);
    ranking->weights[i] = 0.0f;
    ranking->head[i] = false_v;
    ranking->selected[i] = selected ? selected[i] : count <= light_max;
  }
  if (!selected && count > light_max) {
    const uint32_t best = point_light_top(lights, ranking->keys, NULL, count,
                                          light_max, ranking->order);
    for (uint32_t k = 0u; k < best; ++k) {
      ranking->selected[ranking->order[k]] = true_v;
    }
  }

  uint32_t competing = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    if (ranking->selected[i] || ranking->previous[i].weight > 0.0f) {
      ranking->order[competing++] = i;
    }
  }
  if (competing > light_max) {
    point_light_order(lights, ranking->keys, ranking->order, competing,
                      ranking->records, ranking->order);
  }

  uint32_t drawn_count = 0u;
  for (uint32_t pass = 0u; pass < 2u; ++pass) {
    for (uint32_t rank = 0u; rank < competing; ++rank) {
      const uint32_t i = ranking->order[rank];
      const VkrPointLightLimitEntry *previous = &ranking->previous[i];
      const bool8_t showing = previous->weight > 0.0f;
      float32_t weight = 0.0f;
      if (pass == 0u) {
        if (!showing) {
          continue;
        }
        weight = ranking->selected[i] ? Min(previous->weight + step, 1.0f)
                                      : Max(previous->weight - step, 0.0f);
      } else {
        if (showing || !ranking->selected[i]) {
          continue;
        }
        weight = ranking->identified[i] && fade_in ? step : 1.0f;
      }
      if (weight <= 0.0f || drawn_count >= light_max) {
        continue;
      }
      ranking->weights[i] = weight;
      drawn_count++;
    }
  }
  return drawn_count;
}

/* Records the state the next frame ranks and fades from: the identified
   lights selected or drawn, their rows plus one from `rows` when not NULL,
   and whether `offered` lights competed. Without storage for them it
   records no state, so the next frame snaps. */
vkr_internal void point_light_selection_record(
    VkrPointLightSelection *state, const VkrPointLight *lights, uint32_t count,
    const VkrPointLightRanking *ranking, const uint32_t *rows,
    Vec3 camera_position, uint32_t offered) {
  state->entry_count = 0u;
  if (!point_light_selection_reserve(state, count)) {
    state->valid = false_v;
    return;
  }
  if (state->index) {
    MemZero(state->index,
            ((uint64_t)state->index_mask + 1u) * sizeof(*state->index));
  }
  uint32_t drawn_count = 0u;
  bool8_t fading = false_v;
  for (uint32_t i = 0u; i < count; ++i) {
    const float32_t weight = ranking->weights[i];
    if (weight > 0.0f) {
      drawn_count++;
      fading = fading || weight < 1.0f;
    }
    if (!ranking->identified[i] || (!ranking->selected[i] && weight <= 0.0f)) {
      continue;
    }
    /* Identified lights hold distinct render ids. */
    uint32_t slot = point_light_hash(lights[i].render_id) & state->index_mask;
    while (state->index[slot] != 0u) {
      slot = (slot + 1u) & state->index_mask;
    }
    state->entries[state->entry_count] = (VkrPointLightLimitEntry){
        .render_id = lights[i].render_id,
        .weight = weight,
        .selected = ranking->selected[i],
        .shadowed = lights[i].casts_shadow,
        .head = ranking->head[i],
        .row = rows ? rows[i] : 0u,
    };
    state->index[slot] = ++state->entry_count;
  }
  state->camera = camera_position;
  state->contended = fading || offered > drawn_count;
  state->valid = true_v;
}

/* Removes the candidates neither among the best nor showing, which the best
   evicted, and builds the best heap again over the rest. */
vkr_internal void
point_light_candidates_compact(VkrPointLightCandidates *candidates) {
  uint32_t kept = 0u;
  candidates->best_count = 0u;
  for (uint32_t i = 0u; i < candidates->count; ++i) {
    if (!candidates->in_best[i] && !candidates->showing[i]) {
      continue;
    }
    candidates->lights[kept] = candidates->lights[i];
    candidates->keys[kept] = candidates->keys[i];
    candidates->showing[kept] = candidates->showing[i];
    candidates->in_best[kept] = candidates->in_best[i];
    if (candidates->in_best[kept]) {
      candidates->best[candidates->best_count++] = kept;
    }
    kept++;
  }
  candidates->count = kept;
  candidates->evicted = 0u;
  for (uint32_t at = candidates->best_count / 2u; at-- > 0u;) {
    point_light_heap_sift_down(candidates->best, candidates->best_count, at,
                               candidates->lights, candidates->keys);
  }
}

/* Makes room for one more candidate: grows the storage, or past
   VKR_POINT_LIGHT_CANDIDATE_MAX removes the evicted candidates. */
vkr_internal bool8_t
point_light_candidates_reserve(VkrPointLightCandidates *candidates) {
  if (candidates->count < candidates->capacity) {
    return true_v;
  }
  if (candidates->capacity < VKR_POINT_LIGHT_CANDIDATE_MAX) {
    const uint32_t capacity =
        point_light_grown_capacity(candidates->capacity, candidates->count + 1u,
                                   VKR_POINT_LIGHT_CANDIDATE_MAX);
    bool8_t ok = true_v;
    POINT_LIGHT_RESIZE(candidates->lights, capacity, ok);
    POINT_LIGHT_RESIZE(candidates->keys, capacity, ok);
    POINT_LIGHT_RESIZE(candidates->showing, capacity, ok);
    POINT_LIGHT_RESIZE(candidates->in_best, capacity, ok);
    POINT_LIGHT_RESIZE(candidates->best,
                       Min(capacity, VKR_MAX_SCENE_POINT_LIGHTS), ok);
    if (ok) {
      candidates->capacity = capacity;
      return true_v;
    }
  }
  if (candidates->evicted > 0u) {
    point_light_candidates_compact(candidates);
  }
  return candidates->count < candidates->capacity;
}

vkr_internal void
point_light_candidates_release(VkrPointLightCandidates *candidates) {
  vkr_heap_free(candidates->lights);
  vkr_heap_free(candidates->keys);
  vkr_heap_free(candidates->showing);
  vkr_heap_free(candidates->in_best);
  vkr_heap_free(candidates->best);
  MemZero(candidates, sizeof(*candidates));
}

/* Places the `drawn_count` drawn lights `drawn` in the table's rows
   [0, drawn_count), writing each one's row plus one to `ranking->rows`. A
   light keeps its row from `state`, the previous frame's, while the row
   lies in the table and, for a head light, among the head rows. Head lights
   without a row free head rows by evicting the lights outside the head from
   the highest head rows. The lights without a row then take the free rows,
   lowest first in stable order; a light outside the head takes a head row
   only while more remain free than head lights wait for one. `unplaced` is
   scratch for `drawn_count` indices; `ranking->occupant` holds each row's
   light while they are placed. */
vkr_internal void
point_light_assign_rows(const VkrPointLightSelection *state, bool8_t snap,
                        const VkrPointLight *lights, const uint32_t *drawn,
                        uint32_t drawn_count, VkrPointLightRanking *ranking,
                        uint32_t *unplaced) {
  const uint32_t head_rows = Min(drawn_count, VKR_POINT_LIGHT_GRID_LIGHT_MAX);
  uint32_t *occupant = ranking->occupant;
  for (uint32_t row = 0u; row < drawn_count; ++row) {
    occupant[row] = UINT32_MAX;
  }

  /* A camera cut snaps the fades, not the rows: lights keep them. */
  uint32_t unplaced_count = 0u;
  uint32_t head_waiting = 0u;
  for (uint32_t k = 0u; k < drawn_count; ++k) {
    const uint32_t i = drawn[k];
    uint32_t row = 0u;
    if (ranking->identified[i]) {
      row = snap ? point_light_selection_find(state, lights[i].render_id).row
                 : ranking->previous[i].row;
    }
    if (row != 0u && row <= drawn_count &&
        (!ranking->head[i] || row <= head_rows) &&
        occupant[row - 1u] == UINT32_MAX) {
      occupant[row - 1u] = i;
      ranking->rows[i] = row;
      continue;
    }
    unplaced[unplaced_count++] = i;
    head_waiting += ranking->head[i] ? 1u : 0u;
  }

  uint32_t head_free = 0u;
  for (uint32_t row = 0u; row < head_rows; ++row) {
    head_free += occupant[row] == UINT32_MAX ? 1u : 0u;
  }
  /* The head holds at most head_rows lights, so enough head rows hold a
     light outside it. */
  for (uint32_t row = head_rows; row-- > 0u && head_free < head_waiting;) {
    const uint32_t i = occupant[row];
    if (i == UINT32_MAX || ranking->head[i]) {
      continue;
    }
    occupant[row] = UINT32_MAX;
    ranking->rows[i] = 0u;
    unplaced[unplaced_count++] = i;
    head_free++;
  }

  point_light_order(lights, NULL, unplaced, unplaced_count, ranking->records,
                    unplaced);
  uint32_t head_next = 0u;
  uint32_t rest_next = head_rows;
  for (uint32_t k = 0u; k < unplaced_count; ++k) {
    const uint32_t i = unplaced[k];
    const bool8_t head_row = ranking->head[i] || head_free > head_waiting;
    uint32_t *next = head_row ? &head_next : &rest_next;
    while (occupant[*next] != UINT32_MAX) {
      ++*next;
    }
    if (head_row) {
      head_free--;
      head_waiting -= ranking->head[i] ? 1u : 0u;
    }
    occupant[*next] = i;
    ranking->rows[i] = *next + 1u;
    ++*next;
  }
}

/* Keeps the VKR_POINT_LIGHT_GRID_LIGHT_MAX baked lamps nearest the camera: in
   offer order while they fit, then a nearer lamp replaces the farthest. */
vkr_internal void point_light_offer_baked_lamp(VkrLightingSystem *system,
                                               const VkrPointLight *lamp) {
  const VkrPointLightRankKey key = point_light_rank_key(
      lamp, system->point_light_candidates.camera, false_v);
  uint32_t slot = system->baked_lamp_count;
  if (slot == ArrayCount(system->baked_lamps)) {
    slot = 0u;
    for (uint32_t i = 1u; i < system->baked_lamp_count; ++i) {
      if (point_light_key_precedes(
              &system->baked_lamps[slot], system->baked_lamp_keys[slot],
              &system->baked_lamps[i], system->baked_lamp_keys[i])) {
        slot = i;
      }
    }
    if (!point_light_key_precedes(lamp, key, &system->baked_lamps[slot],
                                  system->baked_lamp_keys[slot])) {
      return;
    }
  } else {
    system->baked_lamp_count++;
  }
  system->baked_lamps[slot] = *lamp;
  system->baked_lamp_keys[slot] = key;
}

// ============================================================================
// Chunk Callbacks
// ============================================================================

vkr_internal void sync_point_lights_cb(const VkrArchetype *arch,
                                       VkrChunk *chunk, void *user) {
  (void)arch;
  PointLightSyncContext *ctx = (PointLightSyncContext *)user;

  const VkrScene *scene = ctx->scene;
  uint32_t count = vkr_entity_chunk_count(chunk);

  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  ScenePointLight *lights = (ScenePointLight *)vkr_entity_chunk_column(
      chunk, scene->comp_point_light);

  if (!entities || !transforms || !lights)
    return;

  for (uint32_t i = 0; i < count; i++) {
    const float32_t group =
        vkr_scene_light_group_factor(scene, lights[i].light_group_slot);
    if (!lights[i].enabled || group <= 0.0f ||
        !vkr_scene_entity_visible(scene, entities[i]))
      continue;
    const bool8_t baked =
        ctx->system->static_lights_baked &&
        lights[i].mobility == VKR_LIGHT_MOBILITY_STATIC &&
        !vkr_scene_light_group_left_bake(scene, lights[i].light_group_slot);
    VkrLightingSystem *system = ctx->system;
    if (baked && !lights[i].casts_shadow) {
      continue;
    }

    // Get world position from transform
    Vec3 world_position = mat4_position(transforms[i].world);
    /* A light in a hidden room lights nothing the camera sees; a baked
       lamp's moving-caster shadows go with it. */
    if (system->point_light_hidden &&
        system->point_light_hidden(system->point_light_hidden_context,
                                   world_position, lights[i].range)) {
      continue;
    }
    uint32_t render_id = vkr_scene_get_render_id(scene, entities[i]);
    const Vec3 direction = vkr_scene_light_world_direction(
        scene, &transforms[i], lights[i].direction_local);
    const VkrPointLight light = {
        .position = world_position,
        .color = lights[i].color,
        .intensity = lights[i].intensity * group,
        .constant = lights[i].constant,
        .linear = lights[i].linear,
        .quadratic = lights[i].quadratic,
        .range = lights[i].range,
        .direction = direction,
        .inner_cone_angle = lights[i].inner_cone_angle,
        .outer_cone_angle = lights[i].outer_cone_angle,
        .kind = lights[i].kind,
        .render_id = render_id,
        .source_radius = lights[i].source_radius,
        .casts_shadow = lights[i].casts_shadow,
    };
    /* A baked lamp lights through the lightmap only; it stays a candidate
       for its moving casters' shadows. */
    if (baked) {
      point_light_offer_baked_lamp(system, &light);
    } else {
      vkr_lighting_system_offer_point_light(system, &light);
    }
  }
}

vkr_internal void sync_rectangle_lights_cb(const VkrArchetype *arch,
                                           VkrChunk *chunk, void *user) {
  (void)arch;
  RectangleLightSyncContext *ctx = (RectangleLightSyncContext *)user;
  const VkrScene *scene = ctx->scene;
  const uint32_t count = vkr_entity_chunk_count(chunk);
  VkrEntityId *entities = vkr_entity_chunk_entities(chunk);
  SceneTransform *transforms =
      (SceneTransform *)vkr_entity_chunk_column(chunk, scene->comp_transform);
  SceneRectangleLight *lights = (SceneRectangleLight *)vkr_entity_chunk_column(
      chunk, scene->comp_rectangle_light);
  if (!entities || !transforms || !lights)
    return;

  for (uint32_t i = 0u; i < count; ++i) {
    const float32_t group =
        vkr_scene_light_group_factor(scene, lights[i].light_group_slot);
    if (!lights[i].enabled || group <= 0.0f ||
        (ctx->system->static_lights_baked &&
         lights[i].mobility == VKR_LIGHT_MOBILITY_STATIC &&
         !vkr_scene_light_group_left_bake(scene, lights[i].light_group_slot)) ||
        !vkr_scene_entity_visible(scene, entities[i])) {
      continue;
    }
    VkrQuat world_rotation;
    if (!vkr_scene_rigid_world_rotation(scene, &transforms[i], &world_rotation))
      continue;
    const VkrRectangleLight candidate = {
        .position = mat4_position(transforms[i].world),
        .right = vec3_normalize(
            vkr_quat_rotate_vec3(world_rotation, vec3_new(1.0f, 0.0f, 0.0f))),
        .up = vec3_normalize(
            vkr_quat_rotate_vec3(world_rotation, vec3_new(0.0f, 1.0f, 0.0f))),
        .color = lights[i].color,
        .half_width = lights[i].size.x * 0.5f,
        .half_height = lights[i].size.y * 0.5f,
        .radiance = lights[i].radiance * group,
        .render_id = vkr_scene_get_render_id(scene, entities[i]),
    };
    if (vkr_rectangle_light_valid(&candidate))
      rectangle_light_insert_stable(ctx, candidate);
  }
}

// ============================================================================
// Public API
// ============================================================================

bool8_t vkr_lighting_system_init(VkrLightingSystem *system) {
  if (!system)
    return false_v;

  MemZero(system, sizeof(VkrLightingSystem));

  // Initialize with default directional light (disabled)
  system->directional.enabled = false_v;
  system->directional.direction = (Vec3){0.0f, -1.0f, 0.0f};
  system->directional.color = (Vec3){1.0f, 1.0f, 1.0f};
  system->directional.intensity = 1.0f;
  system->directional.sun_angular_diameter_degrees =
      VKR_DIRECTIONAL_LIGHT_DEFAULT_SUN_ANGULAR_DIAMETER_DEGREES;

  system->point_light_count = 0;
  system->rectangle_light_count = 0;
  system->dirty = true_v;

  return true_v;
}

void vkr_lighting_system_shutdown(VkrLightingSystem *system) {
  if (!system)
    return;
  vkr_heap_free(system->point_lights);
  vkr_heap_free(system->point_light_unfaded_intensity);
  point_light_candidates_release(&system->point_light_candidates);
  point_light_ranking_release(&system->point_light_ranking);
  point_light_selection_release(&system->point_light_selection);
  point_light_selection_release(&system->point_light_selection_next);
  point_light_selection_release(&system->point_light_limit);
  MemZero(system, sizeof(VkrLightingSystem));
}

bool8_t vkr_lighting_system_reserve_point_lights(VkrLightingSystem *system,
                                                 uint32_t count) {
  if (!system) {
    return false_v;
  }
  count = Min(count, VKR_MAX_SCENE_POINT_LIGHTS);
  if (!point_light_ranking_reserve(&system->point_light_ranking, count)) {
    return false_v;
  }
  if (count <= system->point_light_capacity) {
    return true_v;
  }
  const uint32_t capacity = point_light_grown_capacity(
      system->point_light_capacity, count, VKR_MAX_SCENE_POINT_LIGHTS);
  bool8_t ok = true_v;
  POINT_LIGHT_RESIZE(system->point_lights, capacity, ok);
  POINT_LIGHT_RESIZE(system->point_light_unfaded_intensity, capacity, ok);
  if (!ok) {
    return false_v;
  }
  system->point_light_capacity = capacity;
  return true_v;
}

void vkr_lighting_system_sync_from_scene(VkrLightingSystem *system,
                                         const VkrScene *scene,
                                         Vec3 camera_position,
                                         float32_t delta_seconds) {
  if (!system || !scene || !scene->world)
    return;

  // Compile queries if needed (should already be done by scene update)
  if (!scene->queries_valid)
    return;

  // Reset state
  system->directional.enabled = false_v;
  system->point_light_count = 0;
  system->point_light_dropped_count = 0;
  system->rectangle_light_count = 0;
  system->baked_lamp_count = 0u;
  vkr_lighting_system_begin_point_lights(system, camera_position,
                                         delta_seconds);

  /* An enabled atmosphere turns the sun light into its sun (ADR-058): the
     published atmosphere sun replaces this empty record, so the light, sky
     and bake agree. Otherwise the light shines directly. */
  const VkrSceneSun *sun = &scene->sun;
  if (!scene->atmosphere.requested_settings.enabled && sun->found) {
    system->directional.enabled = true_v;
    system->directional.direction = sun->light.direction;
    system->directional.color = sun->light.color;
    system->directional.intensity = sun->light.intensity;
    system->directional.sun_angular_diameter_degrees =
        sun->light.sun_angular_diameter_degrees;
  }

  // Sync point lights
  PointLightSyncContext point_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights, sync_point_lights_cb,
      &point_ctx);

  RectangleLightSyncContext rectangle_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_rectangle_lights,
      sync_rectangle_lights_cb, &rectangle_ctx);
  vkr_lighting_system_select_point_lights(system);
}

void vkr_lighting_system_append_scene(VkrLightingSystem *system,
                                      const VkrScene *scene) {
  if (!system || !scene || !scene->world || !scene->queries_valid)
    return;
  PointLightSyncContext point_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_point_lights, sync_point_lights_cb,
      &point_ctx);
  RectangleLightSyncContext rectangle_ctx = {
      .system = system,
      .scene = scene,
  };
  vkr_entity_query_compiled_each_chunk(
      (VkrQueryCompiled *)&scene->query_rectangle_lights,
      sync_rectangle_lights_cb, &rectangle_ctx);
  vkr_lighting_system_select_point_lights(system);
}

void vkr_lighting_system_begin_point_lights(VkrLightingSystem *system,
                                            Vec3 camera_position,
                                            float32_t delta_seconds) {
  if (!system) {
    return;
  }
  /* Swapped, not copied: each state keeps its own storage. */
  if (system->point_light_selection_next.valid) {
    const VkrPointLightSelection previous = system->point_light_selection;
    system->point_light_selection = system->point_light_selection_next;
    system->point_light_selection_next = previous;
    system->point_light_selection_next.valid = false_v;
  }
  const bool8_t snap = point_light_selection_snaps(
      &system->point_light_selection, camera_position, delta_seconds);
  VkrPointLightCandidates *candidates = &system->point_light_candidates;
  candidates->count = 0u;
  candidates->best_count = 0u;
  candidates->evicted = 0u;
  candidates->offered = 0u;
  candidates->camera = camera_position;
  candidates->snap = snap;
  candidates->step =
      snap ? 1.0f
           : Min(delta_seconds / VKR_POINT_LIGHT_LIMIT_FADE_SECONDS, 1.0f);
}

void vkr_lighting_system_offer_point_light(VkrLightingSystem *system,
                                           const VkrPointLight *light) {
  if (!system || !light) {
    return;
  }
  VkrPointLightCandidates *candidates = &system->point_light_candidates;
  candidates->offered++;
  const VkrPointLightLimitEntry previous =
      candidates->snap || light->render_id == 0u
          ? (VkrPointLightLimitEntry){0}
          : point_light_selection_find(&system->point_light_selection,
                                       light->render_id);
  const VkrPointLightRankKey key =
      point_light_rank_key(light, candidates->camera, previous.selected);
  const bool8_t showing = previous.weight > 0.0f;

  /* It joins the best offered so far while they have room or it ranks
     before the worst of them, which it evicts; past them it stays only
     while it still shows, to fade out in its slot. */
  bool8_t best = candidates->best_count < VKR_MAX_SCENE_POINT_LIGHTS;
  if (!best) {
    const uint32_t worst = candidates->best[0];
    best = point_light_key_precedes(light, key, &candidates->lights[worst],
                                    candidates->keys[worst]);
  }
  if (!best && !showing) {
    return;
  }
  /* Only lights sharing a render id can outnumber the showing places. */
  if (!point_light_candidates_reserve(candidates)) {
    return;
  }
  const uint32_t slot = candidates->count++;
  candidates->lights[slot] = *light;
  candidates->keys[slot] = key;
  candidates->showing[slot] = showing;
  candidates->in_best[slot] = best;
  if (!best) {
    return;
  }
  if (candidates->best_count < VKR_MAX_SCENE_POINT_LIGHTS) {
    candidates->best[candidates->best_count] = slot;
    point_light_heap_sift_up(candidates->best, candidates->best_count,
                             candidates->lights, candidates->keys);
    candidates->best_count++;
    return;
  }
  const uint32_t evicted = candidates->best[0];
  candidates->best[0] = slot;
  point_light_heap_sift_down(candidates->best, candidates->best_count, 0u,
                             candidates->lights, candidates->keys);
  candidates->in_best[evicted] = false_v;
  if (!candidates->showing[evicted]) {
    candidates->evicted++;
  }
}

void vkr_lighting_system_select_point_lights(VkrLightingSystem *system) {
  if (!system) {
    return;
  }
  VkrPointLightCandidates *candidates = &system->point_light_candidates;
  if (candidates->evicted > 0u) {
    point_light_candidates_compact(candidates);
  }
  const uint32_t count = candidates->count;
  const VkrPointLightSelection *state = &system->point_light_selection;
  VkrPointLightRanking *ranking = &system->point_light_ranking;
  /* Without storage to rank them the frame draws no point lights. */
  if (!point_light_ranking_reserve(ranking, count) ||
      !vkr_lighting_system_reserve_point_lights(system, count)) {
    system->point_light_count = 0u;
    system->point_light_dropped_count = candidates->offered;
    system->dirty = true_v;
    return;
  }
  /* The offers ranked the best already. */
  (void)point_light_rank_and_fade(
      state, candidates->lights, count, candidates->camera, candidates->step,
      candidates->snap, state->contended, VKR_MAX_SCENE_POINT_LIGHTS,
      candidates->in_best, ranking);

  /* The drawn lights, at most a table's worth, and their shadow casters
     ranked for the head: a caster in the previous frame's head ranks with
     the incumbent bonus, so casters at its edge do not trade rows and
     shadows every frame. Lights that cast no shadow need no rank. */
  uint32_t *drawn = ranking->drawn;
  uint32_t drawn_count = 0u;
  uint32_t caster_count = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    ranking->rows[i] = 0u;
    if (ranking->weights[i] <= 0.0f) {
      continue;
    }
    drawn[drawn_count++] = i;
    if (candidates->lights[i].casts_shadow) {
      ranking->keys[i] =
          point_light_rank_key(&candidates->lights[i], candidates->camera,
                               ranking->previous[i].head);
      ranking->ranked[caster_count++] = i;
    }
  }
  const uint32_t head_count = point_light_top(
      candidates->lights, ranking->keys, ranking->ranked, caster_count,
      VKR_POINT_LIGHT_GRID_LIGHT_MAX, ranking->order);
  for (uint32_t i = 0u; i < head_count; ++i) {
    ranking->head[ranking->order[i]] = true_v;
  }
  point_light_assign_rows(state, candidates->snap, candidates->lights, drawn,
                          drawn_count, ranking, ranking->ranked);
  point_light_selection_record(
      &system->point_light_selection_next, candidates->lights, count, ranking,
      ranking->rows, candidates->camera, candidates->offered);

  /* Each drawn light in its row, scaled by its fade; only the head casts
     local shadows. */
  for (uint32_t k = 0u; k < drawn_count; ++k) {
    const uint32_t i = drawn[k];
    const uint32_t row = ranking->rows[i] - 1u;
    VkrPointLight light = candidates->lights[i];
    system->point_light_unfaded_intensity[row] = light.intensity;
    light.intensity *= ranking->weights[i];
    light.casts_shadow = ranking->head[i];
    system->point_lights[row] = light;
  }
  system->point_light_count = drawn_count;
  system->point_light_dropped_count = candidates->offered - drawn_count;
  system->dirty = true_v;
}

void vkr_lighting_system_apply_atmosphere_light(
    VkrLightingSystem *system, Vec3 toward_light, Vec3 irradiance,
    float32_t angular_diameter_degrees, float32_t display_exposure) {
  if (!system)
    return;

  /* The directional-light record points along incoming light, which shaders
     negate when forming their surface-to-light vector. A sky with neither a
     lit sun nor a lit moon has no light to light with, and a moon under
     street lamps lights nothing that shows: its four cascades would cost a
     whole frame's shadow budget for an invisible term. */
  const float32_t luminance =
      0.2126f * irradiance.x + 0.7152f * irradiance.y + 0.0722f * irradiance.z;
  const bool8_t visible =
      !(display_exposure > 0.0f) || luminance * display_exposure / VKR_PI >=
                                        VKR_LIGHTING_KEY_LIGHT_VISIBLE_MIN;
  system->directional.enabled =
      visible &&
      (irradiance.x > 0.0f || irradiance.y > 0.0f || irradiance.z > 0.0f);
  system->directional.direction = vec3_negate(toward_light);
  system->directional.color = irradiance;
  system->directional.intensity = 1.0f;
  system->directional.sun_angular_diameter_degrees = angular_diameter_degrees;
  system->dirty = true_v;
}

void vkr_lighting_system_limit_point_lights(VkrLightingSystem *system,
                                            Vec3 camera_position,
                                            float32_t delta_seconds,
                                            uint32_t light_max,
                                            uint32_t shadow_max) {
  if (!system)
    return;
  VkrPointLightSelection *state = &system->point_light_limit;
  const uint32_t count = system->point_light_count;
  const bool8_t snap =
      point_light_selection_snaps(state, camera_position, delta_seconds);
  const float32_t step =
      snap ? 1.0f
           : Min(delta_seconds / VKR_POINT_LIGHT_LIMIT_FADE_SECONDS, 1.0f);
  VkrPointLightRanking *ranking = &system->point_light_ranking;
  if (!point_light_ranking_reserve(ranking, count)) {
    return;
  }
  (void)point_light_rank_and_fade(state, system->point_lights, count,
                                  camera_position, step, snap, true_v,
                                  light_max, NULL, ranking);

  /* Shadows go to the nearest drawn and selected casters, incumbents
     weighted. A fading-out light loses its shadow, which the local shadow
     cache fades out. */
  uint32_t eligible = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    VkrPointLight *light = &system->point_lights[i];
    if (!light->casts_shadow || ranking->weights[i] <= 0.0f ||
        !ranking->selected[i]) {
      light->casts_shadow = false_v;
      continue;
    }
    ranking->keys[i] = point_light_rank_key(light, camera_position,
                                            ranking->previous[i].shadowed);
    ranking->drawn[eligible++] = i;
    light->casts_shadow = false_v;
  }
  const uint32_t shadowed =
      point_light_top(system->point_lights, ranking->keys, ranking->drawn,
                      eligible, shadow_max, ranking->order);
  for (uint32_t k = 0u; k < shadowed; ++k) {
    system->point_lights[ranking->order[k]].casts_shadow = true_v;
  }
  point_light_selection_record(state, system->point_lights, count, ranking,
                               NULL, camera_position, count);

  /* The drawn lights keep their table order, scaled by their fade. */
  uint32_t kept = 0u;
  for (uint32_t i = 0u; i < count; ++i) {
    if (ranking->weights[i] <= 0.0f) {
      continue;
    }
    VkrPointLight light = system->point_lights[i];
    light.intensity *= ranking->weights[i];
    system->point_light_unfaded_intensity[kept] =
        system->point_light_unfaded_intensity[i];
    system->point_lights[kept++] = light;
  }
  system->point_light_count = kept;
  system->point_light_dropped_count += count - kept;
  system->dirty = true_v;
}
