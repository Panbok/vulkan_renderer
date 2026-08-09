#include "core/logger.h"
#include "core/vkr_window.h"
#include "memory/vkr_arena_allocator.h"
#include "renderer/renderer_frontend.h"
#include "renderer/resources/loaders/material_loader.h"
#include "renderer/resources/loaders/texture_loader.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_renderer.h"

#include <stdio.h>
#include <string.h>

static const char *v3_report_kind_name(VkrBindlessVkReportKind kind) {
  static const char *names[] = {
      "API_VERSION",
      "INSTANCE_EXTENSION",
      "DEVICE_EXTENSION",
      "FEATURE",
      "LIMIT",
      "QUEUE",
      "FORMAT",
      "DEVICE_CREATE",
      "LAYOUT",
  };
  return (uint32_t)kind < ArrayCount(names) ? names[kind] : "UNKNOWN";
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  bool8_t validation = false_v;
  bool8_t gpu_assisted = false_v;
  bool8_t windowed = false_v;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--validation") == 0) {
      validation = true_v;
    } else if (strcmp(argv[i], "--gpu-assisted") == 0) {
      validation = true_v;
      gpu_assisted = true_v;
    } else if (strcmp(argv[i], "--windowed") == 0) {
      windowed = true_v;
    } else {
      fprintf(stderr,
              "usage: %s [--validation] [--gpu-assisted] [--windowed]\n",
              argv[0]);
      return 2;
    }
  }

  Arena *arena = arena_create(GB(2));
  if (!arena) {
    return 1;
  }
  log_init(arena);
  VkrAllocator allocator = {.ctx = arena};
  if (!vkr_allocator_arena(&allocator)) {
    arena_destroy(arena);
    return 1;
  }
  EventManager event_manager;
  MemZero(&event_manager, sizeof(event_manager));
  event_manager_create(&event_manager);
  VkrWindow window = {.hidden = true_v};
  const bool8_t window_created =
      !windowed ||
      vkr_window_create(&window, &event_manager, "VKR Bindless Vulkan V3", 100,
                        100, 320u, 240u);
  if (!window_created) {
    event_manager_destroy(&event_manager);
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
    return 1;
  }
  RendererFrontend *frontend = vkr_allocator_alloc(
      &allocator, sizeof(*frontend), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!frontend) {
    if (windowed)
      vkr_window_destroy(&window);
    event_manager_destroy(&event_manager);
    vkr_allocator_release_global_accounting(&allocator);
    arena_destroy(arena);
    return 1;
  }
  MemZero(frontend, sizeof(*frontend));
  VkrRendererBackendConfig config = {
      .present_target = {.kind = windowed ? VKR_PRESENT_TARGET_WINDOWED
                                          : VKR_PRESENT_TARGET_OFFSCREEN,
                         .width = windowed ? 320u : 4u,
                         .height = windowed ? 240u : 4u,
                         .image_count = 3u},
      .validation_enabled = validation,
      .gpu_assisted_validation = gpu_assisted,
  };
  VkrDeviceRequirements requirements = {0};
  VkrRendererError create_error = VKR_RENDERER_ERROR_NONE;
  const bool8_t created = vkr_renderer_initialize(
      frontend, VKR_RENDERER_BACKEND_TYPE_BINDLESS_VULKAN,
      windowed ? &window : NULL, &event_manager, &requirements, &config, 60u,
      &create_error);
  VkrBindlessVulkanRenderer *renderer = frontend->bindless_vulkan_renderer;
  const VkrBindlessVkCapabilityProfile *profile =
      vkr_bindless_vulkan_renderer_profile(renderer);
  if (profile) {
    for (uint32_t candidate_index = 0;
         candidate_index < profile->candidate_count; ++candidate_index) {
      const VkrBindlessVkCandidateReport *candidate =
          &profile->candidates[candidate_index];
      printf("DEVICE[%u] %s api=%u.%u.%u driver=%s offscreen=%s window=%s\n",
             candidate_index, candidate->device_name,
             VK_API_VERSION_MAJOR(candidate->api_version),
             VK_API_VERSION_MINOR(candidate->api_version),
             VK_API_VERSION_PATCH(candidate->api_version),
             candidate->driver_name,
             candidate->offscreen_viable ? "viable" : "rejected",
             candidate->window_viable ? "viable" : "rejected");
      for (uint32_t entry_index = 0; entry_index < candidate->entry_count;
           ++entry_index) {
        const VkrBindlessVkReportEntry *entry =
            &candidate->entries[entry_index];
        printf("  %-20s %-8s %-3s %-44s %s\n", v3_report_kind_name(entry->kind),
               entry->required ? "required" : "record",
               entry->present ? "yes" : "NO", entry->name, entry->detail);
      }
    }
  }
  if (created) {
    printf("V3 DEVICE PASS selected=%u validation=%s\n",
           profile->selected_candidate_index,
           validation ? "enabled" : "disabled");
  }
  bool8_t walking_pass = created;
  uint64_t last_polled = 0u;
  const uint32_t offscreen_extents[][2] = {{4u, 4u}, {7u, 5u}};
  const uint32_t window_extents[][2] = {{320u, 240u}, {400u, 300u}};
  const uint32_t(*extents)[2] = windowed ? window_extents : offscreen_extents;
  uint64_t frame_index = 0u;
  for (uint32_t extent_index = 0; walking_pass && extent_index < 2u;
       ++extent_index) {
    if (extent_index > 0 &&
        ((windowed && !vkr_window_resize(&window, extents[extent_index][0],
                                         extents[extent_index][1])) ||
         vkr_renderer_present_target_recreate(
             frontend, extents[extent_index][0], extents[extent_index][1],
             3u) != VKR_RENDERER_ERROR_NONE)) {
      walking_pass = false_v;
      break;
    }
    const uint32_t frames_at_extent = windowed && extent_index > 0 ? 8u : 1u;
    for (uint32_t frame = 0; walking_pass && frame < frames_at_extent;
         ++frame) {
      VkrFrameSetup setup = {0};
      VkrRenderPacket packet = {
          .packet_version = VKR_RENDER_PACKET_VERSION,
          .frame = {.frame_index = ++frame_index,
                    .window_width = extents[extent_index][0],
                    .window_height = extents[extent_index][1],
                    .viewport_width = extents[extent_index][0],
                    .viewport_height = extents[extent_index][1]},
      };
      VkrRendererFrameMetrics metrics = {0};
      VkrValidationError validation_error = {0};
      VkrBindlessVulkanResult completed = {0};
      walking_pass =
          vkr_renderer_prepare_frame(frontend, &setup) ==
              VKR_RENDERER_ERROR_NONE &&
          setup.window_width == extents[extent_index][0] &&
          setup.window_height == extents[extent_index][1] &&
          vkr_renderer_submit_packet(frontend, &packet, &metrics,
                                     &validation_error) ==
              VKR_RENDERER_ERROR_NONE &&
          metrics.world.draw_calls_issued == 1u &&
          vkr_renderer_wait_idle(frontend) == VKR_RENDERER_ERROR_NONE &&
          vkr_bindless_vulkan_renderer_poll_result(renderer, last_polled,
                                                   &completed) &&
          completed.submit_value == vkr_renderer_get_submit_serial(frontend) &&
          completed.color[0] == 37u && completed.color[1] == 91u &&
          completed.color[2] == 173u && completed.color[3] == 255u &&
          completed.identifier == 0xffad5b25u;
      if (walking_pass) {
        last_polled = completed.submit_value;
        printf("V3 WALK extent=%ux%u submit=%llu image=%u rgba=%u,%u,%u,%u "
               "identifier=0x%08x\n",
               setup.window_width, setup.window_height,
               (unsigned long long)completed.submit_value,
               completed.image_index, completed.color[0], completed.color[1],
               completed.color[2], completed.color[3], completed.identifier);
      }
    }
  }
  VkrBindlessVulkanWsiStats wsi_stats = {0};
  vkr_bindless_vulkan_renderer_wsi_stats(renderer, &wsi_stats);
  if (windowed) {
    walking_pass = walking_pass && wsi_stats.reacquire_proofs > 0u &&
                   wsi_stats.retired_swapchains == 1u &&
                   wsi_stats.retired_swapchains_collected == 1u &&
                   wsi_stats.retired_swapchains_live == 0u;
    printf("V3 WSI %s reacquire=%llu retired=%llu collected=%llu live=%u\n",
           walking_pass ? "PASS" : "FAIL",
           (unsigned long long)wsi_stats.reacquire_proofs,
           (unsigned long long)wsi_stats.retired_swapchains,
           (unsigned long long)wsi_stats.retired_swapchains_collected,
           wsi_stats.retired_swapchains_live);
  }
  if (walking_pass) {
    printf("V3 WALKING %s PASS\n", windowed ? "WINDOWED" : "OFFSCREEN");
    printf("V3 ABI REFLECTION %s\n",
           vkr_bindless_vulkan_renderer_shader_abi_validated(renderer)
               ? "PASS"
               : "FAIL");
  }
  VkrRendererError system_error = VKR_RENDERER_ERROR_NONE;
  const VkrGeometrySystemConfig geometry_config = {
      .max_geometries = 16u,
      .asset_publisher = &frontend->asset_publisher,
  };
  const VkrTextureSystemConfig texture_config = {
      .max_texture_count = 16u,
      .asset_publisher = &frontend->asset_publisher,
  };
  const VkrMaterialSystemConfig material_config = {
      .max_material_count = 16u,
      .asset_publisher = &frontend->asset_publisher,
  };
  const bool8_t shared_loaders =
      walking_pass &&
      vkr_resource_system_init(&frontend->allocator, frontend, NULL, NULL) &&
      vkr_geometry_system_init(&frontend->geometry_system, frontend,
                               &geometry_config, &system_error) &&
      vkr_texture_system_init(frontend, &texture_config, NULL,
                              &frontend->texture_system) &&
      vkr_material_system_init(&frontend->material_system, frontend->arena,
                               &frontend->texture_system, NULL,
                               &material_config);
  walking_pass = walking_pass && shared_loaders;
  if (shared_loaders) {
    vkr_resource_system_register_loader((void *)&frontend->texture_system,
                                        vkr_texture_loader_create());
    vkr_resource_system_register_loader((void *)&frontend->material_system,
                                        vkr_material_loader_create());
    printf("V4 SHARED LOADERS PASS textures=%u materials=%u\n",
           frontend->texture_system.next_free_index,
           frontend->material_system.next_free_index);
  }
  VkrBindlessVulkanMemoryMetrics memory_metrics = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_metrics);
  uint64_t memory_live = 0u;
  uint64_t memory_reserved = 0u;
  for (uint32_t block = 0; block < memory_metrics.block_count; ++block) {
    memory_live += memory_metrics.blocks[block].live_allocations;
    memory_reserved += memory_metrics.blocks[block].live_reserved_bytes;
  }
  if (walking_pass) {
    printf("V3 MEMORY blocks=%u live=%llu reserved=%llu\n",
           memory_metrics.block_count, (unsigned long long)memory_live,
           (unsigned long long)memory_reserved);
  }
  VkrBindlessVulkanPublicationTestResult publication = {0};
  if (walking_pass && !windowed) {
    walking_pass = vkr_bindless_vulkan_renderer_run_publication_test(
        renderer, &publication);
    if (walking_pass) {
      printf("V4 PUBLICATION PASS callbacks=prepared+writable+sampler+"
             "loaded-mesh exact-draws=%u shared=%u replacement=%u "
             "sampler-shared=%u material-republish=%u upload-waits=%u "
             "sampled-live=%llu storage-live=%llu sampler-live=%llu "
             "material-live=%llu published=%llu pending=%llu "
             "retirements=%llu collected=%llu\n",
             publication.exact_draw_count, publication.shared_resource_survived,
             publication.replacement_survived,
             publication.shared_sampler_reused,
             publication.dependent_materials_republished,
             publication.upload_wait_free ? 0u : 1u,
             (unsigned long long)publication.sampled_images.slots_live,
             (unsigned long long)publication.storage_images.slots_live,
             (unsigned long long)publication.samplers.slots_live,
             (unsigned long long)publication.materials.slots_live,
             (unsigned long long)publication.materials.slots_published,
             (unsigned long long)publication.materials.slots_retired,
             (unsigned long long)publication.materials.slots_retirements,
             (unsigned long long)publication.materials.slots_collected);
    }
  }
  uint64_t command_slot_waits = 0u;
  if (walking_pass) {
    walking_pass = vkr_renderer_get_and_reset_command_slot_wait_count(
        frontend, &command_slot_waits);
    if (walking_pass)
      printf("V3 WAITS command-slots=%llu\n",
             (unsigned long long)command_slot_waits);
  }
  VkrBindlessVulkanValidationStats validation_stats = {0};
  vkr_bindless_vulkan_renderer_validation_stats(renderer, &validation_stats);
  printf(
      "V3 VALIDATION setup-notices=%u warnings=%u errors=%u gpu-assisted=%s\n",
      validation_stats.setup_notice_count, validation_stats.warning_count,
      validation_stats.error_count,
      validation_stats.gpu_assisted_unavailable
          ? "unavailable"
          : (gpu_assisted ? "enabled" : "disabled"));
  walking_pass = walking_pass && validation_stats.warning_count == 0u &&
                 validation_stats.error_count == 0u;
  if (created) {
    vkr_renderer_destroy(frontend);
  }
  if (windowed)
    vkr_window_destroy(&window);
  event_manager_destroy(&event_manager);
  vkr_allocator_release_global_accounting(&allocator);
  arena_destroy(arena);
  if (gpu_assisted && validation_stats.gpu_assisted_unavailable) {
    printf("V3 RESULT GPU_ASSISTED_UNAVAILABLE\n");
    return 3;
  }
  return walking_pass ? 0 : 1;
}
