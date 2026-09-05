#include "ibl_math_tests.h"

#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_ibl_sh_pool.h"

static bool32_t test_float32_to_float16_boundaries(void) {
  printf("  Running test_float32_to_float16_boundaries...\n");
  assert(vkr_float32_to_float16(0.0f) == 0x0000u);
  assert(vkr_float32_to_float16(-0.0f) == 0x8000u);
  assert(vkr_float32_to_float16(ldexpf(1.0f, -24)) == 0x0001u);
  assert(vkr_float32_to_float16(ldexpf(1.0f, -14)) == 0x0400u);
  assert(vkr_float32_to_float16(65504.0f) == 0x7bffu);
  assert(vkr_float32_to_float16(70000.0f) == 0x7c00u);
  assert(vkr_float32_to_float16(-1.0f) == 0xbc00u);
  assert(vkr_float32_to_float16(INFINITY) == 0x7c00u);
  assert(vkr_float32_to_float16(-INFINITY) == 0xfc00u);
  assert((vkr_float32_to_float16(NAN) & 0x7c00u) == 0x7c00u);
  assert((vkr_float32_to_float16(NAN) & 0x03ffu) != 0u);
  assert(vkr_float16_to_float32(0x0000u) == 0.0f);
  assert(signbit(vkr_float16_to_float32(0x8000u)));
  assert(vkr_float16_to_float32(0x0001u) == ldexpf(1.0f, -24));
  assert(vkr_float16_to_float32(0x0400u) == ldexpf(1.0f, -14));
  assert(vkr_float16_to_float32(0x3c00u) == 1.0f);
  assert(vkr_float16_to_float32(0xbc00u) == -1.0f);
  assert(isinf(vkr_float16_to_float32(0x7c00u)));
  assert(isnan(vkr_float16_to_float32(0x7e00u)));
  printf("  test_float32_to_float16_boundaries PASSED\n");
  return true_v;
}

static bool32_t test_ibl_cubemap_size_derivation(void) {
  printf("  Running test_ibl_cubemap_size_derivation...\n");
  uint32_t face_size = 0u;
  uint32_t mip_count = 0u;
  assert(vkr_ibl_derive_cubemap_size(4096u, 2048u, 4096u, 16u, &face_size,
                                     &mip_count));
  assert(face_size == 1024u && mip_count == 11u);
  assert(vkr_ibl_derive_cubemap_size(4096u, 2048u, 600u, 16u, &face_size,
                                     &mip_count));
  assert(face_size == 512u && mip_count == 10u);
  assert(vkr_ibl_derive_cubemap_size(4096u, 2048u, 4096u, 5u, &face_size,
                                     &mip_count));
  assert(face_size == 1024u && mip_count == 5u);
  assert(!vkr_ibl_derive_cubemap_size(1024u, 1024u, 1024u, 16u, &face_size,
                                      &mip_count));
  assert(!vkr_ibl_derive_cubemap_size(UINT32_MAX, UINT32_MAX / 2u + 1u, 1024u,
                                      16u, &face_size, &mip_count));
  printf("  test_ibl_cubemap_size_derivation PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_projection_mip_selection(void) {
  printf("  Running test_ibl_sh_projection_mip_selection...\n");
  uint32_t mip = 0u;
  uint32_t extent = 0u;

  // Greatest available extent not larger than 32.
  assert(vkr_ibl_sh_projection_mip(1024u, 11u, &mip, &extent));
  assert(mip == 5u && extent == 32u);
  assert(vkr_ibl_sh_projection_mip(256u, 9u, &mip, &extent));
  assert(mip == 3u && extent == 32u);
  assert(vkr_ibl_sh_projection_mip(32u, 6u, &mip, &extent));
  assert(mip == 0u && extent == 32u);

  // A source already below the target extent uses mip 0.
  assert(vkr_ibl_sh_projection_mip(16u, 5u, &mip, &extent));
  assert(mip == 0u && extent == 16u);
  assert(vkr_ibl_sh_projection_mip(1u, 1u, &mip, &extent));
  assert(mip == 0u && extent == 1u);

  // A truncated mip chain clamps to the last available level rather than
  // addressing a level the image does not have.
  assert(vkr_ibl_sh_projection_mip(1024u, 3u, &mip, &extent));
  assert(mip == 2u && extent == 256u);

  assert(!vkr_ibl_sh_projection_mip(0u, 4u, &mip, &extent));
  assert(!vkr_ibl_sh_projection_mip(64u, 0u, &mip, &extent));
  assert(!vkr_ibl_sh_projection_mip(64u, 4u, NULL, &extent));
  printf("  test_ibl_sh_projection_mip_selection PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_publication_cycle(void) {
  printf("  Running test_ibl_sh_pool_publication_cycle...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  VkrShPoolMetrics metrics;
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  assert(metrics.published_count == 0u && metrics.retired_count == 0u);

  // The black sentinel is never handed out.
  uint32_t slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &slot) == VKR_SH_POOL_STATUS_OK);
  assert(slot != VKR_SH_SLOT_BLACK && slot < VKR_SH_SLOT_CAPACITY);

  assert(vkr_ibl_sh_pool_mark_recorded(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_publish(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.published_count == 1u);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS - 1u);

  // Publication order is enforced: a published slot cannot be re-published and
  // a free slot cannot skip straight to published.
  assert(vkr_ibl_sh_pool_publish(&pool, slot) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, slot) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);

  vkr_ibl_sh_pool_reference(&pool, slot, 7u);
  assert(vkr_ibl_sh_pool_retire(&pool, slot) == VKR_SH_POOL_STATUS_OK);

  // A retired slot stays out of circulation until its last reader completes.
  // Anything less than the recorded serial must not release it.
  assert(vkr_ibl_sh_pool_collect(&pool, 6u) == 0u);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.retired_count == 1u);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS - 1u);

  assert(vkr_ibl_sh_pool_collect(&pool, 7u) == 1u);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.retired_count == 0u);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  printf("  test_ibl_sh_pool_publication_cycle PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_abandon_and_exhaustion(void) {
  printf("  Running test_ibl_sh_pool_abandon_and_exhaustion...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  // A pre-submit failure releases the candidate immediately: nothing was
  // submitted, so no reader can exist.
  uint32_t abandoned = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &abandoned) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_abandon(&pool, abandoned) == VKR_SH_POOL_STATUS_OK);
  VkrShPoolMetrics metrics;
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  assert(metrics.abandon_count == 1u);
  assert(vkr_ibl_sh_pool_abandon(&pool, abandoned) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);

  // Capacity covers two generations of every logical source that can be live.
  uint32_t slots[VKR_SH_REUSABLE_SLOTS];
  for (uint32_t i = 0u; i < VKR_SH_REUSABLE_SLOTS; ++i) {
    assert(vkr_ibl_sh_pool_reserve(&pool, &slots[i]) == VKR_SH_POOL_STATUS_OK);
  }
  assert(VKR_SH_REUSABLE_SLOTS == VKR_SH_LOGICAL_MAX * VKR_SH_GENERATION_COUNT);

  // Exhaustion reports an error rather than waiting or overwriting.
  uint32_t overflow = 0xffffffffu;
  assert(vkr_ibl_sh_pool_reserve(&pool, &overflow) ==
         VKR_SH_POOL_STATUS_EXHAUSTED);
  assert(overflow == 0xffffffffu);
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.exhaustion_count == 1u);
  assert(metrics.free_count == 0u);

  // Every reserved slot is distinct: no two logical sources can be handed the
  // same storage.
  for (uint32_t i = 0u; i < VKR_SH_REUSABLE_SLOTS; ++i) {
    assert(slots[i] != VKR_SH_SLOT_BLACK);
    for (uint32_t j = i + 1u; j < VKR_SH_REUSABLE_SLOTS; ++j) {
      assert(slots[i] != slots[j]);
    }
  }
  printf("  test_ibl_sh_pool_abandon_and_exhaustion PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_replacement_generation(void) {
  printf("  Running test_ibl_sh_pool_replacement_generation...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  // Publish a generation and let frame 10 read it.
  uint32_t old_slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &old_slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, old_slot) ==
         VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_publish(&pool, old_slot) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_reference(&pool, old_slot, 10u);

  // A replacement generation must land on different storage while the old one
  // is still readable by an outstanding frame.
  uint32_t new_slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &new_slot) == VKR_SH_POOL_STATUS_OK);
  assert(new_slot != old_slot);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, new_slot) ==
         VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_publish(&pool, new_slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_retire(&pool, old_slot) == VKR_SH_POOL_STATUS_OK);

  // Frame 10 is still outstanding, so the old storage must not be reusable.
  assert(vkr_ibl_sh_pool_collect(&pool, 9u) == 0u);
  uint32_t probe_slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &probe_slot) == VKR_SH_POOL_STATUS_OK);
  assert(probe_slot != old_slot);
  assert(vkr_ibl_sh_pool_abandon(&pool, probe_slot) == VKR_SH_POOL_STATUS_OK);

  assert(vkr_ibl_sh_pool_collect(&pool, 10u) == 1u);

  // A reference from a later frame must not resurrect a collected slot's
  // retirement serial; slot 0 and free slots ignore references entirely.
  vkr_ibl_sh_pool_reference(&pool, VKR_SH_SLOT_BLACK, 99u);
  vkr_ibl_sh_pool_reference(&pool, old_slot, 99u);
  assert(pool.last_reader_serial[old_slot] == 0u);
  assert(vkr_ibl_sh_pool_retire(&pool, old_slot) ==
         VKR_SH_POOL_STATUS_INVALID_STATE);

  // Scene reset retires the live publication; the pool itself survives.
  vkr_ibl_sh_pool_reference(&pool, new_slot, 12u);
  assert(vkr_ibl_sh_pool_retire(&pool, new_slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_collect(&pool, 11u) == 0u);
  assert(vkr_ibl_sh_pool_collect(&pool, 12u) == 1u);

  // Back to the black-sentinel-only baseline, with no leaked retirements.
  VkrShPoolMetrics metrics;
  vkr_ibl_sh_pool_get_metrics(&pool, &metrics);
  assert(metrics.free_count == VKR_SH_REUSABLE_SLOTS);
  assert(metrics.published_count == 0u && metrics.retired_count == 0u &&
         metrics.reserved_count == 0u && metrics.recorded_count == 0u);
  assert(metrics.exhaustion_count == 0u);
  printf("  test_ibl_sh_pool_replacement_generation PASSED\n");
  return true_v;
}

static bool32_t test_ibl_sh_pool_same_submission_bake_and_read(void) {
  printf("  Running test_ibl_sh_pool_same_submission_bake_and_read...\n");
  VkrShSlotPool pool;
  vkr_ibl_sh_pool_init(&pool);

  // A frame may consume the candidate it bakes, so a RECORDED slot accrues
  // readers before publication. Retiring it must still respect that reader.
  uint32_t slot = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_mark_recorded(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_reference(&pool, slot, 42u);

  // Publication failed after submission succeeded: the slot follows normal
  // retirement rather than immediate reuse, because the GPU accepted the work.
  assert(vkr_ibl_sh_pool_retire(&pool, slot) == VKR_SH_POOL_STATUS_OK);
  assert(vkr_ibl_sh_pool_collect(&pool, 41u) == 0u);
  assert(vkr_ibl_sh_pool_collect(&pool, 42u) == 1u);

  // A reserved-but-never-recorded slot has no reader and never gained one.
  uint32_t reserved = 0u;
  assert(vkr_ibl_sh_pool_reserve(&pool, &reserved) == VKR_SH_POOL_STATUS_OK);
  vkr_ibl_sh_pool_reference(&pool, reserved, 50u);
  assert(pool.last_reader_serial[reserved] == 0u);
  assert(vkr_ibl_sh_pool_abandon(&pool, reserved) == VKR_SH_POOL_STATUS_OK);

  assert(vkr_ibl_sh_pool_reserve(NULL, &reserved) ==
         VKR_SH_POOL_STATUS_INVALID_ARGUMENT);
  assert(vkr_ibl_sh_pool_retire(&pool, VKR_SH_SLOT_BLACK) ==
         VKR_SH_POOL_STATUS_INVALID_ARGUMENT);
  assert(vkr_ibl_sh_pool_retire(&pool, VKR_SH_SLOT_CAPACITY) ==
         VKR_SH_POOL_STATUS_INVALID_ARGUMENT);
  printf("  test_ibl_sh_pool_same_submission_bake_and_read PASSED\n");
  return true_v;
}

bool32_t run_ibl_math_tests(void) {
  printf("--- Starting HDR IBL Math Tests ---\n");
  bool32_t passed = true_v;
  passed &= test_float32_to_float16_boundaries();
  passed &= test_ibl_cubemap_size_derivation();
  passed &= test_ibl_sh_projection_mip_selection();
  passed &= test_ibl_sh_pool_publication_cycle();
  passed &= test_ibl_sh_pool_abandon_and_exhaustion();
  passed &= test_ibl_sh_pool_replacement_generation();
  passed &= test_ibl_sh_pool_same_submission_bake_and_read();
  printf("--- HDR IBL Math Tests Completed ---\n");
  return passed;
}
