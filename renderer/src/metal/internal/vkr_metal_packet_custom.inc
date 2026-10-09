// =============================================================================
// Custom material graphs on the tiled pipeline (ADR-096)
// =============================================================================
//
// A Custom material names the surface function its graph generated. The
// project library (`vkr_bakery materials`) holds every function with its
// tiled forward entries; the renderer gives each function a slot the first
// time a material names it and creates its pipelines on a background queue.
// A material's row carries its slot from then on; the frame's ready mask
// tells the camera classifier which slots shade with their own pipelines,
// and a draw of another slot takes its material's Standard fallback and
// counts as late.

/* Opens the project library, when the catalog has one. */
vkr_internal void vkr_metal_packet_custom_open(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer->custom_library_path[0])
    return;
  NSString *file =
      [NSString stringWithUTF8String:renderer->custom_library_path];
  if (![[NSFileManager defaultManager] fileExistsAtPath:file])
    return;
  NSError *error = nil;
  renderer->custom_library =
      [renderer->device newLibraryWithURL:[NSURL fileURLWithPath:file]
                                    error:&error];
  if (!renderer->custom_library)
    vkr_metal_packet_print_error("project_materials", error);
  else
    log_info("Metal: project materials loaded from %s",
             renderer->custom_library_path);
}

/* The project library at `path`; Custom materials draw their fallback
   without it, so only the queue group can fail. */
vkr_internal bool8_t
vkr_metal_packet_custom_load(VkrMetalPacketRenderer *renderer, const char *path)
    API_AVAILABLE(macos(26.0)) {
  renderer->custom_group = dispatch_group_create();
  if (!renderer->custom_group)
    return false_v;
  if (path && strlen(path) < sizeof(renderer->custom_library_path))
    snprintf(renderer->custom_library_path,
             sizeof(renderer->custom_library_path), "%s", path);
  vkr_metal_packet_custom_open(renderer);
  return true_v;
}

/* Creates slot `slot`'s forward and alpha-tested pipelines for every tiled
   variant on a background queue, then marks it ready or failed. */
vkr_internal void
vkr_metal_packet_custom_build(VkrMetalPacketRenderer *renderer, uint32_t slot)
    API_AVAILABLE(macos(26.0)) {
  VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
  dispatch_group_async(
      renderer->custom_group,
      dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
          const float64_t start = vkr_platform_get_absolute_time();
          bool8_t ok = renderer->custom_library != nil;
          NSString *label = [NSString stringWithUTF8String:graph->function];
          for (uint32_t decals = 0;
               ok && decals < VKR_METAL_TILED_DECAL_VARIANT_COUNT; ++decals) {
            for (uint32_t probes = 0;
                 ok && probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT; ++probes) {
              for (uint32_t i = 0; ok && i < VKR_METAL_TILED_LIGHTING_COUNT;
                   ++i) {
                char name[192];
                snprintf(name, sizeof(name), "%s_%s", graph->function,
                         tiled_lighting[i].forward[decals][probes]);
                graph->forward[decals][probes][i] =
                    vkr_metal_packet_create_pipeline_with_targets(
                        renderer, renderer->custom_library,
                        "vkr_metal_tiled_vertex", renderer->custom_library,
                        name, MTLPixelFormatRGBA16Float,
                        VKR_METAL_PACKET_BLEND_NONE,
                        VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
                        VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
                        VKR_METAL_TILED_SAMPLE_COUNT, label);
                snprintf(name, sizeof(name), "%s_%s", graph->function,
                         tiled_lighting[i].coverage[decals][probes]);
                graph->coverage[decals][probes][i] =
                    vkr_metal_packet_create_pipeline_with_targets(
                        renderer, renderer->custom_library,
                        "vkr_metal_tiled_vertex", renderer->custom_library,
                        name, MTLPixelFormatRGBA16Float,
                        VKR_METAL_PACKET_BLEND_COVERAGE,
                        VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
                        VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
                        VKR_METAL_TILED_SAMPLE_COUNT, label);
                ok = graph->forward[decals][probes][i] != nil &&
                     graph->coverage[decals][probes][i] != nil;
              }
            }
          }
          graph->create_seconds = vkr_platform_get_absolute_time() - start;
          vkr_atomic_uint32_store(&graph->state,
                                  ok ? VKR_METAL_CUSTOM_GRAPH_READY
                                     : VKR_METAL_CUSTOM_GRAPH_FAILED,
                                  VKR_MEMORY_ORDER_RELEASE);
          if (ok)
            log_info("Metal: Custom graph %s ready in %.1f ms (slot %u)",
                     graph->function, graph->create_seconds * 1.0e3, slot);
          else
            log_warn("Metal: Custom graph %s has no pipelines; its materials "
                     "draw their Standard fallback",
                     graph->function);
        }
      });
}

/* The slot of Custom function `function`, given and built the first time a
   material names it; 0 for a Standard material or past the budget. */
vkr_internal uint32_t vkr_metal_packet_custom_slot(
    VkrMetalPacketRenderer *renderer, const char *function)
    API_AVAILABLE(macos(26.0)) {
  if (!function || !function[0])
    return 0u;
  uint32_t free_slot = 0u;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
    if (!graph->function[0]) {
      free_slot = free_slot ? free_slot : slot;
    } else if (strcmp(graph->function, function) == 0) {
      return slot;
    }
  }
  if (!free_slot) {
    log_warn("Metal: Custom graph %s is past the %u-graph budget; its "
             "materials draw their Standard fallback",
             function, VKR_METAL_CUSTOM_GRAPH_SLOTS - 1u);
    return 0u;
  }
  VkrMetalCustomGraph *graph = &renderer->custom_graphs[free_slot];
  snprintf(graph->function, sizeof(graph->function), "%s", function);
  ++renderer->custom_graph_count;
  vkr_atomic_uint32_store(&graph->state, VKR_METAL_CUSTOM_GRAPH_PENDING,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_metal_packet_custom_build(renderer, free_slot);
  return free_slot;
}

/* The slots in `state` now, bit slot - 1. */
vkr_internal uint32_t vkr_metal_packet_custom_state_mask(
    VkrMetalPacketRenderer *renderer, VkrMetalCustomGraphState state) {
  uint32_t mask = 0u;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    if (vkr_atomic_uint32_load(&renderer->custom_graphs[slot].state,
                               VKR_MEMORY_ORDER_ACQUIRE) == (uint32_t)state)
      mask |= 1u << (slot - 1u);
  }
  return mask;
}

void vkr_metal_packet_renderer_pipeline_stats(
    const VkrMetalPacketRenderer *renderer, uint32_t *out_pending_graphs,
    uint64_t *out_late_draws) {
  *out_pending_graphs = 0u;
  *out_late_draws = 0u;
  if (!renderer)
    return;
  *out_late_draws = vkr_atomic_uint64_load(&renderer->custom_late_draws,
                                           VKR_MEMORY_ORDER_RELAXED);
  uint32_t pending = 0u;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    pending += vkr_atomic_uint32_load(&renderer->custom_graphs[slot].state,
                                      VKR_MEMORY_ORDER_ACQUIRE) ==
               VKR_METAL_CUSTOM_GRAPH_PENDING;
  }
  *out_pending_graphs = pending;
}

/* Releases a graph's pipelines; the caller proves the GPU is done with
   them and no creation runs. */
vkr_internal void
vkr_metal_packet_custom_release_pipelines(VkrMetalCustomGraph *graph) {
  for (uint32_t decals = 0; decals < VKR_METAL_TILED_DECAL_VARIANT_COUNT;
       ++decals) {
    for (uint32_t probes = 0; probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT;
         ++probes) {
      for (uint32_t i = 0; i < VKR_METAL_TILED_LIGHTING_COUNT; ++i) {
        [graph->forward[decals][probes][i] release];
        [graph->coverage[decals][probes][i] release];
        graph->forward[decals][probes][i] = nil;
        graph->coverage[decals][probes][i] = nil;
      }
    }
  }
}

/* Waits for the background creations, then releases every graph's
   pipelines and the project library. */
vkr_internal void
vkr_metal_packet_custom_release(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  if (renderer->custom_group) {
    dispatch_group_wait(renderer->custom_group, DISPATCH_TIME_FOREVER);
    dispatch_release(renderer->custom_group);
    renderer->custom_group = NULL;
  }
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    vkr_metal_packet_custom_release_pipelines(&renderer->custom_graphs[slot]);
    MemZero(&renderer->custom_graphs[slot],
            sizeof(renderer->custom_graphs[slot]));
  }
  renderer->custom_graph_count = 0u;
  [renderer->custom_library release];
  renderer->custom_library = nil;
}

void vkr_metal_packet_renderer_request_custom_reload(
    VkrMetalPacketRenderer *renderer) {
  if (renderer)
    vkr_atomic_bool_store(&renderer->custom_reload_requested, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
}

/* Loads the project library again. A function is named by the hash of its
   code, so a graph whose pipelines exist keeps them; graphs whose creation
   failed, such as one the old library lacked, are created again. Slots no
   published row names any more are freed, after the GPU finishes with
   their pipelines. Frame preparation calls it before it records anything,
   when a reload was requested; false asks the next frame to try again. */
vkr_internal bool8_t vkr_metal_packet_custom_reload(
    VkrMetalPacketRenderer *renderer) API_AVAILABLE(macos(26.0)) {
  if (!renderer->custom_group)
    return true_v;
  /* Creations read the library; never block the render thread on them. */
  if (dispatch_group_wait(renderer->custom_group, DISPATCH_TIME_NOW) != 0)
    return false_v;
  bool8_t unused = false_v;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    const VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
    unused |= graph->function[0] && graph->references == 0u;
  }
  /* Recorded frames may still draw with an unused slot's pipelines. */
  if (unused && !vkr_metal_packet_renderer_wait_idle(renderer))
    return false_v;
  uint32_t freed = 0u;
  for (uint32_t slot = 1u; unused && slot < VKR_METAL_CUSTOM_GRAPH_SLOTS;
       ++slot) {
    VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
    if (!graph->function[0] || graph->references != 0u)
      continue;
    vkr_metal_packet_custom_release_pipelines(graph);
    MemZero(graph, sizeof(*graph));
    renderer->custom_graph_count--;
    freed++;
  }
  [renderer->custom_library release];
  renderer->custom_library = nil;
  vkr_metal_packet_custom_open(renderer);
  uint32_t rebuilt = 0u;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
    if (!graph->function[0] ||
        vkr_atomic_uint32_load(&graph->state, VKR_MEMORY_ORDER_ACQUIRE) !=
            VKR_METAL_CUSTOM_GRAPH_FAILED)
      continue;
    vkr_metal_packet_custom_release_pipelines(graph);
    vkr_atomic_uint32_store(&graph->state, VKR_METAL_CUSTOM_GRAPH_PENDING,
                            VKR_MEMORY_ORDER_RELEASE);
    vkr_metal_packet_custom_build(renderer, slot);
    rebuilt++;
  }
  log_info("Metal: project materials reloaded; %u Custom graphs rebuild, "
           "%u unused freed",
           rebuilt, freed);
  return true_v;
}
