// =============================================================================
// Custom material graphs on the tiled pipeline (ADR-096)
// =============================================================================
//
// A Custom material names the surface function its graph generated. The
// project library (`vkr_bakery materials`) holds every function with its
// tiled forward entries; the renderer gives each function a slot the first
// time a material names it and creates its pipelines on the job system.
// A material's row carries its slot from then on; the frame's ready mask
// tells the camera classifier which slots shade with their own pipelines,
// and a draw of another slot takes its material's Standard fallback and
// counts as late.

/* Opens the project library, when the catalog has one. */
vkr_internal void vkr_metal_packet_custom_open(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer->custom_library_path[0])
    return;
  const FilePath library_file = file_path_view(renderer->custom_library_path);
  if (!file_exists(&library_file))
    return;
  NSString *file =
      [NSString stringWithUTF8String:renderer->custom_library_path];
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
   without it. */
vkr_internal bool8_t
vkr_metal_packet_custom_load(VkrMetalPacketRenderer *renderer, const char *path)
    API_AVAILABLE(macos(26.0)) {
  if (path && string_length(path) < sizeof(renderer->custom_library_path))
    string_format(renderer->custom_library_path,
                  sizeof(renderer->custom_library_path), "%s", path);
  vkr_metal_packet_custom_open(renderer);
  renderer->custom_loaded = true_v;
  return true_v;
}

/* Creates slot `slot`'s forward and alpha-tested pipelines of shading
   variant `variant` (ADR-113) for every tiled variant, then marks that
   variant ready or failed. Runs as a background build. */
vkr_internal void
vkr_metal_packet_custom_create(VkrMetalPacketRenderer *renderer, uint32_t slot,
                               uint32_t variant) API_AVAILABLE(macos(26.0)) {
  VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
  MTLFunctionConstantValues *constants =
      renderer->tiled_shading_constants[variant];
  const MTLPixelFormat sun_format = (variant & VKR_METAL_TILED_SUN_VARIANT)
                                        ? VKR_METAL_TILED_SUN_LIGHT_FORMAT
                                        : MTLPixelFormatInvalid;
  @autoreleasepool {
    const float64_t start = vkr_platform_get_absolute_time();
    bool8_t ok = renderer->custom_library != nil;
    NSString *label = [NSString stringWithUTF8String:graph->function];
    for (uint32_t decals = 0;
         ok && decals < VKR_METAL_TILED_DECAL_VARIANT_COUNT; ++decals) {
      for (uint32_t probes = 0;
           ok && probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT; ++probes) {
        for (uint32_t i = 0; ok && i < VKR_METAL_TILED_LIGHTING_COUNT; ++i) {
          char name[192];
          string_format(name, sizeof(name), "%s_%s", graph->function,
                        tiled_lighting[i].forward[decals][probes]);
          graph->forward[variant][decals][probes][i] =
              vkr_metal_packet_create_pipeline_with_targets(
                  renderer, renderer->custom_library, "vkr_metal_tiled_vertex",
                  renderer->custom_library, name, MTLPixelFormatRGBA16Float,
                  sun_format, VKR_METAL_PACKET_BLEND_NONE,
                  VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
                  VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
                  renderer->tiled_sample_count, constants, label);
          string_format(name, sizeof(name), "%s_%s", graph->function,
                        tiled_lighting[i].coverage[decals][probes]);
          graph->coverage[variant][decals][probes][i] =
              vkr_metal_packet_create_pipeline_with_targets(
                  renderer, renderer->custom_library, "vkr_metal_tiled_vertex",
                  renderer->custom_library, name, MTLPixelFormatRGBA16Float,
                  sun_format, VKR_METAL_PACKET_BLEND_COVERAGE,
                  VKR_METAL_PACKET_ABI_VERTEX_DRAW_ROOT,
                  VKR_METAL_PACKET_ABI_DRAW_ROOT, true_v,
                  renderer->tiled_sample_count, constants, label);
          ok = graph->forward[variant][decals][probes][i] != nil &&
               graph->coverage[variant][decals][probes][i] != nil;
        }
      }
    }
    graph->create_seconds = vkr_platform_get_absolute_time() - start;
    vkr_atomic_uint32_store(&graph->state[variant],
                            ok ? VKR_METAL_CUSTOM_GRAPH_READY
                               : VKR_METAL_CUSTOM_GRAPH_FAILED,
                            VKR_MEMORY_ORDER_RELEASE);
    if (ok)
      log_info("Metal: Custom graph %s%s%s ready in %.1f ms (slot %u)",
               graph->function, (variant & 1u) ? " BRDF extension variant" : "",
               (variant & VKR_METAL_TILED_SUN_VARIANT) ? " sun variant" : "",
               graph->create_seconds * 1.0e3, slot);
    else if (variant & VKR_METAL_TILED_SUN_VARIANT)
      log_warn("Metal: Custom graph %s has no sun contact pipelines; its "
               "materials draw their Standard fallback in those frames",
               graph->function);
    else if (variant)
      log_warn("Metal: Custom graph %s has no BRDF extension "
               "pipelines; its extended materials draw without them",
               graph->function);
    else
      log_warn("Metal: Custom graph %s has no pipelines; its materials "
               "draw their Standard fallback",
               graph->function);
  }
}

/* One background pipeline creation: Custom graph `slot` in shading variant
   `variant`, or when `slot` is 0 the tiled BRDF extension variant (variant
   1) or the tiled sun variants (VKR_METAL_TILED_SUN_VARIANT). */
typedef struct VkrMetalBackgroundBuild {
  VkrMetalPacketRenderer *renderer;
  uint32_t slot;
  uint32_t variant;
} VkrMetalBackgroundBuild;

vkr_internal void
vkr_metal_packet_background_run(const VkrMetalBackgroundBuild *build)
    API_AVAILABLE(macos(26.0)) {
  if (build->slot != 0u)
    vkr_metal_packet_custom_create(build->renderer, build->slot,
                                   build->variant);
  else if (build->variant & VKR_METAL_TILED_SUN_VARIANT)
    vkr_metal_packet_build_tiled_sun_pipelines(build->renderer);
  else
    vkr_metal_packet_build_tiled_brdf_pipelines(build->renderer);
  vkr_atomic_uint32_fetch_sub(&build->renderer->background_builds, 1u,
                              VKR_MEMORY_ORDER_RELEASE);
}

vkr_internal bool8_t vkr_metal_packet_background_job(VkrJobContext *context,
                                                     void *payload)
    API_AVAILABLE(macos(26.0)) {
  (void)context;
  vkr_metal_packet_background_run(payload);
  return true_v;
}

vkr_internal void vkr_metal_packet_background_build(
    VkrMetalPacketRenderer *renderer, uint32_t slot, uint32_t variant,
    VkrJobHandle *out_job) API_AVAILABLE(macos(26.0)) {
  const VkrMetalBackgroundBuild build = {
      .renderer = renderer, .slot = slot, .variant = variant};
  vkr_atomic_uint32_fetch_add(&renderer->background_builds, 1u,
                              VKR_MEMORY_ORDER_RELAXED);
  *out_job = (VkrJobHandle){0};
  if (renderer->job_system) {
    Bitset8 type_mask = bitset8_create();
    bitset8_set(&type_mask, VKR_JOB_TYPE_GENERAL);
    const VkrJobDesc desc = {
        .priority = VKR_JOB_PRIORITY_NORMAL,
        .type_mask = type_mask,
        .run = vkr_metal_packet_background_job,
        .payload = &build,
        .payload_size = sizeof(build),
    };
    if (vkr_job_try_submit(renderer->job_system, &desc, out_job))
      return;
  }
  /* No job system, or no free job slot: create the pipelines here. */
  vkr_metal_packet_background_run(&build);
}

/* Waits until every background creation has finished. */
vkr_internal void
vkr_metal_packet_background_wait(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  if (!renderer->job_system)
    return;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    for (uint32_t variant = 0u; variant < VKR_METAL_TILED_SHADING_VARIANT_COUNT;
         ++variant)
      (void)vkr_job_wait(renderer->job_system,
                         renderer->custom_graphs[slot].jobs[variant]);
  }
  (void)vkr_job_wait(renderer->job_system, renderer->tiled_brdf_job);
  (void)vkr_job_wait(renderer->job_system, renderer->tiled_sun_job);
}

/* Starts creating slot `slot`'s pipelines of shading variant `variant`. */
vkr_internal void
vkr_metal_packet_custom_build(VkrMetalPacketRenderer *renderer, uint32_t slot,
                              uint32_t variant) API_AVAILABLE(macos(26.0)) {
  vkr_metal_packet_background_build(
      renderer, slot, variant, &renderer->custom_graphs[slot].jobs[variant]);
}

/* Starts creating slot `slot`'s pipelines of shading variant `variant` once:
   the first request of a named slot whose variant is EMPTY. */
vkr_internal void
vkr_metal_packet_custom_request_variant(VkrMetalPacketRenderer *renderer,
                                        uint32_t slot, uint32_t variant)
    API_AVAILABLE(macos(26.0)) {
  if (slot == 0u || slot >= VKR_METAL_CUSTOM_GRAPH_SLOTS)
    return;
  VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
  if (!graph->function[0] || vkr_atomic_uint32_load(&graph->state[variant],
                                                    VKR_MEMORY_ORDER_ACQUIRE) !=
                                 VKR_METAL_CUSTOM_GRAPH_EMPTY)
    return;
  vkr_atomic_uint32_store(&graph->state[variant],
                          VKR_METAL_CUSTOM_GRAPH_PENDING,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_metal_packet_custom_build(renderer, slot, variant);
}

/* Starts creating slot `slot`'s sun variants of the BRDF variants it has
   asked for, once frames ask for sun contact shadows. */
vkr_internal void
vkr_metal_packet_custom_request_sun(VkrMetalPacketRenderer *renderer,
                                    uint32_t slot) API_AVAILABLE(macos(26.0)) {
  if (!renderer->tiled_sun_requested)
    return;
  for (uint32_t brdf = 0u; brdf < VKR_METAL_TILED_BRDF_VARIANT_COUNT; ++brdf) {
    if (vkr_atomic_uint32_load(&renderer->custom_graphs[slot].state[brdf],
                               VKR_MEMORY_ORDER_ACQUIRE) !=
        VKR_METAL_CUSTOM_GRAPH_EMPTY)
      vkr_metal_packet_custom_request_variant(
          renderer, slot, brdf | VKR_METAL_TILED_SUN_VARIANT);
  }
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
    } else if (string_equals(graph->function, function)) {
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
  string_format(graph->function, sizeof(graph->function), "%s", function);
  ++renderer->custom_graph_count;
  vkr_atomic_uint32_store(&graph->state[0], VKR_METAL_CUSTOM_GRAPH_PENDING,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_metal_packet_custom_build(renderer, free_slot, 0u);
  vkr_metal_packet_custom_request_sun(renderer, free_slot);
  return free_slot;
}

/* Starts creating slot `slot`'s BRDF extension pipelines (ADR-113) the
   first time a published material of the graph uses an extension, and
   their sun variant once frames ask for sun contact shadows. */
vkr_internal void
vkr_metal_packet_custom_request_brdf(VkrMetalPacketRenderer *renderer,
                                     uint32_t slot) API_AVAILABLE(macos(26.0)) {
  vkr_metal_packet_custom_request_variant(renderer, slot, 1u);
  if (slot != 0u && slot < VKR_METAL_CUSTOM_GRAPH_SLOTS)
    vkr_metal_packet_custom_request_sun(renderer, slot);
}

bool8_t
vkr_metal_packet_renderer_tiled_sun_light(VkrMetalPacketRenderer *renderer) {
  if (!renderer)
    return false_v;
  if (@available(macOS 26.0, *)) {
    if (!renderer->tiled_sun_requested && renderer->fragment_library) {
      renderer->tiled_sun_requested = true_v;
      vkr_atomic_uint32_store(&renderer->tiled_sun_state,
                              VKR_METAL_CUSTOM_GRAPH_PENDING,
                              VKR_MEMORY_ORDER_RELAXED);
      log_info("Metal: creating the tiled sun contact pipelines");
      vkr_metal_packet_background_build(
          renderer, 0u, VKR_METAL_TILED_SUN_VARIANT, &renderer->tiled_sun_job);
      for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot)
        vkr_metal_packet_custom_request_sun(renderer, slot);
    }
  }
  return vkr_atomic_uint32_load(&renderer->tiled_sun_state,
                                VKR_MEMORY_ORDER_ACQUIRE) ==
         VKR_METAL_CUSTOM_GRAPH_READY;
}

/* A published row now names Custom slot `slot` (0 for none): it holds a
   reference to the graph, and an extended material asks for the graph's
   extension pipelines (ADR-113). */
vkr_internal void
vkr_metal_packet_custom_reference(VkrMetalPacketRenderer *renderer,
                                  uint32_t slot, const VkrPbrProperties *pbr)
    API_AVAILABLE(macos(26.0)) {
  if (slot == 0u)
    return;
  renderer->custom_graphs[slot].references++;
  if (vkr_pbr_brdf_extensions_active(pbr))
    vkr_metal_packet_custom_request_brdf(renderer, slot);
}

/* The slots whose shading variant `variant` is in `state` now, bit
   slot - 1. */
vkr_internal uint32_t vkr_metal_packet_custom_state_mask(
    VkrMetalPacketRenderer *renderer, VkrMetalCustomGraphState state,
    uint32_t variant) {
  uint32_t mask = 0u;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    if (vkr_atomic_uint32_load(&renderer->custom_graphs[slot].state[variant],
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
    for (uint32_t variant = 0u; variant < VKR_METAL_TILED_SHADING_VARIANT_COUNT;
         ++variant) {
      pending +=
          vkr_atomic_uint32_load(&renderer->custom_graphs[slot].state[variant],
                                 VKR_MEMORY_ORDER_ACQUIRE) ==
          VKR_METAL_CUSTOM_GRAPH_PENDING;
    }
  }
  /* The tiled BRDF extension variant counts as one graph (ADR-113), and so
     do the tiled sun variants. */
  pending += vkr_atomic_uint32_load(&renderer->tiled_brdf_state,
                                    VKR_MEMORY_ORDER_ACQUIRE) ==
             VKR_METAL_CUSTOM_GRAPH_PENDING;
  pending += vkr_atomic_uint32_load(&renderer->tiled_sun_state,
                                    VKR_MEMORY_ORDER_ACQUIRE) ==
             VKR_METAL_CUSTOM_GRAPH_PENDING;
  *out_pending_graphs = pending;
}

/* Releases a graph's pipelines; the caller proves the GPU is done with
   them and no creation runs. */
vkr_internal void
vkr_metal_packet_custom_release_pipelines(VkrMetalCustomGraph *graph,
                                          uint32_t variant) {
  for (uint32_t decals = 0; decals < VKR_METAL_TILED_DECAL_VARIANT_COUNT;
       ++decals) {
    for (uint32_t probes = 0; probes < VKR_METAL_TILED_PROBE_VARIANT_COUNT;
         ++probes) {
      for (uint32_t i = 0; i < VKR_METAL_TILED_LIGHTING_COUNT; ++i) {
        [graph->forward[variant][decals][probes][i] release];
        [graph->coverage[variant][decals][probes][i] release];
        graph->forward[variant][decals][probes][i] = nil;
        graph->coverage[variant][decals][probes][i] = nil;
      }
    }
  }
}

/* Waits for the background creations, then releases every graph's
   pipelines and the project library. */
vkr_internal void
vkr_metal_packet_custom_release(VkrMetalPacketRenderer *renderer)
    API_AVAILABLE(macos(26.0)) {
  vkr_metal_packet_background_wait(renderer);
  renderer->custom_loaded = false_v;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    for (uint32_t variant = 0u; variant < VKR_METAL_TILED_SHADING_VARIANT_COUNT;
         ++variant)
      vkr_metal_packet_custom_release_pipelines(&renderer->custom_graphs[slot],
                                                variant);
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
  if (!renderer->custom_loaded)
    return true_v;
  /* Creations read the library; never block the render thread on them. */
  if (vkr_atomic_uint32_load(&renderer->background_builds,
                             VKR_MEMORY_ORDER_ACQUIRE) != 0u)
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
    for (uint32_t variant = 0u; variant < VKR_METAL_TILED_SHADING_VARIANT_COUNT;
         ++variant)
      vkr_metal_packet_custom_release_pipelines(graph, variant);
    MemZero(graph, sizeof(*graph));
    renderer->custom_graph_count--;
    freed++;
  }
  [renderer->custom_library release];
  renderer->custom_library = nil;
  vkr_metal_packet_custom_open(renderer);
  /* Each shading variant whose creation failed is created again. */
  uint32_t rebuilt = 0u;
  for (uint32_t slot = 1u; slot < VKR_METAL_CUSTOM_GRAPH_SLOTS; ++slot) {
    VkrMetalCustomGraph *graph = &renderer->custom_graphs[slot];
    for (uint32_t variant = 0u;
         graph->function[0] && variant < VKR_METAL_TILED_SHADING_VARIANT_COUNT;
         ++variant) {
      if (vkr_atomic_uint32_load(&graph->state[variant],
                                 VKR_MEMORY_ORDER_ACQUIRE) !=
          VKR_METAL_CUSTOM_GRAPH_FAILED)
        continue;
      vkr_metal_packet_custom_release_pipelines(graph, variant);
      vkr_atomic_uint32_store(&graph->state[variant],
                              VKR_METAL_CUSTOM_GRAPH_PENDING,
                              VKR_MEMORY_ORDER_RELEASE);
      vkr_metal_packet_custom_build(renderer, slot, variant);
      rebuilt++;
    }
  }
  log_info("Metal: project materials reloaded; %u Custom graphs rebuild, "
           "%u unused freed",
           rebuilt, freed);
  return true_v;
}

void vkr_metal_packet_renderer_detach_jobs(VkrMetalPacketRenderer *renderer) {
  if (!renderer)
    return;
  if (@available(macOS 26.0, *))
    vkr_metal_packet_background_wait(renderer);
  renderer->job_system = NULL;
}
