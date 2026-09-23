#include "animation_cooked_tests.h"
#include "test_stats.h"

#include "assets/vkr_animation_cooked.h"
#include "assets/vkr_animation_encode.h"
#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"

#include <assert.h>

static void test_u32(uint8_t *bytes, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    bytes[i] = (uint8_t)(value >> (8u * i));
  }
}

static void test_u64(uint8_t *bytes, uint64_t value) {
  for (uint32_t i = 0; i < 8; ++i) {
    bytes[i] = (uint8_t)(value >> (8u * i));
  }
}

static void test_integrity(uint8_t *bytes, uint64_t size) {
  test_u64(bytes + 8, size);
  MemZero(bytes + 24, 8);
  uint64_t hash = UINT64_C(14695981039346656037);
  for (uint64_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
  }
  test_u64(bytes + 24, hash);
}

/* Independent on-disk fixture: one identity root, one linear translation
 * channel from x=0 at t=0 to x=2 at t=1. Fixed offsets and IEEE bit patterns
 * ensure that agreement between encoder and decoder is not the only oracle. */
static void test_fixture(uint8_t bytes[236]) {
  MemZero(bytes, 236);
  MemCopy(bytes, "VKA1", 4);
  test_u32(bytes + 4, 1);
  test_u64(bytes + 16, UINT64_C(0x123456789abcdef0));
  test_u32(bytes + 32, 1);
  test_u32(bytes + 40, 1);
  test_u32(bytes + 52, UINT32_MAX);
  for (uint32_t i = 0; i < 4; ++i) {
    test_u32(bytes + 60 + i * 20, 0x3f800000u);
  }
  test_u32(bytes + 148, 0x3f800000u);
  test_u32(bytes + 152, 0x3f800000u);
  test_u32(bytes + 156, 0x3f800000u);
  test_u32(bytes + 160, 0x3f800000u);
  test_u32(bytes + 172, 0x3f800000u);
  test_u32(bytes + 176, 1);
  test_u32(bytes + 188, 1);
  test_u32(bytes + 192, 2);
  test_u32(bytes + 200, 0x3f800000u);
  test_u32(bytes + 220, 0x40000000u);
  test_integrity(bytes, 236);
}

static void test_rejected(VkrAllocator *result, VkrAllocator *scratch,
                          const uint8_t *bytes, uint64_t size) {
  VkrAnimationAsset output;
  MemSet(&output, 0xa5, sizeof(output));
  const char *error = NULL;
  uint64_t before = result->stats.total_allocated;
  assert(!vkr_animation_cooked_decode(result, scratch, bytes, size, &output,
                                      &error));
  VkrAnimationAsset zero = {0};
  assert(MemCompare(&output, &zero, sizeof(output)) == 0);
  VKR_TEST_ASSERT_STATS(result->stats.total_allocated == before);
  assert(error != NULL);
}

bool32_t run_animation_cooked_tests(void) {
  Arena *result_arena = arena_create(MB(16), MB(1));
  Arena *scratch_arena = arena_create(MB(16), MB(1));
  assert(result_arena && scratch_arena);
  VkrAllocator result = {.ctx = result_arena};
  VkrAllocator scratch = {.ctx = scratch_arena};
  assert(vkr_allocator_arena(&result));
  assert(vkr_allocator_arena(&scratch));
  VkrAllocatorScope result_scope = vkr_allocator_begin_scope(&result);
  VkrAllocatorScope scratch_scope = vkr_allocator_begin_scope(&scratch);
  assert(vkr_allocator_scope_is_valid(&result_scope));
  assert(vkr_allocator_scope_is_valid(&scratch_scope));

  uint8_t fixture[236];
  test_fixture(fixture);
  VkrAnimationAsset decoded = {0};
  const char *error = NULL;
  assert(vkr_animation_cooked_decode(&result, &scratch, fixture,
                                     sizeof(fixture), &decoded, &error));
  assert(!error);
  assert(decoded.source_fingerprint == UINT64_C(0x123456789abcdef0));
  assert(decoded.node_count == 1 && decoded.clip_count == 1);
  assert(decoded.nodes[0].parent == VKR_ANIMATION_NO_NODE);
  assert(decoded.nodes[0].local.elements[15] == 1.0f);
  assert(decoded.clips[0].channels[0].times[1] == 1.0f);
  assert(decoded.clips[0].channels[0].values[1].x == 2.0f);
  MemZero(fixture, sizeof(fixture));
  assert(decoded.clips[0].channels[0].values[1].x == 2.0f);

  uint8_t *encoded = NULL;
  uint64_t encoded_size = 0;
  assert(vkr_animation_cooked_encode(&scratch, &decoded, &encoded,
                                     &encoded_size, &error));
  test_fixture(fixture);
  assert(encoded_size == sizeof(fixture));
  assert(MemCompare(encoded, fixture, sizeof(fixture)) == 0);

  /* Every truncated prefix is rejected even with a coherent size/checksum,
   * exposing bounds checks beyond the integrity gate. */
  for (uint64_t size = 0; size < sizeof(fixture); ++size) {
    uint8_t truncated[236];
    MemCopy(truncated, fixture, sizeof(truncated));
    if (size >= 48) {
      test_integrity(truncated, size);
    }
    test_rejected(&result, &scratch, truncated, size);
  }
  static const struct {
    uint32_t offset;
    uint32_t value;
  } corruptions[] = {
      {4, 2},             /* Unsupported version. */
      {32, UINT32_MAX},   /* Count bomb. */
      {44, 1},            /* Reserved header field. */
      {48, UINT32_MAX},   /* Name exceeds remaining bytes. */
      {52, 0},            /* Self-parent cycle, semantic validation. */
      {56, 2},            /* Non-boolean matrix flag. */
      {164, 1},           /* Invalid topology permutation. */
      {176, UINT32_MAX},  /* Channel allocation bomb. */
      {180, 1},           /* Missing target node. */
      {184, 3},           /* Unsupported path. */
      {188, 3},           /* Unsupported interpolation. */
      {192, UINT32_MAX},  /* Key allocation bomb. */
      {200, 0},           /* Duplicate key time. */
      {220, 0x7fc00000u}, /* NaN value. */
  };
  for (uint32_t i = 0; i < ArrayCount(corruptions); ++i) {
    uint8_t corrupt[236];
    MemCopy(corrupt, fixture, sizeof(corrupt));
    test_u32(corrupt + corruptions[i].offset, corruptions[i].value);
    test_integrity(corrupt, sizeof(corrupt));
    test_rejected(&result, &scratch, corrupt, sizeof(corrupt));
  }
  fixture[220] ^= 1;
  test_rejected(&result, &scratch, fixture, sizeof(fixture));
  test_fixture(fixture);
  uint8_t extra[237];
  MemCopy(extra, fixture, sizeof(fixture));
  extra[236] = 0;
  test_integrity(extra, sizeof(extra));
  test_rejected(&result, &scratch, extra, sizeof(extra));
  test_rejected(&result, &scratch, fixture, VKR_ANIMATION_COOKED_MAX_BYTES + 1);

  /* Cubic tangent and signed-zero bits survive cooking unchanged. */
  Vec4 cubic[6] = {
      {.elements = {-0.0f, 2.0f, -3.0f, 0.0f}},
      {.elements = {0.0f, 0.0f, 0.0f, 0.0f}},
      {.elements = {4.0f, 5.0f, 6.0f, 0.0f}},
      {.elements = {-7.0f, 8.0f, 9.0f, 0.0f}},
      {.elements = {2.0f, 0.0f, 0.0f, 0.0f}},
      {.elements = {10.0f, -11.0f, 12.0f, 0.0f}},
  };
  decoded.clips[0].channels[0].interpolation = VKR_ANIMATION_CUBIC_SPLINE;
  decoded.clips[0].channels[0].values = cubic;
  decoded.nodes[0].name = (String8){.str = (uint8_t *)"root", .length = 4};
  decoded.clips[0].name = (String8){.str = (uint8_t *)"walk", .length = 4};
  uint32_t joint = 0;
  Mat4 inverse_bind = decoded.nodes[0].local;
  inverse_bind.elements[12] = -2.0f;
  VkrAnimationSkin skin = {
      .name = {.str = (uint8_t *)"rig", .length = 3},
      .skeleton_node = 0,
      .joint_count = 1,
      .joints = &joint,
      .inverse_bind = &inverse_bind,
  };
  decoded.skin_count = 1;
  decoded.skins = &skin;
  assert(vkr_animation_cooked_encode(&scratch, &decoded, &encoded,
                                     &encoded_size, &error));
  VkrAnimationAsset cubic_decoded = {0};
  assert(vkr_animation_cooked_decode(&result, &scratch, encoded, encoded_size,
                                     &cubic_decoded, &error));
  assert(cubic_decoded.nodes[0].name.length == 4);
  assert(MemCompare(cubic_decoded.nodes[0].name.str, "root", 4) == 0);
  assert(MemCompare(cubic_decoded.clips[0].channels[0].values, cubic,
                    sizeof(cubic)) == 0);

  assert(cubic_decoded.skin_count == 1);
  assert(cubic_decoded.skins[0].joint_count == 1);
  assert(cubic_decoded.skins[0].joints[0] == 0);
  assert(cubic_decoded.skins[0].skeleton_node == 0);
  assert(cubic_decoded.skins[0].name.length == 3);
  assert(MemCompare(cubic_decoded.skins[0].name.str, "rig", 3) == 0);
  assert(MemCompare(&cubic_decoded.skins[0].inverse_bind[0], &inverse_bind,
                    sizeof(inverse_bind)) == 0);
  assert(cubic_decoded.clips[0].name.length == 4);
  assert(MemCompare(cubic_decoded.clips[0].name.str, "walk", 4) == 0);

  vkr_allocator_end_scope(&scratch_scope, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  vkr_allocator_end_scope(&result_scope, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  vkr_allocator_release_global_accounting(&scratch);
  vkr_allocator_release_global_accounting(&result);
  arena_destroy(scratch_arena);
  arena_destroy(result_arena);
  return true_v;
}
