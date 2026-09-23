#include "test_main.h"
#include "animation_cooked_tests.h"
#include "animation_graph_tests.h"
#include "animation_import_tests.h"
#include "animation_loader_tests.h"
#include "animation_player_tests.h"
#include "animation_tests.h"
#include "camera_rig_test.h"
#include "character_test.h"
#include "collision_asset_test.h"
#include "editor_project_store_test.h"
#include "gameplay_input_test.h"
#include "gameplay_player_test.h"
#include "hash_test.h"
#include "mesh_skin_tests.h"
#include "physics_test.h"
#include "player_animation_test.h"
#include "scene_animation_tests.h"
#include "scene_physics_test.h"
#include "scene_simulation_test.h"
#include "weapon_test.h"

bool32_t run_path_io_tests(void);
bool32_t run_process_path_tests(void);
int32_t process_path_test_child(void);
bool32_t run_asset_path_tests(void);
bool32_t run_local_shadow_tests(void);

typedef bool32_t (*VkrTestSuite)(void);

typedef struct VkrTestSuiteEntry {
  const char *name;
  VkrTestSuite run;
} VkrTestSuiteEntry;

#define VKR_TEST_SUITE(fn) {#fn, fn}

static const VkrTestSuiteEntry VKR_TEST_SUITES[] = {
    VKR_TEST_SUITE(run_physics_tests),
    VKR_TEST_SUITE(run_collision_asset_tests),
    VKR_TEST_SUITE(run_scene_physics_tests),
    VKR_TEST_SUITE(run_scene_simulation_tests),
    VKR_TEST_SUITE(run_weapon_tests),
    VKR_TEST_SUITE(run_camera_rig_tests),
    VKR_TEST_SUITE(run_gameplay_input_tests),
    VKR_TEST_SUITE(run_gameplay_player_tests),
    VKR_TEST_SUITE(run_player_animation_tests),
    VKR_TEST_SUITE(run_character_tests),
    VKR_TEST_SUITE(run_hash_tests),
    VKR_TEST_SUITE(run_allocator_tests),
    VKR_TEST_SUITE(run_atomic_tests),
    VKR_TEST_SUITE(run_metrics_tests),
    VKR_TEST_SUITE(run_arena_tests),
    VKR_TEST_SUITE(run_array_tests),
    VKR_TEST_SUITE(run_vector_tests),
    VKR_TEST_SUITE(run_queue_tests),
    VKR_TEST_SUITE(run_event_data_buffer_tests),
    VKR_TEST_SUITE(run_threads_tests),
    VKR_TEST_SUITE(run_job_system_tests),
    VKR_TEST_SUITE(run_input_tests),
    VKR_TEST_SUITE(run_debug_overlay_tests),
    VKR_TEST_SUITE(run_json_tests),
    VKR_TEST_SUITE(run_json_writer_tests),
    VKR_TEST_SUITE(run_harness_tests),
    VKR_TEST_SUITE(run_event_tests),
    VKR_TEST_SUITE(run_math_tests),
    VKR_TEST_SUITE(run_vec_tests),
    VKR_TEST_SUITE(run_mat_tests),
    VKR_TEST_SUITE(run_quat_tests),
    VKR_TEST_SUITE(run_transform_tests),
    VKR_TEST_SUITE(run_simd_tests),
    VKR_TEST_SUITE(run_string_tests),
    VKR_TEST_SUITE(run_text_tests),
    VKR_TEST_SUITE(run_font_cooked_tests),
    VKR_TEST_SUITE(run_texture_format_tests),
    VKR_TEST_SUITE(run_texture_hdr_tests),
    VKR_TEST_SUITE(run_texture_lifetime_tests),
    VKR_TEST_SUITE(run_ibl_math_tests),
    VKR_TEST_SUITE(run_lighting_system_tests),
    VKR_TEST_SUITE(run_local_shadow_tests),
    VKR_TEST_SUITE(run_texture_vkt_tests),
    VKR_TEST_SUITE(run_renderer_impl_tests),
    VKR_TEST_SUITE(run_vulkan_tests),
    VKR_TEST_SUITE(run_packet_constants_tests),
    VKR_TEST_SUITE(run_temporal_tests),
    VKR_TEST_SUITE(run_exposure_tests),
    VKR_TEST_SUITE(run_bloom_tests),
    VKR_TEST_SUITE(run_gtao_tests),
    VKR_TEST_SUITE(run_visibility_tests),
    VKR_TEST_SUITE(run_editor_viewport_tests),
    VKR_TEST_SUITE(run_ui_layout_tests),
    VKR_TEST_SUITE(run_shadow_system_tests),
    VKR_TEST_SUITE(run_render_graph_barrier_tests),
    VKR_TEST_SUITE(run_resource_async_state_tests),
    VKR_TEST_SUITE(run_scene_loader_tests),
    VKR_TEST_SUITE(run_scene_edit_tests),
    VKR_TEST_SUITE(run_editor_project_store_tests),
    VKR_TEST_SUITE(run_gltf_importer_tests),
    VKR_TEST_SUITE(run_animation_tests),
    VKR_TEST_SUITE(run_animation_cooked_tests),
    VKR_TEST_SUITE(run_animation_import_tests),
    VKR_TEST_SUITE(run_mesh_skin_tests),
    VKR_TEST_SUITE(run_animation_player_tests),
    VKR_TEST_SUITE(run_animation_graph_tests),
    VKR_TEST_SUITE(run_animation_loader_tests),
    VKR_TEST_SUITE(run_scene_animation_tests),
    VKR_TEST_SUITE(run_material_pbr_tests),
    VKR_TEST_SUITE(run_mesh_cooked_tests),
    VKR_TEST_SUITE(run_filesystem_tests),
    VKR_TEST_SUITE(run_asset_path_tests),
    VKR_TEST_SUITE(run_path_io_tests),
    VKR_TEST_SUITE(run_process_path_tests),
    VKR_TEST_SUITE(run_hashtable_tests),
    VKR_TEST_SUITE(run_freelist_tests),
    VKR_TEST_SUITE(run_metal_memory_tests),
    VKR_TEST_SUITE(run_metal_diagnostics_tests),
    VKR_TEST_SUITE(run_metal_packet_abi_tests),
    VKR_TEST_SUITE(run_metal_capture_ring_tests),
    VKR_TEST_SUITE(run_metal_material_tests),
    VKR_TEST_SUITE(run_pool_tests),
    VKR_TEST_SUITE(run_dmemory_tests),
    VKR_TEST_SUITE(run_entity_tests),
};

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--process-path-test-child") == 0) {
    return process_path_test_child();
  }
  printf("Running tests...\n\n");

  vkr_platform_init();

  Arena *log_arena = arena_create(MB(1), MB(1));
  assert(log_init(log_arena));

  // `--suite <text>` runs only the suites whose registered name contains it.
  const char *filter =
      argc == 3 && strcmp(argv[1], "--suite") == 0 ? argv[2] : NULL;
  bool32_t all_passed = true;
  uint32_t suites_run = 0u;
  for (uint32_t i = 0u; i < ArrayCount(VKR_TEST_SUITES); ++i) {
    const VkrTestSuiteEntry *suite = &VKR_TEST_SUITES[i];
    if (filter && !strstr(suite->name, filter)) {
      continue;
    }
    const float64_t start = vkr_platform_get_absolute_time();
    all_passed &= suite->run();
    printf("[suite] %s %.3f s\n\n", suite->name,
           vkr_platform_get_absolute_time() - start);
    ++suites_run;
  }
  if (filter && suites_run == 0u) {
    printf("No test suite matches '%s'\n", filter);
    all_passed = false;
  }

  vkr_platform_shutdown();

  printf("\nAll tests completed.\n");
  return all_passed ? 0 : 1;
}
