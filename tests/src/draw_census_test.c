#include "draw_census_test.h"

#include "vkr_frame_input.h"
#include "vkr_gpu_abi.h"

#include <assert.h>
#include <stdio.h>

/* Metal's drawn classification word (gpu_draws.metal): bucket + 1 in the
   bucket bits, the Custom and BRDF-extension bits, the LOD state and the
   Custom slot above them. These mirror the kernel's own constants. */
#define METAL_CUSTOM_CLASS_BIT 16u
#define METAL_BRDF_CLASS_BIT 32u
#define METAL_CUSTOM_CLASS_SLOT_SHIFT 27u

/* The fullest LOD state a classification word carries: the deepest level,
   both morph factors at one and the geomorph bit. */
vkr_internal uint32_t census_full_lod_state(void) {
  return (VKR_GPU_DRAW_LOD_MASK << VKR_GPU_DRAW_LOD_SHIFT) |
         (255u << VKR_GPU_DRAW_MORPH_SHIFT) |
         (255u << VKR_GPU_DRAW_PREVIOUS_MORPH_SHIFT) |
         VKR_GPU_DRAW_GEOMORPH_BIT;
}

/* Metal's encode skips a word whose bucket bits are all ones; it reads the
   bucket as those bits minus one. */
vkr_internal bool8_t census_metal_encodes(uint32_t word) {
  return vkr_gpu_draw_word_drawn(word);
}

/* Vulkan's encode skips a word whose bucket bits name no bucket. */
vkr_internal bool8_t census_vulkan_encodes(uint32_t word) {
  return (word & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) <
         VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
}

/* Every rejection reason keeps all ones in the bucket bits and its reason
   above them, and neither backend encodes it; every drawn word, with the
   fullest high bits either backend writes, keeps its bucket and encodes on
   its backend. A reason word never equals the deferred word. */
vkr_internal void test_draw_census_encodings(void) {
  printf("  Running test_draw_census_encodings...\n");
  const VkrGpuDrawRejectReason reasons[] = {
      VKR_GPU_DRAW_REJECT_FILTERED, VKR_GPU_DRAW_REJECT_FRUSTUM,
      VKR_GPU_DRAW_REJECT_DISTANCE, VKR_GPU_DRAW_REJECT_OCCLUDED,
      VKR_GPU_DRAW_REJECT_CASTER_SMALL};
  for (uint32_t i = 0u; i < ArrayCount(reasons); ++i) {
    const uint32_t word = vkr_gpu_draw_rejected(reasons[i]);
    assert((word & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) ==
           VKR_GPU_DRAW_REJECTED_BUCKET);
    assert(word >> VKR_GPU_DRAW_REJECT_REASON_SHIFT == (uint32_t)reasons[i]);
    assert(word != VKR_GPU_DRAW_DEFERRED);
    assert(!vkr_gpu_draw_word_drawn(word));
    assert(!census_metal_encodes(word) && !census_vulkan_encodes(word));
  }
  assert(!census_metal_encodes(VKR_GPU_DRAW_DEFERRED) &&
         !census_vulkan_encodes(VKR_GPU_DRAW_DEFERRED));

  const uint32_t lod = census_full_lod_state();
  for (uint32_t bucket = 0u; bucket < VKR_WORLD_DRAW_STATE_BUCKET_COUNT;
       ++bucket) {
    const uint32_t metal_words[] = {
        (bucket + 1u) | lod,
        (bucket + 1u) | lod | METAL_CUSTOM_CLASS_BIT | METAL_BRDF_CLASS_BIT |
            (31u << METAL_CUSTOM_CLASS_SLOT_SHIFT),
    };
    for (uint32_t i = 0u; i < ArrayCount(metal_words); ++i) {
      assert(census_metal_encodes(metal_words[i]));
      assert((metal_words[i] & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) - 1u ==
             bucket);
      assert(vkr_gpu_draw_camera_state(metal_words[i], 0u) ==
             VKR_GPU_DRAW_CENSUS_DRAWN);
    }
    const uint32_t vulkan_word = bucket | lod;
    assert(census_vulkan_encodes(vulkan_word));
    assert((vulkan_word & VKR_GPU_DRAW_STATE_BUCKET_WORD_MASK) == bucket);
    assert(vkr_gpu_draw_camera_state(vulkan_word, 0u) ==
           VKR_GPU_DRAW_CENSUS_DRAWN);
  }
  /* Vulkan's drawn word for bucket 0 at level 0 is zero. */
  assert(census_vulkan_encodes(0u) &&
         vkr_gpu_draw_camera_state(0u, 0u) == VKR_GPU_DRAW_CENSUS_DRAWN);
  printf("  test_draw_census_encodings PASSED\n");
}

/* The census state of every early word, and of every late word after a
   deferral. */
vkr_internal void test_draw_census_camera_state(void) {
  printf("  Running test_draw_census_camera_state...\n");
  const uint32_t drawn = 3u | census_full_lod_state();
  const uint32_t filtered = vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_FILTERED);
  const uint32_t frustum = vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_FRUSTUM);
  const uint32_t distance = vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_DISTANCE);
  const uint32_t occluded = vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_OCCLUDED);
  const uint32_t small =
      vkr_gpu_draw_rejected(VKR_GPU_DRAW_REJECT_CASTER_SMALL);
  const struct {
    uint32_t early;
    uint32_t late;
    uint32_t state;
  } rows[] = {
      /* One phase, or a candidate the early phase decided: the late word
         is never read. */
      {drawn, filtered, VKR_GPU_DRAW_CENSUS_DRAWN},
      {filtered, drawn, VKR_GPU_DRAW_CENSUS_NOT_CAMERA},
      {frustum, drawn, VKR_GPU_DRAW_CENSUS_FRUSTUM},
      {distance, drawn, VKR_GPU_DRAW_CENSUS_DISTANCE},
      {occluded, drawn, VKR_GPU_DRAW_CENSUS_OCCLUDED},
      {small, drawn, VKR_GPU_DRAW_CENSUS_NOT_CAMERA},
      /* Deferred by the earlier frame's pyramid. */
      {VKR_GPU_DRAW_DEFERRED, drawn, VKR_GPU_DRAW_CENSUS_DRAWN_LATE},
      {VKR_GPU_DRAW_DEFERRED, occluded, VKR_GPU_DRAW_CENSUS_OCCLUDED},
      {VKR_GPU_DRAW_DEFERRED, frustum, VKR_GPU_DRAW_CENSUS_FRUSTUM},
      {VKR_GPU_DRAW_DEFERRED, distance, VKR_GPU_DRAW_CENSUS_DISTANCE},
      /* A deferred candidate is a camera candidate whatever else the late
         view says. */
      {VKR_GPU_DRAW_DEFERRED, filtered, VKR_GPU_DRAW_CENSUS_OCCLUDED},
      {VKR_GPU_DRAW_DEFERRED, VKR_GPU_DRAW_DEFERRED,
       VKR_GPU_DRAW_CENSUS_OCCLUDED},
  };
  for (uint32_t i = 0u; i < ArrayCount(rows); ++i) {
    assert(vkr_gpu_draw_camera_state(rows[i].early, rows[i].late) ==
           rows[i].state);
  }
  printf("  test_draw_census_camera_state PASSED\n");
}

/* Packs states as the census kernels do, one word per eight candidates,
   with a tail of five: every state reads back, and the tail word's unused
   lanes stay zero. */
vkr_internal void test_draw_census_packing(void) {
  printf("  Running test_draw_census_packing...\n");
  enum { CANDIDATES = 13u };
  uint32_t states[CANDIDATES];
  for (uint32_t i = 0u; i < CANDIDATES; ++i) {
    states[i] = (i * 7u + 1u) % VKR_GPU_DRAW_CENSUS_STATE_COUNT;
  }
  const uint32_t word_count = vkr_gpu_draw_census_word_count(CANDIDATES);
  assert(word_count == 2u);
  assert(vkr_gpu_draw_census_word_count(0u) == 0u &&
         vkr_gpu_draw_census_word_count(8u) == 1u &&
         vkr_gpu_draw_census_word_count(9u) == 2u);
  uint32_t words[2] = {0u, 0u};
  for (uint32_t word_index = 0u; word_index < word_count; ++word_index) {
    const uint32_t first = word_index * VKR_GPU_DRAW_CENSUS_STATES_PER_WORD;
    const uint32_t end =
        Min(first + VKR_GPU_DRAW_CENSUS_STATES_PER_WORD, (uint32_t)CANDIDATES);
    uint32_t word = 0u;
    for (uint32_t index = first; index < end; ++index) {
      word = vkr_gpu_draw_census_put(word, index - first, states[index]);
    }
    words[word_index] = word;
  }
  for (uint32_t i = 0u; i < CANDIDATES; ++i) {
    assert(vkr_gpu_draw_census_get(words, i) == states[i]);
  }
  assert(words[1] >> (5u * VKR_GPU_DRAW_CENSUS_STATE_BITS) == 0u);
  /* A full word's last lane takes the top four bits. */
  assert(vkr_gpu_draw_census_put(0u, 7u, VKR_GPU_DRAW_CENSUS_DISTANCE) ==
         (uint32_t)VKR_GPU_DRAW_CENSUS_DISTANCE << 28u);
  printf("  test_draw_census_packing PASSED\n");
}

/* An object drawn by any of its candidates counts as drawn; otherwise drawn
   late, occluded, outside the frustum, past its distance, then not a
   camera candidate, whatever order its candidates come in. */
vkr_internal void test_draw_census_object_reduction(void) {
  printf("  Running test_draw_census_object_reduction...\n");
  const uint32_t order[] = {
      VKR_GPU_DRAW_CENSUS_NOT_CAMERA, VKR_GPU_DRAW_CENSUS_DISTANCE,
      VKR_GPU_DRAW_CENSUS_FRUSTUM,    VKR_GPU_DRAW_CENSUS_OCCLUDED,
      VKR_GPU_DRAW_CENSUS_DRAWN_LATE, VKR_GPU_DRAW_CENSUS_DRAWN};
  for (uint32_t low = 0u; low < ArrayCount(order); ++low) {
    for (uint32_t high = low; high < ArrayCount(order); ++high) {
      assert(vkr_gpu_draw_census_merge(order[low], order[high]) == order[high]);
      assert(vkr_gpu_draw_census_merge(order[high], order[low]) == order[high]);
    }
  }
  /* Folding an object's candidates from not-a-candidate. */
  const uint32_t candidates[] = {
      VKR_GPU_DRAW_CENSUS_FRUSTUM, VKR_GPU_DRAW_CENSUS_OCCLUDED,
      VKR_GPU_DRAW_CENSUS_DISTANCE, VKR_GPU_DRAW_CENSUS_NOT_CAMERA};
  uint32_t object = VKR_GPU_DRAW_CENSUS_NOT_CAMERA;
  for (uint32_t i = 0u; i < ArrayCount(candidates); ++i) {
    object = vkr_gpu_draw_census_merge(object, candidates[i]);
  }
  assert(object == VKR_GPU_DRAW_CENSUS_OCCLUDED);
  printf("  test_draw_census_object_reduction PASSED\n");
}

vkr_internal VkrWorldDrawCandidate census_candidate(uint32_t object_id,
                                                    Mat4 model, Vec4 sphere,
                                                    Vec3 half_extents) {
  VkrWorldDrawCandidate candidate = {0};
  candidate.instance.model = model;
  candidate.instance.object_id = object_id;
  candidate.local_bounding_sphere = sphere;
  candidate.local_half_extents = half_extents;
  return candidate;
}

vkr_internal bool8_t census_near(float32_t a, float32_t b) {
  const float32_t difference = a - b;
  return difference < 1e-4f && difference > -1e-4f;
}

/* One object's candidates union into one box sorted among the others: a
   box turned a quarter about Y swaps its X and Z extents, and a candidate
   without a box takes its sphere under the model's largest scale. */
vkr_internal void test_draw_census_object_bounds(void) {
  printf("  Running test_draw_census_object_bounds...\n");
  /* A quarter turn about Y, moved to (10, 0, 0). */
  Mat4 turned = mat4_identity();
  turned.cols[0] = vec4_new(0.0f, 0.0f, -1.0f, 0.0f);
  turned.cols[2] = vec4_new(1.0f, 0.0f, 0.0f, 0.0f);
  turned.cols[3] = vec4_new(10.0f, 0.0f, 0.0f, 1.0f);
  /* Scaled by two along Y, at (0, 5, 0). */
  Mat4 tall = mat4_identity();
  tall.cols[1] = vec4_new(0.0f, 2.0f, 0.0f, 0.0f);
  tall.cols[3] = vec4_new(0.0f, 5.0f, 0.0f, 1.0f);
  const VkrWorldDrawCandidate candidates[] = {
      census_candidate(9u, turned, vec4_new(0.0f, 0.0f, 0.0f, 3.0f),
                       vec3_new(2.0f, 1.0f, 0.5f)),
      census_candidate(4u, mat4_identity(), vec4_new(1.0f, 1.0f, 1.0f, 1.0f),
                       vec3_new(1.0f, 1.0f, 1.0f)),
      census_candidate(9u, tall, vec4_new(0.0f, 0.0f, 0.0f, 1.0f),
                       vec3_new(0.0f, 0.0f, 0.0f)),
  };
  const VkrWorldPassPayload world = {
      .gpu_candidates = candidates,
      .gpu_candidate_count = ArrayCount(candidates),
  };
  VkrWorldObjectBounds rows[ArrayCount(candidates)];
  assert(vkr_world_object_bounds(&world, rows, ArrayCount(rows) - 1u) == 0u);
  const uint32_t count =
      vkr_world_object_bounds(&world, rows, ArrayCount(rows));
  assert(count == 2u);
  assert(rows[0].object_id == 4u && rows[1].object_id == 9u);
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    assert(census_near(rows[0].min[axis], 0.0f) &&
           census_near(rows[0].max[axis], 2.0f));
  }
  /* The turned box spans x 10 +- 0.5, y +- 1, z +- 2; the sphere of
     radius one scaled by two spans +-2 about (0, 5, 0). */
  assert(census_near(rows[1].min[0], -2.0f) &&
         census_near(rows[1].max[0], 10.5f));
  assert(census_near(rows[1].min[1], -1.0f) &&
         census_near(rows[1].max[1], 7.0f));
  assert(census_near(rows[1].min[2], -2.0f) &&
         census_near(rows[1].max[2], 2.0f));
  printf("  test_draw_census_object_bounds PASSED\n");
}

bool32_t run_draw_census_tests(void) {
  printf("--- Starting draw census tests ---\n");
  test_draw_census_encodings();
  test_draw_census_camera_state();
  test_draw_census_packing();
  test_draw_census_object_reduction();
  test_draw_census_object_bounds();
  printf("--- Draw census tests completed ---\n");
  return true;
}
