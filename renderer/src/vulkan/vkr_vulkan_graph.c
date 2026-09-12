#include "math/vkr_frustum.h"
#include "vulkan/vkr_vulkan_fsr_sdk.h"
#include "vulkan/vkr_vulkan_internal.h"
typedef enum VkrVulkanGraphExecutorKind {
  VKR_VULKAN_GRAPH_EXECUTOR_SHADOW = 0,
  VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW,
  VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0,
  VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1,
  VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING_DEPTH_SEED,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING_RESOLVE,
  VKR_VULKAN_GRAPH_EXECUTOR_PICKING_READBACK,
  VKR_VULKAN_GRAPH_EXECUTOR_IBL_BAKE,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_PREFIX,
  VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_ENCODE,
  VKR_VULKAN_GRAPH_EXECUTOR_TEMPORAL_TRANSFORM,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DEPTH_SEED,
  VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE,
  VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION,
  VKR_VULKAN_GRAPH_EXECUTOR_GBUFFER_RESOLVE,
  VKR_VULKAN_GRAPH_EXECUTOR_LIGHTING_DEFERRED,
  VKR_VULKAN_GRAPH_EXECUTOR_TEMPORAL_RESOLVE,
  VKR_VULKAN_GRAPH_EXECUTOR_FSR31_PREPARE,
  VKR_VULKAN_GRAPH_EXECUTOR_FSR31_UPSCALE,
  VKR_VULKAN_GRAPH_EXECUTOR_FSR31_STABILIZE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_SHADE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COMPACT,
  VKR_VULKAN_GRAPH_EXECUTOR_HZB_BUILD,
  VKR_VULKAN_GRAPH_EXECUTOR_SSR_DEPTH_BASE,
  VKR_VULKAN_GRAPH_EXECUTOR_SSR_DEPTH_MIP,
  VKR_VULKAN_GRAPH_EXECUTOR_SSR_TRACE,
  VKR_VULKAN_GRAPH_EXECUTOR_SSR_TEMPORAL,
  VKR_VULKAN_GRAPH_EXECUTOR_SSR_COMPOSITE,
  VKR_VULKAN_GRAPH_EXECUTOR_SSGI_DEPTH_BASE,
  VKR_VULKAN_GRAPH_EXECUTOR_SSGI_DEPTH_MIP,
  VKR_VULKAN_GRAPH_EXECUTOR_SSGI_TRACE,
  VKR_VULKAN_GRAPH_EXECUTOR_SSGI_TEMPORAL,
  VKR_VULKAN_GRAPH_EXECUTOR_SSGI_COMPOSITE,
  VKR_VULKAN_GRAPH_EXECUTOR_FOG_APPLY,
  VKR_VULKAN_GRAPH_EXECUTOR_FROXEL_INJECT,
  VKR_VULKAN_GRAPH_EXECUTOR_FROXEL_INTEGRATE,
  VKR_VULKAN_GRAPH_EXECUTOR_FROXEL_APPLY,
  VKR_VULKAN_GRAPH_EXECUTOR_SDSM_REDUCE,
  VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN,
  VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR,
  VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND,
  VKR_VULKAN_GRAPH_EXECUTOR_EXPOSURE_HISTOGRAM,
  VKR_VULKAN_GRAPH_EXECUTOR_EXPOSURE_RESOLVE,
  VKR_VULKAN_GRAPH_EXECUTOR_SUBSURFACE_GATHER,
  VKR_VULKAN_GRAPH_EXECUTOR_MOTION_BLUR_TILE_MAX,
  VKR_VULKAN_GRAPH_EXECUTOR_MOTION_BLUR_NEIGHBOR_MAX,
  VKR_VULKAN_GRAPH_EXECUTOR_MOTION_BLUR_RECONSTRUCT,
  VKR_VULKAN_GRAPH_EXECUTOR_DOF_COC,
  VKR_VULKAN_GRAPH_EXECUTOR_DOF_DILATE_HORIZONTAL,
  VKR_VULKAN_GRAPH_EXECUTOR_DOF_DILATE_VERTICAL,
  VKR_VULKAN_GRAPH_EXECUTOR_DOF_PREFILTER,
  VKR_VULKAN_GRAPH_EXECUTOR_DOF_GATHER,
  VKR_VULKAN_GRAPH_EXECUTOR_DOF_COMPOSITE,
  VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_PREFILTER,
  VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_DOWNSAMPLE,
  VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_UPSAMPLE,
  VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_COMBINE,
  VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DOWNSAMPLE,
  VKR_VULKAN_GRAPH_EXECUTOR_GTAO_DEPTH_PREFILTER,
  VKR_VULKAN_GRAPH_EXECUTOR_GTAO_DEPTH_MIP,
  VKR_VULKAN_GRAPH_EXECUTOR_GTAO_EVALUATE,
  VKR_VULKAN_GRAPH_EXECUTOR_GTAO_DENOISE,
  VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP,
  VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP_PREPARE,
  VKR_VULKAN_GRAPH_EXECUTOR_EDITOR,
  VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_CLEAR,
  VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY,
  VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY_PICKING,
  VKR_VULKAN_GRAPH_EXECUTOR_UI,
  VKR_VULKAN_GRAPH_EXECUTOR_METALFX_STAGE,
  VKR_VULKAN_GRAPH_EXECUTOR_METALFX_TEMPORAL,
  VKR_VULKAN_GRAPH_EXECUTOR_METALFX_STABILIZE,
  VKR_VULKAN_GRAPH_EXECUTOR_COUNT,
} VkrVulkanGraphExecutorKind;

enum { VKR_VULKAN_FULLSCREEN_ACES_FITTED = 1u << 4u };

struct VkrVulkanPreparedGraphPass {
  VkrVulkanGraphExecutorKind kind;
  VkDependencyInfo dependencies;
  VkRenderingInfo rendering;
  VkRenderingAttachmentInfo colors[8];
  VkRenderingAttachmentInfo depth;
  VkViewport viewport;
  VkRect2D scissor;
  VkrShadowConfigOverride depth_bias;
  VkrVulkanPreparedWorldDraws world;
  VkrVulkanPreparedOverlay overlay;
  VkrVulkanPreparedText text;
  VkrVulkanPreparedUi ui;
  VkrVulkanPreparedFullscreen fullscreen;
  VkrVulkanPreparedRaster raster;
  VkrVulkanPreparedCompute compute;
  VkrVulkanPreparedUpload upload;
  VkrVulkanPreparedIbl ibl;
  VkrVulkanFsrSdkDispatch fsr31;
  VkCopyImageInfo2 transfer;
  VkImageCopy2 transfer_region;
};

typedef struct VkrVulkanGraphExecutorSpec {
  const char *name;
  VkrRgPassType type;
} VkrVulkanGraphExecutorSpec;

vkr_global const VkrVulkanGraphExecutorSpec s_vk_graph_executors[] = {
    {"pass.shadow.cascade", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.local_shadow", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.local_shadow.transmission0", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.local_shadow.transmission1", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.local_shadow.transmission_overflow", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.picking", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.picking.depth_seed", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.picking.resolve", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.picking.readback", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ibl_bake", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.temporal.transform_history", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.gpu_draw_upload", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.transmission.gpu_draw_classify", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.gpu_draw_prefix", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.gpu_draw_encode", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.depth_seed", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.vbuffer.opaque", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.vbuffer.transmission", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.gbuffer.resolve", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.lighting.deferred", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.temporal.resolve", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.fsr31.prepare", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.fsr31.upscale", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.fsr31.stabilize", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.shade", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.coverage", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.compact", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.hzb.build", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssr.depth_base", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssr.depth_mip", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssr.trace", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssr.temporal", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssr.composite", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssgi.depth_base", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssgi.depth_mip", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssgi.trace", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssgi.temporal", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ssgi.composite", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.fog.apply", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.froxel.inject", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.froxel.integrate", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.froxel.apply", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.sdsm.reduce", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.copy.pre_transmission.fullscreen", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.copy.pre_transmission.editor", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.world.blend", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.exposure.histogram", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.exposure.resolve", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.subsurface.gather", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.motion_blur.tile_max", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.motion_blur.neighbor_max", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.motion_blur.reconstruct", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.dof.coc", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.dof.dilate_horizontal", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.dof.dilate_vertical", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.dof.prefilter", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.dof.gather", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.dof.composite", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.bloom.prefilter", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.bloom.downsample", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.bloom.upsample", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.bloom.combine", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.transmission.downsample", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gtao.depth_prefilter", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gtao.depth_mip", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gtao.evaluate", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.gtao.denoise", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.tonemap", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.tonemap.prepare", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.editor", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.editor.clear", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.editor.overlay", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.editor.overlay.picking", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.ui", VKR_RG_PASS_TYPE_GRAPHICS},
    // Bind the shared graph before its conditions are evaluated. These names
    // are recognized, but active MetalFX passes are rejected by validation.
    {"pass.metalfx.stage", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.metalfx.temporal", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.metalfx.stabilize", VKR_RG_PASS_TYPE_COMPUTE},
};
_Static_assert(ArrayCount(s_vk_graph_executors) ==
                   VKR_VULKAN_GRAPH_EXECUTOR_COUNT,
               "Vulkan graph executor table is incomplete");

vkr_internal bool8_t vkr_vk_graph_executor_kind(
    const VkrRgPass *pass, VkrVulkanGraphExecutorKind *out_kind) {
  const uint32_t encoded = pass->desc.executor_id;
  if (!encoded || encoded > VKR_VULKAN_GRAPH_EXECUTOR_COUNT)
    return false_v;
  *out_kind = (VkrVulkanGraphExecutorKind)(encoded - 1u);
  return true_v;
}

bool8_t vkr_vk_register_graph_executors(VkrVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < ArrayCount(s_vk_graph_executors); ++i) {
    const VkrVulkanGraphExecutorSpec *spec = &s_vk_graph_executors[i];
    const VkrRgPassExecutor executor = {
        .name = string8_create_from_cstr((const uint8_t *)spec->name,
                                         string_length(spec->name)),
        .id = i + 1u,
        .type = spec->type,
    };
    if (!vkr_rg_executor_registry_register(&renderer->executors, &executor))
      return false_v;
  }
  return true_v;
}

bool8_t vkr_vk_validate_graph(const VkrVulkanRenderer *renderer) {
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    VkrVulkanGraphExecutorKind kind;
    if (!vkr_vk_graph_executor_kind(pass, &kind)) {
      log_error("Vulkan graph pass '%.*s' has no executor kind",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    const VkrVulkanGraphExecutorSpec *executor = &s_vk_graph_executors[kind];
    if ((kind == VKR_VULKAN_GRAPH_EXECUTOR_FSR31_PREPARE ||
         kind == VKR_VULKAN_GRAPH_EXECUTOR_FSR31_UPSCALE ||
         kind == VKR_VULKAN_GRAPH_EXECUTOR_FSR31_STABILIZE) &&
        !renderer->config.fsr31_enabled) {
      log_error("FSR 3.1 graph work requires an FSR-enabled Vulkan device");
      return false_v;
    }
    if (kind == VKR_VULKAN_GRAPH_EXECUTOR_METALFX_STAGE ||
        kind == VKR_VULKAN_GRAPH_EXECUTOR_METALFX_TEMPORAL ||
        kind == VKR_VULKAN_GRAPH_EXECUTOR_METALFX_STABILIZE) {
      log_error(
          "Vulkan graph pass '%.*s' requires unsupported MetalFX executor '%s'",
          (int)pass->desc.name.length, pass->desc.name.str, executor->name);
      return false_v;
    }
    if (pass->desc.type != executor->type) {
      log_error("Vulkan graph pass '%.*s' has type %u; executor '%s' "
                "requires type %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (uint32_t)pass->desc.type, executor->name,
                (uint32_t)executor->type);
      return false_v;
    }
    for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
      const VkrRgImageBarrier *barrier =
          vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
      VkrVulkanDependency lowered = {0};
      const VkrVulkanDependencyResult result = vkr_vk_lower_image_dependency(
          barrier->src_access, barrier->dst_access, &barrier->dependency,
          barrier->src_layout != barrier->dst_layout, &lowered);
      if (result != VKR_VULKAN_DEPENDENCY_OK) {
        log_error("Vulkan graph pass '%.*s' image dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_vk_dependency_result_string(result));
        return false_v;
      }
    }
    for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
      const VkrRgBufferBarrier *barrier =
          vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
      VkrVulkanDependency lowered = {0};
      const VkrVulkanDependencyResult result = vkr_vk_lower_buffer_dependency(
          barrier->src_access, barrier->dst_access, &barrier->dependency,
          &lowered);
      if (result != VKR_VULKAN_DEPENDENCY_OK) {
        log_error("Vulkan graph pass '%.*s' buffer dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_vk_dependency_result_string(result));
        return false_v;
      }
    }
  }
  return true_v;
}

vkr_internal VkImageUsageFlags
vkr_vk_graph_image_usage(VkrTextureUsageFlags usage) {
  VkImageUsageFlags result = 0;
  if (usage.set & VKR_TEXTURE_USAGE_SAMPLED)
    result |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_STORAGE)
    result |= VK_IMAGE_USAGE_STORAGE_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_COLOR_ATTACHMENT)
    result |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)
    result |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_SRC)
    result |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_DST)
    result |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  return result;
}

vkr_internal bool8_t vkr_vk_graph_format_supports(VkrVulkanRenderer *renderer,
                                                  VkFormat format,
                                                  VkrTextureUsageFlags usage) {
  VkFormatFeatureFlags2 required = 0u;
  if (usage.set & VKR_TEXTURE_USAGE_SAMPLED)
    required |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_STORAGE)
    required |= VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_COLOR_ATTACHMENT)
    required |= VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT)
    required |= VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_SRC)
    required |= VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT;
  if (usage.set & VKR_TEXTURE_USAGE_TRANSFER_DST)
    required |= VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  VkFormatProperties3 properties3 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
  };
  VkFormatProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &properties3,
  };
  vkGetPhysicalDeviceFormatProperties2(
      vkr_vulkan_device_physical(renderer->device), format, &properties2);
  return required && (properties3.optimalTilingFeatures & required) == required;
}

vkr_internal bool8_t vkr_vk_graph_image_desc_equal(const VkrRgImageDesc *a,
                                                   const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->depth == b->depth && a->format == b->format &&
         a->usage.set == b->usage.set && a->samples == b->samples &&
         a->layers == b->layers && a->mip_levels == b->mip_levels &&
         a->type == b->type && a->flags == b->flags;
}

vkr_internal void
vkr_vk_destroy_graph_image_instance(VkrVulkanRenderer *renderer,
                                    VkrVulkanGraphImageInstance *instance) {
  const VkDevice device = vkr_vk_renderer_device(renderer);
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  if (instance->has_sampled_slot) {
    (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                    instance->sampled_slot, completed);
    (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                     NULL);
  }
  if (instance->has_storage_slot) {
    (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                    instance->storage_slot, completed);
    (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                     NULL);
  }
  for (uint32_t mip = 0u; mip < VKR_VULKAN_TEXTURE_MIP_MAX; ++mip) {
    if (instance->mip_views[mip])
      vkDestroyImageView(device, instance->mip_views[mip], NULL);
    if (instance->has_sampled_mip_slot[mip]) {
      (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                      instance->sampled_mip_slots[mip],
                                      completed);
    }
    if (instance->has_storage_mip_slot[mip]) {
      (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                      instance->storage_mip_slots[mip],
                                      completed);
    }
    for (uint32_t layer = 0; layer < VKR_VULKAN_GRAPH_LAYER_MAX; ++layer) {
      if (instance->mip_layer_views[mip][layer])
        vkDestroyImageView(device, instance->mip_layer_views[mip][layer], NULL);
    }
  }
  (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                   NULL);
  (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                   NULL);
  vkr_vk_destroy_image(renderer, &instance->image);
  MemZero(instance, sizeof(*instance));
}

void vkr_vk_destroy_graph_image(VkrVulkanRenderer *renderer,
                                VkrVulkanGraphImage *slot) {
  if (!slot || slot->external_swapchain) {
    if (slot)
      MemZero(slot, sizeof(*slot));
    return;
  }
  for (uint32_t i = 0; i < slot->instance_count; ++i)
    vkr_vk_destroy_graph_image_instance(renderer, &slot->instances[i]);
  MemZero(slot, sizeof(*slot));
}

vkr_internal bool8_t vkr_vk_create_graph_image_instance(
    VkrVulkanRenderer *renderer, const VkrRgImageDesc *desc,
    VkrVulkanGraphImageInstance *out_instance) {
  const VkFormat format = vkr_vk_texture_format(desc->format);
  VkrTextureUsageFlags checked_usage = desc->usage;
  /* A capture request arrives with a packet, after graph images have already

   * been realized. Capture-enabled renderers therefore provision graph-owned

   * images as legal transfer sources up front; no per-capture image churn is

   * allowed in the frame path. */
  if (renderer->config.capture_ring_capacity > 0u)
    checked_usage.set |= VKR_TEXTURE_USAGE_TRANSFER_SRC;
  const VkImageUsageFlags usage = vkr_vk_graph_image_usage(checked_usage);
  const bool8_t volume = desc->type == VKR_TEXTURE_TYPE_3D;
  if (format == VK_FORMAT_UNDEFINED || usage == 0u ||
      !vkr_vk_graph_format_supports(renderer, format, checked_usage) ||
      desc->samples != 1u || !desc->depth || desc->layers == 0u ||
      desc->layers > VKR_VULKAN_GRAPH_LAYER_MAX || desc->mip_levels == 0u ||
      desc->mip_levels > VKR_VULKAN_TEXTURE_MIP_MAX ||
      (volume && (desc->layers != 1u ||
                  (desc->flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY))) ||
      (!volume && desc->depth != 1u))
    return false_v;
  const bool8_t array_view =
      desc->layers > 1u || (desc->flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY);
  if (!vkr_vk_create_image_ex(renderer, desc->width, desc->height, desc->depth,
                              desc->mip_levels, desc->layers, format, 0u,
                              volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                              volume ? VK_IMAGE_VIEW_TYPE_3D
                                     : (array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                   : VK_IMAGE_VIEW_TYPE_2D),
                              usage, VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH,
                              &out_instance->image, NULL))
    return false_v;
  const VkImageAspectFlags aspects = vkr_vk_format_aspects(format);
  for (uint32_t mip = 0u; mip < desc->mip_levels; ++mip) {
    const VkImageViewCreateInfo mip_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out_instance->image.handle,
        .viewType = volume ? VK_IMAGE_VIEW_TYPE_3D
                           : (array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                         : VK_IMAGE_VIEW_TYPE_2D),
        .format = format,
        .subresourceRange =
            {
                .aspectMask = aspects,
                .baseMipLevel = mip,
                .levelCount = 1u,
                .baseArrayLayer = 0u,
                .layerCount = desc->layers,
            },
    };
    const VkResult mip_view_result =
        vkCreateImageView(vkr_vk_renderer_device(renderer), &mip_view_info,
                          NULL, &out_instance->mip_views[mip]);
    if (mip_view_result != VK_SUCCESS) {
      vkr_vk_record_graph_resource_result(renderer, mip_view_result);
      vkr_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    for (uint32_t layer = 0u; !volume && layer < desc->layers; ++layer) {
      const VkImageViewCreateInfo view_info = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image = out_instance->image.handle,
          .viewType = VK_IMAGE_VIEW_TYPE_2D,
          .format = format,
          .subresourceRange =
              {
                  .aspectMask = aspects,
                  .baseMipLevel = mip,
                  .levelCount = 1u,
                  .baseArrayLayer = layer,
                  .layerCount = 1u,
              },
      };
      const VkResult layer_view_result =
          vkCreateImageView(vkr_vk_renderer_device(renderer), &view_info, NULL,
                            &out_instance->mip_layer_views[mip][layer]);
      if (layer_view_result != VK_SUCCESS) {
        vkr_vk_record_graph_resource_result(renderer, layer_view_result);
        vkr_vk_destroy_graph_image_instance(renderer, out_instance);
        return false_v;
      }
    }
    VkImageView mip_view = out_instance->mip_views[mip];
    if ((desc->usage.set & VKR_TEXTURE_USAGE_SAMPLED) != 0u) {
      if (!vkr_vk_publish_sampled_view(renderer, mip_view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       &out_instance->sampled_mip_slots[mip])) {
        vkr_vk_destroy_graph_image_instance(renderer, out_instance);
        return false_v;
      }
      out_instance->has_sampled_mip_slot[mip] = true_v;
    }
    if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u) {
      if (!vkr_vk_publish_storage_view(renderer, mip_view,
                                       &out_instance->storage_mip_slots[mip])) {
        vkr_vk_destroy_graph_image_instance(renderer, out_instance);
        return false_v;
      }
      out_instance->has_storage_mip_slot[mip] = true_v;
    }
  }
  if ((desc->usage.set & VKR_TEXTURE_USAGE_SAMPLED) != 0u) {
    if (!vkr_vk_publish_sampled_view(renderer, out_instance->image.view,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &out_instance->sampled_slot)) {
      vkr_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_sampled_slot = true_v;
  }
  if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u) {
    if (!vkr_vk_publish_storage_view(renderer, out_instance->image.view,
                                     &out_instance->storage_slot)) {
      vkr_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_storage_slot = true_v;
  }
  return true_v;
}

vkr_internal uint32_t vkr_vk_graph_image_instance_count(
    const VkrVulkanRenderer *renderer, VkrRgResourceFlags flags) {
  if ((flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u)
    return VKR_VULKAN_HISTORY_INSTANCE_COUNT;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    return renderer->targets.image_count;
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    return VKR_VULKAN_FRAME_SLOT_COUNT;
  return 1u;
}

vkr_internal bool8_t vkr_vk_wait_graph_use(VkrVulkanRenderer *renderer,
                                           uint64_t last_use) {
  if (last_use <= renderer->completed_value)
    return true_v;
  if (last_use > renderer->submit_value) {
    log_error("Vulkan graph resource refers to unsubmitted use %llu",
              (unsigned long long)last_use);
    return false_v;
  }
  const VkSemaphoreWaitInfo wait = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1u,
      .pSemaphores = &renderer->timeline,
      .pValues = &last_use,
  };
  const VkResult result =
      vkWaitSemaphores(vkr_vk_renderer_device(renderer), &wait, UINT64_MAX);
  if (result != VK_SUCCESS) {
    log_error("Vulkan graph resource wait failed (submit=%llu, result=%d)",
              (unsigned long long)last_use, (int)result);
    return false_v;
  }
  return vkr_vk_refresh_completed(renderer) >= last_use;
}

bool8_t vkr_vk_realize_graph_images(VkrVulkanRenderer *renderer) {
  if (renderer->graph->images.length > renderer->config.max_graph_images)
    return false_v;
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    const bool8_t external_swapchain =
        image->imported && vkr_string8_equals_cstr(&image->name, "swapchain");
    const uint32_t instance_count =
        vkr_vk_graph_image_instance_count(renderer, image->desc.flags);
    if (!instance_count || instance_count > VKR_VULKAN_TARGET_IMAGE_MAX)
      return false_v;
    if (slot->live && slot->graph_generation == image->generation &&
        slot->external_swapchain == external_swapchain &&
        slot->instance_count == instance_count &&
        vkr_vk_graph_image_desc_equal(&slot->desc, &image->desc))
      continue;
    if (slot->live) {
      uint64_t last_use = 0u;
      for (uint32_t instance = 0u; instance < slot->instance_count;
           ++instance) {
        const VkrVulkanGraphImageInstance *old = &slot->instances[instance];
        last_use = Max(last_use, Max(old->last_use_submit_value,
                                     old->history_producer_submit_value));
      }
      // A resize replaces every instance, including other in-flight slots.
      if (!vkr_vk_wait_graph_use(renderer, last_use))
        return false_v;
      vkr_vk_destroy_graph_image(renderer, slot);
    }
    if (renderer->graph_revision == UINT64_MAX) {
      renderer->terminal_failure = true_v;
      log_fatal("Vulkan graph revision exhausted");
      return false_v;
    }
    ++renderer->graph_revision;
    /* Wholesale reassignment zeroes every instance's retained_states, so
       content_valid goes false. That is the ADR-029 invalidation rule for
       resize, format, layer, mip, and image-count changes: reaching this line
       at all means the descriptor changed, and stale contents must not be
       advertised against a differently shaped image. */
    *slot = (VkrVulkanGraphImage){
        .desc = image->desc,
        .graph_generation = image->generation,
        .instance_count = instance_count,
        .live = true_v,
        .external_swapchain = external_swapchain,
    };
    if (external_swapchain)
      continue;
    for (uint32_t instance = 0; instance < instance_count; ++instance) {
      if (!vkr_vk_create_graph_image_instance(renderer, &image->desc,
                                              &slot->instances[instance])) {
        log_error(
            "Vulkan failed to realize graph image '%.*s' "
            "(%ux%ux%u, type=%u, format=%u, usage=0x%x, samples=%u, layers=%u, "
            "mips=%u, instance=%u/%u)",
            (int)image->name.length, image->name.str, image->desc.width,
            image->desc.height, image->desc.depth, image->desc.type,
            image->desc.format, image->desc.usage.set, image->desc.samples,
            image->desc.layers, image->desc.mip_levels, instance,
            instance_count);
        vkr_vk_destroy_graph_image(renderer, slot);
        return false_v;
      }
    }
  }
  return true_v;
}

/**
 * Resolves one retained subresource slot, or NULL when the request cannot name
 * a live instance. Shared by the read and commit halves of the provider so both
 * agree on bounds and liveness.
 */
vkr_internal VkrRgRetainedState *
vkr_vk_retained_slot(VkrVulkanRenderer *renderer, uint32_t image_index,
                     uint32_t instance_index, uint32_t subresource) {
  if (image_index >= renderer->graph->images.length)
    return NULL;
  const VkrRgImage *image =
      vector_get_VkrRgImage(&renderer->graph->images, image_index);
  VkrVulkanGraphImage *slot = &renderer->graph_images[image_index];
  if (!image || !slot->live || slot->graph_generation != image->generation ||
      instance_index >= slot->instance_count)
    return NULL;
  if (subresource >=
      ArrayCount(slot->instances[instance_index].retained_states))
    return NULL;
  return &slot->instances[instance_index].retained_states[subresource];
}

vkr_internal void vkr_vk_retained_read(void *context, uint32_t image_index,
                                       uint32_t instance_index,
                                       uint32_t subresource,
                                       VkrRgRetainedState *out_state) {
  const VkrRgRetainedState *state = vkr_vk_retained_slot(
      (VkrVulkanRenderer *)context, image_index, instance_index, subresource);
  /* An unresolvable slot reports invalid rather than failing: the compiler
     treats invalid contents as "must be written this frame", which is the safe
     reading of "this backend cannot vouch for it". */
  if (state)
    *out_state = *state;
}

vkr_internal void vkr_vk_retained_commit(void *context, uint32_t image_index,
                                         uint32_t instance_index,
                                         uint32_t subresource,
                                         const VkrRgRetainedState *state) {
  VkrRgRetainedState *slot = vkr_vk_retained_slot(
      (VkrVulkanRenderer *)context, image_index, instance_index, subresource);
  if (slot)
    *slot = *state;
}

void vkr_vk_install_retained_provider(VkrVulkanRenderer *renderer) {
  const VkrRgRetainedStateProvider provider = {
      .context = renderer,
      .read = vkr_vk_retained_read,
      .commit = vkr_vk_retained_commit,
  };
  vkr_rg_set_retained_state_provider(renderer->graph, &provider);
}

void vkr_vulkan_renderer_retained_editor_extent(VkrVulkanRenderer *renderer,
                                                uint32_t *out_width,
                                                uint32_t *out_height) {
  *out_width = 0u;
  *out_height = 0u;
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!vkr_string8_equals_cstr(&image->name, "editor_scene_image"))
      continue;
    const VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    if (!slot->live || slot->graph_generation != image->generation ||
        slot->instance_count != 1u || slot->desc.mip_levels != 1u ||
        slot->desc.layers != 1u ||
        !slot->instances[0].retained_states[0].content_valid)
      return;
    *out_width = slot->desc.width;
    *out_height = slot->desc.height;
    return;
  }
}

void vkr_vulkan_renderer_retained_shadow_token(
    VkrVulkanRenderer *renderer, uint32_t image_index,
    VkrRetainedShadowToken *out_token) {
  *out_token = (VkrRetainedShadowToken){0};
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !vkr_string8_equals_cstr(&image->name, "shadow_map"))
      continue;
    VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    if (!slot->live || slot->graph_generation != image->generation ||
        slot->instance_count != renderer->targets.image_count ||
        image_index >= slot->instance_count || slot->desc.mip_levels != 1u ||
        slot->desc.samples != VKR_SAMPLE_COUNT_1 ||
        slot->desc.type != VKR_TEXTURE_TYPE_2D ||
        slot->desc.width != renderer->prepared_frame.shadow_map_size ||
        slot->desc.height != renderer->prepared_frame.shadow_map_size ||
        slot->desc.format != renderer->prepared_frame.shadow_depth_format ||
        slot->desc.layers != renderer->prepared_frame.shadow_map_layer_count)
      return;
    const VkrVulkanGraphImageInstance *instance = &slot->instances[image_index];
    out_token->resource_generation = slot->graph_generation;
    const uint32_t layer_count = Min(slot->desc.layers, 32u);
    for (uint32_t layer = 0u; layer < layer_count; ++layer) {
      if (instance->retained_states[layer].content_valid)
        out_token->valid_layer_mask |= UINT32_C(1) << layer;
    }
    return;
  }
}

vkr_global const char *const s_vk_local_shadow_transmission_names
    [VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT] = {
        "local_shadow_transmission_depth0",
        "local_shadow_transmission_color0",
        "local_shadow_transmission_depth1",
        "local_shadow_transmission_color1",
        "local_shadow_transmission_overflow",
};

vkr_internal VkrVulkanGraphImage *
vkr_vk_retained_local_shadow_image(VkrVulkanRenderer *renderer,
                                   uint32_t image_index, const char *name,
                                   uint32_t extent, VkrTextureFormat format) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !vkr_string8_equals_cstr(&image->name, name))
      continue;
    VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    if (!slot->live || slot->graph_generation == 0u ||
        slot->graph_generation != image->generation ||
        slot->instance_count != renderer->targets.image_count ||
        image_index >= slot->instance_count || slot->desc.mip_levels != 1u ||
        slot->desc.samples != VKR_SAMPLE_COUNT_1 ||
        slot->desc.type != VKR_TEXTURE_TYPE_2D || slot->desc.width != extent ||
        slot->desc.height != extent || slot->desc.format != format ||
        slot->desc.layers !=
            renderer->prepared_frame.local_shadow_map_layer_count ||
        !slot->instances[image_index].image.handle)
      return NULL;
    const VkrVulkanImage *physical = &slot->instances[image_index].image;
    if (physical->width != extent || physical->height != extent ||
        physical->depth != 1u || physical->mip_levels != 1u ||
        physical->array_layers != slot->desc.layers ||
        physical->format != vkr_vk_texture_format(format))
      return NULL;
    return slot;
  }
  return NULL;
}

vkr_internal uint32_t vkr_vk_retained_local_shadow_valid_mask(
    const VkrVulkanGraphImage *image, uint32_t image_index) {
  uint32_t mask = 0u;
  const VkrVulkanGraphImageInstance *instance = &image->instances[image_index];
  for (uint32_t layer = 0u; layer < image->desc.layers; ++layer) {
    if (instance->retained_states[layer].content_valid)
      mask |= UINT32_C(1) << layer;
  }
  return mask;
}

void vkr_vulkan_renderer_retained_local_shadow_token(
    VkrVulkanRenderer *renderer, uint32_t image_index,
    VkrRetainedLocalShadowToken *out_token) {
  *out_token = (VkrRetainedLocalShadowToken){0};
  VkrVulkanGraphImage *opaque = vkr_vk_retained_local_shadow_image(
      renderer, image_index, "local_shadow_map",
      renderer->prepared_frame.local_shadow_map_size,
      renderer->prepared_frame.shadow_depth_format);
  if (opaque) {
    out_token->resource_generation = opaque->graph_generation;
    out_token->valid_layer_mask =
        vkr_vk_retained_local_shadow_valid_mask(opaque, image_index);
  }
  const uint32_t extent = Min(renderer->prepared_frame.local_shadow_map_size,
                              VKR_LOCAL_SHADOW_TRANSMISSION_MAP_SIZE_MAX);
  uint64_t generations[VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT];
  uint32_t valid_mask = UINT32_MAX;
  for (uint32_t i = 0u; i < VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT; ++i) {
    VkrVulkanGraphImage *image = vkr_vk_retained_local_shadow_image(
        renderer, image_index, s_vk_local_shadow_transmission_names[i], extent,
        i == 1u || i == 3u ? VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT
                           : VKR_TEXTURE_FORMAT_D32_SFLOAT);
    if (!image)
      return;
    generations[i] = image->graph_generation;
    valid_mask &= vkr_vk_retained_local_shadow_valid_mask(image, image_index);
  }
  MemCopy(out_token->transmission_resource_generations, generations,
          sizeof(generations));
  out_token->transmission_valid_layer_mask = valid_mask;
}

vkr_internal bool8_t
vkr_vk_prepare_local_shadow_transmission_sampling(VkrVulkanRenderer *renderer) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->local_shadow_transmission = 0u;
  if (renderer->prepared_frame.local_shadow_transmission_view_count == 0u)
    return true_v;
  const uint32_t extent =
      renderer->prepared_frame.local_shadow_transmission_map_size;
  uint32_t indices[VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT];
  for (uint32_t i = 0u; i < VKR_LOCAL_SHADOW_TRANSMISSION_RESOURCE_COUNT; ++i) {
    VkrVulkanGraphImage *image = vkr_vk_retained_local_shadow_image(
        renderer, renderer->prepared_frame.image_index,
        s_vk_local_shadow_transmission_names[i], extent,
        i == 1u || i == 3u ? VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT
                           : VKR_TEXTURE_FORMAT_D32_SFLOAT);
    if (!image || !image->instances[renderer->prepared_frame.image_index]
                       .has_sampled_slot)
      return false_v;
    indices[i] = image->instances[renderer->prepared_frame.image_index]
                     .sampled_slot.index;
  }
  VkrVulkanLocalShadowTransmission *maps = vkr_vk_frame_upload_allocate(
      slot, sizeof(*maps), _Alignof(VkrVulkanLocalShadowTransmission),
      &slot->local_shadow_transmission, NULL);
  if (!maps)
    return false_v;
  *maps = (VkrVulkanLocalShadowTransmission){
      .depth0_texture = indices[0],
      .color0_texture = indices[1],
      .depth1_texture = indices[2],
      .color1_texture = indices[3],
      .overflow_texture = indices[4],
      .extent = extent,
  };
  return true_v;
}

VkrVulkanGraphImageInstance *vkr_vk_graph_image(VkrVulkanRenderer *renderer,
                                                VkrRgImageHandle handle,
                                                uint32_t image_index) {
  if (!vkr_rg_image_handle_valid(handle) ||
      handle.id > renderer->graph->images.length)
    return NULL;
  VkrVulkanGraphImage *slot = &renderer->graph_images[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation)
    return NULL;
  if (slot->external_swapchain) {
    if (image_index >= renderer->targets.image_count)
      return NULL;
    /* The target set owns this image. The graph slot is only a resolving view.
     */
    slot->instances[0].image = renderer->targets.images[image_index];
    return &slot->instances[0];
  }
  uint32_t instance = 0u;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(slot->desc.flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    instance = image_index;
  else if ((slot->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u)
    instance = renderer->history_output_index;
  else if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    instance = renderer->active_frame_slot;
  return instance < slot->instance_count ? &slot->instances[instance] : NULL;
}

void vkr_vk_mark_graph_images_submitted(VkrVulkanRenderer *renderer,
                                        uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrVulkanGraphImageInstance *instance =
        vkr_vk_graph_image(renderer,
                           (VkrRgImageHandle){.id = (uint32_t)i + 1u,
                                              .generation = image->generation},
                           renderer->prepared_frame.image_index);
    if (instance && !renderer->graph_images[i].external_swapchain)
      instance->last_use_submit_value = submit_value;
  }
}

vkr_internal VkBufferUsageFlags
vkr_vk_graph_buffer_usage(VkrBufferUsageFlags usage) {
  VkBufferUsageFlags result = 0u;
  if (usage.set & VKR_BUFFER_USAGE_VERTEX_BUFFER)
    result |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (usage.set & VKR_BUFFER_USAGE_INDEX_BUFFER)
    result |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (usage.set &
      (VKR_BUFFER_USAGE_GLOBAL_UNIFORM_BUFFER | VKR_BUFFER_USAGE_UNIFORM))
    result |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  if (usage.set & VKR_BUFFER_USAGE_STORAGE)
    result |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (usage.set & VKR_BUFFER_USAGE_TRANSFER_SRC)
    result |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (usage.set & VKR_BUFFER_USAGE_TRANSFER_DST)
    result |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (usage.set & VKR_BUFFER_USAGE_INDIRECT)
    result |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
  return result;
}

void vkr_vk_destroy_graph_buffer(VkrVulkanRenderer *renderer,
                                 VkrVulkanGraphBuffer *slot) {
  if (!slot)
    return;
  for (uint32_t i = 0u; i < slot->instance_count; ++i)
    vkr_vk_destroy_buffer(renderer, &slot->instances[i].buffer);
  MemZero(slot, sizeof(*slot));
}

vkr_internal uint32_t vkr_vk_graph_buffer_instance_count(
    const VkrVulkanRenderer *renderer, VkrRgResourceFlags flags) {
  if ((flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u)
    return VKR_VULKAN_HISTORY_INSTANCE_COUNT;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    return renderer->targets.image_count;
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    return VKR_VULKAN_FRAME_SLOT_COUNT;
  return 1u;
}

bool8_t vkr_vk_realize_graph_buffers(VkrVulkanRenderer *renderer) {
  if (renderer->graph->buffers.length > renderer->config.max_graph_buffers)
    return false_v;
  renderer->gpu_candidate_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->gpu_candidate_instance_buffer_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->transmission_gpu_candidate_instance_buffer_handle =
      VKR_RG_BUFFER_HANDLE_INVALID;
  renderer->temporal_transform_history_handle = VKR_RG_BUFFER_HANDLE_INVALID;
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame)
      continue;
    const VkrRgBufferHandle handle = {
        .id = (uint32_t)i + 1u,
        .generation = buffer->generation,
    };
    if (vkr_string8_equals_cstr(&buffer->name, "gpu_draw_candidates"))
      renderer->gpu_candidate_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name, "gpu_draw_instances"))
      renderer->gpu_candidate_instance_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name,
                                     "transmission_gpu_draw_instances"))
      renderer->transmission_gpu_candidate_instance_buffer_handle = handle;
    else if (vkr_string8_equals_cstr(&buffer->name,
                                     "temporal_transform_history"))
      renderer->temporal_transform_history_handle = handle;
    if (buffer->imported ||
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_EXTERNAL)) {
      log_error("Vulkan graph buffer '%.*s' has no imported native "
                "buffer binding",
                (int)buffer->name.length, buffer->name.str);
      return false_v;
    }
    const uint32_t instance_count =
        vkr_vk_graph_buffer_instance_count(renderer, buffer->desc.flags);
    if (!instance_count || instance_count > VKR_VULKAN_TARGET_IMAGE_MAX)
      return false_v;
    VkrVulkanGraphBuffer *slot = &renderer->graph_buffers[i];
    const bool8_t grow_only =
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_GROW_ONLY) != 0u;
    if (slot->live &&
        (grow_only || slot->graph_generation == buffer->generation) &&
        slot->instance_count == instance_count &&
        (grow_only ? slot->desc.size >= buffer->desc.size
                   : slot->desc.size == buffer->desc.size) &&
        slot->desc.usage.set == buffer->desc.usage.set &&
        slot->desc.flags == buffer->desc.flags) {
      slot->graph_generation = buffer->generation;
      continue;
    }
    if (slot->live) {
      uint64_t last_use = 0u;
      for (uint32_t instance = 0u; instance < slot->instance_count;
           ++instance) {
        const VkrVulkanGraphBufferInstance *old = &slot->instances[instance];
        last_use = Max(last_use, Max(old->last_use_submit_value,
                                     old->history_producer_submit_value));
      }
      if (!vkr_vk_wait_graph_use(renderer, last_use))
        return false_v;
      vkr_vk_destroy_graph_buffer(renderer, slot);
    }
    const VkBufferUsageFlags usage =
        vkr_vk_graph_buffer_usage(buffer->desc.usage);
    if (!buffer->desc.size || !usage)
      return false_v;
    if (renderer->graph_revision == UINT64_MAX) {
      renderer->terminal_failure = true_v;
      log_fatal("Vulkan graph revision exhausted");
      return false_v;
    }
    ++renderer->graph_revision;
    *slot = (VkrVulkanGraphBuffer){
        .desc = buffer->desc,
        .graph_generation = buffer->generation,
        .instance_count = instance_count,
        .live = true_v,
    };
    for (uint32_t instance = 0u; instance < instance_count; ++instance) {
      if (!vkr_vk_create_buffer(renderer, VKR_VULKAN_MEMORY_CLASS_DEVICE,
                                VKR_GPU_ALLOCATION_OWNER_RENDER_GRAPH,
                                buffer->desc.size, usage,
                                &slot->instances[instance].buffer)) {
        vkr_vk_destroy_graph_buffer(renderer, slot);
        return false_v;
      }
    }
  }
  return true_v;
}

bool8_t vkr_vk_select_history_output(VkrVulkanRenderer *renderer) {
  const uint64_t completed = vkr_vk_refresh_completed(renderer);
  uint64_t candidate_use[VKR_VULKAN_HISTORY_INSTANCE_COUNT] = {0};

  for (uint64_t i = 0u; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame ||
        (image->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) == 0u)
      continue;
    VkrVulkanGraphImage *slot = &renderer->graph_images[i];
    if (!slot->live ||
        slot->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT)
      return false_v;
    for (uint32_t candidate = 0u; candidate < VKR_VULKAN_HISTORY_INSTANCE_COUNT;
         ++candidate) {
      const VkrVulkanGraphImageInstance *instance = &slot->instances[candidate];
      const uint64_t use = Max(instance->last_use_submit_value,
                               instance->history_producer_submit_value);
      candidate_use[candidate] = Max(candidate_use[candidate], use);
    }
  }

  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame ||
        (buffer->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) == 0u)
      continue;
    VkrVulkanGraphBuffer *slot = &renderer->graph_buffers[i];
    if (!slot->live ||
        slot->instance_count != VKR_VULKAN_HISTORY_INSTANCE_COUNT)
      return false_v;
    for (uint32_t candidate = 0u; candidate < VKR_VULKAN_HISTORY_INSTANCE_COUNT;
         ++candidate) {
      const VkrVulkanGraphBufferInstance *instance =
          &slot->instances[candidate];
      const uint64_t use = Max(instance->last_use_submit_value,
                               instance->history_producer_submit_value);
      candidate_use[candidate] = Max(candidate_use[candidate], use);
    }
  }

  uint32_t selected = VKR_VULKAN_HISTORY_INSTANCE_COUNT;
  uint64_t selected_use = UINT64_MAX;
  for (uint32_t candidate = 0u; candidate < VKR_VULKAN_HISTORY_INSTANCE_COUNT;
       ++candidate) {
    if (candidate_use[candidate] < selected_use) {
      selected = candidate;
      selected_use = candidate_use[candidate];
    }
  }
  if (selected == VKR_VULKAN_HISTORY_INSTANCE_COUNT)
    return false_v;
  // History families can occupy the whole fixed ring. Wait once for its
  // oldest combined last use before publishing that index for CPU/GPU reuse.
  if (selected_use > completed &&
      !vkr_vk_wait_graph_use(renderer, selected_use)) {
    log_error("Vulkan history output wait failed (instance=%u, submit=%llu)",
              selected, (unsigned long long)selected_use);
    return false_v;
  }
  renderer->history_output_index = selected;
  return true_v;
}

VkrVulkanGraphBufferInstance *vkr_vk_graph_buffer(VkrVulkanRenderer *renderer,
                                                  VkrRgBufferHandle handle) {
  if (!vkr_rg_buffer_handle_valid(handle) ||
      handle.id > renderer->graph->buffers.length)
    return NULL;
  VkrVulkanGraphBuffer *slot = &renderer->graph_buffers[handle.id - 1u];
  if (!slot->live || slot->graph_generation != handle.generation)
    return NULL;
  uint32_t instance = 0u;
  const VkrRgResourceInstanceDomain domain =
      vkr_rg_resource_instance_domain(slot->desc.flags);
  if (domain == VKR_RG_RESOURCE_INSTANCE_PER_IMAGE)
    instance = renderer->prepared_frame.image_index;
  else if ((slot->desc.flags & VKR_RG_RESOURCE_FLAG_HISTORY) != 0u)
    instance = renderer->history_output_index;
  else if (domain == VKR_RG_RESOURCE_INSTANCE_PER_FRAME_SLOT)
    instance = renderer->active_frame_slot;
  return instance < slot->instance_count ? &slot->instances[instance] : NULL;
}

void vkr_vk_mark_graph_buffers_submitted(VkrVulkanRenderer *renderer,
                                         uint64_t submit_value) {
  for (uint64_t i = 0u; i < renderer->graph->buffers.length; ++i) {
    const VkrRgBuffer *buffer =
        vector_get_VkrRgBuffer(&renderer->graph->buffers, i);
    if (!buffer || !buffer->declared_this_frame)
      continue;
    VkrVulkanGraphBufferInstance *instance = vkr_vk_graph_buffer(
        renderer, (VkrRgBufferHandle){.id = (uint32_t)i + 1u,
                                      .generation = buffer->generation});
    if (instance)
      instance->last_use_submit_value = submit_value;
  }
}

vkr_internal bool8_t vkr_vk_prepare_graph_image_barriers(
    VkrVulkanRenderer *renderer, const Vector_VkrRgImageBarrier *barriers,
    VkDependencyInfo *out) {
  if (barriers->length > UINT32_MAX)
    return false_v;
  out->imageMemoryBarrierCount = (uint32_t)barriers->length;
  if (!barriers->length)
    return true_v;
  VkImageMemoryBarrier2 *native = vkr_allocator_alloc(
      &renderer->graph_frame_allocator, barriers->length * sizeof(*native),
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!native)
    return false_v;
  out->pImageMemoryBarriers = native;
  for (uint32_t i = 0u; i < out->imageMemoryBarrierCount; ++i) {
    const VkrRgImageBarrier *barrier =
        vector_get_VkrRgImageBarrier(barriers, i);
    VkrVulkanGraphImageInstance *instance = vkr_vk_graph_image(
        renderer, barrier->image, renderer->prepared_frame.image_index);
    if (!instance)
      return false_v;
    VkrVulkanDependency lowered = {0};
    const VkrVulkanDependencyResult result = vkr_vk_lower_image_dependency(
        barrier->src_access, barrier->dst_access, &barrier->dependency,
        barrier->src_layout != barrier->dst_layout, &lowered);
    if (result != VKR_VULKAN_DEPENDENCY_OK)
      return false_v;
    uint32_t base_mip = 0, mip_count = 0, base_layer = 0, layer_count = 0;
    vkr_image_subresource_range_resolve(&barrier->range,
                                        instance->image.mip_levels,
                                        instance->image.array_layers, &base_mip,
                                        &mip_count, &base_layer, &layer_count);
    const VkrVulkanGraphImage *graph_image =
        &renderer->graph_images[barrier->image.id - 1u];
    if (graph_image->desc.type == VKR_TEXTURE_TYPE_3D) {
      base_layer = 0u;
      layer_count = 1u;
    }
    native[i] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = lowered.src_stages,
        .srcAccessMask = lowered.src_access,
        .dstStageMask = lowered.dst_stages,
        .dstAccessMask = lowered.dst_access,
        .oldLayout = vkr_vk_texture_layout(barrier->src_layout),
        .newLayout = vkr_vk_texture_layout(barrier->dst_layout),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = instance->image.handle,
        .subresourceRange =
            {
                .aspectMask = vkr_vk_format_aspects(instance->image.format),
                .baseMipLevel = base_mip,
                .levelCount = mip_count,
                .baseArrayLayer = base_layer,
                .layerCount = layer_count,
            },
    };
  }
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_graph_pass_barriers(
    VkrVulkanRenderer *renderer, const VkrRgPass *pass, VkDependencyInfo *out) {
  if (pass->pre_buffer_barriers.length > renderer->config.max_graph_buffers) {
    log_error("Vulkan pass needs %llu buffer barriers; capacity=%u",
              (unsigned long long)pass->pre_buffer_barriers.length,
              renderer->config.max_graph_buffers);
    return false_v;
  }
  *out = (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  out->bufferMemoryBarrierCount = (uint32_t)pass->pre_buffer_barriers.length;
  VkBufferMemoryBarrier2 *native = NULL;
  if (out->bufferMemoryBarrierCount) {
    native =
        vkr_allocator_alloc(&renderer->graph_frame_allocator,
                            pass->pre_buffer_barriers.length * sizeof(*native),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!native)
      return false_v;
  }
  out->pBufferMemoryBarriers = native;
  for (uint64_t i = 0u; i < pass->pre_buffer_barriers.length; ++i) {
    const VkrRgBufferBarrier *barrier =
        vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
    VkrVulkanGraphBufferInstance *instance =
        vkr_vk_graph_buffer(renderer, barrier->buffer);
    VkrVulkanDependency lowered = {0};
    if (!instance ||
        vkr_vk_lower_buffer_dependency(barrier->src_access, barrier->dst_access,
                                       &barrier->dependency,
                                       &lowered) != VKR_VULKAN_DEPENDENCY_OK)
      return false_v;
    native[i] = (VkBufferMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = lowered.src_stages,
        .srcAccessMask = lowered.src_access,
        .dstStageMask = lowered.dst_stages,
        .dstAccessMask = lowered.dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = instance->buffer.handle,
        .offset = 0u,
        .size = VK_WHOLE_SIZE,
    };
  }
  return vkr_vk_prepare_graph_image_barriers(renderer,
                                             &pass->pre_image_barriers, out);
}

vkr_internal VkAttachmentLoadOp
vkr_vk_attachment_load_op(VkrAttachmentLoadOp op) {
  switch (op) {
  case VKR_ATTACHMENT_LOAD_OP_LOAD:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case VKR_ATTACHMENT_LOAD_OP_CLEAR:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  default:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
}

vkr_internal VkAttachmentStoreOp
vkr_vk_attachment_store_op(VkrAttachmentStoreOp op) {
  return op == VKR_ATTACHMENT_STORE_OP_STORE ? VK_ATTACHMENT_STORE_OP_STORE
                                             : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

vkr_internal bool8_t vkr_vk_graph_attachment(
    VkrVulkanRenderer *renderer, const VkrRgAttachment *attachment,
    VkImageLayout layout, VkRenderingAttachmentInfo *out_info,
    uint32_t *out_width, uint32_t *out_height, uint32_t *out_layers) {
  VkrVulkanGraphImageInstance *instance = vkr_vk_graph_image(
      renderer, attachment->image, renderer->prepared_frame.image_index);
  if (!instance ||
      attachment->desc.slice.mip_level >= instance->image.mip_levels ||
      attachment->desc.slice.base_layer >= instance->image.array_layers ||
      attachment->desc.slice.layer_count == 0u ||
      attachment->desc.slice.layer_count >
          instance->image.array_layers - attachment->desc.slice.base_layer)
    return false_v;
  const VkrVulkanGraphImage *graph_image =
      &renderer->graph_images[attachment->image.id - 1u];
  /* Dynamic rendering never accepts a 3D image view. The graph validator
     rejects these attachments too; retain the native boundary so an invalid
     graph cannot reach the 2D layer-view table. */
  if (graph_image->desc.type == VKR_TEXTURE_TYPE_3D)
    return false_v;
  VkImageView view =
      graph_image->external_swapchain ? instance->image.view
      : attachment->desc.slice.layer_count == 1u
          ? instance->mip_layer_views[attachment->desc.slice.mip_level]
                                     [attachment->desc.slice.base_layer]
          : instance->mip_views[attachment->desc.slice.mip_level];
  if (!view)
    return false_v;
  *out_info = (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = layout,
      .loadOp = vkr_vk_attachment_load_op(attachment->desc.load_op),
      .storeOp = vkr_vk_attachment_store_op(attachment->desc.store_op),
  };
  MemCopy(&out_info->clearValue, &attachment->desc.clear_value,
          sizeof(out_info->clearValue));
  *out_width =
      Max(1u, instance->image.width >> attachment->desc.slice.mip_level);
  *out_height =
      Max(1u, instance->image.height >> attachment->desc.slice.mip_level);
  *out_layers = attachment->desc.slice.layer_count;
  return true_v;
}

vkr_internal bool8_t vkr_vk_graph_sampled_index(VkrVulkanRenderer *renderer,
                                                const VkrRgPass *pass,
                                                uint32_t binding,
                                                uint32_t *out_index) {
  if (!out_index)
    return false_v;
  const VkrRgImageUse *read =
      vkr_rg_pass_find_image_use(&pass->desc, binding, 0u);
  if (!read)
    return false_v;
  VkrVulkanGraphImageInstance *image = vkr_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  if (!image)
    return false_v;
  if (read->has_slice &&
      (read->slice.mip_count == 0u || read->slice.mip_count == 1u)) {
    const uint32_t mip = read->slice.mip_level;
    if (mip >= image->image.mip_levels || !image->has_sampled_mip_slot[mip])
      return false_v;
    *out_index = image->sampled_mip_slots[mip].index;
  } else {
    if (!image->has_sampled_slot)
      return false_v;
    *out_index = image->sampled_slot.index;
  }
  return true_v;
}

/**
 * @brief Resolves the final HDR input shared by fullscreen and editor draws.
 *
 * Bloom at binding 2 takes precedence over DoF at binding 3, motion blur at
 * binding 4, then the original reconstructed source at binding 0.
 */
vkr_internal bool8_t vkr_vk_graph_fullscreen_source(VkrVulkanRenderer *renderer,
                                                    const VkrRgPass *pass,
                                                    uint32_t *out_index) {
  const uint32_t binding =
      vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u)   ? 5u
      : vkr_rg_pass_find_image_use(&pass->desc, 2u, 0u) ? 2u
      : vkr_rg_pass_find_image_use(&pass->desc, 3u, 0u) ? 3u
      : vkr_rg_pass_find_image_use(&pass->desc, 4u, 0u) ? 4u
                                                        : 0u;
  return vkr_vk_graph_sampled_index(renderer, pass, binding, out_index);
}

vkr_internal bool8_t vkr_vk_prepare_graphics_body(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedGraphPass *prepared,
    const VkrRgPass *pass, VkrVulkanGraphExecutorKind kind,
    uint32_t target_width, uint32_t target_height) {
  const VkrPreparedFrame *packet = renderer->graph->packet;
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!packet)
    return false_v;
  switch (kind) {
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW:
    return vkr_vk_prepare_local_shadow_transmission(
        renderer, &prepared->raster, pass,
        kind == VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW);
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_SHADOW: {
    if (kind == VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW) {
      const uint32_t layer = pass->desc.depth_attachment.desc.slice.base_layer;
      if (!packet->input.local_shadow ||
          layer >= packet->input.local_shadow->view_count)
        return false_v;
      prepared->depth_bias = (VkrShadowConfigOverride){0};
      return vkr_vk_prepare_deferred_raster(renderer, &prepared->raster, pass,
                                            true_v, false_v, true_v);
    }
    if (!packet->input.shadow)
      return true_v;
    const uint32_t cascade = pass->desc.depth_attachment.desc.slice.base_layer;
    if (cascade >= packet->input.shadow->cascade_count)
      return false_v;
    /* No hardcoded fallback: the configured value is the contract, and a
       backend-local default would silently disagree with the other selected
       implementation. A packet without an override means no raster bias. */
    const VkrShadowConfigOverride bias =
        packet->input.shadow->config_override
            ? *packet->input.shadow->config_override
            : (VkrShadowConfigOverride){0};
    prepared->depth_bias = bias;
    return vkr_vk_prepare_deferred_raster(renderer, &prepared->raster, pass,
                                          true_v, false_v, false_v);
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING: {
    if (!packet->input.picking || !packet->input.picking->pending)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->input.globals.projection, packet->input.globals.view);
    return vkr_vk_prepare_packet_draws(
               renderer, &prepared->world, VKR_VULKAN_PACKET_PIPELINE_PICKING,
               slot->world_instances, view_projection, target_width,
               target_height, 0u, 0u, 0u) &&
           (!packet->input.world ||
            vkr_vk_prepare_text_draws(renderer, &prepared->text,
                                      VKR_VULKAN_PACKET_PIPELINE_PICKING_TEXT,
                                      packet->input.world->text_draws,
                                      packet->input.world->text_draw_count,
                                      view_projection, target_width,
                                      target_height, false_v));
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
    return vkr_vk_prepare_deferred_raster(renderer, &prepared->raster, pass,
                                          false_v, false_v, false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
    return vkr_vk_prepare_deferred_raster(renderer, &prepared->raster, pass,
                                          false_v, true_v, false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND: {
    if (!packet->input.world)
      return true_v;
    const Mat4 view_projection = mat4_mul(packet->temporal.jittered_projection,
                                          packet->input.globals.view);
    uint32_t shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
    if (renderer->prepared_frame.shadow_cascade_count > 0u &&
        !vkr_vk_graph_sampled_index(renderer, pass, 0u, &shadow_texture))
      return false_v;
    uint32_t local_shadow_texture = VKR_VULKAN_SENTINEL_SLOT_INDEX;
    if (renderer->prepared_frame.local_shadow_view_count > 0u &&
        !vkr_vk_graph_sampled_index(renderer, pass, 1u, &local_shadow_texture))
      return false_v;
    return vkr_vk_prepare_packet_draws(
               renderer, &prepared->world,
               VKR_VULKAN_PACKET_PIPELINE_WORLD_BLEND, slot->world_instances,
               view_projection, target_width, target_height, shadow_texture, 0u,
               local_shadow_texture) &&
           vkr_vk_prepare_text_draws(
               renderer, &prepared->text, VKR_VULKAN_PACKET_PIPELINE_WORLD_TEXT,
               packet->input.world->text_draws,
               packet->input.world->text_draw_count, view_projection,
               target_width, target_height, false_v);
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY_PICKING:
    return vkr_vk_prepare_editor_overlay(
        renderer, &prepared->overlay,
        kind == VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY_PICKING);
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR: {
    /* The retained sRGB texture already contains exposure, tonemap and FXAA.
       Decode/sample/re-encode it without applying those operations again. */
    uint32_t texture_index = 0u;
    if (!vkr_vk_graph_fullscreen_source(renderer, pass, &texture_index))
      return false_v;
    const Vec4 image_rect = packet->editor_image_rect_px;
    const VkViewport editor_viewport = {
        .x = image_rect.x,
        .y = image_rect.y,
        .width = image_rect.z,
        .height = image_rect.w,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    const VkRect2D editor_scissor = {
        .offset = {(int32_t)image_rect.x, (int32_t)image_rect.y},
        .extent = {(uint32_t)image_rect.z, (uint32_t)image_rect.w},
    };
    prepared->viewport = editor_viewport;
    prepared->scissor = editor_scissor;
    if (!vkr_vk_prepare_packet_fullscreen(
            renderer, &prepared->fullscreen,
            VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL, texture_index, 0u,
            VKR_VULKAN_FULLSCREEN_ALREADY_OUTPUT_ENCODED |
                (packet->input.editor->scene_backdrop_blur
                     ? VKR_VULKAN_FULLSCREEN_SCENE_BLUR : 0u), true_v,
            (uint32_t)image_rect.z, (uint32_t)image_rect.w))
      return false_v;
    return true_v;
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP_PREPARE:
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP: {
    const bool8_t prepare_display_linear =
        kind == VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP_PREPARE;
    const bool8_t source_display_linear =
        vkr_rg_pass_find_image_use(&pass->desc, 5u, 0u) != NULL;
    uint32_t texture_index = 0u;
    const VkrRgBufferUse *exposure_use =
        vkr_rg_pass_find_buffer_use(&pass->desc, 1u, 0u);
    VkrVulkanGraphBufferInstance *exposure_state =
        exposure_use ? vkr_vk_graph_buffer(renderer, exposure_use->buffer)
                     : NULL;
    if (!vkr_vk_graph_fullscreen_source(renderer, pass, &texture_index)) {
      log_error("Vulkan tonemap input has no sampled descriptor");
      return false_v;
    }
    const bool8_t recorded = vkr_vk_prepare_packet_fullscreen(
        renderer, &prepared->fullscreen,
        prepare_display_linear
            ? VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_DISPLAY_LINEAR
            : VKR_VULKAN_PACKET_PIPELINE_FULLSCREEN_FINAL,
        texture_index, exposure_state ? exposure_state->buffer.address : 0u,
        (prepare_display_linear ? VKR_VULKAN_FULLSCREEN_PREPARE_DISPLAY_LINEAR
                                : 0u) |
            (source_display_linear ? VKR_VULKAN_FULLSCREEN_SOURCE_DISPLAY_LINEAR
                                   : 0u) |
            (renderer->config.tonemap_enabled ? VKR_VULKAN_FULLSCREEN_TONEMAP
                                              : 0u) |
            (packet->input.globals.display_transform ==
                     VKR_DISPLAY_TRANSFORM_ACES_FITTED
                 ? VKR_VULKAN_FULLSCREEN_ACES_FITTED
                 : 0u) |
            (renderer->prepared_frame.fsr31_enabled
                 ? VKR_VULKAN_FULLSCREEN_OPAQUE_ALPHA
                 : 0u),
        false_v, target_width, target_height);
    if (!recorded)
      log_error("Vulkan tonemap root allocation failed at %llu/%llu bytes",
                (unsigned long long)slot->frame_upload_cursor,
                (unsigned long long)slot->frame_upload.size);
    return recorded;
  }
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_CLEAR:
    return true_v;
  case VKR_VULKAN_GRAPH_EXECUTOR_UI: {
    if (!packet->input.ui)
      return true_v;
    return vkr_vk_prepare_ui_draw_list(renderer, &prepared->ui,
                                       &packet->input.ui->draw_list,
                                       target_width, target_height);
  }
  default:
    return false_v;
  }
}

vkr_internal bool8_t vkr_vk_prepare_graph_graphics_pass(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedGraphPass *prepared,
    const VkrRgPass *pass, VkrVulkanGraphExecutorKind kind) {
  enum { VKR_VULKAN_GRAPH_COLOR_ATTACHMENT_MAX = 8 };
  if (pass->desc.color_attachments.length >
      VKR_VULKAN_GRAPH_COLOR_ATTACHMENT_MAX)
    return false_v;
  VkRenderingAttachmentInfo *colors = prepared->colors;
  VkRenderingAttachmentInfo *depth = &prepared->depth;
  uint32_t width = 0, height = 0, layers = 0;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    uint32_t attachment_width = 0, attachment_height = 0, attachment_layers = 0;
    const bool8_t attachment_ready = vkr_vk_graph_attachment(
        renderer, vector_get_VkrRgAttachment(&pass->desc.color_attachments, i),
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &colors[i], &attachment_width,
        &attachment_height, &attachment_layers);
    if (!attachment_ready || (layers != 0u && layers != attachment_layers)) {
      log_error("Vulkan graphics pass '%.*s' color attachment %llu "
                "is unavailable (image=%u, layers=%u/%u)",
                (int)pass->desc.name.length, pass->desc.name.str,
                (unsigned long long)i, renderer->prepared_frame.image_index,
                layers, attachment_layers);
      return false_v;
    }
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
    layers = attachment_layers;
  }
  if (pass->desc.has_depth_attachment) {
    uint32_t attachment_width = 0, attachment_height = 0, attachment_layers = 0;
    const bool8_t attachment_ready = vkr_vk_graph_attachment(
        renderer, &pass->desc.depth_attachment,
        pass->desc.depth_attachment.read_only
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        depth, &attachment_width, &attachment_height, &attachment_layers);
    if (!attachment_ready || (layers != 0u && layers != attachment_layers)) {
      log_error("Vulkan graphics pass '%.*s' depth attachment is "
                "unavailable (image=%u, layers=%u/%u)",
                (int)pass->desc.name.length, pass->desc.name.str,
                renderer->prepared_frame.image_index, layers,
                attachment_layers);
      return false_v;
    }
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
    layers = attachment_layers;
  }
  if (!width || !height || !layers) {
    log_error("Vulkan graphics pass '%.*s' has an empty render area",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  prepared->rendering = (VkRenderingInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.extent = {.width = width, .height = height}},
      .layerCount = layers,
      .colorAttachmentCount = (uint32_t)pass->desc.color_attachments.length,
      .pColorAttachments = colors,
      .pDepthAttachment = pass->desc.has_depth_attachment ? depth : NULL,
      .pStencilAttachment =
          pass->desc.has_depth_attachment &&
                  (vkr_vk_format_aspects(
                       vkr_vk_graph_image(renderer,
                                          pass->desc.depth_attachment.image,
                                          renderer->prepared_frame.image_index)
                           ->image.format) &
                   VK_IMAGE_ASPECT_STENCIL_BIT)
              ? depth
              : NULL,
  };
  prepared->viewport = (VkViewport){
      .width = (float32_t)width,
      .height = (float32_t)height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  prepared->scissor = (VkRect2D){.extent = {.width = width, .height = height}};
  return vkr_vk_prepare_graphics_body(renderer, prepared, pass, kind, width,
                                      height);
}

vkr_internal bool8_t vkr_vk_prepare_graph_transfer_pass(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedGraphPass *prepared,
    const VkrRgPass *pass) {
  const VkrRgImageUse *read = vkr_rg_pass_find_image_use(&pass->desc, 0u, 0u);
  const VkrRgImageUse *write = vkr_rg_pass_find_image_use(&pass->desc, 1u, 0u);
  if (!read && !write)
    return true_v;
  if (!read || !write)
    return false_v;
  VkrVulkanGraphImageInstance *source = vkr_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  VkrVulkanGraphImageInstance *destination = vkr_vk_graph_image(
      renderer, write->image, renderer->prepared_frame.image_index);
  if (!source || !destination ||
      source->image.format != destination->image.format ||
      source->image.depth != 1u || destination->image.depth != 1u)
    return false_v;
  prepared->transfer_region = (VkImageCopy2){
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource =
          {.aspectMask = vkr_vk_format_aspects(source->image.format),
           .mipLevel = read->has_slice ? read->slice.mip_level : 0u,
           .baseArrayLayer = read->has_slice ? read->slice.base_layer : 0u,
           .layerCount = read->has_slice ? read->slice.layer_count : 1u},
      .dstSubresource =
          {.aspectMask = vkr_vk_format_aspects(destination->image.format),
           .mipLevel = write->has_slice ? write->slice.mip_level : 0u,
           .baseArrayLayer = write->has_slice ? write->slice.base_layer : 0u,
           .layerCount = write->has_slice ? write->slice.layer_count : 1u},
      .extent =
          {.width = Min(
               Max(1u, source->image.width >>
                           (read->has_slice ? read->slice.mip_level : 0u)),
               Max(1u, destination->image.width >>
                           (write->has_slice ? write->slice.mip_level : 0u))),
           .height = Min(
               Max(1u, source->image.height >>
                           (read->has_slice ? read->slice.mip_level : 0u)),
               Max(1u, destination->image.height >>
                           (write->has_slice ? write->slice.mip_level : 0u))),
           .depth = 1u},
  };
  prepared->transfer = (VkCopyImageInfo2){
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
      .srcImage = source->image.handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstImage = destination->image.handle,
      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .regionCount = 1u,
      .pRegions = &prepared->transfer_region,
  };
  return true_v;
}

uint64_t vkr_vk_graph_upload_bound(VkrVulkanRenderer *renderer,
                                   uint64_t direct_draw_bytes,
                                   uint64_t text_bytes,
                                   uint64_t ui_root_bytes) {
  uint64_t bytes = 0u;
  for (uint64_t order = 0u; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t index = renderer->graph->execution_order.data[order];
    const VkrRgPass *pass = &renderer->graph->passes.data[index];
    VkrVulkanGraphExecutorKind kind;
    if (!vkr_vk_graph_executor_kind(pass, &kind))
      return VKR_VULKAN_FRAME_UPLOAD_SIZE;
    // Reserve fixed-root alignment/headroom separately from view arrays and
    // variable draw data. Local shadows can add sixteen perspective views.
    bytes += 4096u;
    switch (kind) {
    case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY:
      bytes += (uint64_t)(1u + renderer->prepared_frame.shadow_cascade_count +
                          renderer->prepared_frame.local_shadow_view_count +
                          renderer->prepared_frame
                              .local_shadow_transmission_view_count) *
               (sizeof(Mat4) + VKR_FRUSTUM_PLANE_COUNT * sizeof(Vec4));
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_PICKING:
      if (!renderer->graph->packet->input.picking ||
          !renderer->graph->packet->input.picking->pending)
        break;
      bytes += direct_draw_bytes + text_bytes;
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND:
      bytes += direct_draw_bytes + text_bytes;
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_UI:
      bytes += ui_root_bytes;
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_IBL_BAKE:
      bytes +=
          (uint64_t)renderer->pending_ibl_bake_count *
          ((VKR_VULKAN_TEXTURE_MIP_MAX + 1u) * sizeof(VkrVulkanIblRoot) +
           sizeof(VkrVulkanIblShRoot) + 4u * sizeof(VkrVulkanAtmosphereRoot));
      break;
    default:
      break;
    }
    if (bytes >= VKR_VULKAN_FRAME_UPLOAD_SIZE)
      return VKR_VULKAN_FRAME_UPLOAD_SIZE;
  }
  return bytes;
}

vkr_internal bool8_t vkr_vk_prepare_fsr31_dispatch(
    VkrVulkanRenderer *renderer, VkrVulkanFsrSdkDispatch *prepared,
    const VkrRgPass *pass) {
  VkrVulkanFsrSdkImage *images[] = {&prepared->hdr_color,   &prepared->depth,
                                    &prepared->motion,      &prepared->reactive,
                                    &prepared->composition, &prepared->output};
  for (uint32_t i = 0u; i < ArrayCount(images); ++i) {
    const VkrRgImageUse *use = vkr_rg_pass_find_image_use(&pass->desc, i, 0u);
    VkrVulkanGraphImageInstance *image =
        use ? vkr_vk_graph_image(renderer, use->image,
                                 renderer->prepared_frame.image_index)
            : NULL;
    if (!image)
      return false_v;
    *images[i] = (VkrVulkanFsrSdkImage){
        .image = image->image.handle,
        .format = image->image.format,
        .extent = {image->image.width, image->image.height},
        .layout = i == 5u ? VK_IMAGE_LAYOUT_GENERAL
                          : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
  }
  const VkrPreparedFrame *packet = renderer->graph->packet;
  const Mat4 projection = packet->input.globals.projection;
  const float32_t a = projection.elements[10];
  const float32_t b = projection.elements[14];
  prepared->camera_near = b / a;
  prepared->camera_far = b / (1.0f + a);
  prepared->camera_fov_y_radians =
      2.0f * atanf(1.0f / fabsf(projection.elements[5]));
  if (!isfinite(prepared->camera_near) || !isfinite(prepared->camera_far) ||
      prepared->camera_near <= 0.0f ||
      prepared->camera_far <= prepared->camera_near ||
      projection.elements[11] != -1.0f || projection.elements[15] != 0.0f) {
    log_error("FSR 3.1 requires a finite, non-reversed perspective camera");
    return false_v;
  }
  prepared->jitter_x = packet->temporal.jitter_pixels.x;
  prepared->jitter_y = packet->temporal.jitter_pixels.y;
  prepared->motion_scale_x = (float32_t)prepared->motion.extent.width;
  prepared->motion_scale_y = (float32_t)prepared->motion.extent.height;
  prepared->frame_time_ms =
      (float32_t)(packet->input.frame.delta_time * 1000.0);
  prepared->reset = !renderer->frame_slots[renderer->active_frame_slot]
                         .temporal_history_valid;
  prepared->sharpness = 0.0f;
  return true_v;
}

vkr_internal bool8_t vkr_vk_prepare_graph_pass(
    VkrVulkanRenderer *renderer, VkrVulkanPreparedGraphPass *prepared,
    const VkrRgPass *pass) {
  VkrVulkanGraphExecutorKind kind;
  if (!vkr_vk_graph_executor_kind(pass, &kind)) {
    log_error("Vulkan graph pass '%.*s' has no executor kind",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  prepared->kind = kind;
  switch (kind) {
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_SHADOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING:
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
  case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND:
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP_PREPARE:
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_CLEAR:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY_PICKING:
  case VKR_VULKAN_GRAPH_EXECUTOR_UI:
    return vkr_vk_prepare_graph_graphics_pass(renderer, prepared, pass, kind);
  case VKR_VULKAN_GRAPH_EXECUTOR_IBL_BAKE:
    return vkr_vk_prepare_ibl_bakes(renderer, &prepared->ibl);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD:
    return vkr_vk_prepare_deferred_upload(renderer, &prepared->upload, pass,
                                          false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD:
    return vkr_vk_prepare_deferred_upload(renderer, &prepared->upload, pass,
                                          true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_CLASSIFY:
    return vkr_vk_prepare_deferred_cull(renderer, &prepared->compute, pass,
                                        VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY,
                                        false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_PREFIX:
    return vkr_vk_prepare_deferred_cull(renderer, &prepared->compute, pass,
                                        VKR_VULKAN_DEFERRED_PIPELINE_PREFIX,
                                        false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_ENCODE:
    return vkr_vk_prepare_deferred_cull(renderer, &prepared->compute, pass,
                                        VKR_VULKAN_DEFERRED_PIPELINE_ENCODE,
                                        false_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TEMPORAL_TRANSFORM:
    return vkr_vk_prepare_temporal_transform(renderer, &prepared->compute,
                                             pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_CLASSIFY:
    return vkr_vk_prepare_deferred_cull(renderer, &prepared->compute, pass,
                                        VKR_VULKAN_DEFERRED_PIPELINE_CLASSIFY,
                                        true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_PREFIX:
    return vkr_vk_prepare_deferred_cull(renderer, &prepared->compute, pass,
                                        VKR_VULKAN_DEFERRED_PIPELINE_PREFIX,
                                        true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_ENCODE:
    return vkr_vk_prepare_deferred_cull(renderer, &prepared->compute, pass,
                                        VKR_VULKAN_DEFERRED_PIPELINE_ENCODE,
                                        true_v);
  case VKR_VULKAN_GRAPH_EXECUTOR_GBUFFER_RESOLVE:
    return vkr_vk_prepare_deferred_gbuffer(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_LIGHTING_DEFERRED:
    return vkr_vk_prepare_deferred_lighting(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TEMPORAL_RESOLVE:
    return vkr_vk_prepare_temporal_resolve(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FSR31_PREPARE:
    return vkr_vk_prepare_fsr31_inputs(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FSR31_STABILIZE:
    return vkr_vk_prepare_fsr31_stabilize(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FSR31_UPSCALE:
    return vkr_vk_prepare_fsr31_dispatch(renderer, &prepared->fsr31, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_HZB_BUILD:
    return vkr_vk_prepare_deferred_hzb(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSR_DEPTH_BASE:
    return vkr_vk_prepare_ssr_depth_base(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSR_DEPTH_MIP:
    return vkr_vk_prepare_ssr_depth_mip(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSR_TRACE:
    return vkr_vk_prepare_ssr_trace(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSR_TEMPORAL:
    return vkr_vk_prepare_ssr_temporal(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSR_COMPOSITE:
    return vkr_vk_prepare_ssr_composite(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSGI_DEPTH_BASE:
    return vkr_vk_prepare_ssgi_depth_base(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSGI_DEPTH_MIP:
    return vkr_vk_prepare_ssgi_depth_mip(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSGI_TRACE:
    return vkr_vk_prepare_ssgi_trace(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSGI_TEMPORAL:
    return vkr_vk_prepare_ssgi_temporal(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SSGI_COMPOSITE:
    return vkr_vk_prepare_ssgi_composite(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FOG_APPLY:
    return vkr_vk_prepare_fog_apply(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FROXEL_INJECT:
    return vkr_vk_prepare_froxel_inject(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FROXEL_INTEGRATE:
    return vkr_vk_prepare_froxel_integrate(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_FROXEL_APPLY:
    return vkr_vk_prepare_froxel_apply(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SDSM_REDUCE:
    return vkr_vk_prepare_deferred_sdsm(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_EXPOSURE_HISTOGRAM:
    return vkr_vk_prepare_exposure_histogram(renderer, &prepared->compute,
                                             pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_EXPOSURE_RESOLVE:
    return vkr_vk_prepare_exposure_resolve(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_SUBSURFACE_GATHER:
    return vkr_vk_prepare_subsurface(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_MOTION_BLUR_TILE_MAX:
    return vkr_vk_prepare_motion_blur(
        renderer, &prepared->compute, pass,
        VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_TILE_MAX);
  case VKR_VULKAN_GRAPH_EXECUTOR_MOTION_BLUR_NEIGHBOR_MAX:
    return vkr_vk_prepare_motion_blur(
        renderer, &prepared->compute, pass,
        VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_NEIGHBOR_MAX);
  case VKR_VULKAN_GRAPH_EXECUTOR_MOTION_BLUR_RECONSTRUCT:
    return vkr_vk_prepare_motion_blur(
        renderer, &prepared->compute, pass,
        VKR_VULKAN_DEFERRED_PIPELINE_MOTION_BLUR_RECONSTRUCT);
  case VKR_VULKAN_GRAPH_EXECUTOR_DOF_COC:
    return vkr_vk_prepare_dof(renderer, &prepared->compute, pass,
                              VKR_VULKAN_DEFERRED_PIPELINE_DOF_COC);
  case VKR_VULKAN_GRAPH_EXECUTOR_DOF_DILATE_HORIZONTAL:
    return vkr_vk_prepare_dof(
        renderer, &prepared->compute, pass,
        VKR_VULKAN_DEFERRED_PIPELINE_DOF_DILATE_HORIZONTAL);
  case VKR_VULKAN_GRAPH_EXECUTOR_DOF_DILATE_VERTICAL:
    return vkr_vk_prepare_dof(renderer, &prepared->compute, pass,
                              VKR_VULKAN_DEFERRED_PIPELINE_DOF_DILATE_VERTICAL);
  case VKR_VULKAN_GRAPH_EXECUTOR_DOF_PREFILTER:
    return vkr_vk_prepare_dof(renderer, &prepared->compute, pass,
                              VKR_VULKAN_DEFERRED_PIPELINE_DOF_PREFILTER);
  case VKR_VULKAN_GRAPH_EXECUTOR_DOF_GATHER:
    return vkr_vk_prepare_dof(renderer, &prepared->compute, pass,
                              VKR_VULKAN_DEFERRED_PIPELINE_DOF_GATHER);
  case VKR_VULKAN_GRAPH_EXECUTOR_DOF_COMPOSITE:
    return vkr_vk_prepare_dof(renderer, &prepared->compute, pass,
                              VKR_VULKAN_DEFERRED_PIPELINE_DOF_COMPOSITE);
  case VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_PREFILTER:
    return vkr_vk_prepare_bloom_prefilter(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_DOWNSAMPLE:
    return vkr_vk_prepare_bloom_downsample(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_UPSAMPLE:
    return vkr_vk_prepare_bloom_upsample(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_BLOOM_COMBINE:
    return vkr_vk_prepare_bloom_combine(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DOWNSAMPLE:
    return vkr_vk_prepare_transmission_downsample(renderer, &prepared->compute,
                                                  pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_GTAO_DEPTH_PREFILTER:
    return vkr_vk_prepare_gtao_depth_prefilter(renderer, &prepared->compute,
                                               pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_GTAO_DEPTH_MIP:
    return vkr_vk_prepare_gtao_depth_mip(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_GTAO_EVALUATE:
    return vkr_vk_prepare_gtao_evaluate(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_GTAO_DENOISE:
    return vkr_vk_prepare_gtao_denoise(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_SHADE:
    return vkr_vk_prepare_deferred_transmission(renderer, &prepared->compute,
                                                pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN:
  case VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR:
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DEPTH_SEED:
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_DEPTH_SEED:
    return vkr_vk_prepare_graph_transfer_pass(renderer, prepared, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_RESOLVE:
    return vkr_vk_prepare_deferred_picking(renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COVERAGE:
    return vkr_vk_prepare_deferred_transmission_coverage(
        renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_COMPACT:
    return vkr_vk_prepare_deferred_transmission_compact(
        renderer, &prepared->compute, pass);
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_READBACK:
    // The one-pixel copy is recorded after capture selection in
    // record_frame_commands().
    return true_v;
  default:
    return false_v;
  }
}

bool8_t vkr_vk_prepare_graph(VkrVulkanRenderer *renderer) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->pass_timing_count = 0u;
  slot->gpu_compaction_state = NULL;
  slot->transmission_gpu_compaction_state = NULL;
  slot->hzb_history_input = NULL;
  slot->hzb_history_output = NULL;
  slot->temporal_transform_input = NULL;
  slot->temporal_transform_output = NULL;
  slot->temporal_color_input = NULL;
  slot->temporal_color_output = NULL;
  slot->temporal_depth_input = NULL;
  slot->temporal_depth_output = NULL;
  slot->temporal_identity_input = NULL;
  slot->temporal_identity_output = NULL;
  slot->temporal_surface_input = NULL;
  slot->temporal_surface_output = NULL;
  slot->temporal_history_valid = false_v;
  slot->motion_blur_interval_scale = 0.0f;
  slot->ssr_color_input = NULL;
  slot->ssr_color_output = NULL;
  slot->ssr_depth_input = NULL;
  slot->ssr_depth_output = NULL;
  slot->ssr_identity_input = NULL;
  slot->ssr_identity_output = NULL;
  slot->ssr_history_valid = false_v;
  slot->ssgi_color_input = NULL;
  slot->ssgi_color_output = NULL;
  slot->ssgi_depth_input = NULL;
  slot->ssgi_depth_output = NULL;
  slot->ssgi_identity_input = NULL;
  slot->ssgi_identity_output = NULL;
  slot->ssgi_history_valid = false_v;
  slot->froxel_history_input = NULL;
  slot->froxel_history_output = NULL;
  slot->froxel_history_valid = false_v;
  slot->temporal_previous_view_projection =
      renderer->graph->packet->temporal.current_view_projection;
  slot->temporal_previous_frame_index =
      renderer->graph->packet->input.frame.frame_index;
  slot->fsr31_recorded = false_v;
  slot->sdsm_reduce_state = NULL;
  slot->exposure_histogram = NULL;
  slot->exposure_state_history = NULL;
  slot->exposure_state_input = NULL;
  slot->exposure_state_output = NULL;
  const uint64_t count = renderer->graph->execution_order.length;
  if (count > VKR_RENDERER_IMPL_MAX_GRAPH_PASSES) {
    log_error(
        "Vulkan execution schedule exceeds native pass capacity (%llu/%u)",
        (unsigned long long)count, VKR_RENDERER_IMPL_MAX_GRAPH_PASSES);
    return false_v;
  }
  renderer->prepared_graph_passes = NULL;
  if (!vkr_vk_prepare_local_shadow_transmission_sampling(renderer)) {
    log_error("Vulkan failed to prepare local-shadow transmission sampling");
    return false_v;
  }
  if (count) {
    renderer->prepared_graph_passes =
        vkr_allocator_alloc(&renderer->graph_frame_allocator,
                            count * sizeof(*renderer->prepared_graph_passes),
                            VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!renderer->prepared_graph_passes) {
      log_error("Vulkan failed to allocate %llu prepared graph passes",
                (unsigned long long)count);
      return false_v;
    }
    MemZero(renderer->prepared_graph_passes,
            count * sizeof(*renderer->prepared_graph_passes));
  }
  for (uint32_t order = 0u; order < count; ++order) {
    const float64_t cpu_begin = vkr_platform_get_absolute_time();
    const uint32_t pass_index = renderer->graph->execution_order.data[order];
    const VkrRgPass *pass = &renderer->graph->passes.data[pass_index];
    VkrVulkanPreparedGraphPass *prepared =
        &renderer->prepared_graph_passes[order];
    if (!vkr_vk_prepare_graph_pass_barriers(renderer, pass,
                                            &prepared->dependencies) ||
        !vkr_vk_prepare_graph_pass(renderer, prepared, pass)) {
      log_error("Vulkan failed to prepare graph pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    VkrRendererImplPassTiming *timing = &slot->pass_timings[order];
    MemZero(timing, sizeof(*timing));
    const uint64_t length =
        Min(pass->desc.name.length, (uint64_t)sizeof(timing->name) - 1u);
    MemCopy(timing->name, pass->desc.name.str, length);
    timing->pass_index = pass_index;
    timing->cpu_ms = (vkr_platform_get_absolute_time() - cpu_begin) * 1000.0;
  }
  slot->pass_timing_count = (uint32_t)count;
  slot->timestamp_query_count =
      slot->timing_requested ? (uint32_t)count * 2u : 0u;
  renderer->prepared_terminal_barriers =
      (VkDependencyInfo){.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  return vkr_vk_prepare_graph_image_barriers(
      renderer, &renderer->graph->terminal_image_barriers,
      &renderer->prepared_terminal_barriers);
}

vkr_internal void
vkr_vk_record_graph_graphics_pass(VkrVulkanRenderer *renderer,
                                  VkCommandBuffer command,
                                  const VkrVulkanPreparedGraphPass *prepared) {
  vkCmdBeginRendering(command, &prepared->rendering);
  vkCmdSetViewport(command, 0u, 1u, &prepared->viewport);
  vkCmdSetScissor(command, 0u, 1u, &prepared->scissor);
  vkCmdSetCullMode(command, VK_CULL_MODE_NONE);
  vkCmdSetFrontFace(command, VK_FRONT_FACE_COUNTER_CLOCKWISE);
  switch (prepared->kind) {
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW:
  case VKR_VULKAN_GRAPH_EXECUTOR_SHADOW:
    vkCmdSetDepthBias(command, prepared->depth_bias.depth_bias_constant,
                      prepared->depth_bias.depth_bias_clamp,
                      prepared->depth_bias.depth_bias_slope);
    if (prepared->raster.indices)
      vkr_vk_record_prepared_raster(renderer, command, &prepared->raster);
    break;
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
  case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
    vkr_vk_record_prepared_raster(renderer, command, &prepared->raster);
    break;
  case VKR_VULKAN_GRAPH_EXECUTOR_PICKING:
  case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND:
    vkr_vk_record_world_draws(renderer, command, &prepared->world);
    vkr_vk_record_text(renderer, command, &prepared->text);
    break;
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY_PICKING:
    vkr_vk_record_editor_overlay(renderer, command, &prepared->overlay);
    break;
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP_PREPARE:
  case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP:
  case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR:
    vkr_vk_record_fullscreen(renderer, command, &prepared->fullscreen);
    break;
  case VKR_VULKAN_GRAPH_EXECUTOR_UI:
    vkr_vk_record_ui(renderer, command, &prepared->ui);
    break;
  default:
    break;
  }
  vkCmdEndRendering(command);
}

bool8_t vkr_vk_record_graph(VkrVulkanRenderer *renderer,
                            VkCommandBuffer command) {
  VkrVulkanFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const PFN_vkCmdBeginDebugUtilsLabelEXT begin_label =
      vkr_vulkan_device_cmd_begin_debug_label(renderer->device);
  const PFN_vkCmdEndDebugUtilsLabelEXT end_label =
      vkr_vulkan_device_cmd_end_debug_label(renderer->device);
  for (uint32_t i = 0u; i < slot->pass_timing_count; ++i) {
    const VkrVulkanPreparedGraphPass *prepared =
        &renderer->prepared_graph_passes[i];
    VkrRendererImplPassTiming *timing = &slot->pass_timings[i];
    const float64_t cpu_begin = vkr_platform_get_absolute_time();
    if (begin_label) {
      const VkDebugUtilsLabelEXT label = {
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
          .pLabelName = timing->name};
      begin_label(command, &label);
    }
    if (slot->timing_requested)
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                           slot->timestamp_pool, i * 2u);
    if (prepared->dependencies.imageMemoryBarrierCount ||
        prepared->dependencies.bufferMemoryBarrierCount)
      vkCmdPipelineBarrier2(command, &prepared->dependencies);
    switch (prepared->kind) {
    case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION0:
    case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION1:
    case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW_TRANSMISSION_OVERFLOW:
    case VKR_VULKAN_GRAPH_EXECUTOR_LOCAL_SHADOW:
    case VKR_VULKAN_GRAPH_EXECUTOR_SHADOW:
    case VKR_VULKAN_GRAPH_EXECUTOR_PICKING:
    case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_OPAQUE:
    case VKR_VULKAN_GRAPH_EXECUTOR_VBUFFER_TRANSMISSION:
    case VKR_VULKAN_GRAPH_EXECUTOR_WORLD_BLEND:
    case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP_PREPARE:
    case VKR_VULKAN_GRAPH_EXECUTOR_TONEMAP:
    case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR:
    case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_CLEAR:
    case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY:
    case VKR_VULKAN_GRAPH_EXECUTOR_EDITOR_OVERLAY_PICKING:
    case VKR_VULKAN_GRAPH_EXECUTOR_UI:
      vkr_vk_record_graph_graphics_pass(renderer, command, prepared);
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_IBL_BAKE:
      vkr_vk_record_ibl_bakes(renderer, command, &prepared->ibl);
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_GPU_DRAW_UPLOAD:
    case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_GPU_DRAW_UPLOAD:
      vkr_vk_record_prepared_upload(command, &prepared->upload);
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_DEPTH_SEED:
    case VKR_VULKAN_GRAPH_EXECUTOR_TRANSMISSION_DEPTH_SEED:
    case VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN:
    case VKR_VULKAN_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR:
      if (prepared->transfer.regionCount)
        vkCmdCopyImage2(command, &prepared->transfer);
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_PICKING_READBACK:
      break;
    case VKR_VULKAN_GRAPH_EXECUTOR_FSR31_UPSCALE: {
#if VKR_HAS_FSR3_UPSCALER
      VkrVulkanFsrSdkDispatch dispatch = prepared->fsr31;
      dispatch.command_buffer = command;
      // Even a failed dispatch may have changed SDK CPU-side state.
      slot->fsr31_recorded = true_v;
      const VkrVulkanFsrSdkResult result =
          vkr_vulkan_fsr_sdk_dispatch(renderer->fsr31, &dispatch);
      if (result != VKR_VULKAN_FSR_SDK_SUCCESS) {
        if (end_label)
          end_label(command);
        log_error("FSR 3.1 dispatch failed (%u)", (uint32_t)result);
        return false_v;
      }
      // SDK descriptor sets invalidate descriptor-buffer bindings and offsets.
      vkr_vk_bind_descriptor_buffers(renderer, command);
      break;
#else
      if (end_label)
        end_label(command);
      log_error("FSR 3.1 upscaling is unavailable in this Vulkan build");
      return false_v;
#endif
    }
    default:
      vkr_vk_record_prepared_compute(renderer, command, &prepared->compute);
      break;
    }
    if (slot->timing_requested)
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                           slot->timestamp_pool, i * 2u + 1u);
    if (end_label)
      end_label(command);
    timing->cpu_ms += (vkr_platform_get_absolute_time() - cpu_begin) * 1000.0;
  }
  if (renderer->prepared_terminal_barriers.imageMemoryBarrierCount)
    vkCmdPipelineBarrier2(command, &renderer->prepared_terminal_barriers);
  return true_v;
}
