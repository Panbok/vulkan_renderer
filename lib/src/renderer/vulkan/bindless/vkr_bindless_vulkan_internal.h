#ifndef VKR_BINDLESS_VULKAN_INTERNAL_H
#define VKR_BINDLESS_VULKAN_INTERNAL_H

#include "renderer/vulkan/bindless/vkr_bindless_vulkan_renderer.h"

#include "core/logger.h"
#include "filesystem/filesystem.h"
#include "renderer/resources/loaders/mesh_loader.h"
#include "renderer/systems/vkr_geometry_system.h"
#include "renderer/systems/vkr_texture_system.h"
#include "renderer/vkr_capture_ring.h"
#include "renderer/vkr_gpu_abi.h"
#include "renderer/vkr_gpu_memory.h"
#include "renderer/vkr_gpu_slot_table.h"
#include "renderer/vkr_gpu_submit_ring.h"
#include "renderer/vkr_ibl_math.h"
#include "renderer/vkr_packet_constants.h"
#include "renderer/vkr_render_graph_internal.h"
#include "renderer/vkr_rg_json.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_dependency.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_memory.h"
#include "renderer/vulkan/bindless/vkr_bindless_vulkan_wsi.h"

#include "memory/arena.h"
#include "memory/vkr_arena_allocator.h"
#include "memory/vkr_dmemory.h"
#include "platform/vkr_platform.h"

#include <spirv_reflect.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VKR_BINDLESS_VK_PACKET_WORLD_VERT_SPV
#define VKR_BINDLESS_VK_PACKET_WORLD_VERT_SPV "packet.world.vert.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_WORLD_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_WORLD_FRAG_SPV "packet.world.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_SHADOW_VERT_SPV
#define VKR_BINDLESS_VK_PACKET_SHADOW_VERT_SPV "packet.shadow.vert.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_SHADOW_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_SHADOW_FRAG_SPV "packet.shadow.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_PICKING_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_PICKING_FRAG_SPV "packet.picking.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_FULLSCREEN_VERT_SPV
#define VKR_BINDLESS_VK_PACKET_FULLSCREEN_VERT_SPV "packet.fullscreen.vert.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_FULLSCREEN_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_FULLSCREEN_FRAG_SPV "packet.fullscreen.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_SKYBOX_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_SKYBOX_FRAG_SPV "packet.skybox.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_TEXT_VERT_SPV
#define VKR_BINDLESS_VK_PACKET_TEXT_VERT_SPV "packet.text.vert.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_TEXT_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_TEXT_FRAG_SPV "packet.text.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_TEXT_PICKING_FRAG_SPV
#define VKR_BINDLESS_VK_PACKET_TEXT_PICKING_FRAG_SPV                           \
  "packet.text_picking.frag.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_IBL_EQUIRECT_COMP_SPV
#define VKR_BINDLESS_VK_PACKET_IBL_EQUIRECT_COMP_SPV                           \
  "packet.ibl_equirect.comp.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_IBL_IRRADIANCE_COMP_SPV
#define VKR_BINDLESS_VK_PACKET_IBL_IRRADIANCE_COMP_SPV                         \
  "packet.ibl_irradiance.comp.spv"
#endif
#ifndef VKR_BINDLESS_VK_PACKET_IBL_PREFILTER_COMP_SPV
#define VKR_BINDLESS_VK_PACKET_IBL_PREFILTER_COMP_SPV                          \
  "packet.ibl_prefilter.comp.spv"
#endif

enum {
  /**
   * Slot zero of every descriptor heap holds a valid sentinel descriptor — a
   * 1x1 opaque-white image, a flat normal, a default sampler — so a material
   * that omits a texture still resolves to something legal instead of an
   * undefined descriptor. Anything that means "no specific resource" must name
   * this rather than writing a bare 0.
   */
  VKR_BINDLESS_VK_SENTINEL_SLOT_INDEX = 0,
  VKR_BINDLESS_VK_SENTINEL_UPLOAD_SIZE = 4,
  VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX = 8,
  VKR_BINDLESS_VK_RETIRED_SWAPCHAIN_MAX = 8,
  VKR_BINDLESS_VK_GRAPH_LAYER_MAX = 16,
  VKR_BINDLESS_VK_TEXTURE_MIP_MAX = 16,
  VKR_BINDLESS_VK_PENDING_IBL_BAKE_MAX = 32,
  VKR_BINDLESS_VK_FRAME_UPLOAD_SIZE = 16u * 1024u * 1024u,
};

typedef enum VkrBindlessVkPacketPipeline {
  VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW = 0,
  VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING,
  VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_OPAQUE,
  VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND,
  VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_HDR,
  VKR_BINDLESS_VK_PACKET_PIPELINE_SKYBOX,
  VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_FINAL,
  VKR_BINDLESS_VK_PACKET_PIPELINE_UI,
  VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_TEXT,
  VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING_TEXT,
  VKR_BINDLESS_VK_PACKET_PIPELINE_UI_TEXT,
  VKR_BINDLESS_VK_PACKET_PIPELINE_COUNT,
} VkrBindlessVkPacketPipeline;

typedef enum VkrBindlessVkPacketShader {
  VKR_BINDLESS_VK_PACKET_SHADER_WORLD_VERTEX = 0,
  VKR_BINDLESS_VK_PACKET_SHADER_WORLD_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_SHADOW_VERTEX,
  VKR_BINDLESS_VK_PACKET_SHADER_SHADOW_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_PICKING_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_VERTEX,
  VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_SKYBOX_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_TEXT_VERTEX,
  VKR_BINDLESS_VK_PACKET_SHADER_TEXT_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_TEXT_PICKING_FRAGMENT,
  VKR_BINDLESS_VK_PACKET_SHADER_COUNT,
} VkrBindlessVkPacketShader;

typedef enum VkrBindlessVkIblPipeline {
  VKR_BINDLESS_VK_IBL_PIPELINE_EQUIRECT = 0,
  VKR_BINDLESS_VK_IBL_PIPELINE_IRRADIANCE,
  VKR_BINDLESS_VK_IBL_PIPELINE_PREFILTER,
  VKR_BINDLESS_VK_IBL_PIPELINE_COUNT,
} VkrBindlessVkIblPipeline;

typedef enum VkrBindlessVkMaterialFlag {
  VKR_BINDLESS_VK_MATERIAL_TEXTURE_NORMAL = 1u << 0u,
  VKR_BINDLESS_VK_MATERIAL_TEXTURE_ORM = 1u << 1u,
  VKR_BINDLESS_VK_MATERIAL_TEXTURE_EMISSIVE = 1u << 2u,
} VkrBindlessVkMaterialFlag;

typedef struct VKR_SIMD_ALIGN VkrBindlessVkMaterialGpuRow {
  float32_t tint[4];
  uint32_t base_color_texture;
  uint32_t normal_texture;
  uint32_t orm_texture;
  uint32_t emissive_texture;
  uint32_t base_color_sampler;
  uint32_t normal_sampler;
  uint32_t orm_sampler;
  uint32_t emissive_sampler;
  uint32_t material_id;
  uint32_t flags;
  uint32_t alpha_mode;
  uint32_t reserved;
  Vec4 material_emissive;
  Vec4 material_dielectric_specular;
  Vec4 material_surface;
  Vec4 material_alpha;
  Vec4 material_attenuation_color;
} VkrBindlessVkMaterialGpuRow;

typedef struct VkrBindlessVkPushConstants {
  uint64_t root;
  uint32_t material_index;
  uint32_t flags;
} VkrBindlessVkPushConstants;

typedef struct VKR_SIMD_ALIGN VkrBindlessVkIblRoot {
  uint32_t source_texture;
  uint32_t source_sampler;
  uint32_t target_texture;
  uint32_t target_size;
  uint32_t sample_count;
  uint32_t source_face_size;
  uint32_t source_mip_count;
  float32_t roughness;
} VkrBindlessVkIblRoot;

typedef struct VKR_SIMD_ALIGN VkrBindlessVkPacketShadowCascade {
  Mat4 light_view_projection;
  Vec4 split_depth;
} VkrBindlessVkPacketShadowCascade;

typedef struct VKR_SIMD_ALIGN VkrBindlessVkPacketIblProbe {
  uint32_t irradiance_texture;
  uint32_t irradiance_sampler;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  Vec4 center_blend;
  Vec4 extents_weight;
  Vec4 intensity_box;
} VkrBindlessVkPacketIblProbe;

/** Values shared by every indexed draw recorded for one pass. */
typedef struct VKR_SIMD_ALIGN VkrBindlessVkPacketFrameRoot {
  uint64_t instances;
  uint32_t instance_address_padding[2];
  Mat4 view_projection;
  uint64_t materials;
  uint32_t irradiance_texture;
  uint32_t irradiance_sampler;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  /* Retired BRDF-LUT slot pair. The environment BRDF is analytic in
     packet.slang; these stay as named padding so no downstream offset moves. */
  uint32_t reserved_brdf_texture;
  uint32_t reserved_brdf_sampler;
  uint32_t shadow_texture;
  uint32_t shadow_sampler;
  uint32_t transmission_texture;
  uint32_t transmission_sampler;
  Vec4 view_position;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  uint32_t reserved_0[2];
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t reserved_1[3];
  uint64_t point_light_data;
  uint64_t point_light_masks;
  Vec4 point_light_grid_origin_cell_size;
  uint32_t point_light_grid_dimensions_count[4];
  VkrPointLightMask point_light_global_mask;
  uint32_t point_light_count;
  uint32_t point_light_reserved[3];
  uint64_t shadow_cascades;
  Mat4 view;
  uint32_t shadow_cascade_count;
  float32_t shadow_bias;
  uint32_t shadow_reserved[2];
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
} VkrBindlessVkPacketFrameRoot;

/** The only record written per indexed packet draw. */
typedef struct VKR_SIMD_ALIGN VkrBindlessVkPacketDrawRoot {
  uint64_t geometry_rows;
  uint64_t visible_rows;
  uint64_t vertices;
  uint64_t frame;
  uint32_t visible_row_index;
  uint32_t flags;
  uint32_t reserved[2];
} VkrBindlessVkPacketDrawRoot;

/** Non-world utility shaders retain a single-draw root because their model,
 * text controls, or source texture genuinely vary with that draw. */
typedef struct VKR_SIMD_ALIGN VkrBindlessVkPacketUtilityRoot {
  uint64_t vertices;
  uint64_t instances;
  Mat4 view_projection;
  uint64_t materials;
  uint32_t irradiance_texture;
  uint32_t irradiance_sampler;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  uint32_t reserved_brdf_texture;
  uint32_t reserved_brdf_sampler;
  uint32_t shadow_texture;
  uint32_t shadow_sampler;
  uint32_t transmission_texture;
  uint32_t transmission_sampler;
  Vec4 view_position;
  uint32_t material_index;
  uint32_t first_instance;
  uint32_t prefilter_mip_count;
  uint32_t flags;
  Vec4 material_emissive;
  Vec4 material_dielectric_specular;
  Vec4 material_surface;
  Vec4 material_alpha;
  Vec4 material_attenuation_color;
  Vec4 ibl_controls;
  Vec4 directional_direction_enabled;
  Vec4 directional_color_intensity;
  Vec4 ambient_color;
  uint32_t render_mode;
  uint32_t alpha_mode;
  uint32_t material_flags;
  uint32_t reserved_0;
  uint64_t point_light_data;
  uint64_t point_light_masks;
  Vec4 point_light_grid_origin_cell_size;
  uint32_t point_light_grid_dimensions_count[4];
  VkrPointLightMask point_light_global_mask;
  uint32_t point_light_count;
  uint32_t point_light_reserved[3];
  uint64_t shadow_cascades;
  Mat4 view;
  uint32_t shadow_cascade_count;
  float32_t shadow_bias;
  uint32_t shadow_reserved[2];
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t ibl_probe_reserved;
} VkrBindlessVkPacketUtilityRoot;

_Static_assert(sizeof(VkrVertex3d) == 64u, "Shared vertex ABI drift");
_Static_assert(sizeof(VkrBindlessVkMaterialGpuRow) == 144u,
               "Vulkan material row ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, base_color_texture) == 16u,
               "Vulkan material texture ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, base_color_sampler) == 32u,
               "Vulkan material sampler ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, material_id) == 48u,
               "Vulkan material identifier ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, material_emissive) == 64u,
               "Vulkan material parameter ABI drift");
_Static_assert(sizeof(VkrBindlessVkPushConstants) == 16u,
               "Push-constant ABI drift");
_Static_assert(sizeof(VkrBindlessVkIblRoot) == 32u, "IBL-root ABI drift");
_Static_assert(sizeof(VkrBindlessVkPacketFrameRoot) == 432u,
               "Packet frame-root ABI size drift");
_Static_assert(sizeof(VkrBindlessVkPacketDrawRoot) == 48u,
               "Packet draw-root ABI size drift");
_Static_assert(sizeof(VkrBindlessVkPacketUtilityRoot) == 512u,
               "Packet utility-root ABI size drift");

typedef struct VkrBindlessVkAllocation {
  VkDeviceMemory memory;
  VkDeviceSize memory_size;
  VkDeviceSize offset;
  void *mapped;
  uint32_t memory_type_index;
  VkMemoryPropertyFlags properties;
  VkrBindlessVkPooledAllocation pooled_allocation;
  VkrBindlessVkMemoryPoolKey pool_key;
  bool8_t pooled;
  bool8_t dedicated;
  bool8_t retired;
} VkrBindlessVkAllocation;

typedef struct VkrBindlessVkBuffer {
  VkBuffer handle;
  VkrBindlessVkAllocation allocation;
  VkDeviceAddress address;
  VkDeviceSize size;
} VkrBindlessVkBuffer;

typedef struct VkrBindlessVkImage {
  VkImage handle;
  VkImageView view;
  VkrBindlessVkAllocation allocation;
  VkImageLayout layout;
  uint32_t width;
  uint32_t height;
  uint32_t mip_levels;
  uint32_t array_layers;
  VkFormat format;
} VkrBindlessVkImage;

typedef struct VkrBindlessVkDirtyRange {
  VkDeviceSize offset;
  VkDeviceSize end;
  bool8_t dirty;
} VkrBindlessVkDirtyRange;

typedef struct VkrBindlessVkTargetSet {
  VkrBindlessVkImage images[VKR_BINDLESS_VK_TARGET_IMAGE_MAX];
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
} VkrBindlessVkTargetSet;

typedef struct VkrBindlessVkGraphImageInstance {
  VkrBindlessVkImage image;
  VkImageView mip_views[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  VkImageView mip_layer_views[VKR_BINDLESS_VK_TEXTURE_MIP_MAX]
                             [VKR_BINDLESS_VK_GRAPH_LAYER_MAX];
  VkrGpuSlotHandle sampled_mip_slots[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle storage_mip_slots[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle sampled_slot;
  VkrGpuSlotHandle storage_slot;
  uint64_t last_use_submit_value;
  bool8_t has_sampled_mip_slot[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  bool8_t has_storage_mip_slot[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  bool8_t has_sampled_slot;
  bool8_t has_storage_slot;
} VkrBindlessVkGraphImageInstance;

typedef struct VkrBindlessVkGraphImage {
  VkrBindlessVkGraphImageInstance instances[VKR_BINDLESS_VK_TARGET_IMAGE_MAX];
  VkrRgImageDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
  bool8_t external_swapchain;
} VkrBindlessVkGraphImage;

typedef struct VkrBindlessVkGraphBufferInstance {
  VkrBindlessVkBuffer buffer;
  uint64_t last_use_submit_value;
} VkrBindlessVkGraphBufferInstance;

typedef struct VkrBindlessVkGraphBuffer {
  VkrBindlessVkGraphBufferInstance instances[VKR_BINDLESS_VK_TARGET_IMAGE_MAX];
  VkrRgBufferDesc desc;
  uint32_t graph_generation;
  uint32_t instance_count;
  bool8_t live;
} VkrBindlessVkGraphBuffer;

typedef struct VkrBindlessVkRetiredTargetSet {
  VkrBindlessVkTargetSet targets;
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredTargetSet;

typedef struct VkrBindlessVkFrameSlot {
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkQueryPool timestamp_pool;
  VkrBindlessVkBuffer readback;
  VkrBindlessVkBuffer capture_readback;
  VkrBindlessVkBuffer frame_upload;
  VkrCaptureBackendItemPlan capture_plans[VKR_CAPTURE_MAX_ITEMS];
  VkrRgImageHandle capture_images[VKR_CAPTURE_MAX_ITEMS];
  VkrCaptureRequestId capture_request_id;
  uint32_t capture_item_count;
  uint64_t retire_value;
  uint64_t source_frame_index;
  uint32_t image_index;
  uint32_t timestamp_query_count;
  uint32_t pass_timing_count;
  VkrRendererImplPassTiming pass_timings[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
  bool8_t timing_requested;
  bool8_t timing_collected;
  bool8_t acquired_window_image;
  bool8_t reacquired_presented_image;
  uint64_t frame_upload_cursor;
  /** Frame-upload allocation failures this frame. Non-zero means the frame was
   *  rejected for want of upload bytes, not for a malformed packet. */
  uint32_t frame_upload_exhaustions;
  uint64_t world_instances;
  uint64_t shadow_instances;
  uint64_t ui_instances;
  uint64_t editor_instances;
  uint64_t picking_instances;
  uint64_t point_light_data;
  uint64_t point_light_masks;
  uint64_t shadow_cascades;
  uint64_t ibl_probes;
  uint32_t ibl_probe_count;
  uint32_t irradiance_texture;
  uint32_t irradiance_sampler;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  bool8_t ibl_ready;
  uint32_t picking_x;
  uint32_t picking_y;
  bool8_t picking_readback_pending;
  uint32_t indexed_draw_count;
  uint32_t shadow_draw_count;
  uint32_t shadow_opaque_draw_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t shadow_alpha_draw_count[VKR_SHADOW_CASCADE_COUNT_MAX];
  uint32_t opaque_draw_count;
  uint32_t transmission_draw_count;
  uint32_t blend_draw_count;
} VkrBindlessVkFrameSlot;

typedef struct VkrBindlessVkWindowTarget {
  VkSwapchainKHR swapchain;
  VkImage images[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  VkSemaphore render_complete[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  VkFence present_complete[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  uint64_t image_last_submit_value[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  bool8_t image_presented[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  bool8_t present_fence_pending[VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX];
  VkrBindlessVulkanReacquireState reacquire_state;
  uint32_t image_count;
  uint32_t width;
  uint32_t height;
  VkFormat format;
  VkColorSpaceKHR color_space;
  VkPresentModeKHR present_mode;
  bool8_t occupied;
} VkrBindlessVkWindowTarget;

typedef struct VkrBindlessVkRetiredWindowTarget {
  VkrBindlessVkWindowTarget target;
  bool8_t occupied;
} VkrBindlessVkRetiredWindowTarget;

typedef struct VkrBindlessVkPublishedTexture {
  VkrTextureHandle handle;
  VkrTextureHandle ibl_irradiance;
  VkrTextureHandle ibl_prefilter;
  VkrBindlessVkImage image;
  VkrGpuSlotHandle sampled_slot;
  VkImageView storage_views[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  VkrGpuSlotHandle storage_slots[VKR_BINDLESS_VK_TEXTURE_MIP_MAX];
  uint32_t sampler_record_index;
  uint32_t material_reference_count;
  uint32_t ibl_reference_count;
  uint64_t last_use_submit_value;
  uint32_t storage_slot_count;
  bool8_t initialization_pending;
  bool8_t unpublish_requested;
  bool8_t live;
  bool8_t pending_retire;
} VkrBindlessVkPublishedTexture;

typedef struct VkrBindlessVkPendingIblBake {
  VkrTextureHandle equirect;
  VkrTextureHandle source;
  VkrTextureHandle irradiance;
  VkrTextureHandle prefilter;
  bool8_t convert_equirect;
  bool8_t recorded;
} VkrBindlessVkPendingIblBake;

typedef struct VkrBindlessVkPublishedSampler {
  VkrTextureDescription description;
  VkSampler sampler;
  VkrGpuSlotHandle slot;
  uint64_t last_use_submit_value;
  uint32_t mip_levels;
  uint32_t reference_count;
  bool8_t live;
  bool8_t pending_retire;
} VkrBindlessVkPublishedSampler;

typedef struct VkrBindlessVkTextureUploadBatch {
  VkBufferImageCopy2 region;
  uint64_t source_offset;
  uint64_t source_size;
} VkrBindlessVkTextureUploadBatch;

typedef struct VkrBindlessVkPendingTextureInitialization {
  VkrBindlessVkBuffer staging;
  VkrBindlessVkTextureUploadBatch *batches;
  uint64_t batches_size;
  uint8_t *upload_data;
  uint64_t upload_data_size;
  VkrTextureHandle texture;
  uint32_t batch_count;
  uint32_t next_batch;
  uint32_t staged_batch_count;
  bool8_t writable;
} VkrBindlessVkPendingTextureInitialization;

typedef struct VkrBindlessVkPendingBufferInitialization {
  VkrBindlessVkBuffer staging;
  VkBuffer destination;
  uint8_t *upload_data;
  VkDeviceSize size;
  VkDeviceSize next_offset;
  VkDeviceSize destination_offset;
  VkPipelineStageFlags2 destination_stage;
  VkAccessFlags2 destination_access;
  uint32_t geometry_record_index;
} VkrBindlessVkPendingBufferInitialization;

typedef struct VkrBindlessVkRetiredGeometryMegabuffer {
  VkrBindlessVkBuffer vertices;
  VkrBindlessVkBuffer indices;
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredGeometryMegabuffer;

typedef struct VkrBindlessVkGeometryMegabuffer {
  VkrBindlessVkBuffer vertices;
  VkrBindlessVkBuffer indices;
  VkrBindlessVkBuffer copy_source_vertices;
  VkrBindlessVkBuffer copy_source_indices;
  uint64_t copy_vertex_size;
  uint64_t copy_index_size;
  VkrBindlessVkRetiredGeometryMegabuffer retired[4];
  uint64_t vertex_cursor;
  uint64_t index_cursor;
  uint64_t vertex_live_bytes;
  uint64_t index_live_bytes;
  uint64_t vertex_high_water;
  uint64_t index_high_water;
  uint64_t rejected_publications;
  uint64_t generation_replacements;
  uint32_t generation;
  bool8_t live;
  bool8_t copy_pending;
} VkrBindlessVkGeometryMegabuffer;

typedef struct VkrBindlessVkRetiredStagingBuffer {
  VkrBindlessVkBuffer buffer;
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredStagingBuffer;

typedef struct VkrBindlessVkSubmeshRange {
  uint32_t first_index;
  uint32_t index_count;
  int32_t vertex_offset;
} VkrBindlessVkSubmeshRange;

typedef struct VkrBindlessVkPublishedGeometry {
  VkrGeometryHandle handle;
  VkrBindlessVkBuffer vertices;
  VkrBindlessVkBuffer indices;
  VkrGpuGeometryRow gpu_row;
  uint32_t vertex_count;
  uint32_t index_count;
  VkIndexType index_type;
  VkrBindlessVkSubmeshRange *submeshes;
  uint64_t submeshes_size;
  uint32_t submesh_count;
  uint64_t last_use_submit_value;
  uint32_t pending_initialization_count;
  bool8_t live;
  bool8_t pending_retire;
} VkrBindlessVkPublishedGeometry;

typedef struct VkrBindlessVkPublishedMaterial {
  VkrMaterialHandle handle;
  VkrGpuSlotHandle slot;
  VkrBindlessVkMaterialGpuRow row;
  VkrPbrProperties pbr;
  float32_t alpha_cutoff;
  VkrMaterialAlphaMode alpha_mode;
  bool8_t double_sided;
  uint32_t texture_record_indices[4];
  uint8_t pending_texture_count;
  bool8_t live;
} VkrBindlessVkPublishedMaterial;

typedef struct VkrBindlessVkRetiredMaterial {
  uint32_t texture_record_indices[4];
  uint64_t retire_value;
  bool8_t occupied;
} VkrBindlessVkRetiredMaterial;

struct VkrBindlessVulkanRenderer {
  VkrAllocator *allocator;
  VkrBindlessVulkanRendererConfig config;
  VkrDMemory publication_staging_memory;
  VkrDMemory capture_storage_memory;
  Arena *graph_frame_arena;
  VkrAllocator graph_frame_allocator;
  VkrRgJsonGraph json_graph;
  VkrRenderGraph *graph;
  VkrRgExecutorRegistry executors;
  VkrRenderGraphFrameInfo prepared_frame;
  VkrBindlessVkGraphImage *graph_images;
  uint64_t graph_images_size;
  VkrBindlessVkGraphBuffer *graph_buffers;
  uint64_t graph_buffers_size;
  VkImageMemoryBarrier2 *graph_image_barriers;
  uint64_t graph_image_barriers_size;
  VkBufferMemoryBarrier2 *graph_buffer_barriers;
  uint64_t graph_buffer_barriers_size;
  VkrBindlessVulkanDevice *device;
  VkrBindlessVkTargetSet targets;
  VkrBindlessVkRetiredTargetSet retired_targets[4];
  VkrBindlessVkWindowTarget window_target;
  VkrBindlessVkRetiredWindowTarget
      retired_window_targets[VKR_BINDLESS_VK_RETIRED_SWAPCHAIN_MAX];
  VkSemaphore acquire_semaphores[VKR_BINDLESS_VK_FRAME_SLOT_COUNT];
  VkrBindlessVkFrameSlot frame_slots[VKR_BINDLESS_VK_FRAME_SLOT_COUNT];
  VkrGpuSubmitRing command_ring;
  VkrGpuSubmitRingSlot command_ring_slots[VKR_BINDLESS_VK_FRAME_SLOT_COUNT];
  VkrGpuRingSlice active_command_slice;
  VkrCaptureRing capture_ring;
  void *capture_storage;
  uint64_t capture_storage_size;
  VkrBindlessVkMemoryPoolManager *memory_pool;
  VkrBindlessVkBuffer resource_descriptors;
  VkrBindlessVkBuffer sampler_descriptors;
  VkrGpuSlotTable *sampled_image_slots;
  VkrGpuSlotTable *storage_image_slots;
  VkrGpuSlotTable *sampler_slots;
  VkrGpuSlotTable *material_slots;
  VkrBindlessVkPublishedGeometry *published_geometries;
  VkrBindlessVkGeometryMegabuffer geometry_megabuffer;
  VkrBindlessVkPublishedGeometry *retired_geometries;
  VkrBindlessVkPublishedTexture *published_textures;
  VkrBindlessVkPublishedTexture *retired_textures;
  VkrBindlessVkPublishedSampler *published_samplers;
  VkrBindlessVkPublishedMaterial *published_materials;
  VkrBindlessVkRetiredMaterial *retired_materials;
  VkrBindlessVkPendingTextureInitialization *pending_texture_initializations;
  VkrBindlessVkPendingBufferInitialization *pending_buffer_initializations;
  VkrBindlessVkRetiredStagingBuffer *retired_staging_buffers;
  uint64_t published_geometries_size;
  uint64_t retired_geometries_size;
  uint64_t published_textures_size;
  uint64_t retired_textures_size;
  uint64_t published_samplers_size;
  uint64_t published_materials_size;
  uint64_t retired_materials_size;
  uint64_t pending_texture_initializations_size;
  uint64_t pending_buffer_initializations_size;
  uint64_t retired_staging_buffers_size;
  uint32_t pending_texture_initialization_count;
  uint32_t pending_buffer_initialization_count;
  uint32_t pending_buffer_initialization_capacity;
  uint32_t retired_staging_buffer_capacity;
  uint32_t staging_buffer_count;
  void *sampled_image_slot_storage;
  void *storage_image_slot_storage;
  void *sampler_slot_storage;
  void *material_slot_storage;
  uint64_t sampled_image_slot_storage_size;
  uint64_t storage_image_slot_storage_size;
  uint64_t sampler_slot_storage_size;
  uint64_t material_slot_storage_size;
  uint8_t *descriptor_scratch;
  uint32_t descriptor_scratch_size;
  VkrBindlessVkDirtyRange resource_descriptor_dirty;
  VkrBindlessVkDirtyRange sampler_descriptor_dirty;
  VkrBindlessVkDirtyRange material_dirty;
  VkrBindlessVkBuffer upload;
  VkrBindlessVkBuffer materials;
  VkrBindlessVkImage sentinel_image;
  VkSampler sentinel_sampler;
  VkPipelineLayout pipeline_layout;
  VkPipelineCache pipeline_cache;
  char pipeline_cache_path[1024];
  VkShaderModule packet_shaders[VKR_BINDLESS_VK_PACKET_SHADER_COUNT];
  VkPipeline packet_pipelines[VKR_BINDLESS_VK_PACKET_PIPELINE_COUNT];
  VkShaderModule ibl_shaders[VKR_BINDLESS_VK_IBL_PIPELINE_COUNT];
  VkPipeline ibl_pipelines[VKR_BINDLESS_VK_IBL_PIPELINE_COUNT];
  VkrBindlessVkPendingIblBake
      pending_ibl_bakes[VKR_BINDLESS_VK_PENDING_IBL_BAKE_MAX];
  uint32_t pending_ibl_bake_count;
  VkSemaphore timeline;
  uint64_t submit_value;
  uint64_t completed_value;
  uint64_t upload_wait_count;
  uint64_t frame_upload_exhaustion_count;
  uint64_t command_slot_wait_count;
  uint32_t active_frame_slot;
  uint32_t next_image_index;
  bool8_t frame_active;
  bool8_t sentinel_uploaded;
  bool8_t target_dirty;
  bool8_t terminal_failure;
};

VkDevice
vkr_bindless_vk_renderer_device(const VkrBindlessVulkanRenderer *renderer);
VkFormat vkr_bindless_vk_texture_format(VkrTextureFormat format);
VkImageAspectFlags vkr_bindless_vk_format_aspects(VkFormat format);
VkImageLayout vkr_bindless_vk_texture_layout(VkrTextureLayout layout);
VkrBindlessVkGraphImageInstance *
vkr_bindless_vk_graph_image(VkrBindlessVulkanRenderer *renderer,
                            VkrRgImageHandle handle, uint32_t image_index);
VkrBindlessVkGraphBufferInstance *
vkr_bindless_vk_graph_buffer(VkrBindlessVulkanRenderer *renderer,
                             VkrRgBufferHandle handle);
VkrBindlessVkPublishedTexture *
vkr_bindless_vk_published_texture(VkrBindlessVulkanRenderer *renderer,
                                  VkrTextureHandle handle, uint32_t *out_index);
VkrBindlessVkPublishedTexture *
vkr_bindless_vk_texture_publication(VkrBindlessVulkanRenderer *renderer,
                                    VkrTextureHandle handle);
bool8_t vkr_bindless_vk_asset_unpublish_texture(void *state,
                                                VkrTextureHandle handle);
bool8_t
vkr_bindless_vk_create_acquire_semaphores(VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vk_create_pipelines(VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vk_create_resources(VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vk_flush(const VkrBindlessVulkanRenderer *renderer,
                              const VkrBindlessVkAllocation *allocation,
                              VkDeviceSize offset, VkDeviceSize size);
bool8_t
vkr_bindless_vk_flush_publication_ranges(VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vk_invalidate(const VkrBindlessVulkanRenderer *renderer,
                                   const VkrBindlessVkAllocation *allocation,
                                   VkDeviceSize offset, VkDeviceSize size);
bool8_t
vkr_bindless_vk_pipeline_cache_initialize(VkrBindlessVulkanRenderer *renderer);
bool8_t
vkr_bindless_vk_realize_graph_images(VkrBindlessVulkanRenderer *renderer);
bool8_t
vkr_bindless_vk_realize_graph_buffers(VkrBindlessVulkanRenderer *renderer);
void vkr_bindless_vk_mark_graph_images_submitted(
    VkrBindlessVulkanRenderer *renderer, uint64_t submit_value);
void vkr_bindless_vk_mark_graph_buffers_submitted(
    VkrBindlessVulkanRenderer *renderer, uint64_t submit_value);
bool8_t
vkr_bindless_vk_register_graph_executors(VkrBindlessVulkanRenderer *renderer);
bool8_t
vkr_bindless_vk_validate_graph(const VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vk_asset_unpublish_geometry(void *state,
                                                 VkrGeometryHandle handle);
bool8_t vkr_bindless_vk_asset_unpublish_material(void *state,
                                                 VkrMaterialHandle handle);
bool8_t vkr_bindless_vk_collect_captures(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t completed_value);
bool8_t vkr_bindless_vk_commit_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer, uint64_t retire_value);
bool8_t vkr_bindless_vk_commit_texture_initializations(
    VkrBindlessVulkanRenderer *renderer, uint64_t retire_value);
bool8_t vkr_bindless_vk_create_buffer(VkrBindlessVulkanRenderer *renderer,
                                      VkrBindlessVkMemoryClass memory_class,
                                      VkDeviceSize size,
                                      VkBufferUsageFlags usage,
                                      VkrBindlessVkBuffer *out_buffer);
bool8_t vkr_bindless_vk_create_descriptor_slot_tables(
    VkrBindlessVulkanRenderer *renderer);
bool8_t vkr_bindless_vk_create_image(VkrBindlessVulkanRenderer *renderer,
                                     uint32_t width, uint32_t height,
                                     VkImageUsageFlags usage,
                                     VkrBindlessVkImage *out_image);
bool8_t vkr_bindless_vk_create_image_ex(
    VkrBindlessVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t mip_levels, uint32_t array_layers, VkFormat format,
    VkImageCreateFlags flags, VkImageViewType view_type,
    VkImageUsageFlags usage, VkrBindlessVkImage *out_image);
bool8_t vkr_bindless_vk_create_target_set(VkrBindlessVulkanRenderer *renderer,
                                          uint32_t width, uint32_t height,
                                          uint32_t image_count,
                                          VkrBindlessVkTargetSet *out_targets);
bool8_t vkr_bindless_vk_create_window_target(
    VkrBindlessVulkanRenderer *renderer, uint32_t requested_width,
    uint32_t requested_height, uint32_t requested_image_count,
    VkSwapchainKHR old_swapchain, VkrBindlessVkWindowTarget *out_target);
bool8_t vkr_bindless_vk_format_block_info(VkFormat format, uint32_t *out_width,
                                          uint32_t *out_height,
                                          uint32_t *out_bytes);
bool8_t vkr_bindless_vk_mark_dirty(VkrBindlessVkDirtyRange *dirty,
                                   const VkrBindlessVkBuffer *buffer,
                                   VkDeviceSize offset, VkDeviceSize size);
bool8_t vkr_bindless_vk_plan_capture(VkrBindlessVulkanRenderer *renderer,
                                     const VkrRenderPacket *packet,
                                     VkrBindlessVkFrameSlot *slot);
bool8_t
vkr_bindless_vk_prepare_packet_uploads(VkrBindlessVulkanRenderer *renderer,
                                       VkrBindlessVkFrameSlot *slot,
                                       const VkrRenderPacket *packet);
bool8_t vkr_bindless_vk_publish_sampled_view(
    VkrBindlessVulkanRenderer *renderer, VkImageView view,
    VkImageLayout image_layout, VkrGpuSlotHandle *out_handle);
bool8_t vkr_bindless_vk_publish_sentinel_descriptors(
    VkrBindlessVulkanRenderer *renderer);
bool8_t
vkr_bindless_vk_publish_storage_view(VkrBindlessVulkanRenderer *renderer,
                                     VkImageView view,
                                     VkrGpuSlotHandle *out_handle);
bool8_t vkr_bindless_vk_record_capture(VkrBindlessVulkanRenderer *renderer,
                                       VkCommandBuffer command,
                                       VkrBindlessVkFrameSlot *slot);
bool8_t vkr_bindless_vk_record_graph(VkrBindlessVulkanRenderer *renderer,
                                     VkCommandBuffer command);
bool8_t vkr_bindless_vk_record_ibl_bakes(VkrBindlessVulkanRenderer *renderer,
                                         VkCommandBuffer command);
bool8_t vkr_bindless_vk_record_packet_draws(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkPacketPipeline pipeline, const VkrDrawItem *draws,
    uint32_t draw_count, uint64_t instances, Mat4 view_projection,
    bool8_t alpha_cutout, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t transmission_pass);
bool8_t vkr_bindless_vk_record_packet_fullscreen(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkPacketPipeline pipeline, uint32_t texture_index,
    uint32_t flags);
bool8_t
vkr_bindless_vk_record_packet_skybox(VkrBindlessVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrSkyboxPassPayload *skybox);
bool8_t vkr_bindless_vk_record_text_draws(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkPacketPipeline pipeline, const VkrPreparedTextDraw *draws,
    uint32_t draw_count, Mat4 view_projection, uint32_t target_width,
    uint32_t target_height, bool8_t ui_domain);
bool8_t
vkr_bindless_vk_recreate_window_target(VkrBindlessVulkanRenderer *renderer,
                                       uint32_t width, uint32_t height,
                                       uint32_t image_count);
bool8_t vkr_bindless_vk_retire_allocation(VkrBindlessVulkanRenderer *renderer,
                                          VkrBindlessVkAllocation *allocation,
                                          uint64_t retire_value);
bool8_t vkr_bindless_vk_retire_buffer(VkrBindlessVulkanRenderer *renderer,
                                      VkrBindlessVkBuffer *buffer,
                                      uint64_t retire_value);
bool8_t vkr_bindless_vk_stage_next_publication_batch(
    VkrBindlessVulkanRenderer *renderer);
bool8_t
vkr_bindless_vk_window_presents_complete(VkrBindlessVulkanRenderer *renderer,
                                         VkrBindlessVkWindowTarget *target,
                                         bool8_t wait);
uint64_t vkr_bindless_vk_refresh_completed(VkrBindlessVulkanRenderer *renderer);
uint64_t vkr_bindless_vk_align_up(uint64_t value, uint64_t alignment);
void vkr_bindless_vk_collect_asset_publications(
    VkrBindlessVulkanRenderer *renderer, uint64_t completed);
void vkr_bindless_vk_collect_retired_targets(
    VkrBindlessVulkanRenderer *renderer, uint64_t completed_value);
void vkr_bindless_vk_destroy_buffer(VkrBindlessVulkanRenderer *renderer,
                                    VkrBindlessVkBuffer *buffer);
void vkr_bindless_vk_destroy_frame_slots(VkrBindlessVulkanRenderer *renderer);
void vkr_bindless_vk_destroy_graph_image(VkrBindlessVulkanRenderer *renderer,
                                         VkrBindlessVkGraphImage *slot);
void vkr_bindless_vk_destroy_graph_buffer(VkrBindlessVulkanRenderer *renderer,
                                          VkrBindlessVkGraphBuffer *slot);
void vkr_bindless_vk_destroy_image(VkrBindlessVulkanRenderer *renderer,
                                   VkrBindlessVkImage *image);
void vkr_bindless_vk_destroy_target_set(VkrBindlessVulkanRenderer *renderer,
                                        VkrBindlessVkTargetSet *targets);
void vkr_bindless_vk_destroy_window_target(VkrBindlessVulkanRenderer *renderer,
                                           VkrBindlessVkWindowTarget *target);
void vkr_bindless_vk_pipeline_cache_shutdown(
    VkrBindlessVulkanRenderer *renderer);
void *vkr_bindless_vk_frame_upload_allocate(VkrBindlessVkFrameSlot *slot,
                                            uint64_t size, uint64_t alignment,
                                            uint64_t *out_address,
                                            uint64_t *out_offset);
void vkr_bindless_vk_cmd_image_barrier(
    VkCommandBuffer command_buffer, VkImage image,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
    VkImageLayout old_layout, VkImageLayout new_layout);
void vkr_bindless_vk_collect_retired_window_targets(
    VkrBindlessVulkanRenderer *renderer, uint64_t completed_submit_value);
void vkr_bindless_vk_discard_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer);
void vkr_bindless_vk_discard_texture_initializations(
    VkrBindlessVulkanRenderer *renderer);
void vkr_bindless_vk_record_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command);
void vkr_bindless_vk_record_texture_initializations(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command);

#endif
