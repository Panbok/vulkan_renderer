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

#ifndef VKR_BINDLESS_VK_VERTEX_SPV
#define VKR_BINDLESS_VK_VERTEX_SPV "walking.vert.spv"
#endif
#ifndef VKR_BINDLESS_VK_FRAGMENT_SPV
#define VKR_BINDLESS_VK_FRAGMENT_SPV "walking.frag.spv"
#endif
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
  VKR_BINDLESS_VK_ROOT_OFFSET = 0,
  VKR_BINDLESS_VK_VERTEX_OFFSET = 112,
  VKR_BINDLESS_VK_INDEX_OFFSET = 304,
  VKR_BINDLESS_VK_TEXTURE_OFFSET = 312,
  VKR_BINDLESS_VK_UPLOAD_SIZE = 320,
  VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX = 8,
  VKR_BINDLESS_VK_RETIRED_SWAPCHAIN_MAX = 8,
  VKR_BINDLESS_VK_GRAPH_LAYER_MAX = 16,
  VKR_BINDLESS_VK_TEXTURE_MIP_MAX = 16,
  VKR_BINDLESS_VK_PENDING_IBL_BAKE_MAX = 32,
  VKR_BINDLESS_VK_FRAME_UPLOAD_SIZE = 16u * 1024u * 1024u,
  VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET = 8u,
  VKR_BINDLESS_VK_IBL_TEXEL_BYTES = 8u,
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

typedef struct VkrBindlessVkDrawRoot {
  uint64_t vertices;
  float32_t tint[4];
  float32_t transform[16];
  uint32_t texture_index;
  uint32_t sampler_index;
  uint64_t materials;
} VkrBindlessVkDrawRoot;

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
  uint32_t reserved[2];
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

/** Vulkan packet root. Texture references are descriptor-heap index/sampler
 *
 * pairs while every table and stream remains a device address. */
typedef struct VKR_SIMD_ALIGN VkrBindlessVkPacketDrawRoot {
  uint64_t vertices;
  uint64_t instances;
  Mat4 view_projection;
  uint64_t materials;
  uint32_t irradiance_texture;
  uint32_t irradiance_sampler;
  uint32_t prefilter_texture;
  uint32_t prefilter_sampler;
  uint32_t brdf_texture;
  uint32_t brdf_sampler;
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
} VkrBindlessVkPacketDrawRoot;

_Static_assert(sizeof(VkrVertex3d) == 64u, "Shared vertex ABI drift");
_Static_assert(sizeof(VkrBindlessVkDrawRoot) == 104u, "Draw-root ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, vertices) == 0u,
               "Draw-root vertices ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, tint) == 8u,
               "Draw-root tint ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, transform) == 24u,
               "Draw-root transform ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, texture_index) == 88u,
               "Draw-root texture ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, sampler_index) == 92u,
               "Draw-root sampler ABI drift");
_Static_assert(offsetof(VkrBindlessVkDrawRoot, materials) == 96u,
               "Draw-root materials ABI drift");
_Static_assert(sizeof(VkrBindlessVkMaterialGpuRow) == 64u,
               "Vulkan material row ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, base_color_texture) == 16u,
               "Vulkan material texture ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, base_color_sampler) == 32u,
               "Vulkan material sampler ABI drift");
_Static_assert(offsetof(VkrBindlessVkMaterialGpuRow, material_id) == 48u,
               "Vulkan material identifier ABI drift");
_Static_assert(sizeof(VkrBindlessVkPushConstants) == 16u,
               "Push-constant ABI drift");
_Static_assert(sizeof(VkrBindlessVkIblRoot) == 32u, "IBL-root ABI drift");
_Static_assert(sizeof(VkrBindlessVkPacketDrawRoot) % 16u == 0u,
               "Packet draw-root ABI alignment drift");

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
  VkImageView layer_views[VKR_BINDLESS_VK_GRAPH_LAYER_MAX];
  VkrGpuSlotHandle sampled_slot;
  VkrGpuSlotHandle storage_slot;
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
  uint32_t ibl_capture_mip_count;
  bool8_t ibl_capture_pending;
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
  VkPipelineStageFlags2 destination_stage;
  VkAccessFlags2 destination_access;
  uint32_t geometry_record_index;
} VkrBindlessVkPendingBufferInitialization;

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
  VkShaderModule vertex_shader;
  VkShaderModule fragment_shader;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
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
  uint64_t command_slot_wait_count;
  uint64_t wsi_reacquire_proofs;
  uint64_t wsi_retired_swapchains;
  uint64_t wsi_retired_swapchains_collected;
  uint32_t active_frame_slot;
  uint32_t next_image_index;
  uint32_t active_material_index;
  uint32_t active_geometry_record_index;
  bool8_t active_geometry;
  bool8_t frame_active;
  bool8_t sentinel_uploaded;
  bool8_t shader_abi_validated;
  bool8_t target_dirty;
  bool8_t terminal_failure;
};

static bool8_t
vkr_bindless_vk_create_packet_pipelines(VkrBindlessVulkanRenderer *renderer);
static bool8_t
vkr_bindless_vk_create_ibl_pipelines(VkrBindlessVulkanRenderer *renderer);
static uint64_t
vkr_bindless_vk_refresh_completed(VkrBindlessVulkanRenderer *renderer);
static bool8_t vkr_bindless_vk_publish_sampled_view(
    VkrBindlessVulkanRenderer *renderer, VkImageView view,
    VkImageLayout image_layout, VkrGpuSlotHandle *out_handle);
static bool8_t
vkr_bindless_vk_publish_storage_view(VkrBindlessVulkanRenderer *renderer,
                                     VkImageView view,
                                     VkrGpuSlotHandle *out_handle);
static VkrBindlessVkPublishedTexture *
vkr_bindless_vk_published_texture(VkrBindlessVulkanRenderer *renderer,
                                  VkrTextureHandle handle, uint32_t *out_index);
static VkrBindlessVkPublishedTexture *
vkr_bindless_vk_texture_publication(VkrBindlessVulkanRenderer *renderer,
                                    VkrTextureHandle handle);

static bool8_t
vkr_bindless_vk_pipeline_cache_initialize(VkrBindlessVulkanRenderer *renderer) {
  const char *path = getenv("VKR_PIPELINE_CACHE_PATH");
  const size_t path_length = path ? strlen(path) : 0u;
  if (path_length >= sizeof(renderer->pipeline_cache_path)) {
    log_error("Bindless Vulkan pipeline cache path exceeds %zu bytes",
              sizeof(renderer->pipeline_cache_path) - 1u);
    return false_v;
  }
  if (path_length) {
    MemCopy(renderer->pipeline_cache_path, path, path_length + 1u);
    log_info("Pipeline cache path: %s", renderer->pipeline_cache_path);
  }
  void *initial_data = NULL;
  size_t initial_size = 0u;
  if (path_length) {
    FILE *file = fopen(renderer->pipeline_cache_path, "rb");
    if (file) {
      if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        if (end > 0 && (uint64_t)end <= MB(64) &&
            fseek(file, 0, SEEK_SET) == 0) {
          initial_size = (size_t)end;
          initial_data = vkr_allocator_alloc(renderer->allocator, initial_size,
                                             VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
          if (!initial_data ||
              fread(initial_data, 1u, initial_size, file) != initial_size) {
            if (initial_data) {
              vkr_allocator_free(renderer->allocator, initial_data,
                                 initial_size,
                                 VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
            }
            initial_data = NULL;
            initial_size = 0u;
          }
        }
      }
      fclose(file);
    }
  }
  VkPipelineCacheCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
      .initialDataSize = initial_size,
      .pInitialData = initial_data,
  };
  VkResult result =
      vkCreatePipelineCache(vkr_bindless_vulkan_device_handle(renderer->device),
                            &info, NULL, &renderer->pipeline_cache);
  if (initial_data) {
    vkr_allocator_free(renderer->allocator, initial_data, initial_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (result != VK_SUCCESS && initial_size) {
    info.initialDataSize = 0u;
    info.pInitialData = NULL;
    result = vkCreatePipelineCache(
        vkr_bindless_vulkan_device_handle(renderer->device), &info, NULL,
        &renderer->pipeline_cache);
  }
  if (result != VK_SUCCESS)
    return false_v;
  if (initial_size)
    log_info("Loaded pipeline cache data: %zu bytes", initial_size);
  log_info("Initialized Vulkan pipeline cache with %s data",
           initial_size ? "persisted" : "empty");
  return true_v;
}

static void
vkr_bindless_vk_pipeline_cache_shutdown(VkrBindlessVulkanRenderer *renderer) {
  if (!renderer->pipeline_cache)
    return;
  VkDevice device = vkr_bindless_vulkan_device_handle(renderer->device);
  if (renderer->pipeline_cache_path[0]) {
    size_t size = 0u;
    if (vkGetPipelineCacheData(device, renderer->pipeline_cache, &size, NULL) ==
            VK_SUCCESS &&
        size > 0u) {
      const size_t allocation_size = size;
      void *data = vkr_allocator_alloc(renderer->allocator, allocation_size,
                                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (data && vkGetPipelineCacheData(device, renderer->pipeline_cache,
                                         &size, data) == VK_SUCCESS) {
        char temporary[sizeof(renderer->pipeline_cache_path) + 5u];
        const int written = snprintf(temporary, sizeof(temporary), "%s.tmp",
                                     renderer->pipeline_cache_path);
        FILE *file = written > 0 && (size_t)written < sizeof(temporary)
                         ? fopen(temporary, "wb")
                         : NULL;
        const bool8_t wrote = file && fwrite(data, 1u, size, file) == size;
        const bool8_t closed = file && fclose(file) == 0;
        if (wrote && closed) {
          const FilePath temporary_path = {
              .path = string8_create_from_cstr((const uint8_t *)temporary,
                                               string_length(temporary)),
              .type = FILE_PATH_TYPE_ABSOLUTE,
          };
          const FilePath cache_path = {
              .path = string8_create_from_cstr(
                  (const uint8_t *)renderer->pipeline_cache_path,
                  string_length(renderer->pipeline_cache_path)),
              .type = FILE_PATH_TYPE_ABSOLUTE,
          };
          if (file_rename(&temporary_path, &cache_path, true_v) ==
              FILE_ERROR_NONE) {
            log_info("Saved pipeline cache data: %zu bytes -> %s", size,
                     renderer->pipeline_cache_path);
          } else {
            (void)file_remove(&temporary_path);
          }
        }
      }
      if (data) {
        vkr_allocator_free(renderer->allocator, data, allocation_size,
                           VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      }
    }
  }
  vkDestroyPipelineCache(device, renderer->pipeline_cache, NULL);
  renderer->pipeline_cache = VK_NULL_HANDLE;
}

static void vkr_bindless_vk_graph_noop(VkrRgPassContext *ctx, void *user_data) {
  (void)ctx;
  (void)user_data;
}

typedef enum VkrBindlessVkGraphExecutorKind {
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_SHADOW = 0,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING_READBACK,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_IBL_BAKE,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_SKYBOX,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_OPAQUE,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_TRANSMISSION,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_BLEND,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_TONEMAP,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_EDITOR,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_UI,
  VKR_BINDLESS_VK_GRAPH_EXECUTOR_COUNT,
} VkrBindlessVkGraphExecutorKind;

typedef struct VkrBindlessVkGraphExecutorSpec {
  const char *name;
  VkrRgPassType type;
} VkrBindlessVkGraphExecutorSpec;

static const VkrBindlessVkGraphExecutorSpec s_bindless_vk_graph_executors[] = {
    {"pass.shadow.cascade", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.picking", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.picking.readback", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.ibl_bake", VKR_RG_PASS_TYPE_COMPUTE},
    {"pass.skybox", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.world.opaque", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.copy.pre_transmission.fullscreen", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.copy.pre_transmission.editor", VKR_RG_PASS_TYPE_TRANSFER},
    {"pass.world.transmission", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.world.blend", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.tonemap", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.editor", VKR_RG_PASS_TYPE_GRAPHICS},
    {"pass.ui", VKR_RG_PASS_TYPE_GRAPHICS},
};
_Static_assert(ArrayCount(s_bindless_vk_graph_executors) ==
                   VKR_BINDLESS_VK_GRAPH_EXECUTOR_COUNT,
               "Bindless Vulkan graph executor table is incomplete");

static bool8_t
vkr_bindless_vk_graph_executor_kind(const VkrRgPass *pass,
                                    VkrBindlessVkGraphExecutorKind *out_kind) {
  const uintptr_t encoded = (uintptr_t)pass->desc.user_data;
  if (!encoded || encoded > VKR_BINDLESS_VK_GRAPH_EXECUTOR_COUNT)
    return false_v;
  *out_kind = (VkrBindlessVkGraphExecutorKind)(encoded - 1u);
  return true_v;
}

static bool8_t
vkr_bindless_vk_register_graph_executors(VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < ArrayCount(s_bindless_vk_graph_executors); ++i) {
    const VkrBindlessVkGraphExecutorSpec *spec =
        &s_bindless_vk_graph_executors[i];
    const VkrRgPassExecutor executor = {
        .name = string8_create_from_cstr((const uint8_t *)spec->name,
                                         string_length(spec->name)),
        .execute = vkr_bindless_vk_graph_noop,
        .user_data = (void *)(uintptr_t)(i + 1u),
    };
    if (!vkr_rg_executor_registry_register(&renderer->executors, &executor))
      return false_v;
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_validate_graph(const VkrBindlessVulkanRenderer *renderer) {
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    VkrBindlessVkGraphExecutorKind kind;
    if (!vkr_bindless_vk_graph_executor_kind(pass, &kind)) {
      log_error("Bindless Vulkan graph pass '%.*s' has no executor kind",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    const VkrBindlessVkGraphExecutorSpec *executor =
        &s_bindless_vk_graph_executors[kind];
    if (pass->desc.type != executor->type) {
      log_error("Bindless Vulkan graph pass '%.*s' has type %u; executor '%s' "
                "requires type %u",
                (int)pass->desc.name.length, pass->desc.name.str,
                (uint32_t)pass->desc.type, executor->name,
                (uint32_t)executor->type);
      return false_v;
    }
    for (uint64_t i = 0; i < pass->pre_image_barriers.length; ++i) {
      const VkrRgImageBarrier *barrier =
          vector_get_VkrRgImageBarrier(&pass->pre_image_barriers, i);
      VkrBindlessVkDependency lowered = {0};
      const VkrBindlessVkDependencyResult result =
          vkr_bindless_vk_lower_image_dependency(
              barrier->src_access, barrier->dst_access, &barrier->dependency,
              barrier->src_layout != barrier->dst_layout, &lowered);
      if (result != VKR_BINDLESS_VK_DEPENDENCY_OK) {
        log_error("Bindless Vulkan graph pass '%.*s' image dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_bindless_vk_dependency_result_string(result));
        return false_v;
      }
    }
    for (uint64_t i = 0; i < pass->pre_buffer_barriers.length; ++i) {
      const VkrRgBufferBarrier *barrier =
          vector_get_VkrRgBufferBarrier(&pass->pre_buffer_barriers, i);
      VkrBindlessVkDependency lowered = {0};
      const VkrBindlessVkDependencyResult result =
          vkr_bindless_vk_lower_buffer_dependency(
              barrier->src_access, barrier->dst_access, &barrier->dependency,
              &lowered);
      if (result != VKR_BINDLESS_VK_DEPENDENCY_OK) {
        log_error("Bindless Vulkan graph pass '%.*s' buffer dependency could "
                  "not be lowered: %s",
                  (int)pass->desc.name.length, pass->desc.name.str,
                  vkr_bindless_vk_dependency_result_string(result));
        return false_v;
      }
    }
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_recreate_window_target(VkrBindlessVulkanRenderer *renderer,
                                       uint32_t width, uint32_t height,
                                       uint32_t image_count);
static void
vkr_bindless_vk_collect_asset_publications(VkrBindlessVulkanRenderer *renderer,
                                           uint64_t completed);

static VkDevice
vkr_bindless_vk_renderer_device(const VkrBindlessVulkanRenderer *renderer) {
  return vkr_bindless_vulkan_device_handle(renderer->device);
}

static bool8_t vkr_bindless_vk_choose_memory_type(
    const VkrBindlessVulkanRenderer *renderer, uint32_t memory_type_bits,
    VkrBindlessVkMemoryClass memory_class, uint32_t *out_index,
    VkMemoryPropertyFlags *out_properties) {
  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_bindless_vulkan_device_memory_properties(renderer->device);
  int32_t best_rank = INT32_MAX;
  uint32_t best_index = UINT32_MAX;
  for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags available =
        memory->memoryTypes[i].propertyFlags;
    if (!(memory_type_bits & (1u << i)))
      continue;
    const int32_t rank =
        vkr_bindless_vulkan_memory_type_rank(memory_class, available);
    if (rank >= 0 && rank < best_rank) {
      best_rank = rank;
      best_index = i;
    }
  }
  if (best_index == UINT32_MAX)
    return false_v;
  *out_index = best_index;
  *out_properties = memory->memoryTypes[best_index].propertyFlags;
  if (memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE && best_rank == 2)
    log_warn("Bindless Vulkan DEVICE placement degraded to memory type %u",
             best_index);
  return true_v;
}

static VkFormat vkr_bindless_vk_texture_format(VkrTextureFormat format) {
  switch (format) {
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_UNORM:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case VKR_TEXTURE_FORMAT_B8G8R8A8_SRGB:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_UINT:
    return VK_FORMAT_R8G8B8A8_UINT;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SNORM:
    return VK_FORMAT_R8G8B8A8_SNORM;
  case VKR_TEXTURE_FORMAT_R8G8B8A8_SINT:
    return VK_FORMAT_R8G8B8A8_SINT;
  case VKR_TEXTURE_FORMAT_BC7_UNORM:
    return VK_FORMAT_BC7_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_BC7_SRGB:
    return VK_FORMAT_BC7_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_BC5_UNORM:
    return VK_FORMAT_BC5_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_UNORM:
    return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ETC2_R8G8B8A8_SRGB:
    return VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_UNORM:
    return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_ASTC_4x4_SRGB:
    return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
  case VKR_TEXTURE_FORMAT_EAC_R11G11_UNORM:
    return VK_FORMAT_EAC_R11G11_UNORM_BLOCK;
  case VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  case VKR_TEXTURE_FORMAT_R8_UNORM:
    return VK_FORMAT_R8_UNORM;
  case VKR_TEXTURE_FORMAT_R16_SFLOAT:
    return VK_FORMAT_R16_SFLOAT;
  case VKR_TEXTURE_FORMAT_R32_SFLOAT:
    return VK_FORMAT_R32_SFLOAT;
  case VKR_TEXTURE_FORMAT_R32_UINT:
    return VK_FORMAT_R32_UINT;
  case VKR_TEXTURE_FORMAT_R8G8_UNORM:
    return VK_FORMAT_R8G8_UNORM;
  case VKR_TEXTURE_FORMAT_D16_UNORM:
    return VK_FORMAT_D16_UNORM;
  case VKR_TEXTURE_FORMAT_D32_SFLOAT:
    return VK_FORMAT_D32_SFLOAT;
  case VKR_TEXTURE_FORMAT_D24_UNORM_S8_UINT:
    return VK_FORMAT_D24_UNORM_S8_UINT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

static bool8_t vkr_bindless_vk_format_block_info(VkFormat format,
                                                 uint32_t *out_width,
                                                 uint32_t *out_height,
                                                 uint32_t *out_bytes) {
  uint32_t width = 1u;
  uint32_t height = 1u;
  uint32_t bytes = 0u;
  switch (format) {
  case VK_FORMAT_R8_UNORM:
    bytes = 1u;
    break;
  case VK_FORMAT_R16_SFLOAT:
  case VK_FORMAT_R8G8_UNORM:
    bytes = 2u;
    break;
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_R8G8B8A8_UINT:
  case VK_FORMAT_R8G8B8A8_SNORM:
  case VK_FORMAT_R8G8B8A8_SINT:
  case VK_FORMAT_R32_SFLOAT:
  case VK_FORMAT_R32_UINT:
    bytes = 4u;
    break;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    bytes = 8u;
    break;
  case VK_FORMAT_BC7_UNORM_BLOCK:
  case VK_FORMAT_BC7_SRGB_BLOCK:
  case VK_FORMAT_BC5_UNORM_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
  case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
  case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
  case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
  case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    width = 4u;
    height = 4u;
    bytes = 16u;
    break;
  default:
    return false_v;
  }
  *out_width = width;
  *out_height = height;
  *out_bytes = bytes;
  return true_v;
}

static VkImageAspectFlags vkr_bindless_vk_format_aspects(VkFormat format) {
  switch (format) {
  case VK_FORMAT_D16_UNORM:
  case VK_FORMAT_D32_SFLOAT:
    return VK_IMAGE_ASPECT_DEPTH_BIT;
  case VK_FORMAT_D24_UNORM_S8_UINT:
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  default:
    return VK_IMAGE_ASPECT_COLOR_BIT;
  }
}

static VkImageLayout vkr_bindless_vk_texture_layout(VkrTextureLayout layout) {
  switch (layout) {
  case VKR_TEXTURE_LAYOUT_UNDEFINED:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  case VKR_TEXTURE_LAYOUT_GENERAL:
    return VK_IMAGE_LAYOUT_GENERAL;
  case VKR_TEXTURE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_TRANSFER_DST_OPTIMAL:
    return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  case VKR_TEXTURE_LAYOUT_PRESENT_SRC_KHR:
    return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  default:
    return VK_IMAGE_LAYOUT_UNDEFINED;
  }
}

static bool8_t vkr_bindless_vk_flush(const VkrBindlessVulkanRenderer *renderer,
                                     const VkrBindlessVkAllocation *allocation,
                                     VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true_v;
  }
  const VkDeviceSize atom =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.nonCoherentAtomSize;
  VkrBindlessVkMappedRange aligned = {0};
  if (offset > UINT64_MAX - allocation->offset ||
      !vkr_bindless_vulkan_noncoherent_range(allocation->offset + offset, size,
                                             allocation->memory_size, atom,
                                             &aligned))
    return false_v;
  const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = aligned.offset,
      .size = aligned.size,
  };
  return vkFlushMappedMemoryRanges(vkr_bindless_vk_renderer_device(renderer),
                                   1u, &range) == VK_SUCCESS;
}

static bool8_t vkr_bindless_vk_mark_dirty(VkrBindlessVkDirtyRange *dirty,
                                          const VkrBindlessVkBuffer *buffer,
                                          VkDeviceSize offset,
                                          VkDeviceSize size) {
  if (!dirty || !buffer || !size || offset > buffer->size ||
      size > buffer->size - offset)
    return false_v;
  const VkDeviceSize end = offset + size;
  if (!dirty->dirty) {
    *dirty = (VkrBindlessVkDirtyRange){
        .offset = offset,
        .end = end,
        .dirty = true_v,
    };
  } else {
    dirty->offset = Min(dirty->offset, offset);
    dirty->end = Max(dirty->end, end);
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_flush_publication_ranges(VkrBindlessVulkanRenderer *renderer) {
  VkrBindlessVkDirtyRange *ranges[] = {
      &renderer->resource_descriptor_dirty,
      &renderer->sampler_descriptor_dirty,
      &renderer->material_dirty,
  };
  VkrBindlessVkBuffer *buffers[] = {
      &renderer->resource_descriptors,
      &renderer->sampler_descriptors,
      &renderer->materials,
  };
  for (uint32_t i = 0; i < ArrayCount(ranges); ++i) {
    VkrBindlessVkDirtyRange *range = ranges[i];
    if (range->dirty &&
        !vkr_bindless_vk_flush(renderer, &buffers[i]->allocation, range->offset,
                               range->end - range->offset))
      return false_v;
  }
  for (uint32_t i = 0; i < ArrayCount(ranges); ++i)
    MemZero(ranges[i], sizeof(*ranges[i]));
  return true_v;
}

static bool8_t
vkr_bindless_vk_invalidate(const VkrBindlessVulkanRenderer *renderer,
                           const VkrBindlessVkAllocation *allocation,
                           VkDeviceSize offset, VkDeviceSize size) {
  if (allocation->properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    return true_v;
  }
  const VkDeviceSize atom =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.nonCoherentAtomSize;
  VkrBindlessVkMappedRange aligned = {0};
  if (offset > UINT64_MAX - allocation->offset ||
      !vkr_bindless_vulkan_noncoherent_range(allocation->offset + offset, size,
                                             allocation->memory_size, atom,
                                             &aligned))
    return false_v;
  const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = allocation->memory,
      .offset = aligned.offset,
      .size = aligned.size,
  };
  return vkInvalidateMappedMemoryRanges(
             vkr_bindless_vk_renderer_device(renderer), 1u, &range) ==
         VK_SUCCESS;
}

static bool8_t
vkr_bindless_vk_release_allocation(VkrBindlessVulkanRenderer *renderer,
                                   VkrBindlessVkAllocation *allocation) {
  if (allocation->pooled)
    return vkr_bindless_vulkan_memory_pool_release(
        renderer->memory_pool, &allocation->pooled_allocation,
        renderer->completed_value, renderer->completed_value);
  if (!allocation->memory)
    return true_v;

  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (allocation->mapped)
    vkUnmapMemory(device, allocation->memory);
  vkFreeMemory(device, allocation->memory, NULL);
  if (allocation->dedicated)
    vkr_bindless_vulkan_memory_pool_record_dedicated_release(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size,
        allocation->retired);
  return true_v;
}

static void vkr_bindless_vk_destroy_buffer(VkrBindlessVulkanRenderer *renderer,
                                           VkrBindlessVkBuffer *buffer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (buffer->handle)
    vkDestroyBuffer(device, buffer->handle, NULL);
  if (!vkr_bindless_vk_release_allocation(renderer, &buffer->allocation))
    log_error("Bindless Vulkan failed to release a proven buffer placement");
  MemZero(buffer, sizeof(*buffer));
}

static bool8_t
vkr_bindless_vk_retire_allocation(VkrBindlessVulkanRenderer *renderer,
                                  VkrBindlessVkAllocation *allocation,
                                  uint64_t retire_value) {
  if (!allocation || allocation->retired)
    return false_v;
  if (allocation->pooled) {
    if (!vkr_bindless_vulkan_memory_pool_retire(renderer->memory_pool,
                                                &allocation->pooled_allocation,
                                                retire_value))
      return false_v;
  } else if (allocation->dedicated) {
    if (!vkr_bindless_vulkan_memory_pool_record_dedicated_retire(
            renderer->memory_pool, allocation->pool_key,
            allocation->memory_size))
      return false_v;
  } else {
    return false_v;
  }
  allocation->retired = true_v;
  return true_v;
}

static bool8_t
vkr_bindless_vk_retire_buffer(VkrBindlessVulkanRenderer *renderer,
                              VkrBindlessVkBuffer *buffer,
                              uint64_t retire_value) {
  return buffer && buffer->handle &&
         vkr_bindless_vk_retire_allocation(renderer, &buffer->allocation,
                                           retire_value);
}

static bool8_t
vkr_bindless_vk_create_buffer(VkrBindlessVulkanRenderer *renderer,
                              VkrBindlessVkMemoryClass memory_class,
                              VkDeviceSize size, VkBufferUsageFlags usage,
                              VkrBindlessVkBuffer *out_buffer) {
  MemZero(out_buffer, sizeof(*out_buffer));
  out_buffer->size = size;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
  };
  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  const VkDeviceBufferMemoryRequirements device_requirements = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,
      .pCreateInfo = &buffer_info,
  };
  vkGetDeviceBufferMemoryRequirements(device, &device_requirements,
                                      &requirements);
  VkrBindlessVkAllocation *allocation = &out_buffer->allocation;
  if (!vkr_bindless_vk_choose_memory_type(
          renderer, requirements.memoryRequirements.memoryTypeBits,
          memory_class, &allocation->memory_type_index,
          &allocation->properties)) {
    return false_v;
  }
  const bool8_t has_address =
      (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
  allocation->pool_key = (VkrBindlessVkMemoryPoolKey){
      .memory_class = memory_class,
      .kind = VKR_BINDLESS_VK_MEMORY_KIND_BUFFER,
      .memory_type_index = allocation->memory_type_index,
      .device_address_required = has_address,
  };
  const uint64_t pool_block_size =
      memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD
          ? renderer->config.upload_buffer_block_size
      : memory_class == VKR_BINDLESS_VK_MEMORY_CLASS_READBACK
          ? renderer->config.readback_buffer_block_size
          : renderer->config.device_buffer_block_size;
  const bool8_t dedicated =
      dedicated_requirements.requiresDedicatedAllocation ||
      dedicated_requirements.prefersDedicatedAllocation ||
      requirements.memoryRequirements.size > pool_block_size;
  if (!dedicated &&
      !vkr_bindless_vulkan_memory_pool_allocate(
          renderer->memory_pool, allocation->pool_key, allocation->properties,
          requirements.memoryRequirements.size,
          requirements.memoryRequirements.alignment,
          &allocation->pooled_allocation))
    return false_v;
  if (vkCreateBuffer(device, &buffer_info, NULL, &out_buffer->handle) !=
      VK_SUCCESS) {
    if (allocation->pooled_allocation.valid)
      (void)vkr_bindless_vulkan_memory_pool_release(
          renderer->memory_pool, &allocation->pooled_allocation,
          renderer->completed_value, renderer->completed_value);
    return false_v;
  }
  if (dedicated) {
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = out_buffer->handle,
    };
    VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = has_address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u,
        .pNext = &dedicated_info,
    };
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext =
            has_address ? (const void *)&flags : (const void *)&dedicated_info,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = allocation->memory_type_index,
    };
    allocation->memory_size = requirements.memoryRequirements.size;
    allocation->dedicated = true_v;
    const VkResult allocate_result =
        vkAllocateMemory(device, &allocate_info, NULL, &allocation->memory);
    if (allocate_result != VK_SUCCESS) {
      log_error("Bindless Vulkan dedicated buffer allocation failed "
                "(size=%llu, type=%u, class=%u, result=%d)",
                (unsigned long long)allocation->memory_size,
                allocation->memory_type_index, (uint32_t)memory_class,
                (int)allocate_result);
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
    vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size);
    VkResult map_result = VK_SUCCESS;
    if ((allocation->properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        memory_class != VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE)
      map_result =
          vkMapMemory(device, allocation->memory, 0u, allocation->memory_size,
                      0u, &allocation->mapped);
    if (map_result != VK_SUCCESS) {
      log_error("Bindless Vulkan dedicated buffer map failed "
                "(size=%llu, type=%u, class=%u, result=%d)",
                (unsigned long long)allocation->memory_size,
                allocation->memory_type_index, (uint32_t)memory_class,
                (int)map_result);
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
  } else {
    allocation->pooled = true_v;
    allocation->memory = allocation->pooled_allocation.memory;
    allocation->memory_size = allocation->pooled_allocation.memory_size;
    allocation->offset = allocation->pooled_allocation.offset;
    allocation->mapped = allocation->pooled_allocation.mapped;
  }
  const VkResult bind_result = vkBindBufferMemory(
      device, out_buffer->handle, allocation->memory, allocation->offset);
  if (bind_result != VK_SUCCESS) {
    log_error("Bindless Vulkan buffer bind failed "
              "(size=%llu, type=%u, class=%u, offset=%llu, result=%d)",
              (unsigned long long)allocation->memory_size,
              allocation->memory_type_index, (uint32_t)memory_class,
              (unsigned long long)allocation->offset, (int)bind_result);
    vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
    return false_v;
  }
  if (has_address) {
    const VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = out_buffer->handle,
    };
    out_buffer->address = vkGetBufferDeviceAddress(device, &address_info);
    if (!out_buffer->address) {
      vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
      return false_v;
    }
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_create_upload_buffers(VkrBindlessVulkanRenderer *renderer) {
  if (!vkr_gpu_abi_validate_host()) {
    log_error("Bindless Vulkan shared host ABI validation failed");
    return false_v;
  }
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  return vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             resource_layout->size,
             VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             &renderer->resource_descriptors) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             sampler_layout->size,
             VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
             &renderer->sampler_descriptors) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             VKR_BINDLESS_VK_UPLOAD_SIZE,
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
             &renderer->upload) &&
         vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
             (VkDeviceSize)renderer->config.material_slot_capacity *
                 sizeof(VkrBindlessVkMaterialGpuRow),
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, &renderer->materials);
}

static void vkr_bindless_vk_destroy_image(VkrBindlessVulkanRenderer *renderer,
                                          VkrBindlessVkImage *image) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (image->view)
    vkDestroyImageView(device, image->view, NULL);
  if (image->handle)
    vkDestroyImage(device, image->handle, NULL);
  if (!vkr_bindless_vk_release_allocation(renderer, &image->allocation))
    log_error("Bindless Vulkan failed to release a proven image placement");
  MemZero(image, sizeof(*image));
}

static bool8_t vkr_bindless_vk_create_image_ex(
    VkrBindlessVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t mip_levels, uint32_t array_layers, VkFormat format,
    VkImageCreateFlags flags, VkImageViewType view_type,
    VkImageUsageFlags usage, VkrBindlessVkImage *out_image) {
  if (!width || !height || !mip_levels || !array_layers ||
      format == VK_FORMAT_UNDEFINED)
    return false_v;
  MemZero(out_image, sizeof(*out_image));
  out_image->width = width;
  out_image->height = height;
  out_image->mip_levels = mip_levels;
  out_image->array_layers = array_layers;
  out_image->format = format;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = flags,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {.width = width, .height = height, .depth = 1u},
      .mipLevels = mip_levels,
      .arrayLayers = array_layers,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  VkMemoryDedicatedRequirements dedicated_requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
  };
  VkMemoryRequirements2 requirements = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
      .pNext = &dedicated_requirements,
  };
  const VkDeviceImageMemoryRequirements device_requirements = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,
      .pCreateInfo = &image_info,
  };
  vkGetDeviceImageMemoryRequirements(device, &device_requirements,
                                     &requirements);
  VkrBindlessVkAllocation *allocation = &out_image->allocation;
  if (!vkr_bindless_vk_choose_memory_type(
          renderer, requirements.memoryRequirements.memoryTypeBits,
          VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE, &allocation->memory_type_index,
          &allocation->properties)) {
    return false_v;
  }
  allocation->pool_key = (VkrBindlessVkMemoryPoolKey){
      .memory_class = VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE,
      .kind = VKR_BINDLESS_VK_MEMORY_KIND_IMAGE,
      .memory_type_index = allocation->memory_type_index,
  };
  const bool8_t dedicated =
      dedicated_requirements.requiresDedicatedAllocation ||
      dedicated_requirements.prefersDedicatedAllocation ||
      requirements.memoryRequirements.size >
          renderer->config.device_image_block_size;
  if (!dedicated &&
      !vkr_bindless_vulkan_memory_pool_allocate(
          renderer->memory_pool, allocation->pool_key, allocation->properties,
          requirements.memoryRequirements.size,
          requirements.memoryRequirements.alignment,
          &allocation->pooled_allocation)) {
    log_error("Bindless Vulkan image pool allocation failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, bytes=%llu, type=%u)",
              width, height, mip_levels, array_layers, format,
              (unsigned long long)requirements.memoryRequirements.size,
              allocation->memory_type_index);
    return false_v;
  }
  const VkResult create_result =
      vkCreateImage(device, &image_info, NULL, &out_image->handle);
  if (create_result != VK_SUCCESS) {
    log_error("Bindless Vulkan native image creation failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, result=%d)",
              width, height, mip_levels, array_layers, format,
              (int)create_result);
    if (allocation->pooled_allocation.valid)
      (void)vkr_bindless_vulkan_memory_pool_release(
          renderer->memory_pool, &allocation->pooled_allocation,
          renderer->completed_value, renderer->completed_value);
    return false_v;
  }
  if (dedicated) {
    const VkMemoryDedicatedAllocateInfo dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = out_image->handle,
    };
    const VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated_info,
        .allocationSize = requirements.memoryRequirements.size,
        .memoryTypeIndex = allocation->memory_type_index,
    };
    allocation->memory_size = requirements.memoryRequirements.size;
    allocation->dedicated = true_v;
    const VkResult allocate_result =
        vkAllocateMemory(device, &allocate_info, NULL, &allocation->memory);
    if (allocate_result != VK_SUCCESS) {
      log_error("Bindless Vulkan dedicated image allocation failed "
                "(%ux%u, mips=%u, layers=%u, format=%u, bytes=%llu, type=%u, "
                "result=%d)",
                width, height, mip_levels, array_layers, format,
                (unsigned long long)allocation->memory_size,
                allocation->memory_type_index, (int)allocate_result);
      vkr_bindless_vulkan_memory_pool_record_native_failure(
          renderer->memory_pool);
      vkr_bindless_vk_destroy_image(renderer, out_image);
      return false_v;
    }
    vkr_bindless_vulkan_memory_pool_record_dedicated_allocate(
        renderer->memory_pool, allocation->pool_key, allocation->memory_size);
  } else {
    allocation->pooled = true_v;
    allocation->memory = allocation->pooled_allocation.memory;
    allocation->memory_size = allocation->pooled_allocation.memory_size;
    allocation->offset = allocation->pooled_allocation.offset;
  }
  const VkResult bind_result = vkBindImageMemory(
      device, out_image->handle, allocation->memory, allocation->offset);
  if (bind_result != VK_SUCCESS) {
    log_error("Bindless Vulkan image bind failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, offset=%llu, result=%d)",
              width, height, mip_levels, array_layers, format,
              (unsigned long long)allocation->offset, (int)bind_result);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = out_image->handle,
      .viewType = view_type,
      .format = format,
      .subresourceRange = {.aspectMask = vkr_bindless_vk_format_aspects(format),
                           .levelCount = mip_levels,
                           .layerCount = array_layers},
  };
  const VkResult view_result =
      vkCreateImageView(device, &view_info, NULL, &out_image->view);
  if (view_result != VK_SUCCESS) {
    log_error("Bindless Vulkan image view creation failed "
              "(%ux%u, mips=%u, layers=%u, format=%u, view=%u, result=%d)",
              width, height, mip_levels, array_layers, format, view_type,
              (int)view_result);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_bindless_vk_create_image(VkrBindlessVulkanRenderer *renderer,
                                            uint32_t width, uint32_t height,
                                            VkImageUsageFlags usage,
                                            VkrBindlessVkImage *out_image) {
  return vkr_bindless_vk_create_image_ex(
      renderer, width, height, 1u, 1u, VK_FORMAT_R8G8B8A8_UNORM, 0u,
      VK_IMAGE_VIEW_TYPE_2D, usage, out_image);
}

static VkImageUsageFlags
vkr_bindless_vk_graph_image_usage(VkrTextureUsageFlags usage) {
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

static bool8_t vkr_bindless_vk_graph_image_desc_equal(const VkrRgImageDesc *a,
                                                      const VkrRgImageDesc *b) {
  return a->width == b->width && a->height == b->height &&
         a->format == b->format && a->usage.set == b->usage.set &&
         a->samples == b->samples && a->layers == b->layers &&
         a->mip_levels == b->mip_levels && a->type == b->type &&
         a->flags == b->flags;
}

static void vkr_bindless_vk_destroy_graph_image_instance(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkGraphImageInstance *instance) {
  const VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
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
  for (uint32_t layer = 0; layer < VKR_BINDLESS_VK_GRAPH_LAYER_MAX; ++layer) {
    if (instance->layer_views[layer])
      vkDestroyImageView(device, instance->layer_views[layer], NULL);
  }
  vkr_bindless_vk_destroy_image(renderer, &instance->image);
  MemZero(instance, sizeof(*instance));
}

static void
vkr_bindless_vk_destroy_graph_image(VkrBindlessVulkanRenderer *renderer,
                                    VkrBindlessVkGraphImage *slot) {
  if (!slot || slot->external_swapchain) {
    if (slot)
      MemZero(slot, sizeof(*slot));
    return;
  }
  for (uint32_t i = 0; i < slot->instance_count; ++i)
    vkr_bindless_vk_destroy_graph_image_instance(renderer, &slot->instances[i]);
  MemZero(slot, sizeof(*slot));
}

static bool8_t vkr_bindless_vk_create_graph_image_instance(
    VkrBindlessVulkanRenderer *renderer, const VkrRgImageDesc *desc,
    VkrBindlessVkGraphImageInstance *out_instance) {
  const VkFormat format = vkr_bindless_vk_texture_format(desc->format);
  VkImageUsageFlags usage = vkr_bindless_vk_graph_image_usage(desc->usage);
  /* A capture request arrives with a packet, after graph images have already

   * been realized. Capture-enabled renderers therefore provision graph-owned

   * images as legal transfer sources up front; no per-capture image churn is

   * allowed in the frame path. */
  if (renderer->config.capture_ring_capacity > 0u)
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (format == VK_FORMAT_UNDEFINED || usage == 0u || desc->samples != 1u ||
      desc->layers == 0u || desc->layers > VKR_BINDLESS_VK_GRAPH_LAYER_MAX ||
      desc->mip_levels == 0u)
    return false_v;
  const bool8_t array_view =
      desc->layers > 1u || (desc->flags & VKR_RG_RESOURCE_FLAG_FORCE_ARRAY);
  if (!vkr_bindless_vk_create_image_ex(
          renderer, desc->width, desc->height, desc->mip_levels, desc->layers,
          format, 0u,
          array_view ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
          usage, &out_instance->image))
    return false_v;
  if (desc->layers == 1u)
    goto publish_descriptors;
  const VkImageAspectFlags aspects = vkr_bindless_vk_format_aspects(format);
  for (uint32_t layer = 0; layer < desc->layers; ++layer) {
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out_instance->image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange =
            {
                .aspectMask = aspects,
                .levelCount = desc->mip_levels,
                .baseArrayLayer = layer,
                .layerCount = 1u,
            },
    };
    if (vkCreateImageView(vkr_bindless_vk_renderer_device(renderer), &view_info,
                          NULL,
                          &out_instance->layer_views[layer]) != VK_SUCCESS) {
      vkr_bindless_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
  }
publish_descriptors:
  if ((desc->usage.set & VKR_TEXTURE_USAGE_SAMPLED) != 0u) {
    if (!vkr_bindless_vk_publish_sampled_view(
            renderer, out_instance->image.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            &out_instance->sampled_slot)) {
      vkr_bindless_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_sampled_slot = true_v;
  }
  if ((desc->usage.set & VKR_TEXTURE_USAGE_STORAGE) != 0u) {
    if (!vkr_bindless_vk_publish_storage_view(
            renderer, out_instance->image.view, &out_instance->storage_slot)) {
      vkr_bindless_vk_destroy_graph_image_instance(renderer, out_instance);
      return false_v;
    }
    out_instance->has_storage_slot = true_v;
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_realize_graph_images(VkrBindlessVulkanRenderer *renderer) {
  if (renderer->graph->images.length > renderer->config.max_graph_images)
    return false_v;
  for (uint64_t i = 0; i < renderer->graph->images.length; ++i) {
    const VkrRgImage *image =
        vector_get_VkrRgImage(&renderer->graph->images, i);
    if (!image || !image->declared_this_frame)
      continue;
    VkrBindlessVkGraphImage *slot = &renderer->graph_images[i];
    const bool8_t external_swapchain =
        image->imported && vkr_string8_equals_cstr(&image->name, "swapchain");
    const uint32_t instance_count =
        (image->desc.flags & VKR_RG_RESOURCE_FLAG_PER_IMAGE)
            ? renderer->targets.image_count
            : 1u;
    if (slot->live && slot->graph_generation == image->generation &&
        slot->external_swapchain == external_swapchain &&
        slot->instance_count == instance_count &&
        vkr_bindless_vk_graph_image_desc_equal(&slot->desc, &image->desc))
      continue;
    if (slot->live) {
      if (!vkr_bindless_vulkan_renderer_wait_idle(renderer))
        return false_v;
      vkr_bindless_vk_destroy_graph_image(renderer, slot);
    }
    *slot = (VkrBindlessVkGraphImage){
        .desc = image->desc,
        .graph_generation = image->generation,
        .instance_count = instance_count,
        .live = true_v,
        .external_swapchain = external_swapchain,
    };
    if (external_swapchain)
      continue;
    for (uint32_t instance = 0; instance < instance_count; ++instance) {
      if (!vkr_bindless_vk_create_graph_image_instance(
              renderer, &image->desc, &slot->instances[instance])) {
        log_error("Bindless Vulkan failed to realize graph image '%.*s' "
                  "(%ux%u, format=%u, usage=0x%x, samples=%u, layers=%u, "
                  "mips=%u, instance=%u/%u)",
                  (int)image->name.length, image->name.str, image->desc.width,
                  image->desc.height, image->desc.format, image->desc.usage.set,
                  image->desc.samples, image->desc.layers,
                  image->desc.mip_levels, instance, instance_count);
        vkr_bindless_vk_destroy_graph_image(renderer, slot);
        return false_v;
      }
    }
  }
  return true_v;
}

static VkrBindlessVkGraphImageInstance *
vkr_bindless_vk_graph_image(VkrBindlessVulkanRenderer *renderer,
                            VkrRgImageHandle handle, uint32_t image_index) {
  if (!vkr_rg_image_handle_valid(handle) ||
      handle.id > renderer->graph->images.length)
    return NULL;
  VkrBindlessVkGraphImage *slot = &renderer->graph_images[handle.id - 1u];
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
  const uint32_t instance = slot->instance_count > 1u ? image_index : 0u;
  return instance < slot->instance_count ? &slot->instances[instance] : NULL;
}

static bool8_t vkr_bindless_vk_record_graph_image_barriers(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const Vector_VkrRgImageBarrier *barriers) {
  if (barriers->length > renderer->config.max_graph_images)
    return false_v;
  for (uint64_t i = 0; i < barriers->length; ++i) {
    const VkrRgImageBarrier *barrier =
        vector_get_VkrRgImageBarrier(barriers, i);
    VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
        renderer, barrier->image, renderer->prepared_frame.image_index);
    if (!instance)
      return false_v;
    VkrBindlessVkDependency lowered = {0};
    const VkrBindlessVkDependencyResult result =
        vkr_bindless_vk_lower_image_dependency(
            barrier->src_access, barrier->dst_access, &barrier->dependency,
            barrier->src_layout != barrier->dst_layout, &lowered);
    if (result != VKR_BINDLESS_VK_DEPENDENCY_OK)
      return false_v;
    uint32_t base_mip = 0, mip_count = 0, base_layer = 0, layer_count = 0;
    vkr_image_subresource_range_resolve(&barrier->range,
                                        instance->image.mip_levels,
                                        instance->image.array_layers, &base_mip,
                                        &mip_count, &base_layer, &layer_count);
    renderer->graph_image_barriers[i] = (VkImageMemoryBarrier2){
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = lowered.src_stages,
        .srcAccessMask = lowered.src_access,
        .dstStageMask = lowered.dst_stages,
        .dstAccessMask = lowered.dst_access,
        .oldLayout = vkr_bindless_vk_texture_layout(barrier->src_layout),
        .newLayout = vkr_bindless_vk_texture_layout(barrier->dst_layout),
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = instance->image.handle,
        .subresourceRange =
            {
                .aspectMask =
                    vkr_bindless_vk_format_aspects(instance->image.format),
                .baseMipLevel = base_mip,
                .levelCount = mip_count,
                .baseArrayLayer = base_layer,
                .layerCount = layer_count,
            },
    };
  }
  if (barriers->length > 0u) {
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = (uint32_t)barriers->length,
        .pImageMemoryBarriers = renderer->graph_image_barriers,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_record_graph_pass_barriers(VkrBindlessVulkanRenderer *renderer,
                                           VkCommandBuffer command,
                                           const VkrRgPass *pass) {
  if (pass->pre_buffer_barriers.length > 0u) {
    log_error("Bindless Vulkan graph pass '%.*s' declared buffers before the "
              "V5 graph-buffer resolver was configured",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }
  return vkr_bindless_vk_record_graph_image_barriers(renderer, command,
                                                     &pass->pre_image_barriers);
}

static VkAttachmentLoadOp
vkr_bindless_vk_attachment_load_op(VkrAttachmentLoadOp op) {
  switch (op) {
  case VKR_ATTACHMENT_LOAD_OP_LOAD:
    return VK_ATTACHMENT_LOAD_OP_LOAD;
  case VKR_ATTACHMENT_LOAD_OP_CLEAR:
    return VK_ATTACHMENT_LOAD_OP_CLEAR;
  default:
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  }
}

static VkAttachmentStoreOp
vkr_bindless_vk_attachment_store_op(VkrAttachmentStoreOp op) {
  return op == VKR_ATTACHMENT_STORE_OP_STORE ? VK_ATTACHMENT_STORE_OP_STORE
                                             : VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

static bool8_t vkr_bindless_vk_graph_attachment(
    VkrBindlessVulkanRenderer *renderer, const VkrRgAttachment *attachment,
    VkImageLayout layout, VkRenderingAttachmentInfo *out_info,
    uint32_t *out_width, uint32_t *out_height) {
  VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
      renderer, attachment->image, renderer->prepared_frame.image_index);
  if (!instance ||
      attachment->desc.slice.mip_level >= instance->image.mip_levels ||
      attachment->desc.slice.base_layer >= instance->image.array_layers ||
      attachment->desc.slice.layer_count == 0u ||
      attachment->desc.slice.layer_count >
          instance->image.array_layers - attachment->desc.slice.base_layer)
    return false_v;
  VkImageView view = instance->image.view;
  if (instance->image.array_layers > 1u &&
      attachment->desc.slice.layer_count == 1u)
    view = instance->layer_views[attachment->desc.slice.base_layer];
  if (!view)
    return false_v;
  *out_info = (VkRenderingAttachmentInfo){
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = view,
      .imageLayout = layout,
      .loadOp = vkr_bindless_vk_attachment_load_op(attachment->desc.load_op),
      .storeOp = vkr_bindless_vk_attachment_store_op(attachment->desc.store_op),
  };
  MemCopy(&out_info->clearValue, &attachment->desc.clear_value,
          sizeof(out_info->clearValue));
  *out_width =
      Max(1u, instance->image.width >> attachment->desc.slice.mip_level);
  *out_height =
      Max(1u, instance->image.height >> attachment->desc.slice.mip_level);
  return true_v;
}

static uint64_t vkr_bindless_vk_align_up(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1u) & ~(alignment - 1u);
}

static void *vkr_bindless_vk_frame_upload_allocate(VkrBindlessVkFrameSlot *slot,
                                                   uint64_t size,
                                                   uint64_t alignment,
                                                   uint64_t *out_address,
                                                   uint64_t *out_offset) {
  if (!slot || !size || !alignment || (alignment & (alignment - 1u)) != 0u)
    return NULL;
  const uint64_t offset =
      vkr_bindless_vk_align_up(slot->frame_upload_cursor, alignment);
  if (offset > slot->frame_upload.size ||
      size > slot->frame_upload.size - offset)
    return NULL;
  slot->frame_upload_cursor = offset + size;
  if (out_address)
    *out_address = slot->frame_upload.address + offset;
  if (out_offset)
    *out_offset = offset;
  return (uint8_t *)slot->frame_upload.allocation.mapped + offset;
}

static bool8_t
vkr_bindless_vk_upload_instances(VkrBindlessVkFrameSlot *slot,
                                 const VkrInstanceDataGPU *instances,
                                 uint32_t count, uint64_t *out_address) {
  *out_address = 0u;
  if (count == 0u)
    return true_v;
  if (!instances || count > VKR_INSTANCE_BUFFER_MAX_INSTANCES)
    return false_v;
  const uint64_t size = (uint64_t)count * sizeof(*instances);
  void *destination = vkr_bindless_vk_frame_upload_allocate(
      slot, size, _Alignof(VkrInstanceDataGPU), out_address, NULL);
  if (!destination)
    return false_v;
  MemCopy(destination, instances, size);
  return true_v;
}

static bool8_t vkr_bindless_vk_resolve_sampled_pair(
    VkrBindlessVulkanRenderer *renderer, VkrTextureHandle handle,
    uint32_t *out_texture, uint32_t *out_sampler) {
  VkrBindlessVkPublishedTexture *texture =
      vkr_bindless_vk_published_texture(renderer, handle, NULL);
  if (!texture || texture->initialization_pending ||
      texture->sampler_record_index >= renderer->config.sampler_capacity)
    return false_v;
  const VkrBindlessVkPublishedSampler *sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  if (!sampler->live)
    return false_v;
  *out_texture = texture->sampled_slot.index;
  *out_sampler = sampler->slot.index;
  texture->last_use_submit_value = renderer->submit_value + 1u;
  return true_v;
}

static bool8_t
vkr_bindless_vk_upload_packet_tables(VkrBindlessVulkanRenderer *renderer,
                                     VkrBindlessVkFrameSlot *slot,
                                     const VkrRenderPacket *packet) {
  slot->point_light_data = 0u;
  slot->point_light_masks = 0u;
  slot->shadow_cascades = 0u;
  slot->ibl_probes = 0u;
  slot->ibl_probe_count = 0u;
  slot->irradiance_texture = 0u;
  slot->irradiance_sampler = 0u;
  slot->prefilter_texture = 0u;
  slot->prefilter_sampler = 0u;
  slot->ibl_ready = false_v;

  const VkrFrameLighting *lighting = packet->lighting;
  if (lighting && lighting->point_light_count) {
    const uint64_t light_bytes =
        (uint64_t)lighting->point_light_count * 4u * sizeof(Vec4);
    Vec4 *packed = vkr_bindless_vk_frame_upload_allocate(
        slot, light_bytes, _Alignof(Vec4), &slot->point_light_data, NULL);
    if (!packed)
      return false_v;
    for (uint32_t i = 0u; i < lighting->point_light_count; ++i) {
      const VkrPointLight *light = &lighting->point_lights[i];
      packed[i * 4u + 0u] =
          (Vec4){light->position.x, light->position.y, light->position.z,
                 light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                     ? cosf(light->inner_cone_angle)
                     : light->constant};
      packed[i * 4u + 1u] =
          (Vec4){light->color.x, light->color.y, light->color.z,
                 light->kind == VKR_POINT_LIGHT_KIND_GLTF_SPOT
                     ? cosf(light->outer_cone_angle)
                     : light->linear};
      packed[i * 4u + 2u] = (Vec4){light->intensity, light->quadratic,
                                   light->range, (float32_t)light->kind};
      packed[i * 4u + 3u] = (Vec4){light->direction.x, light->direction.y,
                                   light->direction.z, 0.0f};
    }
    const uint64_t mask_bytes =
        (uint64_t)lighting->point_light_grid->cell_count *
        sizeof(VkrPointLightMask);
    if (mask_bytes) {
      void *masks = vkr_bindless_vk_frame_upload_allocate(
          slot, mask_bytes, _Alignof(VkrPointLightMask),
          &slot->point_light_masks, NULL);
      if (!masks)
        return false_v;
      MemCopy(masks, lighting->point_light_grid->masks, mask_bytes);
    }
  }

  if (packet->shadow && packet->shadow->cascade_count) {
    const uint64_t cascade_bytes = (uint64_t)packet->shadow->cascade_count *
                                   sizeof(VkrBindlessVkPacketShadowCascade);
    VkrBindlessVkPacketShadowCascade *cascades =
        vkr_bindless_vk_frame_upload_allocate(
            slot, cascade_bytes, _Alignof(VkrBindlessVkPacketShadowCascade),
            &slot->shadow_cascades, NULL);
    if (!cascades)
      return false_v;
    for (uint32_t i = 0u; i < packet->shadow->cascade_count; ++i) {
      cascades[i] = (VkrBindlessVkPacketShadowCascade){
          .light_view_projection = packet->shadow->light_view_proj[i],
          .split_depth =
              (Vec4){packet->shadow->split_depths[i], 0.0f, 0.0f, 0.0f},
      };
    }
  }

  if (lighting && lighting->ibl_enabled && lighting->ibl_source.id) {
    VkrBindlessVkPublishedTexture *source =
        vkr_bindless_vk_published_texture(renderer, lighting->ibl_source, NULL);
    if (source &&
        vkr_bindless_vk_resolve_sampled_pair(renderer, source->ibl_irradiance,
                                             &slot->irradiance_texture,
                                             &slot->irradiance_sampler) &&
        vkr_bindless_vk_resolve_sampled_pair(renderer, source->ibl_prefilter,
                                             &slot->prefilter_texture,
                                             &slot->prefilter_sampler)) {
      slot->ibl_ready = true_v;
    }
  }

  if (lighting && lighting->ibl_probe_count) {
    const uint64_t probe_bytes = (uint64_t)lighting->ibl_probe_count *
                                 sizeof(VkrBindlessVkPacketIblProbe);
    VkrBindlessVkPacketIblProbe *probes = vkr_bindless_vk_frame_upload_allocate(
        slot, probe_bytes, _Alignof(VkrBindlessVkPacketIblProbe),
        &slot->ibl_probes, NULL);
    if (!probes)
      return false_v;
    for (uint32_t i = 0u; i < lighting->ibl_probe_count; ++i) {
      const VkrFrameIblProbe *probe = &lighting->ibl_probes[i];
      VkrBindlessVkPacketIblProbe packed = {
          .center_blend = {probe->center.x, probe->center.y, probe->center.z,
                           probe->blend_distance},
          .extents_weight = {probe->extents.x, probe->extents.y,
                             probe->extents.z, probe->weight},
          .intensity_box = {probe->intensity, probe->diffuse_intensity,
                            probe->specular_intensity,
                            probe->box_projection_enabled ? 1.0f : 0.0f},
      };
      if (!vkr_bindless_vk_resolve_sampled_pair(renderer, probe->irradiance,
                                                &packed.irradiance_texture,
                                                &packed.irradiance_sampler) ||
          !vkr_bindless_vk_resolve_sampled_pair(renderer, probe->prefilter,
                                                &packed.prefilter_texture,
                                                &packed.prefilter_sampler))
        continue;
      probes[slot->ibl_probe_count++] = packed;
    }
    if (!slot->ibl_probe_count)
      slot->ibl_probes = 0u;
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_prepare_packet_uploads(VkrBindlessVulkanRenderer *renderer,
                                       VkrBindlessVkFrameSlot *slot,
                                       const VkrRenderPacket *packet) {
  slot->frame_upload_cursor = 0u;
  slot->world_instances = 0u;
  slot->shadow_instances = 0u;
  slot->ui_instances = 0u;
  slot->editor_instances = 0u;
  slot->picking_instances = 0u;
  slot->indexed_draw_count = 0u;
  slot->shadow_draw_count = 0u;
  MemZero(slot->shadow_opaque_draw_count,
          sizeof(slot->shadow_opaque_draw_count));
  MemZero(slot->shadow_alpha_draw_count, sizeof(slot->shadow_alpha_draw_count));
  slot->opaque_draw_count = 0u;
  slot->transmission_draw_count = 0u;
  slot->blend_draw_count = 0u;
  return vkr_bindless_vk_upload_packet_tables(renderer, slot, packet) &&
         (!packet->world ||
          vkr_bindless_vk_upload_instances(slot, packet->world->instances,
                                           packet->world->instance_count,
                                           &slot->world_instances)) &&
         (!packet->shadow ||
          vkr_bindless_vk_upload_instances(slot, packet->shadow->instances,
                                           packet->shadow->instance_count,
                                           &slot->shadow_instances)) &&
         (!packet->ui ||
          vkr_bindless_vk_upload_instances(slot, packet->ui->instances,
                                           packet->ui->instance_count,
                                           &slot->ui_instances)) &&
         (!packet->editor ||
          vkr_bindless_vk_upload_instances(slot, packet->editor->instances,
                                           packet->editor->instance_count,
                                           &slot->editor_instances)) &&
         (!packet->picking ||
          vkr_bindless_vk_upload_instances(slot, packet->picking->instances,
                                           packet->picking->instance_count,
                                           &slot->picking_instances));
}

static VkrBindlessVkPublishedGeometry *
vkr_bindless_vk_resolve_geometry(VkrBindlessVulkanRenderer *renderer,
                                 VkrGeometryHandle handle) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity)
    return NULL;
  VkrBindlessVkPublishedGeometry *geometry =
      &renderer->published_geometries[handle.id - 1u];
  return geometry->live && geometry->handle.generation == handle.generation &&
                 geometry->pending_initialization_count == 0u
             ? geometry
             : NULL;
}

static VkrBindlessVkPublishedMaterial *
vkr_bindless_vk_resolve_material(VkrBindlessVulkanRenderer *renderer,
                                 VkrMaterialHandle handle) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity)
    return NULL;
  VkrBindlessVkPublishedMaterial *material =
      &renderer->published_materials[handle.id - 1u];
  return material->live && material->handle.generation == handle.generation
             ? material
             : NULL;
}

static VkrBindlessVkPacketDrawRoot *
vkr_bindless_vk_packet_root(VkrBindlessVulkanRenderer *renderer,
                            VkrBindlessVkFrameSlot *slot,
                            uint64_t *out_address) {
  VkrBindlessVkPacketDrawRoot *root = vkr_bindless_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrBindlessVkPacketDrawRoot), out_address,
      NULL);
  if (root)
    MemZero(root, sizeof(*root));
  return root;
}

static void vkr_bindless_vk_fill_packet_root(
    VkrBindlessVulkanRenderer *renderer, VkrBindlessVkPacketDrawRoot *root,
    const VkrBindlessVkFrameSlot *slot, const VkrRenderPacket *packet,
    const VkrBindlessVkPublishedGeometry *geometry,
    const VkrBindlessVkPublishedMaterial *material, uint64_t instances,
    uint32_t first_instance, Mat4 view_projection, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t transmission_pass) {
  root->vertices = geometry->vertices.address;
  root->instances = instances;
  root->view_projection = view_projection;
  root->materials = renderer->materials.address;
  root->irradiance_texture = slot->irradiance_texture;
  root->irradiance_sampler = slot->irradiance_sampler;
  root->prefilter_texture = slot->prefilter_texture;
  root->prefilter_sampler = slot->prefilter_sampler;
  root->shadow_texture = shadow_texture;
  root->shadow_sampler = 0u;
  root->transmission_texture = transmission_texture;
  root->transmission_sampler = 0u;
  root->view_position =
      (Vec4){packet->globals.view_position.x, packet->globals.view_position.y,
             packet->globals.view_position.z, 1.0f};
  root->material_index = material->slot.index;
  root->first_instance = first_instance;
  root->prefilter_mip_count = VKR_IBL_PREFILTER_MIP_COUNT;
  root->material_emissive =
      (Vec4){material->pbr.emissive_factor.x, material->pbr.emissive_factor.y,
             material->pbr.emissive_factor.z, 0.0f};
  root->material_dielectric_specular = (Vec4){
      material->pbr.dielectric_specular.x, material->pbr.dielectric_specular.y,
      material->pbr.dielectric_specular.z, transmission_pass ? 1.0f : 0.0f};
  root->material_surface =
      (Vec4){material->pbr.metallic, material->pbr.roughness,
             material->pbr.normal_scale, material->pbr.occlusion_strength};
  root->material_alpha =
      (Vec4){material->alpha_cutoff, material->pbr.transmission_factor,
             material->pbr.ior, material->pbr.thickness_factor};
  root->material_attenuation_color = (Vec4){
      material->pbr.attenuation_color.x, material->pbr.attenuation_color.y,
      material->pbr.attenuation_color.z, material->pbr.attenuation_distance};
  root->ibl_controls =
      packet->lighting
          ? (Vec4){packet->lighting->ibl_intensity,
                   packet->lighting->ibl_diffuse_intensity,
                   packet->lighting->ibl_specular_intensity,
                   1.0f / (float32_t)(packet->frame.viewport_width
                                          ? packet->frame.viewport_width
                                          : packet->frame.window_width)}
          : (Vec4){1.0f, 1.0f, 1.0f,
                   1.0f / (float32_t)(packet->frame.viewport_width
                                          ? packet->frame.viewport_width
                                          : packet->frame.window_width)};
  root->directional_direction_enabled =
      packet->lighting
          ? (Vec4){packet->lighting->directional_direction.x,
                   packet->lighting->directional_direction.y,
                   packet->lighting->directional_direction.z,
                   packet->lighting->directional_enabled ? 1.0f : 0.0f}
          : vec4_zero();
  root->directional_color_intensity =
      packet->lighting ? (Vec4){packet->lighting->directional_color.x,
                                packet->lighting->directional_color.y,
                                packet->lighting->directional_color.z,
                                packet->lighting->directional_intensity}
                       : vec4_zero();
  root->ambient_color =
      (Vec4){packet->globals.ambient_color.x, packet->globals.ambient_color.y,
             packet->globals.ambient_color.z,
             1.0f / (float32_t)(packet->frame.viewport_height
                                    ? packet->frame.viewport_height
                                    : packet->frame.window_height)};
  root->render_mode = packet->globals.render_mode;
  root->alpha_mode = material->alpha_mode;
  root->flags = slot->ibl_ready ? 1u : 0u;
  root->material_flags = slot->ibl_ready ? 1u : 0u;
  root->point_light_data = slot->point_light_data;
  root->point_light_masks = slot->point_light_masks;
  if (packet->lighting && packet->lighting->point_light_count) {
    root->point_light_grid_origin_cell_size =
        (Vec4){packet->lighting->point_light_grid->origin.x,
               packet->lighting->point_light_grid->origin.y,
               packet->lighting->point_light_grid->origin.z,
               packet->lighting->point_light_grid->cell_size};
    root->point_light_grid_dimensions_count[0] =
        packet->lighting->point_light_grid->dimensions[0];
    root->point_light_grid_dimensions_count[1] =
        packet->lighting->point_light_grid->dimensions[1];
    root->point_light_grid_dimensions_count[2] =
        packet->lighting->point_light_grid->dimensions[2];
    root->point_light_grid_dimensions_count[3] =
        packet->lighting->point_light_grid->cell_count;
    root->point_light_global_mask =
        packet->lighting->point_light_grid->global_mask;
  }
  root->point_light_count =
      packet->lighting ? packet->lighting->point_light_count : 0u;
  root->view = packet->globals.view;
  root->shadow_cascades = slot->shadow_cascades;
  root->shadow_cascade_count =
      packet->shadow ? packet->shadow->cascade_count : 0u;
  root->shadow_bias = 0.0001f;
  root->ibl_probes = slot->ibl_probes;
  root->ibl_probe_count = slot->ibl_probe_count;
}

static bool8_t vkr_bindless_vk_record_packet_draws(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkPacketPipeline pipeline, const VkrDrawItem *draws,
    uint32_t draw_count, uint64_t instances, uint32_t instance_count,
    Mat4 view_projection, bool8_t alpha_cutout, uint32_t shadow_texture,
    uint32_t transmission_texture, bool8_t transmission_pass) {
  if (draw_count == 0u)
    return true_v;
  if (!draws || !instances)
    return false_v;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  for (uint32_t i = 0; i < draw_count; ++i) {
    const VkrDrawItem *draw = &draws[i];
    if (draw->instance_count == 0u || draw->first_instance > instance_count ||
        draw->instance_count > instance_count - draw->first_instance)
      return false_v;
    VkrBindlessVkPublishedGeometry *geometry =
        vkr_bindless_vk_resolve_geometry(renderer, draw->geometry);
    VkrBindlessVkPublishedMaterial *material =
        vkr_bindless_vk_resolve_material(renderer, draw->material);
    if (!geometry && draw->geometry.id > 0u &&
        draw->geometry.id <= renderer->config.sampled_image_capacity) {
      const VkrBindlessVkPublishedGeometry *pending_geometry =
          &renderer->published_geometries[draw->geometry.id - 1u];
      if (pending_geometry->live &&
          pending_geometry->handle.generation == draw->geometry.generation &&
          pending_geometry->pending_initialization_count)
        continue;
    }
    if (!geometry || !material ||
        draw->submesh_index >= geometry->submesh_count)
      return false_v;
    bool8_t material_ready = true_v;
    for (uint32_t texture_index = 0u;
         texture_index < ArrayCount(material->texture_record_indices);
         ++texture_index) {
      const uint32_t record_index =
          material->texture_record_indices[texture_index];
      if (record_index != UINT32_MAX &&
          renderer->published_textures[record_index].initialization_pending) {
        material_ready = false_v;
        break;
      }
    }
    if (!material_ready)
      continue;
    const VkrBindlessVkSubmeshRange *range =
        &geometry->submeshes[draw->submesh_index];
    uint64_t root_address = 0u;
    VkrBindlessVkPacketDrawRoot *root =
        vkr_bindless_vk_packet_root(renderer, slot, &root_address);
    if (!root)
      return false_v;
    vkr_bindless_vk_fill_packet_root(
        renderer, root, slot, renderer->graph->packet, geometry, material,
        instances, draw->first_instance, view_projection, shadow_texture,
        transmission_texture, transmission_pass);
    const VkrBindlessVkPushConstants push = {
        .root = root_address,
        .material_index = material->slot.index,
        .flags = alpha_cutout ? 1u : 0u,
    };
    vkCmdBindIndexBuffer2(command, geometry->indices.handle, 0u,
                          geometry->indices.size, geometry->index_type);
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdDrawIndexed(command, range->index_count, draw->instance_count,
                     range->first_index, range->vertex_offset, 0u);
    const uint64_t pending_submit = renderer->submit_value + 1u;
    geometry->last_use_submit_value = pending_submit;
    for (uint32_t texture = 0u;
         texture < ArrayCount(material->texture_record_indices); ++texture) {
      const uint32_t record_index = material->texture_record_indices[texture];
      if (record_index != UINT32_MAX)
        renderer->published_textures[record_index].last_use_submit_value =
            pending_submit;
    }
    slot->indexed_draw_count++;
    if (pipeline == VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW)
      slot->shadow_draw_count++;
    else if (pipeline == VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND)
      slot->blend_draw_count++;
    else if (pipeline == VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_OPAQUE) {
      if (transmission_pass)
        slot->transmission_draw_count++;
      else
        slot->opaque_draw_count++;
    }
  }
  return true_v;
}

static bool8_t vkr_bindless_vk_record_text_draws(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkPacketPipeline pipeline, const VkrPreparedTextDraw *draws,
    uint32_t draw_count, Mat4 view_projection, uint32_t target_width,
    uint32_t target_height, bool8_t ui_domain) {
  if (draw_count == 0u)
    return true_v;
  if (!draws || !target_width || !target_height)
    return false_v;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  for (uint32_t i = 0u; i < draw_count; ++i) {
    const VkrPreparedTextDraw *draw = &draws[i];
    if (!draw->vertices || !draw->indices || draw->vertex_count == 0u ||
        draw->index_count == 0u || draw->max_index >= draw->vertex_count)
      return false_v;
    VkrBindlessVkPublishedTexture *atlas =
        vkr_bindless_vk_published_texture(renderer, draw->atlas, NULL);
    /* Retained text can become visible while its asynchronously loaded atlas

     * is still crossing the asset-publisher seam. Omit that draw until the

     * logical publication exists; a not-yet-ready atlas must not reject the

     * otherwise coherent frame. If initialization is queued, its copy and

     * transition are recorded before the graph in this same command buffer. */
    if (!atlas || atlas->initialization_pending ||
        atlas->sampler_record_index >= renderer->config.sampler_capacity)
      continue;
    VkrBindlessVkPublishedSampler *sampler =
        &renderer->published_samplers[atlas->sampler_record_index];
    if (!sampler->live)
      continue;

    const uint64_t vertex_bytes =
        (uint64_t)draw->vertex_count * sizeof(*draw->vertices);
    const uint64_t index_bytes =
        (uint64_t)draw->index_count * sizeof(*draw->indices);
    uint64_t vertex_address = 0u;
    void *vertices = vkr_bindless_vk_frame_upload_allocate(
        slot, vertex_bytes, _Alignof(VkrTextVertex), &vertex_address, NULL);
    uint64_t index_offset = 0u;
    void *indices = vkr_bindless_vk_frame_upload_allocate(
        slot, index_bytes, _Alignof(uint32_t), NULL, &index_offset);
    uint64_t root_address = 0u;
    VkrBindlessVkPacketDrawRoot *root =
        vkr_bindless_vk_packet_root(renderer, slot, &root_address);
    if (!vertices || !indices || !root)
      return false_v;
    MemCopy(vertices, draw->vertices, vertex_bytes);
    MemCopy(indices, draw->indices, index_bytes);
    root->vertices = vertex_address;
    root->view_projection = view_projection;
    root->view = draw->model;
    root->transmission_texture = atlas->sampled_slot.index;
    root->transmission_sampler = sampler->slot.index;
    root->material_alpha.x = draw->screen_px_range;
    root->material_flags = draw->font_mode;
    root->first_instance = draw->object_id;
    root->flags = ui_domain ? 1u : 0u;
    root->point_light_grid_origin_cell_size =
        (Vec4){(float32_t)target_width, (float32_t)target_height, 0.0f, 0.0f};
    const VkrBindlessVkPushConstants push = {.root = root_address};
    vkCmdBindIndexBuffer2(command, slot->frame_upload.handle, index_offset,
                          index_bytes, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(command, renderer->pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT |
                           VK_SHADER_STAGE_COMPUTE_BIT,
                       0u, sizeof(push), &push);
    vkCmdDrawIndexed(command, draw->index_count, 1u, 0u, 0, 0u);
    atlas->last_use_submit_value = renderer->submit_value + 1u;
    slot->indexed_draw_count++;
  }
  return true_v;
}

static bool8_t vkr_bindless_vk_record_packet_fullscreen(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkPacketPipeline pipeline, uint32_t texture_index,
    uint32_t flags) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrBindlessVkPacketDrawRoot *root =
      vkr_bindless_vk_packet_root(renderer, slot, &root_address);
  if (!root)
    return false_v;
  root->materials = renderer->materials.address;
  root->transmission_texture = texture_index;
  root->transmission_sampler = 0u;
  root->ibl_controls.x = renderer->graph->packet->globals.exposure;
  const VkrBindlessVkPushConstants push = {
      .root = root_address,
      .material_index = 0u,
      .flags = flags,
  };
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    renderer->packet_pipelines[pipeline]);
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  vkCmdDraw(command, 3u, 1u, 0u, 0u);
  return true_v;
}

static bool8_t
vkr_bindless_vk_record_packet_skybox(VkrBindlessVulkanRenderer *renderer,
                                     VkCommandBuffer command,
                                     const VkrSkyboxPassPayload *skybox) {
  if (!skybox)
    return true_v;
  VkrBindlessVkPublishedTexture *cubemap =
      vkr_bindless_vk_published_texture(renderer, skybox->cubemap, NULL);
  if (!cubemap)
    return false_v;
  if (cubemap->initialization_pending)
    return true_v;
  if (cubemap->image.array_layers != 6u ||
      cubemap->sampler_record_index >= renderer->config.sampler_capacity)
    return false_v;
  VkrBindlessVkPublishedSampler *sampler =
      &renderer->published_samplers[cubemap->sampler_record_index];
  if (!sampler->live)
    return false_v;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrBindlessVkPacketDrawRoot *root =
      vkr_bindless_vk_packet_root(renderer, slot, &root_address);
  if (!root)
    return false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  root->view_projection =
      mat4_inverse(mat4_mul(packet->globals.projection, packet->globals.view));
  root->view_position =
      (Vec4){packet->globals.view_position.x, packet->globals.view_position.y,
             packet->globals.view_position.z, 1.0f};
  root->transmission_texture = cubemap->sampled_slot.index;
  root->transmission_sampler = sampler->slot.index;
  const VkrBindlessVkPushConstants push = {.root = root_address};
  vkCmdBindPipeline(
      command, VK_PIPELINE_BIND_POINT_GRAPHICS,
      renderer->packet_pipelines[VKR_BINDLESS_VK_PACKET_PIPELINE_SKYBOX]);
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  vkCmdDraw(command, 3u, 1u, 0u, 0u);
  cubemap->last_use_submit_value = renderer->submit_value + 1u;
  return true_v;
}

static bool8_t
vkr_bindless_vk_graph_sampled_index(VkrBindlessVulkanRenderer *renderer,
                                    const VkrRgPass *pass, uint32_t read_index,
                                    uint32_t *out_index) {
  if (!out_index || read_index >= pass->desc.image_reads.length)
    return false_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, read_index);
  VkrBindlessVkGraphImageInstance *image = vkr_bindless_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  if (!image || !image->has_sampled_slot)
    return false_v;
  *out_index = image->sampled_slot.index;
  return true_v;
}

static bool8_t vkr_bindless_vk_record_graphics_body(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, VkrBindlessVkGraphExecutorKind kind) {
  const VkrRenderPacket *packet = renderer->graph->packet;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!packet)
    return false_v;
  switch (kind) {
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SHADOW: {
    if (!packet->shadow)
      return true_v;
    const uint32_t cascade = pass->desc.depth_attachment.desc.slice.base_layer;
    if (cascade >= packet->shadow->cascade_count)
      return false_v;
    const VkrShadowConfigOverride *override = packet->shadow->config_override;
    vkCmdSetDepthBias(command, override ? override->depth_bias_constant : 1.25f,
                      override ? override->depth_bias_clamp : 0.0f,
                      override ? override->depth_bias_slope : 1.75f);
    const uint32_t opaque_draw_begin = slot->shadow_draw_count;
    if (!vkr_bindless_vk_record_packet_draws(
            renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW,
            packet->shadow->opaque_draws, packet->shadow->opaque_draw_count,
            slot->shadow_instances, packet->shadow->instance_count,
            packet->shadow->light_view_proj[cascade], false_v, 0u, 0u, false_v))
      return false_v;
    slot->shadow_opaque_draw_count[cascade] =
        slot->shadow_draw_count - opaque_draw_begin;
    const uint32_t alpha_draw_begin = slot->shadow_draw_count;
    if (!vkr_bindless_vk_record_packet_draws(
            renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW,
            packet->shadow->alpha_draws, packet->shadow->alpha_draw_count,
            slot->shadow_instances, packet->shadow->instance_count,
            packet->shadow->light_view_proj[cascade], true_v, 0u, 0u, false_v))
      return false_v;
    slot->shadow_alpha_draw_count[cascade] =
        slot->shadow_draw_count - alpha_draw_begin;
    return true_v;
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING: {
    if (!packet->picking || !packet->picking->pending)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->globals.projection, packet->globals.view);
    return vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING,
               packet->picking->draws, packet->picking->draw_count,
               slot->picking_instances, packet->picking->instance_count,
               view_projection, false_v, 0u, 0u, false_v) &&
           (!packet->world ||
            vkr_bindless_vk_record_text_draws(
                renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING_TEXT,
                packet->world->text_draws, packet->world->text_draw_count,
                view_projection, renderer->config.width,
                renderer->config.height, false_v)) &&
           (!packet->ui ||
            vkr_bindless_vk_record_text_draws(
                renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING_TEXT,
                packet->ui->text_draws, packet->ui->text_draw_count,
                mat4_identity(), renderer->config.width,
                renderer->config.height, true_v));
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_OPAQUE: {
    if (!packet->world)
      return true_v;
    uint32_t shadow_texture = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &shadow_texture))
      return false_v;
    return vkr_bindless_vk_record_packet_draws(
        renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_OPAQUE,
        packet->world->opaque_draws, packet->world->opaque_draw_count,
        slot->world_instances, packet->world->instance_count,
        mat4_mul(packet->globals.projection, packet->globals.view), false_v,
        shadow_texture, 0u, false_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_TRANSMISSION: {
    if (!packet->world)
      return true_v;
    uint32_t shadow_texture = 0u;
    uint32_t transmission_texture = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &shadow_texture) ||
        !vkr_bindless_vk_graph_sampled_index(renderer, pass, 1u,
                                             &transmission_texture))
      return false_v;
    return vkr_bindless_vk_record_packet_draws(
        renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND,
        packet->world->transmission_draws,
        packet->world->transmission_draw_count, slot->world_instances,
        packet->world->instance_count,
        mat4_mul(packet->globals.projection, packet->globals.view), false_v,
        shadow_texture, transmission_texture, true_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_BLEND: {
    if (!packet->world)
      return true_v;
    const Mat4 view_projection =
        mat4_mul(packet->globals.projection, packet->globals.view);
    uint32_t shadow_texture = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &shadow_texture))
      return false_v;
    return vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND,
               packet->world->transparent_draws,
               packet->world->transparent_draw_count, slot->world_instances,
               packet->world->instance_count, view_projection, false_v,
               shadow_texture, 0u, false_v) &&
           vkr_bindless_vk_record_text_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_TEXT,
               packet->world->text_draws, packet->world->text_draw_count,
               view_projection, renderer->config.width, renderer->config.height,
               false_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_EDITOR: {
    uint32_t texture_index = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &texture_index))
      return false_v;
    if (!vkr_bindless_vk_record_packet_fullscreen(
            renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_FINAL,
            texture_index, 0u))
      return false_v;
    return !packet->editor ||
           vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_UI,
               packet->editor->draws, packet->editor->draw_count,
               slot->editor_instances, packet->editor->instance_count,
               mat4_identity(), false_v, 0u, 0u, false_v);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_TONEMAP: {
    uint32_t texture_index = 0u;
    if (!vkr_bindless_vk_graph_sampled_index(renderer, pass, 0u,
                                             &texture_index))
      return false_v;
    return vkr_bindless_vk_record_packet_fullscreen(
        renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_FINAL,
        texture_index, 2u);
  }
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SKYBOX:
    return vkr_bindless_vk_record_packet_skybox(renderer, command,
                                                packet->skybox);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_UI: {
    if (!packet->ui)
      return true_v;
    return vkr_bindless_vk_record_packet_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_UI,
               packet->ui->draws, packet->ui->draw_count, slot->ui_instances,
               packet->ui->instance_count, mat4_identity(), false_v, 0u, 0u,
               false_v) &&
           vkr_bindless_vk_record_text_draws(
               renderer, command, VKR_BINDLESS_VK_PACKET_PIPELINE_UI_TEXT,
               packet->ui->text_draws, packet->ui->text_draw_count,
               mat4_identity(), renderer->config.width, renderer->config.height,
               true_v);
  }
  default:
    return false_v;
  }
}

static bool8_t vkr_bindless_vk_record_graph_graphics_pass(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    const VkrRgPass *pass, VkrBindlessVkGraphExecutorKind kind) {
  enum { VKR_BINDLESS_VK_GRAPH_COLOR_ATTACHMENT_MAX = 8 };
  if (pass->desc.color_attachments.length >
      VKR_BINDLESS_VK_GRAPH_COLOR_ATTACHMENT_MAX)
    return false_v;
  VkRenderingAttachmentInfo colors[VKR_BINDLESS_VK_GRAPH_COLOR_ATTACHMENT_MAX] =
      {0};
  VkRenderingAttachmentInfo depth = {0};
  uint32_t width = 0, height = 0;
  for (uint64_t i = 0; i < pass->desc.color_attachments.length; ++i) {
    uint32_t attachment_width = 0, attachment_height = 0;
    if (!vkr_bindless_vk_graph_attachment(
            renderer,
            vector_get_VkrRgAttachment(&pass->desc.color_attachments, i),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, &colors[i],
            &attachment_width, &attachment_height))
      return false_v;
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
  }
  if (pass->desc.has_depth_attachment) {
    uint32_t attachment_width = 0, attachment_height = 0;
    if (!vkr_bindless_vk_graph_attachment(
            renderer, &pass->desc.depth_attachment,
            pass->desc.depth_attachment.read_only
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            &depth, &attachment_width, &attachment_height))
      return false_v;
    width = width ? Min(width, attachment_width) : attachment_width;
    height = height ? Min(height, attachment_height) : attachment_height;
  }
  if (!width || !height)
    return false_v;
  const VkRenderingInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {.extent = {.width = width, .height = height}},
      .layerCount = 1u,
      .colorAttachmentCount = (uint32_t)pass->desc.color_attachments.length,
      .pColorAttachments = colors,
      .pDepthAttachment = pass->desc.has_depth_attachment ? &depth : NULL,
      .pStencilAttachment =
          pass->desc.has_depth_attachment &&
                  (vkr_bindless_vk_format_aspects(
                       vkr_bindless_vk_graph_image(
                           renderer, pass->desc.depth_attachment.image,
                           renderer->prepared_frame.image_index)
                           ->image.format) &
                   VK_IMAGE_ASPECT_STENCIL_BIT)
              ? &depth
              : NULL,
  };
  vkCmdBeginRendering(command, &rendering);
  const VkViewport viewport = {
      .width = (float32_t)width,
      .height = (float32_t)height,
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor = {.extent = {.width = width, .height = height}};
  vkCmdSetViewport(command, 0u, 1u, &viewport);
  vkCmdSetScissor(command, 0u, 1u, &scissor);
  if (!vkr_bindless_vk_record_graphics_body(renderer, command, pass, kind)) {
    vkCmdEndRendering(command);
    return false_v;
  }
  vkCmdEndRendering(command);
  return true_v;
}

static bool8_t
vkr_bindless_vk_record_graph_transfer_pass(VkrBindlessVulkanRenderer *renderer,
                                           VkCommandBuffer command,
                                           const VkrRgPass *pass) {
  if (pass->desc.image_reads.length == 0u ||
      pass->desc.image_writes.length == 0u)
    return true_v;
  const VkrRgImageUse *read =
      vector_get_VkrRgImageUse(&pass->desc.image_reads, 0u);
  const VkrRgImageUse *write =
      vector_get_VkrRgImageUse(&pass->desc.image_writes, 0u);
  VkrBindlessVkGraphImageInstance *source = vkr_bindless_vk_graph_image(
      renderer, read->image, renderer->prepared_frame.image_index);
  VkrBindlessVkGraphImageInstance *destination = vkr_bindless_vk_graph_image(
      renderer, write->image, renderer->prepared_frame.image_index);
  if (!source || !destination ||
      source->image.format != destination->image.format)
    return false_v;
  const VkImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
      .srcSubresource = {.aspectMask = vkr_bindless_vk_format_aspects(
                             source->image.format),
                         .layerCount = 1u},
      .dstSubresource = {.aspectMask = vkr_bindless_vk_format_aspects(
                             destination->image.format),
                         .layerCount = 1u},
      .extent = {.width = Min(source->image.width, destination->image.width),
                 .height = Min(source->image.height, destination->image.height),
                 .depth = 1u},
  };
  const VkCopyImageInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
      .srcImage = source->image.handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstImage = destination->image.handle,
      .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .regionCount = 1u,
      .pRegions = &region,
  };
  vkCmdCopyImage2(command, &copy);
  return true_v;
}

static void vkr_bindless_vk_cmd_ibl_image_barrier(
    VkCommandBuffer command, VkImage image, uint32_t mip_level,
    uint32_t layer_count, VkPipelineStageFlags2 src_stage,
    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
    VkAccessFlags2 dst_access, VkImageLayout old_layout,
    VkImageLayout new_layout) {
  const VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange =
          {
              .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
              .baseMipLevel = mip_level,
              .levelCount = 1u,
              .baseArrayLayer = 0u,
              .layerCount = layer_count,
          },
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
}

static bool8_t vkr_bindless_vk_record_ibl_dispatch(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkIblPipeline pipeline,
    const VkrBindlessVkPublishedTexture *source,
    const VkrBindlessVkPublishedTexture *target, uint32_t target_mip,
    uint32_t sample_count, float32_t roughness) {
  if (!source || !target || target_mip >= target->storage_slot_count ||
      source->sampler_record_index >= renderer->config.sampler_capacity)
    return false_v;
  const VkrBindlessVkPublishedSampler *sampler =
      &renderer->published_samplers[source->sampler_record_index];
  if (!sampler->live)
    return false_v;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  uint64_t root_address = 0u;
  VkrBindlessVkIblRoot *root = vkr_bindless_vk_frame_upload_allocate(
      slot, sizeof(*root), _Alignof(VkrBindlessVkIblRoot), &root_address, NULL);
  if (!root)
    return false_v;
  *root = (VkrBindlessVkIblRoot){
      .source_texture = source->sampled_slot.index,
      .source_sampler = sampler->slot.index,
      .target_texture = target->storage_slots[target_mip].index,
      .target_size = Max(1u, target->image.width >> target_mip),
      .sample_count = sample_count,
      .source_face_size = source->image.width,
      .source_mip_count = source->image.mip_levels,
      .roughness = roughness,
  };
  const VkrBindlessVkPushConstants push = {.root = root_address};
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                    renderer->ibl_pipelines[pipeline]);
  vkCmdPushConstants(command, renderer->pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_COMPUTE_BIT,
                     0u, sizeof(push), &push);
  vkCmdDispatch(command, (root->target_size + 7u) / 8u,
                (root->target_size + 7u) / 8u, target->image.array_layers);
  return true_v;
}

static void
vkr_bindless_vk_record_ibl_source_mips(VkCommandBuffer command,
                                       VkrBindlessVkPublishedTexture *source) {
  if (source->image.mip_levels == 1u) {
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, source->image.handle, 0u, source->image.array_layers,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL);
    return;
  }
  for (uint32_t mip = 1u; mip < source->image.mip_levels; ++mip) {
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, source->image.handle, mip - 1u, source->image.array_layers,
        mip == 1u ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                  : VK_PIPELINE_STAGE_2_BLIT_BIT,
        mip == 1u ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
                  : VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        mip == 1u ? VK_IMAGE_LAYOUT_GENERAL
                  : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, source->image.handle, mip, source->image.array_layers,
        VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const int32_t source_width =
        (int32_t)Max(1u, source->image.width >> (mip - 1u));
    const int32_t source_height =
        (int32_t)Max(1u, source->image.height >> (mip - 1u));
    const int32_t target_width = (int32_t)Max(1u, source->image.width >> mip);
    const int32_t target_height = (int32_t)Max(1u, source->image.height >> mip);
    const VkImageBlit2 region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = mip - 1u,
                .baseArrayLayer = 0u,
                .layerCount = source->image.array_layers,
            },
        .srcOffsets = {{0, 0, 0}, {source_width, source_height, 1}},
        .dstSubresource =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel = mip,
                .baseArrayLayer = 0u,
                .layerCount = source->image.array_layers,
            },
        .dstOffsets = {{0, 0, 0}, {target_width, target_height, 1}},
    };
    const VkBlitImageInfo2 blit = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = source->image.handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = source->image.handle,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1u,
        .pRegions = &region,
        .filter = VK_FILTER_LINEAR,
    };
    vkCmdBlitImage2(command, &blit);
  }
  for (uint32_t mip = 0u; mip < source->image.mip_levels; ++mip) {
    const bool8_t last = mip == source->image.mip_levels - 1u;
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, source->image.handle, mip, source->image.array_layers,
        VK_PIPELINE_STAGE_2_BLIT_BIT,
        last ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        last ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
             : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL);
  }
}

static bool8_t
vkr_bindless_vk_record_ibl_bakes(VkrBindlessVulkanRenderer *renderer,
                                 VkCommandBuffer command) {
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    VkrBindlessVkPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    job->recorded = false_v;
    VkrBindlessVkPublishedTexture *source =
        vkr_bindless_vk_texture_publication(renderer, job->source);
    VkrBindlessVkPublishedTexture *irradiance =
        vkr_bindless_vk_texture_publication(renderer, job->irradiance);
    VkrBindlessVkPublishedTexture *prefilter =
        vkr_bindless_vk_texture_publication(renderer, job->prefilter);
    VkrBindlessVkPublishedTexture *equirect =
        job->convert_equirect
            ? vkr_bindless_vk_texture_publication(renderer, job->equirect)
            : NULL;
    if (!source || !irradiance || !prefilter ||
        (job->convert_equirect && !equirect))
      return false_v;
    const VkrBindlessVkPublishedTexture *input =
        job->convert_equirect ? equirect : source;
    if (input->initialization_pending) {
      continue;
    }
    if (job->convert_equirect) {
      if (!vkr_bindless_vk_record_ibl_dispatch(
              renderer, command, VKR_BINDLESS_VK_IBL_PIPELINE_EQUIRECT,
              equirect, source, 0u, 1u, 0.0f))
        return false_v;
      vkr_bindless_vk_record_ibl_source_mips(command, source);
    }
    if (!vkr_bindless_vk_record_ibl_dispatch(
            renderer, command, VKR_BINDLESS_VK_IBL_PIPELINE_IRRADIANCE, source,
            irradiance, 0u, 128u, 0.0f))
      return false_v;
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, irradiance->image.handle, 0u, irradiance->image.array_layers,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t mip = 0u; mip < prefilter->image.mip_levels; ++mip) {
      const float32_t roughness =
          prefilter->image.mip_levels > 1u
              ? (float32_t)mip / (float32_t)(prefilter->image.mip_levels - 1u)
              : 0.0f;
      if (!vkr_bindless_vk_record_ibl_dispatch(
              renderer, command, VKR_BINDLESS_VK_IBL_PIPELINE_PREFILTER, source,
              prefilter, mip, 256u, roughness))
        return false_v;
      vkr_bindless_vk_cmd_ibl_image_barrier(
          command, prefilter->image.handle, mip, prefilter->image.array_layers,
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
          VK_IMAGE_LAYOUT_GENERAL);
    }
    job->recorded = true_v;
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_record_graph_pass(VkrBindlessVulkanRenderer *renderer,
                                  VkCommandBuffer command,
                                  const VkrRgPass *pass) {
  VkrBindlessVkGraphExecutorKind kind;
  if (!vkr_bindless_vk_graph_executor_kind(pass, &kind)) {
    log_error("Bindless Vulkan graph pass '%.*s' has no executor kind",
              (int)pass->desc.name.length, pass->desc.name.str);
    return false_v;
  }

  switch (kind) {
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SHADOW:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_SKYBOX:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_OPAQUE:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_TRANSMISSION:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_WORLD_BLEND:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_TONEMAP:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_EDITOR:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_UI:
    return vkr_bindless_vk_record_graph_graphics_pass(renderer, command, pass,
                                                      kind);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_IBL_BAKE:
    return vkr_bindless_vk_record_ibl_bakes(renderer, command);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_FULLSCREEN:
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_COPY_PRE_TRANSMISSION_EDITOR:
    return vkr_bindless_vk_record_graph_transfer_pass(renderer, command, pass);
  case VKR_BINDLESS_VK_GRAPH_EXECUTOR_PICKING_READBACK:
    // The one-pixel copy is recorded after capture selection in record_draw().
    return true_v;
  default:
    return false_v;
  }
}

static bool8_t vkr_bindless_vk_record_graph(VkrBindlessVulkanRenderer *renderer,
                                            VkCommandBuffer command) {
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  slot->pass_timing_count = 0u;
  for (uint64_t order = 0; order < renderer->graph->execution_order.length;
       ++order) {
    const uint32_t pass_index =
        *vector_get_uint32_t(&renderer->graph->execution_order, order);
    const VkrRgPass *pass =
        vector_get_VkrRgPass(&renderer->graph->passes, pass_index);
    const float64_t cpu_begin = vkr_platform_get_absolute_time();
    const uint32_t timing_index = slot->pass_timing_count;
    const bool8_t timestamp_pass =
        slot->timing_requested &&
        timing_index < VKR_RENDERER_IMPL_MAX_PASS_TIMINGS;
    if (timestamp_pass) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                           slot->timestamp_pool, timing_index * 2u);
    }
    if (!vkr_bindless_vk_record_graph_pass_barriers(renderer, command, pass)) {
      log_error("Bindless Vulkan failed to record barriers for pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (!vkr_bindless_vk_record_graph_pass(renderer, command, pass)) {
      log_error("Bindless Vulkan failed to record pass '%.*s'",
                (int)pass->desc.name.length, pass->desc.name.str);
      return false_v;
    }
    if (timestamp_pass) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                           slot->timestamp_pool, timing_index * 2u + 1u);
    }
    if (slot->pass_timing_count < VKR_RENDERER_IMPL_MAX_PASS_TIMINGS) {
      VkrRendererImplPassTiming *timing =
          &slot->pass_timings[slot->pass_timing_count++];
      MemZero(timing, sizeof(*timing));
      const uint64_t length =
          Min(pass->desc.name.length,
              (uint64_t)VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY - 1u);
      if (length > 0u)
        MemCopy(timing->name, pass->desc.name.str, length);
      timing->name[length] = '\0';
      timing->pass_index = pass_index;
      timing->cpu_ms = (vkr_platform_get_absolute_time() - cpu_begin) * 1000.0;
      timing->valid = false_v;
    }
  }
  slot->timestamp_query_count =
      slot->timing_requested ? slot->pass_timing_count * 2u : 0u;
  return vkr_bindless_vk_record_graph_image_barriers(
      renderer, command, &renderer->graph->terminal_image_barriers);
}

static bool8_t vkr_bindless_vk_record_ibl_validation_capture(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command,
    VkrBindlessVkFrameSlot *slot) {
  slot->ibl_capture_pending = false_v;
  slot->ibl_capture_mip_count = 0u;
  VkrBindlessVkPendingIblBake *captured = NULL;
  for (uint32_t i = 0u; i < renderer->pending_ibl_bake_count; ++i) {
    if (renderer->pending_ibl_bakes[i].recorded) {
      captured = &renderer->pending_ibl_bakes[i];
      break;
    }
  }
  if (!captured)
    return true_v;
  VkrBindlessVkPublishedTexture *irradiance =
      vkr_bindless_vk_texture_publication(renderer, captured->irradiance);
  VkrBindlessVkPublishedTexture *prefilter =
      vkr_bindless_vk_texture_publication(renderer, captured->prefilter);
  if (!irradiance || !prefilter ||
      prefilter->image.mip_levels > VKR_IBL_PREFILTER_MIP_COUNT)
    return false_v;

  vkr_bindless_vk_cmd_ibl_image_barrier(
      command, irradiance->image.handle, 0u, irradiance->image.array_layers,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
      VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy2 region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .bufferOffset = VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .mipLevel = 0u,
                           .baseArrayLayer = 0u,
                           .layerCount = 1u},
      .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
  };
  VkCopyImageToBufferInfo2 copy = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = irradiance->image.handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstBuffer = slot->readback.handle,
      .regionCount = 1u,
      .pRegions = &region,
  };
  vkCmdCopyImageToBuffer2(command, &copy);
  vkr_bindless_vk_cmd_ibl_image_barrier(
      command, irradiance->image.handle, 0u, irradiance->image.array_layers,
      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
      VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      VK_IMAGE_LAYOUT_GENERAL);

  for (uint32_t mip = 0u; mip < prefilter->image.mip_levels; ++mip) {
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, prefilter->image.handle, mip, prefilter->image.array_layers,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    region.bufferOffset =
        VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET +
        (uint64_t)(mip + 1u) * VKR_BINDLESS_VK_IBL_TEXEL_BYTES;
    region.imageSubresource.mipLevel = mip;
    copy.srcImage = prefilter->image.handle;
    vkCmdCopyImageToBuffer2(command, &copy);
    vkr_bindless_vk_cmd_ibl_image_barrier(
        command, prefilter->image.handle, mip, prefilter->image.array_layers,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
  }
  slot->ibl_capture_mip_count = prefilter->image.mip_levels;
  slot->ibl_capture_pending = true_v;
  return true_v;
}

static uint64_t vkr_bindless_vk_capture_align(uint64_t value) {
  return (value + VKR_CAPTURE_BUFFER_ALIGNMENT - 1u) &
         ~(uint64_t)(VKR_CAPTURE_BUFFER_ALIGNMENT - 1u);
}

static bool8_t
vkr_bindless_vk_capture_source(const VkrCaptureChannelDescription *channel,
                               const VkrRenderPacket *packet,
                               const char **out_name, uint32_t *out_layer) {
  const char *name = channel->source_name;
  uint32_t layer = 0u;
  if (string_equals(channel->name, "scene_color")) {
    name = packet->frame.editor_enabled ? "scene_color" : "hdr_scene_color";
  } else if (string_equals(channel->name, "depth")) {
    name = packet->frame.editor_enabled ? "scene_depth" : "swapchain_depth";
  } else if (string_n_equals(channel->name, "shadow_cascade_", 15u)) {
    const char suffix = channel->name[15];
    if (suffix < '0' || suffix > '3')
      return false_v;
    layer = (uint32_t)(suffix - '0');
  } else if (string_equals(channel->name, "picking_ids") &&
             (!packet->picking || !packet->picking->pending)) {
    return false_v;
  }
  *out_name = name;
  *out_layer = layer;
  return true_v;
}

static bool8_t vkr_bindless_vk_plan_capture(VkrBindlessVulkanRenderer *renderer,
                                            const VkrRenderPacket *packet,
                                            VkrBindlessVkFrameSlot *slot) {
  slot->capture_request_id = 0u;
  slot->capture_item_count = 0u;
  const VkrCaptureBatchRequest *request =
      packet->debug ? packet->debug->capture : NULL;
  if (!request)
    return true_v;
  if (!renderer->capture_ring.initialized || request->request_id == 0u ||
      !request->items || request->item_count == 0u ||
      request->item_count > VKR_CAPTURE_MAX_ITEMS)
    return false_v;

  uint64_t seen = 0u;
  uint64_t offset = 0u;
  for (uint32_t i = 0; i < request->item_count; ++i) {
    const VkrCaptureItemRequest *item = &request->items[i];
    const VkrCaptureChannelDescription *channel =
        vkr_renderer_capture_channel_get(item->channel);
    if (!channel || item->channel >= 64u || (seen & (1ull << item->channel)) ||
        item->mip != 0u || item->layer != 0u)
      return false_v;
    seen |= 1ull << item->channel;

    const char *source_name = NULL;
    uint32_t source_layer = 0u;
    if (!vkr_bindless_vk_capture_source(channel, packet, &source_name,
                                        &source_layer))
      return false_v;
    const String8 graph_name = string8_create_from_cstr(
        (const uint8_t *)source_name, string_length(source_name));
    const VkrRgImageHandle handle =
        vkr_rg_find_image(renderer->graph, graph_name);
    VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
        renderer, handle, renderer->prepared_frame.image_index);
    if (!instance || source_layer >= instance->image.array_layers)
      return false_v;

    const VkrRgImage *graph_image =
        vector_get_VkrRgImage(&renderer->graph->images, handle.id - 1u);
    VkrTextureFormatInfo format_info = {0};
    if (!graph_image ||
        !vkr_texture_format_get_info(graph_image->desc.format, &format_info) ||
        format_info.block_width != 1u || format_info.block_height != 1u ||
        format_info.bytes_per_block == 0u)
      return false_v;
    const uint64_t row_pitch =
        (uint64_t)instance->image.width * format_info.bytes_per_block;
    const uint64_t data_size = row_pitch * instance->image.height;
    offset = vkr_bindless_vk_capture_align(offset);
    if (data_size == 0u || offset > UINT64_MAX - data_size ||
        offset + data_size > renderer->config.capture_max_batch_bytes)
      return false_v;

    slot->capture_images[i] = handle;
    slot->capture_plans[i] = (VkrCaptureBackendItemPlan){
        .result = {.channel = item->channel,
                   .width = instance->image.width,
                   .height = instance->image.height,
                   .row_pitch = row_pitch,
                   .format = graph_image->desc.format,
                   .value_kind = channel->value_kind,
                   .color_space = channel->color_space,
                   .origin = VKR_CAPTURE_ORIGIN_TOP_LEFT,
                   .data_size = data_size,
                   .mip = 0u,
                   .layer = source_layer,
                   .display_exposure = packet->globals.exposure},
        .buffer_offset = offset,
    };
    string_format(slot->capture_plans[i].result.producer_resource,
                  sizeof(slot->capture_plans[i].result.producer_resource), "%s",
                  source_name);
    offset += data_size;
  }
  const VkrRendererError reserve =
      vkr_capture_ring_reserve(&renderer->capture_ring, request,
                               slot->capture_plans, packet->frame.frame_index);
  if (reserve != VKR_RENDERER_ERROR_NONE) {
    log_error("Bindless Vulkan capture reservation %llu failed (%u)",
              (unsigned long long)request->request_id, reserve);
    return false_v;
  }
  slot->capture_request_id = request->request_id;
  slot->capture_item_count = request->item_count;
  return true_v;
}

static bool8_t
vkr_bindless_vk_record_capture(VkrBindlessVulkanRenderer *renderer,
                               VkCommandBuffer command,
                               VkrBindlessVkFrameSlot *slot) {
  if (slot->capture_request_id == 0u)
    return true_v;
  for (uint32_t i = 0; i < slot->capture_item_count; ++i) {
    const VkrRgImageHandle handle = slot->capture_images[i];
    VkrBindlessVkGraphImageInstance *instance = vkr_bindless_vk_graph_image(
        renderer, handle, renderer->prepared_frame.image_index);
    const VkrRgImage *graph_image =
        vkr_rg_image_handle_valid(handle)
            ? vector_get_VkrRgImage(&renderer->graph->images, handle.id - 1u)
            : NULL;
    if (!instance || !graph_image ||
        graph_image->final_layout == VKR_TEXTURE_LAYOUT_UNDEFINED)
      return false_v;
    const VkrCaptureBackendItemPlan *plan = &slot->capture_plans[i];
    const VkImageAspectFlags aspects =
        vkr_bindless_vk_format_aspects(instance->image.format);
    const VkImageLayout old_layout =
        vkr_bindless_vk_texture_layout(graph_image->final_layout);
    if (old_layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      const VkImageMemoryBarrier2 barrier = {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
          .srcAccessMask =
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
          .oldLayout = old_layout,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = instance->image.handle,
          .subresourceRange = {.aspectMask = aspects,
                               .baseMipLevel = plan->result.mip,
                               .levelCount = 1u,
                               .baseArrayLayer = plan->result.layer,
                               .layerCount = 1u},
      };
      const VkDependencyInfo dependency = {
          .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
          .imageMemoryBarrierCount = 1u,
          .pImageMemoryBarriers = &barrier,
      };
      vkCmdPipelineBarrier2(command, &dependency);
    }
    const VkBufferImageCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = plan->buffer_offset,
        .imageSubresource = {.aspectMask = aspects,
                             .mipLevel = plan->result.mip,
                             .baseArrayLayer = plan->result.layer,
                             .layerCount = 1u},
        .imageExtent = {.width = plan->result.width,
                        .height = plan->result.height,
                        .depth = 1u},
    };
    const VkCopyImageToBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = instance->image.handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstBuffer = slot->capture_readback.handle,
        .regionCount = 1u,
        .pRegions = &region,
    };
    vkCmdCopyImageToBuffer2(command, &copy);
  }
  const VkBufferMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->capture_readback.handle,
      .size = VK_WHOLE_SIZE,
  };
  const VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command, &dependency);
  return true_v;
}

static void
vkr_bindless_vk_destroy_target_set(VkrBindlessVulkanRenderer *renderer,
                                   VkrBindlessVkTargetSet *targets) {
  for (uint32_t i = 0; i < targets->image_count; ++i) {
    vkr_bindless_vk_destroy_image(renderer, &targets->images[i]);
  }
  MemZero(targets, sizeof(*targets));
}

static bool8_t vkr_bindless_vk_create_target_set(
    VkrBindlessVulkanRenderer *renderer, uint32_t width, uint32_t height,
    uint32_t image_count, VkrBindlessVkTargetSet *out_targets) {
  if (!width || !height || !image_count ||
      image_count > VKR_BINDLESS_VK_TARGET_IMAGE_MAX) {
    return false_v;
  }
  MemZero(out_targets, sizeof(*out_targets));
  out_targets->width = width;
  out_targets->height = height;
  out_targets->image_count = image_count;
  for (uint32_t i = 0; i < image_count; ++i) {
    if (!vkr_bindless_vk_create_image(renderer, width, height,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      &out_targets->images[i])) {
      vkr_bindless_vk_destroy_target_set(renderer, out_targets);
      return false_v;
    }
  }
  return true_v;
}

static void
vkr_bindless_vk_destroy_window_target(VkrBindlessVulkanRenderer *renderer,
                                      VkrBindlessVkWindowTarget *target) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0; i < target->image_count; ++i) {
    if (target->render_complete[i])
      vkDestroySemaphore(device, target->render_complete[i], NULL);
    if (target->present_complete[i])
      vkDestroyFence(device, target->present_complete[i], NULL);
  }
  if (target->swapchain)
    vkDestroySwapchainKHR(device, target->swapchain, NULL);
  MemZero(target, sizeof(*target));
}

static bool8_t
vkr_bindless_vk_window_presents_complete(VkrBindlessVulkanRenderer *renderer,
                                         VkrBindlessVkWindowTarget *target,
                                         bool8_t wait) {
  if (!vkr_bindless_vulkan_device_present_fences_enabled(renderer->device))
    return true_v;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0u; i < target->image_count; ++i) {
    if (!target->present_fence_pending[i])
      continue;
    const VkResult result =
        wait ? vkWaitForFences(device, 1u, &target->present_complete[i],
                               VK_TRUE, UINT64_MAX)
             : vkGetFenceStatus(device, target->present_complete[i]);
    if (result == VK_NOT_READY)
      return false_v;
    if (result != VK_SUCCESS) {
      log_error("Bindless Vulkan present-fence completion failed: %d", result);
      return false_v;
    }
  }
  return true_v;
}

static void vkr_bindless_vk_collect_retired_window_targets(
    VkrBindlessVulkanRenderer *renderer, uint64_t completed_submit_value) {
  const VkrBindlessVulkanReacquireResult reacquire =
      vkr_bindless_vulkan_reacquire_complete(
          &renderer->window_target.reacquire_state, completed_submit_value);
  if (reacquire.image_present_complete)
    renderer->wsi_reacquire_proofs++;
  if (!renderer->window_target.reacquire_state.successor_present_complete)
    return;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    VkrBindlessVkRetiredWindowTarget *retired =
        &renderer->retired_window_targets[i];
    if (retired->occupied && vkr_bindless_vk_window_presents_complete(
                                 renderer, &retired->target, false_v)) {
      vkr_bindless_vk_destroy_window_target(renderer, &retired->target);
      retired->occupied = false_v;
      renderer->wsi_retired_swapchains_collected++;
    }
  }
}

static VkSurfaceFormatKHR
vkr_bindless_vk_choose_surface_format(const VkSurfaceFormatKHR *formats,
                                      uint32_t count) {
  if (count == 1u && formats[0].format == VK_FORMAT_UNDEFINED)
    return (VkSurfaceFormatKHR){VK_FORMAT_B8G8R8A8_SRGB,
                                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
  for (uint32_t i = 0; i < count; ++i) {
    if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return formats[i];
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return formats[i];
  }
  return formats[0];
}

static VkPresentModeKHR
vkr_bindless_vk_choose_present_mode(const VkPresentModeKHR *modes,
                                    uint32_t count,
                                    VkrPresentMode requested_mode) {
  VkPresentModeKHR requested = VK_PRESENT_MODE_FIFO_KHR;
  if (requested_mode == VKR_PRESENT_MODE_IMMEDIATE)
    requested = VK_PRESENT_MODE_IMMEDIATE_KHR;
  else if (requested_mode == VKR_PRESENT_MODE_MAILBOX ||
           requested_mode == VKR_PRESENT_MODE_DEFAULT)
    requested = VK_PRESENT_MODE_MAILBOX_KHR;
  for (uint32_t i = 0; i < count; ++i) {
    if (modes[i] == requested)
      return modes[i];
  }
  return VK_PRESENT_MODE_FIFO_KHR;
}

static bool8_t vkr_bindless_vk_create_window_target(
    VkrBindlessVulkanRenderer *renderer, uint32_t requested_width,
    uint32_t requested_height, uint32_t requested_image_count,
    VkSwapchainKHR old_swapchain, VkrBindlessVkWindowTarget *out_target) {
  VkPhysicalDevice physical =
      vkr_bindless_vulkan_device_physical(renderer->device);
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkSurfaceKHR surface = vkr_bindless_vulkan_device_surface(renderer->device);
  VkSurfaceCapabilitiesKHR capabilities = {0};
  uint32_t format_count = 0, mode_count = 0;
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface,
                                                &capabilities) != VK_SUCCESS ||
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                           NULL) != VK_SUCCESS ||
      !format_count || format_count > 64u ||
      vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
                                                NULL) != VK_SUCCESS ||
      !mode_count || mode_count > 64u)
    return false_v;
  VkSurfaceFormatKHR formats[64];
  VkPresentModeKHR modes[64];
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                           formats) != VK_SUCCESS ||
      vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &mode_count,
                                                modes) != VK_SUCCESS)
    return false_v;

  const VkSurfaceFormatKHR surface_format =
      vkr_bindless_vk_choose_surface_format(formats, format_count);
  VkFormatProperties source_properties = {0}, target_properties = {0};
  vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM,
                                      &source_properties);
  vkGetPhysicalDeviceFormatProperties(physical, surface_format.format,
                                      &target_properties);
  if ((source_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_SRC_BIT) == 0u ||
      (target_properties.optimalTilingFeatures &
       VK_FORMAT_FEATURE_BLIT_DST_BIT) == 0u)
    return false_v;
  VkExtent2D extent = capabilities.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent.width = Clamp(requested_width, capabilities.minImageExtent.width,
                         capabilities.maxImageExtent.width);
    extent.height = Clamp(requested_height, capabilities.minImageExtent.height,
                          capabilities.maxImageExtent.height);
  }
  if (!extent.width || !extent.height)
    return false_v;
  if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ==
      0u)
    return false_v;
  uint32_t minimum_count =
      Max(requested_image_count, capabilities.minImageCount);
  if (capabilities.maxImageCount)
    minimum_count = Min(minimum_count, capabilities.maxImageCount);
  VkCompositeAlphaFlagBitsKHR composite_alpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  const VkCompositeAlphaFlagBitsKHR composite_candidates[] = {
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
  };
  for (uint32_t i = 0; i < ArrayCount(composite_candidates); ++i) {
    if (capabilities.supportedCompositeAlpha & composite_candidates[i]) {
      composite_alpha = composite_candidates[i];
      break;
    }
  }
  VkSwapchainCreateInfoKHR create_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = surface,
      .minImageCount = minimum_count,
      .imageFormat = surface_format.format,
      .imageColorSpace = surface_format.colorSpace,
      .imageExtent = extent,
      .imageArrayLayers = 1u,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = capabilities.currentTransform,
      .compositeAlpha = composite_alpha,
      .presentMode = vkr_bindless_vk_choose_present_mode(
          modes, mode_count, renderer->config.requested_present_mode),
      .clipped = VK_TRUE,
      .oldSwapchain = old_swapchain,
  };
  MemZero(out_target, sizeof(*out_target));
  if (vkCreateSwapchainKHR(device, &create_info, NULL,
                           &out_target->swapchain) != VK_SUCCESS)
    return false_v;
  uint32_t actual_count = 0;
  if (vkGetSwapchainImagesKHR(device, out_target->swapchain, &actual_count,
                              NULL) != VK_SUCCESS ||
      !actual_count || actual_count > VKR_BINDLESS_VK_SWAPCHAIN_IMAGE_MAX) {
    vkr_bindless_vk_destroy_window_target(renderer, out_target);
    return false_v;
  }
  out_target->image_count = actual_count;
  if (vkGetSwapchainImagesKHR(device, out_target->swapchain, &actual_count,
                              out_target->images) != VK_SUCCESS) {
    vkr_bindless_vk_destroy_window_target(renderer, out_target);
    return false_v;
  }
  out_target->width = extent.width;
  out_target->height = extent.height;
  out_target->format = surface_format.format;
  out_target->color_space = surface_format.colorSpace;
  out_target->present_mode = create_info.presentMode;
  for (uint32_t i = 0; i < actual_count; ++i) {
    VkSemaphoreCreateInfo semaphore_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(device, &semaphore_info, NULL,
                          &out_target->render_complete[i]) != VK_SUCCESS) {
      vkr_bindless_vk_destroy_window_target(renderer, out_target);
      return false_v;
    }
    if (vkr_bindless_vulkan_device_present_fences_enabled(renderer->device)) {
      const VkFenceCreateInfo fence_info = {
          .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
      };
      if (vkCreateFence(device, &fence_info, NULL,
                        &out_target->present_complete[i]) != VK_SUCCESS) {
        vkr_bindless_vk_destroy_window_target(renderer, out_target);
        return false_v;
      }
    }
  }
  out_target->occupied = true_v;
  return true_v;
}

static bool8_t
vkr_bindless_vk_create_acquire_semaphores(VkrBindlessVulkanRenderer *renderer) {
  VkSemaphoreCreateInfo info = {.sType =
                                    VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < ArrayCount(renderer->acquire_semaphores); ++i) {
    if (vkCreateSemaphore(vkr_bindless_vk_renderer_device(renderer), &info,
                          NULL, &renderer->acquire_semaphores[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

static void
vkr_bindless_vk_collect_retired_targets(VkrBindlessVulkanRenderer *renderer,
                                        uint64_t completed_value) {
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
    VkrBindlessVkRetiredTargetSet *retired = &renderer->retired_targets[i];
    if (retired->occupied && retired->retire_value <= completed_value) {
      vkr_bindless_vk_destroy_target_set(renderer, &retired->targets);
      MemZero(retired, sizeof(*retired));
    }
  }
}

static bool8_t
vkr_bindless_vk_create_frame_slots(VkrBindlessVulkanRenderer *renderer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  const VkDeviceSize readback_size =
      VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET +
      (1u + VKR_IBL_PREFILTER_MIP_COUNT) * VKR_BINDLESS_VK_IBL_TEXEL_BYTES;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex =
            vkr_bindless_vulkan_device_queue_family(renderer->device),
    };
    if (vkCreateCommandPool(device, &pool_info, NULL, &slot->command_pool) !=
        VK_SUCCESS) {
      return false_v;
    }
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = slot->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1u,
    };
    const VkQueryPoolCreateInfo query_info = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = VKR_RENDERER_IMPL_MAX_PASS_TIMINGS * 2u,
    };
    if (vkAllocateCommandBuffers(device, &command_info,
                                 &slot->command_buffer) != VK_SUCCESS ||
        vkCreateQueryPool(device, &query_info, NULL, &slot->timestamp_pool) !=
            VK_SUCCESS ||
        !vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_READBACK, readback_size,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot->readback) ||
        !vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD,
            VKR_BINDLESS_VK_FRAME_UPLOAD_SIZE,
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            &slot->frame_upload) ||
        (renderer->config.capture_ring_capacity > 0u &&
         !vkr_bindless_vk_create_buffer(
             renderer, VKR_BINDLESS_VK_MEMORY_CLASS_READBACK,
             renderer->config.capture_max_batch_bytes,
             VK_BUFFER_USAGE_TRANSFER_DST_BIT, &slot->capture_readback))) {
      return false_v;
    }
  }
  return true_v;
}

static void
vkr_bindless_vk_destroy_frame_slots(VkrBindlessVulkanRenderer *renderer) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    vkr_bindless_vk_destroy_buffer(renderer, &slot->frame_upload);
    vkr_bindless_vk_destroy_buffer(renderer, &slot->capture_readback);
    vkr_bindless_vk_destroy_buffer(renderer, &slot->readback);
    if (slot->timestamp_pool)
      vkDestroyQueryPool(device, slot->timestamp_pool, NULL);
    if (slot->command_pool) {
      vkDestroyCommandPool(device, slot->command_pool, NULL);
    }
    MemZero(slot, sizeof(*slot));
  }
}

static bool8_t
vkr_bindless_vk_write_upload_data(VkrBindlessVulkanRenderer *renderer) {
  uint8_t *mapped = renderer->upload.allocation.mapped;
  MemZero(mapped, renderer->upload.size);
  const VkrVertex3d vertices[] = {
      {.position = {-1.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {3.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 2.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {-1.0f, 3.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 2.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
  };
  const uint16_t indices[] = {0u, 1u, 2u};
  VkrBindlessVkDrawRoot root = {
      .vertices = renderer->upload.address + VKR_BINDLESS_VK_VERTEX_OFFSET,
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .texture_index = 0u,
      .sampler_index = 0u,
      .materials = renderer->materials.address,
  };
  root.transform[0] = 1.0f;
  root.transform[5] = 1.0f;
  root.transform[10] = 1.0f;
  root.transform[15] = 1.0f;
  const uint8_t sentinel_pixel[] = {37u, 91u, 173u, 255u};
  MemCopy(mapped + VKR_BINDLESS_VK_ROOT_OFFSET, &root, sizeof(root));
  MemCopy(mapped + VKR_BINDLESS_VK_VERTEX_OFFSET, vertices, sizeof(vertices));
  MemCopy(mapped + VKR_BINDLESS_VK_INDEX_OFFSET, indices, sizeof(indices));
  MemCopy(mapped + VKR_BINDLESS_VK_TEXTURE_OFFSET, sentinel_pixel,
          sizeof(sentinel_pixel));
  return vkr_bindless_vk_flush(renderer, &renderer->upload.allocation, 0u,
                               VKR_BINDLESS_VK_TEXTURE_OFFSET +
                                   sizeof(sentinel_pixel));
}

static bool8_t
vkr_bindless_vk_create_resources(VkrBindlessVulkanRenderer *renderer) {
  if (!vkr_bindless_vk_create_upload_buffers(renderer)) {
    log_error("Bindless Vulkan failed to create upload buffers");
    return false_v;
  }
  if (!vkr_bindless_vk_create_image(renderer, 1u, 1u,
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_STORAGE_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    &renderer->sentinel_image)) {
    log_error("Bindless Vulkan failed to create the sentinel image");
    return false_v;
  }
  if (!vkr_bindless_vk_create_target_set(
          renderer, renderer->config.width, renderer->config.height,
          renderer->config.image_count, &renderer->targets)) {
    log_error("Bindless Vulkan failed to create render targets");
    return false_v;
  }
  if (!vkr_bindless_vk_create_frame_slots(renderer)) {
    log_error("Bindless Vulkan failed to create frame slots");
    return false_v;
  }
  const VkDeviceSize descriptor_alignment =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device)
          ->descriptorBufferOffsetAlignment;
  if ((renderer->resource_descriptors.address % descriptor_alignment) != 0u ||
      (renderer->sampler_descriptors.address % descriptor_alignment) != 0u ||
      !vkr_bindless_vk_write_upload_data(renderer)) {
    log_error("Bindless Vulkan descriptor alignment or initial upload failed");
    return false_v;
  }
  VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_NEAREST,
      .minFilter = VK_FILTER_NEAREST,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .maxLod = 0.0f,
  };
  if (vkCreateSampler(vkr_bindless_vk_renderer_device(renderer), &sampler_info,
                      NULL, &renderer->sentinel_sampler) != VK_SUCCESS) {
    log_error("Bindless Vulkan failed to create the sentinel sampler");
    return false_v;
  }
  return true_v;
}

static bool8_t vkr_bindless_vk_create_descriptor_slot_tables(
    VkrBindlessVulkanRenderer *renderer) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  if (!properties->sampledImageDescriptorSize ||
      !properties->storageImageDescriptorSize ||
      !properties->samplerDescriptorSize ||
      properties->sampledImageDescriptorSize > UINT32_MAX ||
      properties->storageImageDescriptorSize > UINT32_MAX ||
      properties->samplerDescriptorSize > UINT32_MAX) {
    log_error("Bindless Vulkan descriptor row size is not representable");
    return false_v;
  }
  const VkrGpuSlotTableConfig sampled_config = {
      .max_slots = renderer->config.sampled_image_capacity,
      .max_retirements = renderer->config.sampled_image_capacity,
      .row_size = (uint32_t)properties->sampledImageDescriptorSize,
  };
  const VkrGpuSlotTableConfig sampler_config = {
      .max_slots = renderer->config.sampler_capacity,
      .max_retirements = renderer->config.sampler_capacity,
      .row_size = (uint32_t)properties->samplerDescriptorSize,
  };
  const VkrGpuSlotTableConfig storage_config = {
      .max_slots = renderer->config.storage_image_capacity,
      .max_retirements = renderer->config.storage_image_capacity,
      .row_size = (uint32_t)properties->storageImageDescriptorSize,
  };
  const VkrGpuSlotTableConfig material_config = {
      .max_slots = renderer->config.material_slot_capacity,
      .max_retirements = renderer->config.material_slot_capacity,
      .row_size = sizeof(VkrBindlessVkMaterialGpuRow),
  };
  renderer->sampled_image_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&sampled_config);
  renderer->sampler_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&sampler_config);
  renderer->storage_image_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&storage_config);
  renderer->material_slot_storage_size =
      vkr_gpu_slot_table_storage_requirement(&material_config);
  renderer->published_geometries_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->published_geometries);
  renderer->retired_geometries_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->retired_geometries);
  renderer->published_textures_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->published_textures);
  renderer->retired_textures_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->retired_textures);
  renderer->published_samplers_size =
      (uint64_t)renderer->config.sampler_capacity *
      sizeof(*renderer->published_samplers);
  renderer->published_materials_size =
      (uint64_t)renderer->config.material_record_capacity *
      sizeof(*renderer->published_materials);
  renderer->retired_materials_size =
      (uint64_t)renderer->config.material_record_capacity *
      sizeof(*renderer->retired_materials);
  renderer->pending_texture_initializations_size =
      (uint64_t)renderer->config.sampled_image_capacity *
      sizeof(*renderer->pending_texture_initializations);
  renderer->pending_buffer_initialization_capacity =
      renderer->config.publication_staging_capacity;
  renderer->pending_buffer_initializations_size =
      (uint64_t)renderer->pending_buffer_initialization_capacity *
      sizeof(*renderer->pending_buffer_initializations);
  renderer->retired_staging_buffer_capacity =
      renderer->config.publication_staging_capacity;
  renderer->retired_staging_buffers_size =
      (uint64_t)renderer->retired_staging_buffer_capacity *
      sizeof(*renderer->retired_staging_buffers);
  renderer->descriptor_scratch_size =
      (uint32_t)Max(Max(properties->sampledImageDescriptorSize,
                        properties->storageImageDescriptorSize),
                    properties->samplerDescriptorSize);
  renderer->sampled_image_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->sampled_image_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->sampler_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->sampler_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->storage_image_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->storage_image_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->material_slot_storage = vkr_allocator_alloc(
      renderer->allocator, renderer->material_slot_storage_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->descriptor_scratch = vkr_allocator_alloc(
      renderer->allocator, renderer->descriptor_scratch_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_geometries = vkr_allocator_alloc(
      renderer->allocator, renderer->published_geometries_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_geometries = vkr_allocator_alloc(
      renderer->allocator, renderer->retired_geometries_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_textures = vkr_allocator_alloc(
      renderer->allocator, renderer->published_textures_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_textures =
      vkr_allocator_alloc(renderer->allocator, renderer->retired_textures_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_samplers = vkr_allocator_alloc(
      renderer->allocator, renderer->published_samplers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->published_materials = vkr_allocator_alloc(
      renderer->allocator, renderer->published_materials_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_materials =
      vkr_allocator_alloc(renderer->allocator, renderer->retired_materials_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->pending_texture_initializations = vkr_allocator_alloc(
      renderer->allocator, renderer->pending_texture_initializations_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->pending_buffer_initializations = vkr_allocator_alloc(
      renderer->allocator, renderer->pending_buffer_initializations_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->retired_staging_buffers = vkr_allocator_alloc(
      renderer->allocator, renderer->retired_staging_buffers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer->sampled_image_slot_storage ||
      !renderer->storage_image_slot_storage ||
      !renderer->sampler_slot_storage || !renderer->material_slot_storage ||
      !renderer->descriptor_scratch || !renderer->published_geometries ||
      !renderer->retired_geometries || !renderer->published_textures ||
      !renderer->retired_textures || !renderer->published_samplers ||
      !renderer->published_materials || !renderer->retired_materials ||
      !renderer->pending_texture_initializations ||
      !renderer->pending_buffer_initializations ||
      !renderer->retired_staging_buffers) {
    return false_v;
  }
  MemZero(renderer->published_geometries, renderer->published_geometries_size);
  MemZero(renderer->retired_geometries, renderer->retired_geometries_size);
  MemZero(renderer->published_textures, renderer->published_textures_size);
  MemZero(renderer->retired_textures, renderer->retired_textures_size);
  MemZero(renderer->published_samplers, renderer->published_samplers_size);
  MemZero(renderer->published_materials, renderer->published_materials_size);
  MemZero(renderer->retired_materials, renderer->retired_materials_size);
  MemZero(renderer->pending_texture_initializations,
          renderer->pending_texture_initializations_size);
  MemZero(renderer->pending_buffer_initializations,
          renderer->pending_buffer_initializations_size);
  MemZero(renderer->retired_staging_buffers,
          renderer->retired_staging_buffers_size);
  return vkr_gpu_slot_table_create(
             &sampled_config, renderer->sampled_image_slot_storage,
             renderer->sampled_image_slot_storage_size,
             (uint8_t *)renderer->resource_descriptors.allocation.mapped +
                 resource_layout->sampled_image_offset,
             &renderer->sampled_image_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &sampler_config, renderer->sampler_slot_storage,
             renderer->sampler_slot_storage_size,
             (uint8_t *)renderer->sampler_descriptors.allocation.mapped +
                 sampler_layout->sampler_offset,
             &renderer->sampler_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &storage_config, renderer->storage_image_slot_storage,
             renderer->storage_image_slot_storage_size,
             (uint8_t *)renderer->resource_descriptors.allocation.mapped +
                 resource_layout->storage_image_offset,
             &renderer->storage_image_slots) == VKR_GPU_SLOT_STATUS_OK &&
         vkr_gpu_slot_table_create(
             &material_config, renderer->material_slot_storage,
             renderer->material_slot_storage_size,
             renderer->materials.allocation.mapped,
             &renderer->material_slots) == VKR_GPU_SLOT_STATUS_OK;
}

static bool8_t vkr_bindless_vk_publish_sentinel_descriptors(
    VkrBindlessVulkanRenderer *renderer) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorImageInfo image_info = {
      .imageView = renderer->sentinel_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
  };
  VkDescriptorImageInfo storage_info = {
      .imageView = renderer->sentinel_image.view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  VkDescriptorGetInfoEXT image_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &image_info,
  };
  VkDescriptorGetInfoEXT sampler_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &renderer->sentinel_sampler,
  };
  VkDescriptorGetInfoEXT storage_get = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .data.pStorageImage = &storage_info,
  };
  PFN_vkGetDescriptorEXT get_descriptor =
      vkr_bindless_vulkan_device_get_descriptor(renderer->device);
  VkrGpuSlotHandle sampled_handle = {0};
  VkrGpuSlotHandle sampler_handle = {0};
  VkrGpuSlotHandle storage_handle = {0};
  VkrGpuSlotHandle material_handle = {0};
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &image_get,
                 properties->sampledImageDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampled_image_slots,
                                 renderer->descriptor_scratch,
                                 &sampled_handle) != VKR_GPU_SLOT_STATUS_OK ||
      sampled_handle.index != 0u) {
    return false_v;
  }
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &sampler_get,
                 properties->samplerDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampler_slots,
                                 renderer->descriptor_scratch,
                                 &sampler_handle) != VKR_GPU_SLOT_STATUS_OK ||
      sampler_handle.index != 0u) {
    return false_v;
  }
  get_descriptor(vkr_bindless_vk_renderer_device(renderer), &storage_get,
                 properties->storageImageDescriptorSize,
                 renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->storage_image_slots,
                                 renderer->descriptor_scratch,
                                 &storage_handle) != VKR_GPU_SLOT_STATUS_OK ||
      storage_handle.index != 0u) {
    return false_v;
  }
  const VkrBindlessVkMaterialGpuRow material = {
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .base_color_texture = sampled_handle.index,
      .normal_texture = sampled_handle.index,
      .orm_texture = sampled_handle.index,
      .emissive_texture = sampled_handle.index,
      .base_color_sampler = sampler_handle.index,
      .normal_sampler = sampler_handle.index,
      .orm_sampler = sampler_handle.index,
      .emissive_sampler = sampler_handle.index,
      .material_id = 0xffad5b25u,
  };
  if (vkr_gpu_slot_table_publish(renderer->material_slots, &material,
                                 &material_handle) != VKR_GPU_SLOT_STATUS_OK ||
      material_handle.index != 0u) {
    return false_v;
  }
  return vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                    &renderer->resource_descriptors,
                                    resource_layout->sampled_image_offset,
                                    properties->sampledImageDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                    &renderer->resource_descriptors,
                                    resource_layout->storage_image_offset,
                                    properties->storageImageDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->sampler_descriptor_dirty,
                                    &renderer->sampler_descriptors,
                                    sampler_layout->sampler_offset,
                                    properties->samplerDescriptorSize) &&
         vkr_bindless_vk_mark_dirty(&renderer->material_dirty,
                                    &renderer->materials, 0u, sizeof(material));
}

static SpvReflectBlockVariable *
vkr_bindless_vk_reflect_member(SpvReflectBlockVariable *parent,
                               const char *name) {
  if (!parent || !name)
    return NULL;
  for (uint32_t i = 0; i < parent->member_count; ++i) {
    SpvReflectBlockVariable *member = &parent->members[i];
    if (member->name && string_equals(member->name, name))
      return member;
  }
  return NULL;
}

static bool8_t
vkr_bindless_vk_reflect_member_offset(SpvReflectBlockVariable *parent,
                                      const char *name, uint32_t offset,
                                      SpvReflectBlockVariable **out_member) {
  SpvReflectBlockVariable *member =
      vkr_bindless_vk_reflect_member(parent, name);
  if (out_member)
    *out_member = member;
  if (!member || member->offset != offset) {
    log_error("Bindless Vulkan shader ABI member %s is %s (offset %u, "
              "expected %u)",
              name, member ? "misaligned" : "missing",
              member ? member->offset : UINT32_MAX, offset);
    return false_v;
  }
  return true_v;
}

static uint32_t
vkr_bindless_vk_reflected_struct_size(const SpvReflectBlockVariable *value) {
  uint32_t size = 0u;
  if (!value)
    return size;
  for (uint32_t i = 0; i < value->member_count; ++i) {
    const SpvReflectBlockVariable *member = &value->members[i];
    const uint32_t member_size = Max(member->size, member->padded_size);
    if (member->offset <= UINT32_MAX - member_size)
      size = Max(size, member->offset + member_size);
  }
  return size;
}

static bool8_t vkr_bindless_vk_validate_vertex_pointer_abi(
    SpvReflectBlockVariable *push_block) {
  if (!push_block || push_block->size != sizeof(VkrBindlessVkPushConstants)) {
    log_error("Bindless Vulkan vertex push block is %u bytes (expected %zu)",
              push_block ? push_block->size : 0u,
              sizeof(VkrBindlessVkPushConstants));
    return false_v;
  }
  SpvReflectBlockVariable *root = NULL;
  bool8_t valid = vkr_bindless_vk_reflect_member_offset(
      push_block, "root", offsetof(VkrBindlessVkPushConstants, root), &root);
  valid &= vkr_bindless_vk_reflect_member_offset(
      push_block, "material_index",
      offsetof(VkrBindlessVkPushConstants, material_index), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      push_block, "flags", offsetof(VkrBindlessVkPushConstants, flags), NULL);
  if (!root || root->member_count == 0u) {
    log_error("Bindless Vulkan reflection did not recurse through draw-root "
              "PhysicalStorageBuffer");
    return false_v;
  }

  SpvReflectBlockVariable *vertices = NULL;
  SpvReflectBlockVariable *materials = NULL;
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "vertices", offsetof(VkrBindlessVkDrawRoot, vertices), &vertices);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "tint", offsetof(VkrBindlessVkDrawRoot, tint), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "transform", offsetof(VkrBindlessVkDrawRoot, transform), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "texture_index", offsetof(VkrBindlessVkDrawRoot, texture_index),
      NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "sampler_index", offsetof(VkrBindlessVkDrawRoot, sampler_index),
      NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      root, "materials", offsetof(VkrBindlessVkDrawRoot, materials),
      &materials);
  if (!vertices || vertices->member_count == 0u || !materials ||
      materials->member_count == 0u) {
    log_error("Bindless Vulkan reflection did not recurse through vertex and "
              "material PhysicalStorageBuffer pointers");
    return false_v;
  }

  const VkrGpuAbiRecord *vertex_abi = vkr_gpu_abi_record(VKR_GPU_ABI_VERTEX);
  const uint32_t reflected_vertex_size =
      vkr_bindless_vk_reflected_struct_size(vertices);
  if (!vertex_abi || reflected_vertex_size != vertex_abi->expected_size) {
    log_error("Bindless Vulkan reflected vertex size is %u (expected %u)",
              reflected_vertex_size,
              vertex_abi ? vertex_abi->expected_size : 0u);
    valid = false_v;
  }
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "position", offsetof(VkrVertex3d, position), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "normal", offsetof(VkrVertex3d, normal), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "texcoord", offsetof(VkrVertex3d, texcoord), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "color", offsetof(VkrVertex3d, colour), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      vertices, "tangent", offsetof(VkrVertex3d, tangent), NULL);
  const uint32_t reflected_material_size =
      vkr_bindless_vk_reflected_struct_size(materials);
  if (reflected_material_size != sizeof(VkrBindlessVkMaterialGpuRow)) {
    log_error("Bindless Vulkan reflected material size is %u (expected "
              "%zu)",
              reflected_material_size, sizeof(VkrBindlessVkMaterialGpuRow));
    valid = false_v;
  }
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "tint", offsetof(VkrBindlessVkMaterialGpuRow, tint), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "base_color_texture",
      offsetof(VkrBindlessVkMaterialGpuRow, base_color_texture), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "base_color_sampler",
      offsetof(VkrBindlessVkMaterialGpuRow, base_color_sampler), NULL);
  valid &= vkr_bindless_vk_reflect_member_offset(
      materials, "material_id",
      offsetof(VkrBindlessVkMaterialGpuRow, material_id), NULL);
  return valid;
}

static bool8_t vkr_bindless_vk_validate_shader_module_abi(
    VkrBindlessVulkanRenderer *renderer, const char *path,
    const char *entry_point, SpvReflectShaderStageFlagBits stage) {
  FilePath shader_path =
      file_path_create(path, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u) {
    log_error("Bindless Vulkan could not load SPIR-V for ABI validation: %s",
              path);
    return false_v;
  }
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult create_result =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (create_result != SPV_REFLECT_RESULT_SUCCESS) {
    log_error("Bindless Vulkan SPIR-V reflection failed for %s (%d)", path,
              create_result);
    return false_v;
  }

  bool8_t valid = true_v;
  const SpvReflectEntryPoint *entry =
      spvReflectGetEntryPoint(&module, entry_point);
  if (!entry || entry->shader_stage != stage) {
    log_error("Bindless Vulkan entry point %s is missing or has wrong stage",
              entry_point);
    valid = false_v;
  }
  uint32_t push_count = 0u;
  SpvReflectBlockVariable *push_blocks[1] = {0};
  if (spvReflectEnumerateEntryPointPushConstantBlocks(&module, entry_point,
                                                      &push_count, NULL) !=
          SPV_REFLECT_RESULT_SUCCESS ||
      push_count != 1u ||
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, entry_point, &push_count, push_blocks) !=
          SPV_REFLECT_RESULT_SUCCESS) {
    log_error("Bindless Vulkan entry point %s has %u push blocks (expected 1)",
              entry_point, push_count);
    valid = false_v;
  } else if (stage == SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) {
    valid &= vkr_bindless_vk_validate_vertex_pointer_abi(push_blocks[0]);
  } else if (push_blocks[0]->size != sizeof(VkrBindlessVkPushConstants)) {
    log_error("Bindless Vulkan fragment push block is %u bytes (expected %zu)",
              push_blocks[0]->size, sizeof(VkrBindlessVkPushConstants));
    valid = false_v;
  }

  if (stage == SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT) {
    uint32_t binding_count = 0u;
    SpvReflectDescriptorBinding *bindings[4] = {0};
    if (spvReflectEnumerateEntryPointDescriptorBindings(&module, entry_point,
                                                        &binding_count, NULL) !=
            SPV_REFLECT_RESULT_SUCCESS ||
        binding_count > ArrayCount(bindings) ||
        spvReflectEnumerateEntryPointDescriptorBindings(
            &module, entry_point, &binding_count, bindings) !=
            SPV_REFLECT_RESULT_SUCCESS) {
      log_error("Bindless Vulkan fragment descriptor reflection failed");
      valid = false_v;
    } else {
      bool8_t sampled = false_v;
      bool8_t sampler = false_v;
      for (uint32_t i = 0; i < binding_count; ++i) {
        const SpvReflectDescriptorBinding *binding = bindings[i];
        const bool8_t runtime_array =
            binding->array.dims_count == 1u && binding->array.dims[0] == 0u;
        sampled |= binding->set == 0u && binding->binding == 0u &&
                   binding->descriptor_type ==
                       SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
                   runtime_array;
        sampler |=
            binding->set == 1u && binding->binding == 0u &&
            binding->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER &&
            runtime_array;
      }
      if (!sampled || !sampler) {
        log_error("Bindless Vulkan runtime descriptor arrays are incomplete "
                  "(sampled=%u sampler=%u)",
                  sampled, sampler);
        valid = false_v;
      }
    }
  }
  spvReflectDestroyShaderModule(&module);
  return valid;
}

static bool8_t
vkr_bindless_vk_validate_shader_abi(VkrBindlessVulkanRenderer *renderer) {
  return vkr_bindless_vk_validate_shader_module_abi(
             renderer, VKR_BINDLESS_VK_VERTEX_SPV, "vert_main",
             SPV_REFLECT_SHADER_STAGE_VERTEX_BIT) &&
         vkr_bindless_vk_validate_shader_module_abi(
             renderer, VKR_BINDLESS_VK_FRAGMENT_SPV, "frag_main",
             SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT);
}

static bool8_t
vkr_bindless_vk_validate_packet_root_abi(VkrBindlessVulkanRenderer *renderer) {
  FilePath shader_path =
      file_path_create(VKR_BINDLESS_VK_PACKET_TEXT_VERT_SPV,
                       renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0u;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0u)
    return false_v;
  SpvReflectShaderModule module;
  MemZero(&module, sizeof(module));
  const SpvReflectResult created =
      spvReflectCreateShaderModule((size_t)size, bytes, &module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  if (created != SPV_REFLECT_RESULT_SUCCESS)
    return false_v;
  uint32_t count = 0u;
  SpvReflectBlockVariable *blocks[1] = {0};
  bool8_t valid =
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, "text_vertex", &count, NULL) == SPV_REFLECT_RESULT_SUCCESS &&
      count == 1u &&
      spvReflectEnumerateEntryPointPushConstantBlocks(
          &module, "text_vertex", &count, blocks) == SPV_REFLECT_RESULT_SUCCESS;
  SpvReflectBlockVariable *root =
      valid ? vkr_bindless_vk_reflect_member(blocks[0], "root") : NULL;
  if (!root || root->member_count == 0u) {
    valid = false_v;
  } else {
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "vertices", offsetof(VkrBindlessVkPacketDrawRoot, vertices),
        NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "view_projection",
        offsetof(VkrBindlessVkPacketDrawRoot, view_projection), NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "transmission_texture",
        offsetof(VkrBindlessVkPacketDrawRoot, transmission_texture), NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "transmission_sampler",
        offsetof(VkrBindlessVkPacketDrawRoot, transmission_sampler), NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "first_instance",
        offsetof(VkrBindlessVkPacketDrawRoot, first_instance), NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "flags", offsetof(VkrBindlessVkPacketDrawRoot, flags), NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "material_flags",
        offsetof(VkrBindlessVkPacketDrawRoot, material_flags), NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "point_light_grid_origin_cell_size",
        offsetof(VkrBindlessVkPacketDrawRoot,
                 point_light_grid_origin_cell_size),
        NULL);
    valid &= vkr_bindless_vk_reflect_member_offset(
        root, "view", offsetof(VkrBindlessVkPacketDrawRoot, view), NULL);
  }
  spvReflectDestroyShaderModule(&module);
  return valid;
}

static bool8_t
vkr_bindless_vk_create_shader_module(VkrBindlessVulkanRenderer *renderer,
                                     const char *path,
                                     VkShaderModule *out_module) {
  FilePath shader_path =
      file_path_create(path, renderer->allocator, FILE_PATH_TYPE_ABSOLUTE);
  uint8_t *bytes = NULL;
  uint64_t size = 0;
  if (file_load_spirv_shader(&shader_path, renderer->allocator, &bytes,
                             &size) != FILE_ERROR_NONE ||
      size == 0 || (size % sizeof(uint32_t)) != 0) {
    return false_v;
  }
  VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)size,
      .pCode = (const uint32_t *)bytes,
  };
  const VkResult result =
      vkCreateShaderModule(vkr_bindless_vk_renderer_device(renderer),
                           &module_info, NULL, out_module);
  vkr_allocator_free(renderer->allocator, bytes, size,
                     VKR_ALLOCATOR_MEMORY_TAG_FILE);
  return result == VK_SUCCESS;
}

static bool8_t
vkr_bindless_vk_create_pipeline(VkrBindlessVulkanRenderer *renderer) {
  renderer->shader_abi_validated =
      vkr_bindless_vk_validate_shader_abi(renderer) &&
      vkr_bindless_vk_validate_packet_root_abi(renderer);
  if (!renderer->shader_abi_validated ||
      !vkr_bindless_vk_create_shader_module(
          renderer, VKR_BINDLESS_VK_VERTEX_SPV, &renderer->vertex_shader) ||
      !vkr_bindless_vk_create_shader_module(
          renderer, VKR_BINDLESS_VK_FRAGMENT_SPV, &renderer->fragment_shader)) {
    return false_v;
  }
  const VkrBindlessVulkanDescriptorLayout *resource_layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *sampler_layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  VkDescriptorSetLayout layouts[] = {
      resource_layout->handle,
      sampler_layout->handle,
  };
  VkPushConstantRange push_range = {
      .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                    VK_SHADER_STAGE_COMPUTE_BIT,
      .offset = 0u,
      .size = sizeof(VkrBindlessVkPushConstants),
  };
  VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = ArrayCount(layouts),
      .pSetLayouts = layouts,
      .pushConstantRangeCount = 1u,
      .pPushConstantRanges = &push_range,
  };
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  if (vkCreatePipelineLayout(device, &layout_info, NULL,
                             &renderer->pipeline_layout) != VK_SUCCESS) {
    return false_v;
  }
  VkPipelineShaderStageCreateInfo stages[] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = renderer->vertex_shader,
          .pName = "vert_main",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = renderer->fragment_shader,
          .pName = "frag_main",
      },
  };
  VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  VkPipelineViewportStateCreateInfo viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .lineWidth = 1.0f,
  };
  VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  VkPipelineColorBlendAttachmentState color_attachment = {
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1u,
      .pAttachments = &color_attachment,
  };
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = ArrayCount(dynamic_states),
      .pDynamicStates = dynamic_states,
  };
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1u,
      .pColorAttachmentFormats = &color_format,
  };
  VkGraphicsPipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering,
      .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .stageCount = ArrayCount(stages),
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = renderer->pipeline_layout,
  };
  if (vkCreateGraphicsPipelines(device, renderer->pipeline_cache, 1u,
                                &pipeline_info, NULL,
                                &renderer->pipeline) != VK_SUCCESS)
    return false_v;
  return vkr_bindless_vk_create_packet_pipelines(renderer) &&
         vkr_bindless_vk_create_ibl_pipelines(renderer);
}

static bool8_t vkr_bindless_vk_create_packet_pipeline(
    VkrBindlessVulkanRenderer *renderer, VkrBindlessVkPacketPipeline pipeline,
    VkrBindlessVkPacketShader vertex_shader,
    VkrBindlessVkPacketShader fragment_shader, VkFormat color_format,
    VkFormat depth_format, bool8_t depth_test, bool8_t depth_write,
    bool8_t blend_enabled, bool8_t depth_bias) {
  const VkPipelineShaderStageCreateInfo stages[] = {
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = renderer->packet_shaders[vertex_shader],
          .pName =
              vertex_shader == VKR_BINDLESS_VK_PACKET_SHADER_WORLD_VERTEX
                  ? "world_vertex"
              : vertex_shader == VKR_BINDLESS_VK_PACKET_SHADER_SHADOW_VERTEX
                  ? "shadow_vertex"
              : vertex_shader == VKR_BINDLESS_VK_PACKET_SHADER_TEXT_VERTEX
                  ? "text_vertex"
                  : "fullscreen_vertex",
      },
      {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = renderer->packet_shaders[fragment_shader],
          .pName =
              fragment_shader == VKR_BINDLESS_VK_PACKET_SHADER_WORLD_FRAGMENT
                  ? "world_fragment"
              : fragment_shader == VKR_BINDLESS_VK_PACKET_SHADER_SHADOW_FRAGMENT
                  ? "shadow_fragment"
              : fragment_shader ==
                      VKR_BINDLESS_VK_PACKET_SHADER_PICKING_FRAGMENT
                  ? "picking_fragment"
              : fragment_shader == VKR_BINDLESS_VK_PACKET_SHADER_TEXT_FRAGMENT
                  ? "text_fragment"
              : fragment_shader ==
                      VKR_BINDLESS_VK_PACKET_SHADER_TEXT_PICKING_FRAGMENT
                  ? "text_picking_fragment"
              : fragment_shader == VKR_BINDLESS_VK_PACKET_SHADER_SKYBOX_FRAGMENT
                  ? "skybox_fragment"
                  : "fullscreen_fragment",
      },
  };
  const VkPipelineVertexInputStateCreateInfo vertex_input = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
  };
  const VkPipelineInputAssemblyStateCreateInfo input_assembly = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
  };
  const VkPipelineViewportStateCreateInfo viewport = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1u,
      .scissorCount = 1u,
  };
  const VkPipelineRasterizationStateCreateInfo raster = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .cullMode = VK_CULL_MODE_NONE,
      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
      .depthBiasEnable = depth_bias,
      .lineWidth = 1.0f,
  };
  const VkPipelineMultisampleStateCreateInfo multisample = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
  };
  const VkPipelineDepthStencilStateCreateInfo depth_stencil = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth_test,
      .depthWriteEnable = depth_write,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
  };
  const VkPipelineColorBlendAttachmentState color_attachment = {
      .blendEnable = blend_enabled,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
  };
  const VkPipelineColorBlendStateCreateInfo blend = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = color_format != VK_FORMAT_UNDEFINED ? 1u : 0u,
      .pAttachments =
          color_format != VK_FORMAT_UNDEFINED ? &color_attachment : NULL,
  };
  const VkDynamicState dynamic_states[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_DEPTH_BIAS,
  };
  const VkPipelineDynamicStateCreateInfo dynamic = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = depth_bias ? ArrayCount(dynamic_states) : 2u,
      .pDynamicStates = dynamic_states,
  };
  const VkPipelineRenderingCreateInfo rendering = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = color_format != VK_FORMAT_UNDEFINED ? 1u : 0u,
      .pColorAttachmentFormats =
          color_format != VK_FORMAT_UNDEFINED ? &color_format : NULL,
      .depthAttachmentFormat = depth_format,
  };
  const VkGraphicsPipelineCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .pNext = &rendering,
      .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
      .stageCount = ArrayCount(stages),
      .pStages = stages,
      .pVertexInputState = &vertex_input,
      .pInputAssemblyState = &input_assembly,
      .pViewportState = &viewport,
      .pRasterizationState = &raster,
      .pMultisampleState = &multisample,
      .pDepthStencilState =
          depth_format != VK_FORMAT_UNDEFINED ? &depth_stencil : NULL,
      .pColorBlendState = &blend,
      .pDynamicState = &dynamic,
      .layout = renderer->pipeline_layout,
  };
  return vkCreateGraphicsPipelines(
             vkr_bindless_vk_renderer_device(renderer),
             renderer->pipeline_cache, 1u, &create_info, NULL,
             &renderer->packet_pipelines[pipeline]) == VK_SUCCESS;
}

static bool8_t
vkr_bindless_vk_create_packet_pipelines(VkrBindlessVulkanRenderer *renderer) {
  static const char *const paths[VKR_BINDLESS_VK_PACKET_SHADER_COUNT] = {
      VKR_BINDLESS_VK_PACKET_WORLD_VERT_SPV,
      VKR_BINDLESS_VK_PACKET_WORLD_FRAG_SPV,
      VKR_BINDLESS_VK_PACKET_SHADOW_VERT_SPV,
      VKR_BINDLESS_VK_PACKET_SHADOW_FRAG_SPV,
      VKR_BINDLESS_VK_PACKET_PICKING_FRAG_SPV,
      VKR_BINDLESS_VK_PACKET_FULLSCREEN_VERT_SPV,
      VKR_BINDLESS_VK_PACKET_FULLSCREEN_FRAG_SPV,
      VKR_BINDLESS_VK_PACKET_SKYBOX_FRAG_SPV,
      VKR_BINDLESS_VK_PACKET_TEXT_VERT_SPV,
      VKR_BINDLESS_VK_PACKET_TEXT_FRAG_SPV,
      VKR_BINDLESS_VK_PACKET_TEXT_PICKING_FRAG_SPV,
  };
  for (uint32_t i = 0u; i < VKR_BINDLESS_VK_PACKET_SHADER_COUNT; ++i) {
    if (!vkr_bindless_vk_create_shader_module(renderer, paths[i],
                                              &renderer->packet_shaders[i]))
      return false_v;
  }
  return vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_SHADOW,
             VKR_BINDLESS_VK_PACKET_SHADER_SHADOW_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_SHADOW_FRAGMENT, VK_FORMAT_UNDEFINED,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, true_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_PICKING_FRAGMENT, VK_FORMAT_R32_UINT,
             VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_OPAQUE,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             true_v, false_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_BLEND,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, true_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_HDR,
             VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, false_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_SKYBOX,
             VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_SKYBOX_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, false_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_FULLSCREEN_FINAL,
             VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_FULLSCREEN_FRAGMENT,
             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, false_v, false_v,
             false_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_UI,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_WORLD_FRAGMENT,
             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, false_v, false_v,
             true_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_WORLD_TEXT,
             VKR_BINDLESS_VK_PACKET_SHADER_TEXT_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_TEXT_FRAGMENT,
             VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, true_v,
             false_v, true_v, false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_PICKING_TEXT,
             VKR_BINDLESS_VK_PACKET_SHADER_TEXT_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_TEXT_PICKING_FRAGMENT,
             VK_FORMAT_R32_UINT, VK_FORMAT_D32_SFLOAT, true_v, true_v, false_v,
             false_v) &&
         vkr_bindless_vk_create_packet_pipeline(
             renderer, VKR_BINDLESS_VK_PACKET_PIPELINE_UI_TEXT,
             VKR_BINDLESS_VK_PACKET_SHADER_TEXT_VERTEX,
             VKR_BINDLESS_VK_PACKET_SHADER_TEXT_FRAGMENT,
             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, false_v, false_v,
             true_v, false_v);
}

static bool8_t
vkr_bindless_vk_create_ibl_pipelines(VkrBindlessVulkanRenderer *renderer) {
  static const char *const paths[VKR_BINDLESS_VK_IBL_PIPELINE_COUNT] = {
      VKR_BINDLESS_VK_PACKET_IBL_EQUIRECT_COMP_SPV,
      VKR_BINDLESS_VK_PACKET_IBL_IRRADIANCE_COMP_SPV,
      VKR_BINDLESS_VK_PACKET_IBL_PREFILTER_COMP_SPV,
  };
  static const char *const entries[VKR_BINDLESS_VK_IBL_PIPELINE_COUNT] = {
      "ibl_equirect",
      "ibl_irradiance",
      "ibl_prefilter",
  };
  for (uint32_t i = 0u; i < VKR_BINDLESS_VK_IBL_PIPELINE_COUNT; ++i) {
    if (!vkr_bindless_vk_create_shader_module(renderer, paths[i],
                                              &renderer->ibl_shaders[i]))
      return false_v;
    const VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = renderer->ibl_shaders[i],
        .pName = entries[i],
    };
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT,
        .stage = stage,
        .layout = renderer->pipeline_layout,
    };
    if (vkCreateComputePipelines(vkr_bindless_vk_renderer_device(renderer),
                                 renderer->pipeline_cache, 1u, &info, NULL,
                                 &renderer->ibl_pipelines[i]) != VK_SUCCESS)
      return false_v;
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_create_timeline(VkrBindlessVulkanRenderer *renderer) {
  VkSemaphoreTypeCreateInfo type_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
      .initialValue = 0u,
  };
  VkSemaphoreCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
      .pNext = &type_info,
  };
  return vkCreateSemaphore(vkr_bindless_vk_renderer_device(renderer),
                           &create_info, NULL,
                           &renderer->timeline) == VK_SUCCESS;
}

static uint64_t
vkr_bindless_vk_refresh_completed(VkrBindlessVulkanRenderer *renderer) {
  uint64_t completed = renderer->completed_value;
  if (renderer->timeline && vkGetSemaphoreCounterValue(
                                vkr_bindless_vk_renderer_device(renderer),
                                renderer->timeline, &completed) == VK_SUCCESS) {
    renderer->completed_value = completed;
  }
  return renderer->completed_value;
}

static bool8_t
vkr_bindless_vk_collect_captures(VkrBindlessVulkanRenderer *renderer,
                                 uint64_t completed_value) {
  if (!renderer->capture_ring.initialized)
    return true_v;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->capture_request_id && slot->retire_value &&
        slot->retire_value <= completed_value &&
        !vkr_bindless_vk_invalidate(renderer,
                                    &slot->capture_readback.allocation, 0u,
                                    renderer->config.capture_max_batch_bytes)) {
      return false_v;
    }
  }
  vkr_capture_ring_collect(&renderer->capture_ring, completed_value);
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->capture_request_id && slot->retire_value &&
        slot->retire_value <= completed_value) {
      slot->capture_request_id = 0u;
      slot->capture_item_count = 0u;
    }
  }
  return true_v;
}

static bool8_t
vkr_bindless_vk_collect_slot_timings(VkrBindlessVulkanRenderer *renderer,
                                     VkrBindlessVkFrameSlot *slot) {
  if (!slot->timing_requested || slot->timing_collected)
    return true_v;
  if (!slot->retire_value || slot->retire_value > renderer->completed_value ||
      !slot->timestamp_query_count)
    return false_v;
  uint64_t timestamps[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS * 2u] = {0};
  if (vkGetQueryPoolResults(
          vkr_bindless_vk_renderer_device(renderer), slot->timestamp_pool, 0u,
          slot->timestamp_query_count,
          (VkDeviceSize)slot->timestamp_query_count * sizeof(*timestamps),
          timestamps, sizeof(*timestamps),
          VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return false_v;
  const float64_t timestamp_ms =
      (float64_t)vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.timestampPeriod /
      1000000.0;
  for (uint32_t i = 0; i < slot->pass_timing_count; ++i) {
    const uint64_t begin = timestamps[i * 2u];
    const uint64_t end = timestamps[i * 2u + 1u];
    slot->pass_timings[i].valid = end >= begin;
    if (slot->pass_timings[i].valid)
      slot->pass_timings[i].gpu_ms = (float64_t)(end - begin) * timestamp_ms;
  }
  slot->timing_collected = true_v;
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_create(
    const VkrBindlessVulkanRendererConfig *config,
    VkrBindlessVulkanRenderer **out_renderer) {
  if (!out_renderer)
    return false_v;
  *out_renderer = NULL;
  if (!config || !config->allocator || !config->width || !config->height ||
      (config->target_kind == VKR_PRESENT_TARGET_OFFSCREEN &&
       !config->image_count) ||
      config->image_count > VKR_BINDLESS_VK_TARGET_IMAGE_MAX ||
      !config->sampled_image_capacity || !config->storage_image_capacity ||
      !config->sampler_capacity || !config->material_record_capacity ||
      !config->device_buffer_block_size || !config->device_image_block_size ||
      !config->upload_buffer_block_size ||
      !config->readback_buffer_block_size || !config->memory_block_capacity ||
      !config->memory_blocks_per_pool ||
      config->memory_blocks_per_pool > config->memory_block_capacity ||
      !config->memory_block_allocation_capacity ||
      config->publication_staging_capacity < 2u ||
      ((config->capture_ring_capacity == 0u) !=
       (config->capture_max_batch_bytes == 0u)) ||
      config->capture_ring_capacity > VKR_BINDLESS_VK_FRAME_SLOT_COUNT ||
      config->capture_ring_capacity > VKR_CAPTURE_RING_CAPACITY_MAX ||
      (uint64_t)config->material_slot_capacity <
          (uint64_t)config->material_record_capacity * 2u + 1u) {
    log_error("Invalid bindless Vulkan renderer configuration (target=%u, "
              "extent=%ux%u, images=%u)",
              config ? (uint32_t)config->target_kind : UINT32_MAX,
              config ? config->width : 0u, config ? config->height : 0u,
              config ? config->image_count : 0u);
    return false_v;
  }
  VkrBindlessVulkanRenderer *renderer = vkr_allocator_alloc(
      config->allocator, sizeof(*renderer), VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!renderer) {
    return false_v;
  }
  MemZero(renderer, sizeof(*renderer));
  renderer->allocator = config->allocator;
  renderer->config = *config;
  renderer->capture_storage_size = vkr_capture_ring_storage_requirement(
      config->capture_ring_capacity, config->capture_max_batch_bytes);
  if (config->capture_ring_capacity > 0u) {
    if (renderer->capture_storage_size > SIZE_MAX - KB(64)) {
      vkr_allocator_free(config->allocator, renderer, sizeof(*renderer),
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      return false_v;
    }
    const uint64_t capture_allocator_size =
        renderer->capture_storage_size + KB(64);
    if (!vkr_dmemory_create(capture_allocator_size, capture_allocator_size,
                            &renderer->capture_storage_memory)) {
      vkr_allocator_free(config->allocator, renderer, sizeof(*renderer),
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      return false_v;
    }
    renderer->capture_storage = vkr_dmemory_alloc(
        &renderer->capture_storage_memory, renderer->capture_storage_size);
    if (!renderer->capture_storage ||
        !vkr_capture_ring_init(
            &renderer->capture_ring, config->capture_ring_capacity,
            config->capture_max_batch_bytes, renderer->capture_storage,
            renderer->capture_storage_size)) {
      vkr_dmemory_destroy(&renderer->capture_storage_memory);
      vkr_allocator_free(config->allocator, renderer, sizeof(*renderer),
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      return false_v;
    }
  }
  if (!renderer->config.graph_path)
    renderer->config.graph_path = "assets/render_graphs/main.rendergraph.json";
  if (!renderer->config.max_graph_images)
    renderer->config.max_graph_images = 128u;
  if (!renderer->config.max_graph_passes)
    renderer->config.max_graph_passes = 64u;
  if (vkr_gpu_submit_ring_create(
          &renderer->command_ring, VKR_BINDLESS_VK_FRAME_SLOT_COUNT,
          VKR_BINDLESS_VK_FRAME_SLOT_COUNT, renderer->command_ring_slots,
          sizeof(renderer->command_ring_slots)) !=
      VKR_GPU_SUBMIT_RING_STATUS_OK) {
    if (renderer->capture_storage_memory.base_memory) {
      vkr_dmemory_destroy(&renderer->capture_storage_memory);
    }
    vkr_allocator_free(config->allocator, renderer, sizeof(*renderer),
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  *out_renderer = renderer;
  if (!vkr_dmemory_create(MB(8), GB(2),
                          &renderer->publication_staging_memory)) {
    log_error("Bindless Vulkan failed to reserve publication source memory");
    return false_v;
  }
  renderer->graph_images_size = (uint64_t)renderer->config.max_graph_images *
                                sizeof(*renderer->graph_images);
  renderer->graph_image_barriers_size =
      (uint64_t)renderer->config.max_graph_images *
      sizeof(*renderer->graph_image_barriers);
  renderer->graph_buffer_barriers_size =
      (uint64_t)renderer->config.max_graph_images *
      sizeof(*renderer->graph_buffer_barriers);
  renderer->graph_images =
      vkr_allocator_alloc(renderer->allocator, renderer->graph_images_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_image_barriers = vkr_allocator_alloc(
      renderer->allocator, renderer->graph_image_barriers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_buffer_barriers = vkr_allocator_alloc(
      renderer->allocator, renderer->graph_buffer_barriers_size,
      VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  renderer->graph_frame_arena = arena_create(MB(8), KB(256));
  renderer->graph_frame_allocator =
      (VkrAllocator){.ctx = renderer->graph_frame_arena};
  if (!renderer->graph_images || !renderer->graph_image_barriers ||
      !renderer->graph_buffer_barriers || !renderer->graph_frame_arena ||
      !vkr_allocator_arena(&renderer->graph_frame_allocator) ||
      !vkr_rg_executor_registry_init(&renderer->executors,
                                     renderer->allocator) ||
      !vkr_bindless_vk_register_graph_executors(renderer) ||
      !vkr_rg_json_load_file(renderer->allocator, renderer->config.graph_path,
                             &renderer->json_graph) ||
      (renderer->graph = vkr_rg_create(renderer->allocator)) == NULL ||
      !vkr_rg_set_frame_allocator(renderer->graph,
                                  &renderer->graph_frame_allocator)) {
    log_error("Bindless Vulkan failed to initialize the authored render graph");
    return false_v;
  }
  MemZero(renderer->graph_images, renderer->graph_images_size);
  MemZero(renderer->graph_image_barriers, renderer->graph_image_barriers_size);
  MemZero(renderer->graph_buffer_barriers,
          renderer->graph_buffer_barriers_size);
  VkrBindlessVulkanDeviceConfig device_config = {
      .allocator = config->allocator,
      .window = config->window,
      .sampled_image_capacity = config->sampled_image_capacity,
      .storage_image_capacity = config->storage_image_capacity,
      .sampler_capacity = config->sampler_capacity,
      .root_push_constant_size = sizeof(VkrBindlessVkPushConstants),
      .windowed = config->target_kind != VKR_PRESENT_TARGET_OFFSCREEN,
      .enable_validation = config->enable_validation,
      .enable_synchronization_validation =
          config->enable_synchronization_validation,
      .enable_gpu_assisted = config->enable_gpu_assisted,
  };
  if (!vkr_bindless_vulkan_device_create(&device_config, &renderer->device) ||
      !vkr_bindless_vk_create_timeline(renderer) ||
      !vkr_bindless_vk_pipeline_cache_initialize(renderer)) {
    log_error("Bindless Vulkan failed to create the selected device, timeline, "
              "or pipeline cache");
    return false_v;
  }
  const VkrBindlessVkMemoryPoolConfig memory_config = {
      .allocator = renderer->allocator,
      .device = vkr_bindless_vk_renderer_device(renderer),
      .block_sizes =
          {
              [VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE] =
                  {
                      [VKR_BINDLESS_VK_MEMORY_KIND_BUFFER] =
                          config->device_buffer_block_size,
                      [VKR_BINDLESS_VK_MEMORY_KIND_IMAGE] =
                          config->device_image_block_size,
                  },
              [VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD] =
                  {
                      [VKR_BINDLESS_VK_MEMORY_KIND_BUFFER] =
                          config->upload_buffer_block_size,
                      [VKR_BINDLESS_VK_MEMORY_KIND_IMAGE] =
                          config->upload_buffer_block_size,
                  },
              [VKR_BINDLESS_VK_MEMORY_CLASS_READBACK] =
                  {
                      [VKR_BINDLESS_VK_MEMORY_KIND_BUFFER] =
                          config->readback_buffer_block_size,
                      [VKR_BINDLESS_VK_MEMORY_KIND_IMAGE] =
                          config->readback_buffer_block_size,
                  },
          },
      .max_blocks = config->memory_block_capacity,
      .max_blocks_per_pool = config->memory_blocks_per_pool,
      .max_allocations_per_block = config->memory_block_allocation_capacity,
  };
  if (!vkr_bindless_vulkan_memory_pool_create(&memory_config,
                                              &renderer->memory_pool)) {
    log_error("Bindless Vulkan failed to create the pooled allocator");
    return false_v;
  }
  if (config->target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    if (!vkr_bindless_vk_create_acquire_semaphores(renderer) ||
        !vkr_bindless_vk_create_window_target(
            renderer, config->width, config->height, config->image_count,
            VK_NULL_HANDLE, &renderer->window_target)) {
      log_error("Bindless Vulkan failed to create the window target");
      return false_v;
    }
    renderer->config.width = renderer->window_target.width;
    renderer->config.height = renderer->window_target.height;
    renderer->config.image_count = renderer->window_target.image_count;
  }
  if (!vkr_bindless_vk_create_resources(renderer)) {
    log_error("Bindless Vulkan failed to create renderer resources");
    return false_v;
  }
  if (!vkr_bindless_vk_create_descriptor_slot_tables(renderer)) {
    log_error("Bindless Vulkan failed to create descriptor tables");
    return false_v;
  }
  if (!vkr_bindless_vk_publish_sentinel_descriptors(renderer)) {
    log_error("Bindless Vulkan failed to publish sentinel descriptors");
    return false_v;
  }
  if (!vkr_bindless_vk_create_pipeline(renderer)) {
    log_error("Bindless Vulkan failed to create renderer pipeline");
    return false_v;
  }
  return true_v;
}

static void vkr_bindless_vk_cmd_image_barrier_range(
    VkCommandBuffer command_buffer, VkImage image,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
    VkImageLayout old_layout, VkImageLayout new_layout, uint32_t level_count,
    uint32_t layer_count) {
  VkImageMemoryBarrier2 barrier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask = src_stage,
      .srcAccessMask = src_access,
      .dstStageMask = dst_stage,
      .dstAccessMask = dst_access,
      .oldLayout = old_layout,
      .newLayout = new_layout,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .levelCount = level_count,
                           .layerCount = layer_count},
  };
  VkDependencyInfo dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 1u,
      .pImageMemoryBarriers = &barrier,
  };
  vkCmdPipelineBarrier2(command_buffer, &dependency);
}

static void vkr_bindless_vk_cmd_image_barrier(
    VkCommandBuffer command_buffer, VkImage image,
    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
    VkImageLayout old_layout, VkImageLayout new_layout) {
  vkr_bindless_vk_cmd_image_barrier_range(command_buffer, image, src_stage,
                                          src_access, dst_stage, dst_access,
                                          old_layout, new_layout, 1u, 1u);
}

static void *
vkr_bindless_vk_publication_source_alloc(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t size) {
  VkrDMemory *memory = &renderer->publication_staging_memory;
  if (size > UINT64_MAX - 64u)
    return NULL;
  const uint64_t required_free = size + 64u;
  if (vkr_dmemory_get_free_space(memory) < required_free) {
    if (memory->total_size >= memory->reserve_size)
      return NULL;
    const uint64_t growth_available = memory->reserve_size - memory->total_size;
    const uint64_t growth_required =
        required_free - vkr_dmemory_get_free_space(memory);
    const uint64_t growth_slack = growth_required <= UINT64_MAX - MB(8)
                                      ? growth_required + MB(8)
                                      : UINT64_MAX;
    const uint64_t grow_by = Min(growth_slack, growth_available);
    if (grow_by < growth_required ||
        !vkr_dmemory_resize(memory, memory->total_size + grow_by))
      return NULL;
  }
  void *result = vkr_dmemory_alloc(memory, size);
  if (!result && memory->total_size < memory->reserve_size) {
    const uint64_t grow_by =
        Min(size + MB(8), memory->reserve_size - memory->total_size);
    if (grow_by && vkr_dmemory_resize(memory, memory->total_size + grow_by))
      result = vkr_dmemory_alloc(memory, size);
  }
  return result;
}

static void vkr_bindless_vk_release_texture_initialization(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkPendingTextureInitialization *initialization) {
  vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
  if (initialization->batches)
    vkr_allocator_free(renderer->allocator, initialization->batches,
                       initialization->batches_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (initialization->upload_data)
    (void)vkr_dmemory_free(&renderer->publication_staging_memory,
                           initialization->upload_data,
                           initialization->upload_data_size);
  MemZero(initialization, sizeof(*initialization));
}

static bool8_t vkr_bindless_vk_enqueue_texture_initialization(
    VkrBindlessVulkanRenderer *renderer,
    const VkrBindlessVkPendingTextureInitialization *initialization) {
  if (renderer->pending_texture_initialization_count >=
      renderer->config.sampled_image_capacity)
    return false_v;
  renderer->pending_texture_initializations
      [renderer->pending_texture_initialization_count++] = *initialization;
  return true_v;
}

static bool8_t vkr_bindless_vk_upload_prepared_texture(
    VkrBindlessVulkanRenderer *renderer, const VkrTexturePreparedLoad *prepared,
    VkrTextureHandle texture, VkrBindlessVkImage *out_image,
    VkrBindlessVkPendingTextureInitialization *out_initialization) {
  if (!prepared || !out_image || !out_initialization ||
      !prepared->upload_data || !prepared->upload_data_size ||
      !prepared->upload_regions || !prepared->upload_region_count ||
      !prepared->upload_mip_levels || !prepared->upload_array_layers ||
      (prepared->description.sample_count != 0u &&
       prepared->description.sample_count != VKR_SAMPLE_COUNT_1) ||
      (prepared->description.type != VKR_TEXTURE_TYPE_2D &&
       prepared->description.type != VKR_TEXTURE_TYPE_CUBE_MAP)) {
    log_error("Bindless Vulkan rejected invalid prepared texture metadata");
    return false_v;
  }
  const VkFormat format =
      vkr_bindless_vk_texture_format(prepared->description.format);
  if (format == VK_FORMAT_UNDEFINED) {
    log_error("Bindless Vulkan does not map prepared texture format %u",
              prepared->description.format);
    return false_v;
  }
  VkFormatProperties format_properties;
  vkGetPhysicalDeviceFormatProperties(
      vkr_bindless_vulkan_device_physical(renderer->device), format,
      &format_properties);
  if ((format_properties.optimalTilingFeatures &
       (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) !=
      (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
       VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) {
    log_error("Bindless Vulkan format %u lacks sampled/transfer-dst support",
              format);
    return false_v;
  }

  const bool8_t cube = prepared->description.type == VKR_TEXTURE_TYPE_CUBE_MAP;
  if (cube && prepared->upload_array_layers != 6u) {
    log_error("Bindless Vulkan cube texture has %u layers instead of 6",
              prepared->upload_array_layers);
    return false_v;
  }
  if (!vkr_bindless_vk_create_image_ex(
          renderer, prepared->description.width, prepared->description.height,
          prepared->upload_mip_levels, prepared->upload_array_layers, format,
          cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
          cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
          out_image)) {
    log_error("Bindless Vulkan failed to create prepared texture image "
              "(%ux%u, format=%u, mips=%u, layers=%u, bytes=%llu)",
              prepared->description.width, prepared->description.height, format,
              prepared->upload_mip_levels, prepared->upload_array_layers,
              (unsigned long long)prepared->upload_data_size);
    return false_v;
  }

  uint32_t block_width = 0u;
  uint32_t block_height = 0u;
  uint32_t block_bytes = 0u;
  if (!vkr_bindless_vk_format_block_info(format, &block_width, &block_height,
                                         &block_bytes)) {
    log_error("Bindless Vulkan cannot lower upload blocks for format %u",
              format);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  const uint64_t staging_limit = renderer->config.upload_buffer_block_size;
  uint64_t batch_count_u64 = 0u;
  bool8_t valid = true_v;
  for (uint32_t i = 0u; i < prepared->upload_region_count; ++i) {
    const VkrTextureUploadRegion *source = &prepared->upload_regions[i];
    if (!source->width || !source->height || !source->depth ||
        source->mip_level >= prepared->upload_mip_levels ||
        source->array_layer >= prepared->upload_array_layers ||
        source->byte_offset > prepared->upload_data_size ||
        source->byte_size > prepared->upload_data_size - source->byte_offset) {
      valid = false_v;
      break;
    }
    if (source->byte_size <= staging_limit) {
      batch_count_u64++;
      continue;
    }
    const uint64_t blocks_w = (source->width + block_width - 1u) / block_width;
    const uint64_t blocks_h =
        (source->height + block_height - 1u) / block_height;
    const uint64_t row_bytes = blocks_w * block_bytes;
    const uint64_t expected_size = row_bytes * blocks_h * source->depth;
    const uint64_t rows_per_batch = row_bytes ? staging_limit / row_bytes : 0u;
    if (!row_bytes || !rows_per_batch || expected_size != source->byte_size) {
      valid = false_v;
      break;
    }
    batch_count_u64 += (uint64_t)source->depth *
                       ((blocks_h + rows_per_batch - 1u) / rows_per_batch);
  }
  if (!valid || !batch_count_u64 || batch_count_u64 > UINT32_MAX) {
    log_error("Bindless Vulkan rejected texture upload region layout");
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  const uint32_t batch_count = (uint32_t)batch_count_u64;
  const uint64_t batches_size =
      batch_count_u64 * sizeof(VkrBindlessVkTextureUploadBatch);
  VkrBindlessVkTextureUploadBatch *batches = vkr_allocator_alloc(
      renderer->allocator, batches_size, VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  uint8_t *upload_data = vkr_bindless_vk_publication_source_alloc(
      renderer, prepared->upload_data_size);
  if (!batches || !upload_data) {
    log_error("Bindless Vulkan failed to retain %llu texture bytes and %u "
              "upload regions",
              (unsigned long long)prepared->upload_data_size, batch_count);
    if (batches)
      vkr_allocator_free(renderer->allocator, batches, batches_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (upload_data)
      (void)vkr_dmemory_free(&renderer->publication_staging_memory, upload_data,
                             prepared->upload_data_size);
    vkr_bindless_vk_destroy_image(renderer, out_image);
    return false_v;
  }
  MemZero(batches, batches_size);
  MemCopy(upload_data, prepared->upload_data, prepared->upload_data_size);
  uint32_t batch_index = 0u;
  for (uint32_t i = 0; i < prepared->upload_region_count; ++i) {
    const VkrTextureUploadRegion *source = &prepared->upload_regions[i];
    if (source->byte_size <= staging_limit) {
      VkrBindlessVkTextureUploadBatch *batch = &batches[batch_index++];
      batch->source_offset = source->byte_offset;
      batch->source_size = source->byte_size;
      batch->region = (VkBufferImageCopy2){
          .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
          .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                               .mipLevel = source->mip_level,
                               .baseArrayLayer = source->array_layer,
                               .layerCount = 1u},
          .imageExtent = {.width = source->width,
                          .height = source->height,
                          .depth = source->depth},
      };
      continue;
    }
    const uint64_t blocks_w = (source->width + block_width - 1u) / block_width;
    const uint64_t blocks_h =
        (source->height + block_height - 1u) / block_height;
    const uint64_t row_bytes = blocks_w * block_bytes;
    const uint64_t rows_per_batch = staging_limit / row_bytes;
    for (uint32_t z = 0u; z < source->depth; ++z) {
      for (uint64_t first_row = 0u; first_row < blocks_h;
           first_row += rows_per_batch) {
        const uint64_t row_count = Min(rows_per_batch, blocks_h - first_row);
        const uint32_t y = (uint32_t)(first_row * block_height);
        VkrBindlessVkTextureUploadBatch *batch = &batches[batch_index++];
        batch->source_offset = source->byte_offset +
                               ((uint64_t)z * blocks_h + first_row) * row_bytes;
        batch->source_size = row_count * row_bytes;
        batch->region = (VkBufferImageCopy2){
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .mipLevel = source->mip_level,
                                 .baseArrayLayer = source->array_layer,
                                 .layerCount = 1u},
            .imageOffset = {.x = 0, .y = (int32_t)y, .z = (int32_t)z},
            .imageExtent = {.width = source->width,
                            .height = Min((uint32_t)(row_count * block_height),
                                          source->height - y),
                            .depth = 1u},
        };
      }
    }
  }
  valid = batch_index == batch_count;
  if (valid) {
    *out_initialization = (VkrBindlessVkPendingTextureInitialization){
        .batches = batches,
        .batches_size = batches_size,
        .upload_data = upload_data,
        .upload_data_size = prepared->upload_data_size,
        .texture = texture,
        .batch_count = batch_count,
    };
    out_image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  } else {
    log_error("Bindless Vulkan rejected an out-of-range texture upload region");
    VkrBindlessVkPendingTextureInitialization initialization = {
        .batches = batches,
        .batches_size = batches_size,
        .upload_data = upload_data,
        .upload_data_size = prepared->upload_data_size,
        .batch_count = batch_count,
    };
    vkr_bindless_vk_release_texture_initialization(renderer, &initialization);
    vkr_bindless_vk_destroy_image(renderer, out_image);
  }
  return valid;
}

static bool8_t
vkr_bindless_vk_stage_next_buffer_batch(VkrBindlessVulkanRenderer *renderer) {
  if (renderer->staging_buffer_count)
    return true_v;
  for (uint32_t i = 0u; i < renderer->pending_buffer_initialization_count;
       ++i) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    if (initialization->next_offset >= initialization->size)
      continue;
    const VkDeviceSize chunk_size =
        Min((VkDeviceSize)renderer->config.upload_buffer_block_size,
            initialization->size - initialization->next_offset);
    if (!vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD, chunk_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &initialization->staging)) {
      log_error("Bindless Vulkan failed to create bounded %llu-byte buffer "
                "staging chunk at offset %llu/%llu",
                (unsigned long long)chunk_size,
                (unsigned long long)initialization->next_offset,
                (unsigned long long)initialization->size);
      return false_v;
    }
    MemCopy(initialization->staging.allocation.mapped,
            initialization->upload_data + initialization->next_offset,
            chunk_size);
    if (!vkr_bindless_vk_flush(renderer, &initialization->staging.allocation,
                               0u, chunk_size)) {
      vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
      return false_v;
    }
    renderer->staging_buffer_count++;
    return true_v;
  }
  return true_v;
}

/** Retains source bytes in CPU memory and materializes at most one bounded
 *
 * host-visible batch at a time. This keeps large texture publication below
 *
 * small discrete-GPU host-visible heap budgets without waiting the queue. */
static bool8_t
vkr_bindless_vk_stage_next_texture_batch(VkrBindlessVulkanRenderer *renderer) {
  if (renderer->staging_buffer_count)
    return true_v;
  for (uint32_t i = 0u; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    if (initialization->writable ||
        initialization->next_batch >= initialization->batch_count)
      continue;
    VkrBindlessVkTextureUploadBatch *first_batch =
        &initialization->batches[initialization->next_batch];
    if (first_batch->source_size > renderer->config.upload_buffer_block_size) {
      log_error("Bindless Vulkan texture upload region %u/%u is %llu bytes; "
                "the bounded staging limit is %llu bytes",
                initialization->next_batch + 1u, initialization->batch_count,
                (unsigned long long)first_batch->source_size,
                (unsigned long long)renderer->config.upload_buffer_block_size);
      return false_v;
    }
    uint64_t staging_size = 0u;
    uint32_t staged_count = 0u;
    for (uint32_t batch_index = initialization->next_batch;
         batch_index < initialization->batch_count; ++batch_index) {
      const VkrBindlessVkTextureUploadBatch *batch =
          &initialization->batches[batch_index];
      const uint64_t offset = vkr_bindless_vk_align_up(staging_size, 16u);
      if (offset > renderer->config.upload_buffer_block_size ||
          batch->source_size >
              renderer->config.upload_buffer_block_size - offset)
        break;
      staging_size = offset + batch->source_size;
      staged_count++;
    }
    if (!vkr_bindless_vk_create_buffer(
            renderer, VKR_BINDLESS_VK_MEMORY_CLASS_UPLOAD, staging_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &initialization->staging)) {
      log_error("Bindless Vulkan failed to create bounded %llu-byte texture "
                "staging chunk at region %u/%u",
                (unsigned long long)staging_size,
                initialization->next_batch + 1u, initialization->batch_count);
      return false_v;
    }
    uint64_t destination_offset = 0u;
    for (uint32_t batch_offset = 0u; batch_offset < staged_count;
         ++batch_offset) {
      VkrBindlessVkTextureUploadBatch *batch =
          &initialization->batches[initialization->next_batch + batch_offset];
      destination_offset = vkr_bindless_vk_align_up(destination_offset, 16u);
      batch->region.bufferOffset = destination_offset;
      MemCopy((uint8_t *)initialization->staging.allocation.mapped +
                  destination_offset,
              initialization->upload_data + batch->source_offset,
              batch->source_size);
      destination_offset += batch->source_size;
    }
    if (!vkr_bindless_vk_flush(renderer, &initialization->staging.allocation,
                               0u, staging_size)) {
      log_error("Bindless Vulkan failed to flush bounded texture staging "
                "chunk at region %u/%u",
                initialization->next_batch + 1u, initialization->batch_count);
      vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
      return false_v;
    }
    initialization->staged_batch_count = staged_count;
    renderer->staging_buffer_count++;
    return true_v;
  }
  return true_v;
}

static bool8_t vkr_bindless_vk_stage_next_publication_batch(
    VkrBindlessVulkanRenderer *renderer) {
  return vkr_bindless_vk_stage_next_buffer_batch(renderer) &&
         vkr_bindless_vk_stage_next_texture_batch(renderer);
}

static void vkr_bindless_vk_record_texture_initializations(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command) {
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    const VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    const VkrBindlessVkPublishedTexture *texture =
        vkr_bindless_vk_texture_publication(renderer, initialization->texture);
    if (!texture) {
      log_fatal("Bindless Vulkan lost texture %u:%u before initialization "
                "recording",
                initialization->texture.id, initialization->texture.generation);
      return;
    }
    if (initialization->writable) {
      vkr_bindless_vk_cmd_image_barrier_range(
          command, texture->image.handle, VK_PIPELINE_STAGE_2_NONE,
          VK_ACCESS_2_NONE,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
          texture->image.mip_levels, texture->image.array_layers);
      continue;
    }
    if (initialization->next_batch >= initialization->batch_count ||
        !initialization->staged_batch_count || !initialization->staging.handle)
      continue;
    if (initialization->next_batch == 0u)
      vkr_bindless_vk_cmd_image_barrier_range(
          command, texture->image.handle, VK_PIPELINE_STAGE_2_NONE,
          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture->image.mip_levels,
          texture->image.array_layers);
    for (uint32_t batch_offset = 0u;
         batch_offset < initialization->staged_batch_count; ++batch_offset) {
      const VkrBindlessVkTextureUploadBatch *batch =
          &initialization->batches[initialization->next_batch + batch_offset];
      const VkCopyBufferToImageInfo2 copy_info = {
          .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
          .srcBuffer = initialization->staging.handle,
          .dstImage = texture->image.handle,
          .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .regionCount = 1u,
          .pRegions = &batch->region,
      };
      vkCmdCopyBufferToImage2(command, &copy_info);
    }
    if (initialization->next_batch + initialization->staged_batch_count ==
        initialization->batch_count)
      vkr_bindless_vk_cmd_image_barrier_range(
          command, texture->image.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture->image.mip_levels,
          texture->image.array_layers);
  }
}

static void vkr_bindless_vk_record_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer, VkCommandBuffer command) {
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    const VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    if (!initialization->staging.handle)
      continue;
    const VkBufferCopy2 region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2,
        .dstOffset = initialization->next_offset,
        .size = initialization->staging.size,
    };
    const VkCopyBufferInfo2 copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,
        .srcBuffer = initialization->staging.handle,
        .dstBuffer = initialization->destination,
        .regionCount = 1u,
        .pRegions = &region,
    };
    vkCmdCopyBuffer2(command, &copy);
    if (initialization->next_offset + initialization->staging.size <
        initialization->size)
      continue;
    const VkBufferMemoryBarrier2 barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStageMask = initialization->destination_stage,
        .dstAccessMask = initialization->destination_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = initialization->destination,
        .size = VK_WHOLE_SIZE,
    };
    const VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .bufferMemoryBarrierCount = 1u,
        .pBufferMemoryBarriers = &barrier,
    };
    vkCmdPipelineBarrier2(command, &dependency);
  }
}

static VkrBindlessVkRetiredStagingBuffer *
vkr_bindless_vk_reserve_staging_retirement(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t slot = 0; slot < renderer->retired_staging_buffer_capacity;
       ++slot) {
    if (!renderer->retired_staging_buffers[slot].occupied)
      return &renderer->retired_staging_buffers[slot];
  }
  return NULL;
}

static void vkr_bindless_vk_release_buffer_initialization(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkPendingBufferInitialization *initialization) {
  vkr_bindless_vk_destroy_buffer(renderer, &initialization->staging);
  if (initialization->upload_data)
    (void)vkr_dmemory_free(&renderer->publication_staging_memory,
                           initialization->upload_data, initialization->size);
  MemZero(initialization, sizeof(*initialization));
}

static void vkr_bindless_vk_commit_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer, uint64_t retire_value) {
  uint32_t write_index = 0u;
  const uint32_t pending_count = renderer->pending_buffer_initialization_count;
  for (uint32_t read_index = 0u; read_index < pending_count; ++read_index) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[read_index];
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[initialization->geometry_record_index];
    if (!initialization->staging.handle) {
      if (write_index != read_index) {
        renderer->pending_buffer_initializations[write_index] = *initialization;
        MemZero(initialization, sizeof(*initialization));
      }
      write_index++;
      continue;
    }
    VkrBindlessVkRetiredStagingBuffer *retired =
        vkr_bindless_vk_reserve_staging_retirement(renderer);
    if (!retired)
      log_fatal("Bindless Vulkan lost bounded staging retirement capacity");
    if (!vkr_bindless_vk_retire_buffer(renderer, &initialization->staging,
                                       retire_value))
      log_fatal("Bindless Vulkan failed to retire submitted staging memory");
    *retired = (VkrBindlessVkRetiredStagingBuffer){
        .buffer = initialization->staging,
        .retire_value = retire_value,
        .occupied = true_v,
    };
    initialization->next_offset += initialization->staging.size;
    MemZero(&initialization->staging, sizeof(initialization->staging));
    geometry->last_use_submit_value =
        Max(geometry->last_use_submit_value, retire_value);
    if (initialization->next_offset == initialization->size) {
      if (geometry->pending_initialization_count)
        geometry->pending_initialization_count--;
      vkr_bindless_vk_release_buffer_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index) {
      renderer->pending_buffer_initializations[write_index] = *initialization;
      MemZero(initialization, sizeof(*initialization));
    }
    write_index++;
  }
  for (uint32_t i = write_index; i < pending_count; ++i)
    MemZero(&renderer->pending_buffer_initializations[i],
            sizeof(renderer->pending_buffer_initializations[i]));
  renderer->pending_buffer_initialization_count = write_index;
}

static void vkr_bindless_vk_discard_buffer_initializations(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->pending_buffer_initialization_count; ++i) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[i];
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[initialization->geometry_record_index];
    if (initialization->staging.handle && renderer->staging_buffer_count)
      renderer->staging_buffer_count--;
    if (geometry->pending_initialization_count)
      geometry->pending_initialization_count--;
    vkr_bindless_vk_release_buffer_initialization(renderer, initialization);
  }
  renderer->pending_buffer_initialization_count = 0u;
}

static void vkr_bindless_vk_discard_geometry_initializations(
    VkrBindlessVulkanRenderer *renderer, uint32_t geometry_record_index) {
  uint32_t write_index = 0u;
  for (uint32_t read_index = 0;
       read_index < renderer->pending_buffer_initialization_count;
       ++read_index) {
    VkrBindlessVkPendingBufferInitialization *initialization =
        &renderer->pending_buffer_initializations[read_index];
    if (initialization->geometry_record_index == geometry_record_index) {
      if (initialization->staging.handle && renderer->staging_buffer_count)
        renderer->staging_buffer_count--;
      vkr_bindless_vk_release_buffer_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index)
      renderer->pending_buffer_initializations[write_index] = *initialization;
    write_index++;
  }
  for (uint32_t i = write_index;
       i < renderer->pending_buffer_initialization_count; ++i)
    MemZero(&renderer->pending_buffer_initializations[i],
            sizeof(renderer->pending_buffer_initializations[i]));
  renderer->pending_buffer_initialization_count = write_index;
  renderer->published_geometries[geometry_record_index]
      .pending_initialization_count = 0u;
}

static void vkr_bindless_vk_commit_texture_initializations(
    VkrBindlessVulkanRenderer *renderer, uint64_t retire_value) {
  uint32_t write_index = 0u;
  const uint32_t pending_count = renderer->pending_texture_initialization_count;
  for (uint32_t read_index = 0; read_index < pending_count; ++read_index) {
    VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[read_index];
    VkrBindlessVkPublishedTexture *texture =
        vkr_bindless_vk_texture_publication(renderer, initialization->texture);
    if (!texture) {
      log_fatal("Bindless Vulkan lost texture %u:%u before initialization "
                "commit",
                initialization->texture.id, initialization->texture.generation);
      return;
    }
    bool8_t progressed = initialization->writable;
    if (!initialization->writable &&
        initialization->next_batch < initialization->batch_count &&
        initialization->staging.handle && initialization->staged_batch_count) {
      VkrBindlessVkRetiredStagingBuffer *retired =
          vkr_bindless_vk_reserve_staging_retirement(renderer);
      if (!retired)
        log_fatal("Bindless Vulkan lost bounded staging retirement "
                  "capacity");
      if (!vkr_bindless_vk_retire_buffer(renderer, &initialization->staging,
                                         retire_value))
        log_fatal("Bindless Vulkan failed to retire submitted staging "
                  "memory");
      *retired = (VkrBindlessVkRetiredStagingBuffer){
          .buffer = initialization->staging,
          .retire_value = retire_value,
          .occupied = true_v,
      };
      MemZero(&initialization->staging, sizeof(initialization->staging));
      initialization->next_batch += initialization->staged_batch_count;
      initialization->staged_batch_count = 0u;
      progressed = true_v;
    }
    if (progressed)
      texture->last_use_submit_value =
          Max(texture->last_use_submit_value, retire_value);
    const bool8_t completed =
        initialization->writable ||
        initialization->next_batch == initialization->batch_count;
    if (completed) {
      texture->initialization_pending = false_v;
      vkr_bindless_vk_release_texture_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index) {
      renderer->pending_texture_initializations[write_index] = *initialization;
      MemZero(initialization, sizeof(*initialization));
    }
    write_index++;
  }
  for (uint32_t i = write_index; i < pending_count; ++i)
    MemZero(&renderer->pending_texture_initializations[i],
            sizeof(renderer->pending_texture_initializations[i]));
  renderer->pending_texture_initialization_count = write_index;
}

static void vkr_bindless_vk_discard_texture_initializations(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->pending_texture_initialization_count;
       ++i) {
    VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[i];
    VkrBindlessVkPublishedTexture *texture =
        vkr_bindless_vk_texture_publication(renderer, initialization->texture);
    if (texture)
      texture->initialization_pending = false_v;
    if (initialization->staging.handle && renderer->staging_buffer_count)
      renderer->staging_buffer_count--;
    vkr_bindless_vk_release_texture_initialization(renderer, initialization);
  }
  renderer->pending_texture_initialization_count = 0u;
}

static void vkr_bindless_vk_cancel_texture_initialization(
    VkrBindlessVulkanRenderer *renderer, VkrTextureHandle texture_handle) {
  uint32_t write_index = 0u;
  const uint32_t pending_count = renderer->pending_texture_initialization_count;
  for (uint32_t read_index = 0u; read_index < pending_count; ++read_index) {
    VkrBindlessVkPendingTextureInitialization *initialization =
        &renderer->pending_texture_initializations[read_index];
    if (initialization->texture.id == texture_handle.id &&
        initialization->texture.generation == texture_handle.generation) {
      if (initialization->staging.handle && renderer->staging_buffer_count)
        renderer->staging_buffer_count--;
      vkr_bindless_vk_release_texture_initialization(renderer, initialization);
      continue;
    }
    if (write_index != read_index) {
      renderer->pending_texture_initializations[write_index] = *initialization;
      MemZero(initialization, sizeof(*initialization));
    }
    write_index++;
  }
  for (uint32_t i = write_index; i < pending_count; ++i)
    MemZero(&renderer->pending_texture_initializations[i],
            sizeof(renderer->pending_texture_initializations[i]));
  renderer->pending_texture_initialization_count = write_index;
  VkrBindlessVkPublishedTexture *texture =
      vkr_bindless_vk_texture_publication(renderer, texture_handle);
  if (texture)
    texture->initialization_pending = false_v;
}

bool8_t vkr_bindless_vulkan_renderer_prepare_frame(
    VkrBindlessVulkanRenderer *renderer, uint64_t source_frame_index,
    uint32_t shadow_map_size, uint32_t shadow_cascade_count,
    VkrFrameSetup *out_setup) {
  if (!renderer || !out_setup || renderer->frame_active ||
      renderer->terminal_failure || !shadow_map_size ||
      shadow_cascade_count > VKR_SHADOW_CASCADE_COUNT_MAX) {
    return false_v;
  }
  uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  if (!vkr_bindless_vk_collect_captures(renderer, completed))
    return false_v;
  vkr_bindless_vk_collect_retired_targets(renderer, completed);
  vkr_bindless_vk_collect_retired_window_targets(renderer, completed);
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  if (renderer->target_dirty &&
      !vkr_bindless_vk_recreate_window_target(renderer, renderer->config.width,
                                              renderer->config.height,
                                              renderer->config.image_count))
    return false_v;
  completed = vkr_bindless_vk_refresh_completed(renderer);
  const uint32_t slot_index = renderer->command_ring.next_slot;
  VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[slot_index];
  if (slot->retire_value > completed) {
    VkSemaphoreWaitInfo wait_info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1u,
        .pSemaphores = &renderer->timeline,
        .pValues = &slot->retire_value,
    };
    if (vkWaitSemaphores(vkr_bindless_vk_renderer_device(renderer), &wait_info,
                         UINT64_MAX) != VK_SUCCESS) {
      return false_v;
    }
    renderer->command_slot_wait_count++;
    completed = vkr_bindless_vk_refresh_completed(renderer);
    if (!vkr_bindless_vk_collect_captures(renderer, completed))
      return false_v;
    vkr_bindless_vk_collect_retired_targets(renderer, completed);
    vkr_bindless_vk_collect_asset_publications(renderer, completed);
  }
  if (slot->retire_value && slot->retire_value <= completed &&
      slot->timing_requested && !slot->timing_collected &&
      !vkr_bindless_vk_collect_slot_timings(renderer, slot))
    return false_v;
  if (!vkr_bindless_vk_stage_next_publication_batch(renderer))
    return false_v;
  if (vkResetCommandPool(vkr_bindless_vk_renderer_device(renderer),
                         slot->command_pool, 0u) != VK_SUCCESS) {
    return false_v;
  }
  if (vkr_gpu_submit_ring_acquire(&renderer->command_ring, 1u, completed,
                                  &renderer->active_command_slice) !=
          VKR_GPU_SUBMIT_RING_STATUS_OK ||
      renderer->active_command_slice.slot_index != slot_index)
    return false_v;
  renderer->active_frame_slot = slot_index;
  renderer->frame_active = true_v;
  slot->source_frame_index = source_frame_index;
  slot->acquired_window_image = false_v;
  slot->reacquired_presented_image = false_v;
  if (renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN) {
    slot->image_index = renderer->next_image_index;
    renderer->next_image_index =
        (renderer->next_image_index + 1u) % renderer->targets.image_count;
  } else {
    const VkResult acquire_result =
        vkAcquireNextImageKHR(vkr_bindless_vk_renderer_device(renderer),
                              renderer->window_target.swapchain, UINT64_MAX,
                              renderer->acquire_semaphores[slot_index],
                              VK_NULL_HANDLE, &slot->image_index);
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      renderer->target_dirty = true_v;
      vkr_bindless_vulkan_renderer_cancel_frame(renderer);
      return false_v;
    }
    if (acquire_result != VK_SUCCESS && acquire_result != VK_SUBOPTIMAL_KHR) {
      vkr_bindless_vulkan_renderer_cancel_frame(renderer);
      return false_v;
    }
    if (acquire_result == VK_SUBOPTIMAL_KHR)
      renderer->target_dirty = true_v;
    slot->acquired_window_image = true_v;
    const uint32_t image_index = slot->image_index;
    slot->reacquired_presented_image =
        renderer->window_target.image_presented[image_index];
    if (renderer->window_target.present_fence_pending[image_index]) {
      VkFence *present_fence =
          &renderer->window_target.present_complete[image_index];
      if (vkWaitForFences(vkr_bindless_vk_renderer_device(renderer), 1u,
                          present_fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS ||
          vkResetFences(vkr_bindless_vk_renderer_device(renderer), 1u,
                        present_fence) != VK_SUCCESS) {
        vkr_bindless_vulkan_renderer_cancel_frame(renderer);
        return false_v;
      }
      renderer->window_target.present_fence_pending[image_index] = false_v;
    }
    const uint64_t image_submit =
        renderer->window_target.image_last_submit_value[image_index];
    if (image_submit > completed) {
      VkSemaphoreWaitInfo image_wait = {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
          .semaphoreCount = 1u,
          .pSemaphores = &renderer->timeline,
          .pValues = &image_submit,
      };
      if (vkWaitSemaphores(vkr_bindless_vk_renderer_device(renderer),
                           &image_wait, UINT64_MAX) != VK_SUCCESS) {
        vkr_bindless_vulkan_renderer_cancel_frame(renderer);
        return false_v;
      }
    }
  }
  *out_setup = (VkrFrameSetup){
      .image_index = slot->image_index,
      .window_width =
          renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? renderer->targets.width
              : renderer->window_target.width,
      .window_height =
          renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN
              ? renderer->targets.height
              : renderer->window_target.height,
      .swapchain_format = VKR_TEXTURE_FORMAT_R8G8B8A8_SRGB,
      .swapchain_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
  };
  const VkrBindlessVkImage *target =
      &renderer->targets.images[slot->image_index];
  renderer->prepared_frame = (VkrRenderGraphFrameInfo){
      .frame_index = (uint32_t)source_frame_index,
      .image_index = slot->image_index,
      .delta_time = 1.0 / 60.0,
      .target_width = out_setup->window_width,
      .target_height = out_setup->window_height,
      .window_width = out_setup->window_width,
      .window_height = out_setup->window_height,
      .viewport_width = out_setup->window_width,
      .viewport_height = out_setup->window_height,
      .target_color_format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
      .target_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .target_color_initial_state =
          target->layout == VK_IMAGE_LAYOUT_UNDEFINED
              ? (VkrPresentTargetImageState){
                    .access = VKR_IMAGE_ACCESS_NONE,
                    .layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
                }
              : (VkrPresentTargetImageState){
                    .access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
                    .layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                },
      .target_depth_initial_state = {
          .access = VKR_IMAGE_ACCESS_NONE,
          .layout = VKR_TEXTURE_LAYOUT_UNDEFINED,
      },
      .target_terminal_state = {
          .access = VKR_IMAGE_ACCESS_TRANSFER_SRC,
          .layout = VKR_TEXTURE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
      .shadow_depth_format = VKR_TEXTURE_FORMAT_D32_SFLOAT,
      .shadow_map_size = shadow_map_size,
      .shadow_cascade_count = shadow_cascade_count,
  };
  return true_v;
}

static bool8_t vkr_bindless_vk_record_draw(VkrBindlessVulkanRenderer *renderer,
                                           VkrBindlessVkFrameSlot *slot) {
  VkCommandBuffer command = slot->command_buffer;
  if (!vkr_bindless_vk_prepare_packet_uploads(renderer, slot,
                                              renderer->graph->packet))
    return false_v;
  VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (slot->timing_requested)
    vkResetQueryPool(vkr_bindless_vk_renderer_device(renderer),
                     slot->timestamp_pool, 0u,
                     VKR_RENDERER_IMPL_MAX_PASS_TIMINGS * 2u);
  if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
    return false_v;
  }
  vkr_bindless_vk_record_buffer_initializations(renderer, command);
  vkr_bindless_vk_record_texture_initializations(renderer, command);
  if (!renderer->sentinel_uploaded) {
    vkr_bindless_vk_cmd_image_barrier(
        command, renderer->sentinel_image.handle, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy2 copy_region = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset = VKR_BINDLESS_VK_TEXTURE_OFFSET,
        .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .layerCount = 1u},
        .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
    };
    VkCopyBufferToImageInfo2 copy_info = {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = renderer->upload.handle,
        .dstImage = renderer->sentinel_image.handle,
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1u,
        .pRegions = &copy_region,
    };
    vkCmdCopyBufferToImage2(command, &copy_info);
    vkr_bindless_vk_cmd_image_barrier(
        command, renderer->sentinel_image.handle, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  const VkDescriptorBufferBindingInfoEXT descriptor_bindings[] = {
      {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
          .address = renderer->resource_descriptors.address,
          .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
          .address = renderer->sampler_descriptors.address,
          .usage = VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
      },
  };
  vkr_bindless_vulkan_device_cmd_bind_descriptor_buffers(renderer->device)(
      command, ArrayCount(descriptor_bindings), descriptor_bindings);
  const uint32_t buffer_indices[] = {0u, 1u};
  const VkDeviceSize descriptor_offsets[] = {0u, 0u};
  vkr_bindless_vulkan_device_cmd_set_descriptor_offsets(renderer->device)(
      command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipeline_layout, 0u,
      ArrayCount(buffer_indices), buffer_indices, descriptor_offsets);
  vkr_bindless_vulkan_device_cmd_set_descriptor_offsets(renderer->device)(
      command, VK_PIPELINE_BIND_POINT_COMPUTE, renderer->pipeline_layout, 0u,
      ArrayCount(buffer_indices), buffer_indices, descriptor_offsets);

  if (!vkr_bindless_vk_record_graph(renderer, command) ||
      !vkr_bindless_vk_record_ibl_validation_capture(renderer, command, slot))
    return false_v;

  VkrBindlessVkImage *target = &renderer->targets.images[slot->image_index];
  if (!vkr_bindless_vk_record_capture(renderer, command, slot))
    return false_v;
  VkrBindlessVkImage *readback_image = target;
  uint32_t readback_x = 0u;
  uint32_t readback_y = 0u;
  slot->picking_readback_pending = false_v;
  const VkrRenderPacket *packet = renderer->graph->packet;
  if (packet->picking && packet->picking->pending) {
    const VkrRgImageHandle picking_handle =
        vkr_rg_find_image(renderer->graph, string8_lit("picking_color"));
    VkrBindlessVkGraphImageInstance *picking = vkr_bindless_vk_graph_image(
        renderer, picking_handle, slot->image_index);
    if (!picking || packet->picking->x >= picking->image.width ||
        packet->picking->y >= picking->image.height)
      return false_v;
    readback_image = &picking->image;
    readback_x = packet->picking->x;
    readback_y = packet->picking->y;
    slot->picking_x = readback_x;
    slot->picking_y = readback_y;
    slot->picking_readback_pending = true_v;
  }
  VkBufferImageCopy2 readback_region = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
      .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
      .imageOffset = {.x = (int32_t)readback_x,
                      .y = (int32_t)readback_y,
                      .z = 0},
      .imageExtent = {.width = 1u, .height = 1u, .depth = 1u},
  };
  VkCopyImageToBufferInfo2 readback_info = {
      .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
      .srcImage = readback_image->handle,
      .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .dstBuffer = slot->readback.handle,
      .regionCount = 1u,
      .pRegions = &readback_region,
  };
  vkCmdCopyImageToBuffer2(command, &readback_info);
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    VkrBindlessVkWindowTarget *window = &renderer->window_target;
    const uint32_t image_index = slot->image_index;
    vkr_bindless_vk_cmd_image_barrier(
        command, window->images[image_index], VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        window->image_presented[image_index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                             : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageBlit2 blit_region = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2,
        .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
        .srcOffsets = {{0, 0, 0},
                       {(int32_t)target->width, (int32_t)target->height, 1}},
        .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                           .layerCount = 1u},
        .dstOffsets = {{0, 0, 0},
                       {(int32_t)window->width, (int32_t)window->height, 1}},
    };
    const VkBlitImageInfo2 blit_info = {
        .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,
        .srcImage = target->handle,
        .srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .dstImage = window->images[image_index],
        .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .regionCount = 1u,
        .pRegions = &blit_region,
        .filter = VK_FILTER_NEAREST,
    };
    vkCmdBlitImage2(command, &blit_info);
    vkr_bindless_vk_cmd_image_barrier(
        command, window->images[image_index], VK_PIPELINE_STAGE_2_BLIT_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  VkBufferMemoryBarrier2 readback_barrier = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
      .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = slot->readback.handle,
      .size = VK_WHOLE_SIZE,
  };
  VkDependencyInfo readback_dependency = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .bufferMemoryBarrierCount = 1u,
      .pBufferMemoryBarriers = &readback_barrier,
  };
  vkCmdPipelineBarrier2(command, &readback_dependency);
  if (!vkr_bindless_vk_flush(renderer, &slot->frame_upload.allocation, 0u,
                             slot->frame_upload_cursor))
    return false_v;
  if (vkEndCommandBuffer(command) != VK_SUCCESS) {
    return false_v;
  }
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_submit_packet(
    VkrBindlessVulkanRenderer *renderer, const VkrRenderPacket *packet,
    VkrBindlessVulkanResult *out_result) {
  if (!renderer || !packet || !renderer->frame_active) {
    return false_v;
  }
  renderer->prepared_frame.picking_pending =
      packet->picking && packet->picking->pending;
  if (packet->shadow) {
    renderer->prepared_frame.shadow_cascade_count =
        Min(packet->shadow->cascade_count, VKR_SHADOW_CASCADE_COUNT_MAX);
  }
  vkr_rg_begin_frame(renderer->graph, &renderer->prepared_frame);
  vkr_rg_set_packet(renderer->graph, packet);
  if (!vkr_rg_build_from_json(renderer->graph, &renderer->json_graph,
                              &renderer->prepared_frame,
                              &renderer->executors)) {
    log_error("Bindless Vulkan failed to build the authored render graph");
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_rg_compile_schedule(renderer->graph)) {
    log_error("Bindless Vulkan failed to compile the authored render graph");
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (renderer->graph->images.length > renderer->config.max_graph_images ||
      renderer->graph->passes.length > renderer->config.max_graph_passes) {
    log_error("Bindless Vulkan authored graph exceeds configured capacity "
              "(%llu/%u images, %llu/%u passes)",
              (unsigned long long)renderer->graph->images.length,
              renderer->config.max_graph_images,
              (unsigned long long)renderer->graph->passes.length,
              renderer->config.max_graph_passes);
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_bindless_vk_validate_graph(renderer)) {
    log_error("Bindless Vulkan failed to validate the authored graph");
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_bindless_vk_realize_graph_images(renderer)) {
    log_error("Bindless Vulkan failed to realize authored graph images");
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  if (!vkr_bindless_vk_plan_capture(renderer, packet, slot)) {
    log_error("Bindless Vulkan failed to plan the requested capture batch");
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  slot->timing_requested = packet->debug && packet->debug->enable_timing &&
                           packet->debug->capture_pass_timestamps;
  slot->timing_collected = !slot->timing_requested;
  slot->timestamp_query_count = 0u;
  if (!vkr_bindless_vk_record_draw(renderer, slot)) {
    if (slot->capture_request_id)
      (void)vkr_capture_ring_fail(&renderer->capture_ring,
                                  slot->capture_request_id,
                                  VKR_RENDERER_ERROR_COMMAND_RECORDING_FAILED);
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  if (!vkr_bindless_vk_flush_publication_ranges(renderer)) {
    if (slot->capture_request_id)
      (void)vkr_capture_ring_fail(&renderer->capture_ring,
                                  slot->capture_request_id,
                                  VKR_RENDERER_ERROR_FRAME_PREPARATION_FAILED);
    vkr_bindless_vulkan_renderer_cancel_frame(renderer);
    return false_v;
  }
  const uint64_t signal_value = renderer->submit_value + 1u;
  VkCommandBufferSubmitInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = slot->command_buffer,
  };
  VkSemaphoreSubmitInfo signal_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = renderer->timeline,
      .value = signal_value,
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  VkSemaphoreSubmitInfo binary_signal = {0};
  VkSemaphoreSubmitInfo acquire_wait = {0};
  VkSemaphoreSubmitInfo signals[2] = {signal_info};
  uint32_t signal_count = 1u;
  uint32_t wait_count = 0u;
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    acquire_wait = (VkSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderer->acquire_semaphores[renderer->active_frame_slot],
        .stageMask = VK_PIPELINE_STAGE_2_BLIT_BIT,
    };
    binary_signal = (VkSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = renderer->window_target.render_complete[slot->image_index],
        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
    };
    signals[signal_count++] = binary_signal;
    wait_count = 1u;
  }
  VkSubmitInfo2 submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = wait_count,
      .pWaitSemaphoreInfos = wait_count ? &acquire_wait : NULL,
      .commandBufferInfoCount = 1u,
      .pCommandBufferInfos = &command_info,
      .signalSemaphoreInfoCount = signal_count,
      .pSignalSemaphoreInfos = signals,
  };
  if (vkQueueSubmit2(vkr_bindless_vulkan_device_queue(renderer->device), 1u,
                     &submit_info, VK_NULL_HANDLE) != VK_SUCCESS) {
    if (slot->capture_request_id)
      (void)vkr_capture_ring_fail(&renderer->capture_ring,
                                  slot->capture_request_id,
                                  VKR_RENDERER_ERROR_SUBMISSION_FAILED);
    vkr_gpu_submit_ring_cancel(&renderer->command_ring,
                               renderer->active_command_slice);
    renderer->frame_active = false_v;
    renderer->terminal_failure = true_v;
    return false_v;
  }
  renderer->submit_value = signal_value;
  slot->retire_value = signal_value;
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN)
    vkr_bindless_vulkan_reacquire_record(
        &renderer->window_target.reacquire_state,
        slot->reacquired_presented_image, signal_value);
  uint32_t pending_ibl_write = 0u;
  const uint32_t pending_ibl_count = renderer->pending_ibl_bake_count;
  for (uint32_t i = 0u; i < pending_ibl_count; ++i) {
    VkrBindlessVkPendingIblBake *job = &renderer->pending_ibl_bakes[i];
    if (!job->recorded) {
      if (pending_ibl_write != i)
        renderer->pending_ibl_bakes[pending_ibl_write] = *job;
      pending_ibl_write++;
      continue;
    }
    const VkrTextureHandle handles[] = {job->equirect, job->source,
                                        job->irradiance, job->prefilter};
    for (uint32_t handle_index = job->convert_equirect ? 0u : 1u;
         handle_index < ArrayCount(handles); ++handle_index) {
      VkrBindlessVkPublishedTexture *texture =
          vkr_bindless_vk_texture_publication(renderer, handles[handle_index]);
      if (texture) {
        texture->last_use_submit_value =
            Max(texture->last_use_submit_value, signal_value);
        if (!texture->ibl_reference_count)
          log_fatal("Bindless Vulkan lost an IBL texture ownership reference");
        texture->ibl_reference_count--;
        if (!texture->ibl_reference_count && texture->unpublish_requested) {
          texture->live = false_v;
          texture->pending_retire = true_v;
        }
      }
    }
    MemZero(&renderer->pending_ibl_bakes[i],
            sizeof(renderer->pending_ibl_bakes[i]));
  }
  for (uint32_t i = pending_ibl_write; i < pending_ibl_count; ++i)
    MemZero(&renderer->pending_ibl_bakes[i],
            sizeof(renderer->pending_ibl_bakes[i]));
  renderer->pending_ibl_bake_count = pending_ibl_write;
  if (slot->capture_request_id &&
      !vkr_capture_ring_submit(&renderer->capture_ring,
                               slot->capture_request_id, signal_value,
                               slot->capture_readback.allocation.mapped)) {
    log_fatal("Bindless Vulkan lost a reserved capture after queue submit");
  }
  vkr_bindless_vk_commit_buffer_initializations(renderer, signal_value);
  vkr_bindless_vk_commit_texture_initializations(renderer, signal_value);
  renderer->targets.images[slot->image_index].layout =
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  renderer->sentinel_image.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  if (renderer->active_geometry)
    renderer->published_geometries[renderer->active_geometry_record_index]
        .last_use_submit_value = signal_value;
  if (vkr_gpu_submit_ring_submit(&renderer->command_ring,
                                 renderer->active_command_slice,
                                 signal_value) != VKR_GPU_SUBMIT_RING_STATUS_OK)
    log_fatal("Vulkan command ring lost its acquired slot after queue submit");
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    VkrBindlessVkWindowTarget *window = &renderer->window_target;
    const uint32_t image_index = slot->image_index;
    window->image_last_submit_value[image_index] = signal_value;
    const VkSwapchainPresentFenceInfoKHR present_fence_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1u,
        .pFences = &window->present_complete[image_index],
    };
    VkPresentInfoKHR present_info = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext =
            window->present_complete[image_index] ? &present_fence_info : NULL,
        .waitSemaphoreCount = 1u,
        .pWaitSemaphores = &window->render_complete[image_index],
        .swapchainCount = 1u,
        .pSwapchains = &window->swapchain,
        .pImageIndices = &image_index,
    };
    const VkResult present_result = vkQueuePresentKHR(
        vkr_bindless_vulkan_device_queue(renderer->device), &present_info);
    const VkrBindlessVulkanPresentResult disposition =
        vkr_bindless_vulkan_present_result_classify(present_result);
    if (disposition.present_completion_tracking_required) {
      window->image_presented[image_index] = true_v;
      window->present_fence_pending[image_index] =
          window->present_complete[image_index] != VK_NULL_HANDLE;
    }
    if (disposition.target_recreate_required)
      renderer->target_dirty = true_v;
    if (disposition.acquired_image_recovery_required ||
        !disposition.enqueue_state_known || disposition.device_lost) {
      renderer->frame_active = false_v;
      renderer->terminal_failure = true_v;
      return false_v;
    }
    slot->acquired_window_image = false_v;
  }
  renderer->sentinel_uploaded = true_v;
  renderer->frame_active = false_v;
  vkr_rg_end_frame(renderer->graph);
  if (out_result) {
    *out_result = (VkrBindlessVulkanResult){
        .submit_value = signal_value,
        .source_frame_index = slot->source_frame_index,
        .indexed_draw_count = slot->indexed_draw_count,
        .shadow_draw_count = slot->shadow_draw_count,
        .opaque_draw_count = slot->opaque_draw_count,
        .transmission_draw_count = slot->transmission_draw_count,
        .blend_draw_count = slot->blend_draw_count,
        .image_index = slot->image_index,
        .pass_timing_count = slot->pass_timing_count,
    };
    MemCopy(out_result->shadow_opaque_draw_count,
            slot->shadow_opaque_draw_count,
            sizeof(out_result->shadow_opaque_draw_count));
    MemCopy(out_result->shadow_alpha_draw_count, slot->shadow_alpha_draw_count,
            sizeof(out_result->shadow_alpha_draw_count));
    MemCopy(out_result->pass_timings, slot->pass_timings,
            (uint64_t)slot->pass_timing_count *
                sizeof(*out_result->pass_timings));
  }
  return true_v;
}

bool8_t
vkr_bindless_vulkan_renderer_poll_result(VkrBindlessVulkanRenderer *renderer,
                                         uint64_t after_submit_value,
                                         VkrBindlessVulkanResult *out_result) {
  if (!renderer || !out_result) {
    return false_v;
  }
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  VkrBindlessVkFrameSlot *best = NULL;
  for (uint32_t i = 0; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->retire_value > after_submit_value &&
        slot->retire_value <= completed &&
        (!best || slot->retire_value < best->retire_value)) {
      best = slot;
    }
  }
  if (!best || !vkr_bindless_vk_invalidate(renderer, &best->readback.allocation,
                                           0u, 4u)) {
    return false_v;
  }
  if (best->timing_requested && !best->timing_collected &&
      !vkr_bindless_vk_collect_slot_timings(renderer, best))
    return false_v;
  const uint8_t *color = best->readback.allocation.mapped;
  *out_result = (VkrBindlessVulkanResult){
      .submit_value = best->retire_value,
      .source_frame_index = best->source_frame_index,
      .indexed_draw_count = best->indexed_draw_count,
      .shadow_draw_count = best->shadow_draw_count,
      .opaque_draw_count = best->opaque_draw_count,
      .transmission_draw_count = best->transmission_draw_count,
      .blend_draw_count = best->blend_draw_count,
      .image_index = best->image_index,
      .color = {color[0], color[1], color[2], color[3]},
      .identifier = (uint32_t)color[0] | ((uint32_t)color[1] << 8u) |
                    ((uint32_t)color[2] << 16u) | ((uint32_t)color[3] << 24u),
      .pass_timing_count = best->pass_timing_count,
      .readback_ready = true_v,
  };
  MemCopy(out_result->pass_timings, best->pass_timings,
          (uint64_t)best->pass_timing_count *
              sizeof(*out_result->pass_timings));
  return true_v;
}

VkrCaptureStatus
vkr_bindless_vulkan_renderer_capture_poll(VkrBindlessVulkanRenderer *renderer,
                                          VkrCaptureRequestId request_id,
                                          VkrCapturePollResult *out_result) {
  if (!renderer || !out_result || request_id == 0u) {
    if (out_result)
      MemZero(out_result, sizeof(*out_result));
    return VKR_CAPTURE_STATUS_NOT_FOUND;
  }
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  if (!vkr_bindless_vk_collect_captures(renderer, completed)) {
    MemZero(out_result, sizeof(*out_result));
    out_result->status = VKR_CAPTURE_STATUS_FAILED;
    out_result->error = VKR_RENDERER_ERROR_DEVICE_ERROR;
    return out_result->status;
  }
  return vkr_capture_ring_poll(&renderer->capture_ring, request_id, completed,
                               out_result);
}

bool8_t vkr_bindless_vulkan_renderer_capture_release(
    VkrBindlessVulkanRenderer *renderer, VkrCaptureRequestId request_id) {
  return renderer && request_id != 0u &&
         vkr_capture_ring_release(&renderer->capture_ring, request_id);
}

static bool8_t
vkr_bindless_vk_recreate_window_target(VkrBindlessVulkanRenderer *renderer,
                                       uint32_t width, uint32_t height,
                                       uint32_t image_count) {
  if (renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN)
    return false_v;
  if (!vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  vkr_bindless_vk_collect_retired_window_targets(renderer,
                                                 renderer->completed_value);
  VkrBindlessVkRetiredWindowTarget *retired = NULL;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    if (!renderer->retired_window_targets[i].occupied) {
      retired = &renderer->retired_window_targets[i];
      break;
    }
  }
  if (!retired) {
    log_error("Bindless Vulkan exhausted %u deferred swapchains before a "
              "successor presentation completed",
              (uint32_t)ArrayCount(renderer->retired_window_targets));
    return false_v;
  }

  VkrBindlessVkWindowTarget replacement_window = {0};
  if (!vkr_bindless_vk_create_window_target(
          renderer, width, height, image_count,
          renderer->window_target.swapchain, &replacement_window))
    return false_v;
  VkrBindlessVkTargetSet replacement_targets = {0};
  if (!vkr_bindless_vk_create_target_set(
          renderer, replacement_window.width, replacement_window.height,
          replacement_window.image_count, &replacement_targets)) {
    vkr_bindless_vk_destroy_window_target(renderer, &replacement_window);
    return false_v;
  }
  retired->target = renderer->window_target;
  retired->occupied = true_v;
  renderer->wsi_retired_swapchains++;
  renderer->window_target = replacement_window;
  vkr_bindless_vk_destroy_target_set(renderer, &renderer->targets);
  renderer->targets = replacement_targets;
  renderer->config.width = replacement_window.width;
  renderer->config.height = replacement_window.height;
  renderer->config.image_count = replacement_window.image_count;
  renderer->target_dirty = false_v;
  return true_v;
}

static bool8_t
vkr_bindless_vk_present_cancelled_frame(VkrBindlessVulkanRenderer *renderer,
                                        VkrBindlessVkFrameSlot *slot) {
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  VkrBindlessVkWindowTarget *window = &renderer->window_target;
  const uint32_t image_index = slot->image_index;
  if (vkResetCommandPool(device, slot->command_pool, 0u) != VK_SUCCESS)
    return false_v;
  const VkCommandBufferBeginInfo begin_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
  };
  if (vkBeginCommandBuffer(slot->command_buffer, &begin_info) != VK_SUCCESS)
    return false_v;
  if (!window->image_presented[image_index]) {
    vkr_bindless_vk_cmd_image_barrier(
        slot->command_buffer, window->images[image_index],
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  if (vkEndCommandBuffer(slot->command_buffer) != VK_SUCCESS)
    return false_v;

  const uint64_t signal_value = renderer->submit_value + 1u;
  const VkCommandBufferSubmitInfo command_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = slot->command_buffer,
  };
  const VkSemaphoreSubmitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
      .semaphore = renderer->acquire_semaphores[renderer->active_frame_slot],
      .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
  };
  const VkSemaphoreSubmitInfo signals[] = {
      {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = renderer->timeline,
          .value = signal_value,
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
          .semaphore = window->render_complete[image_index],
          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      },
  };
  const VkSubmitInfo2 submit_info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = 1u,
      .pWaitSemaphoreInfos = &wait_info,
      .commandBufferInfoCount = 1u,
      .pCommandBufferInfos = &command_info,
      .signalSemaphoreInfoCount = ArrayCount(signals),
      .pSignalSemaphoreInfos = signals,
  };
  if (vkQueueSubmit2(vkr_bindless_vulkan_device_queue(renderer->device), 1u,
                     &submit_info, VK_NULL_HANDLE) != VK_SUCCESS)
    return false_v;

  renderer->submit_value = signal_value;
  slot->retire_value = signal_value;
  vkr_bindless_vulkan_reacquire_record(
      &window->reacquire_state, slot->reacquired_presented_image, signal_value);
  window->image_last_submit_value[image_index] = signal_value;
  if (vkr_gpu_submit_ring_submit(&renderer->command_ring,
                                 renderer->active_command_slice,
                                 signal_value) != VKR_GPU_SUBMIT_RING_STATUS_OK)
    log_fatal("Vulkan command ring lost a cancelled frame after queue submit");

  const VkSwapchainPresentFenceInfoKHR present_fence_info = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
      .swapchainCount = 1u,
      .pFences = &window->present_complete[image_index],
  };
  const VkPresentInfoKHR present_info = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .pNext =
          window->present_complete[image_index] ? &present_fence_info : NULL,
      .waitSemaphoreCount = 1u,
      .pWaitSemaphores = &window->render_complete[image_index],
      .swapchainCount = 1u,
      .pSwapchains = &window->swapchain,
      .pImageIndices = &image_index,
  };
  const VkrBindlessVulkanPresentResult disposition =
      vkr_bindless_vulkan_present_result_classify(vkQueuePresentKHR(
          vkr_bindless_vulkan_device_queue(renderer->device), &present_info));
  if (disposition.present_completion_tracking_required) {
    window->image_presented[image_index] = true_v;
    window->present_fence_pending[image_index] =
        window->present_complete[image_index] != VK_NULL_HANDLE;
  }
  if (disposition.target_recreate_required)
    renderer->target_dirty = true_v;
  if (!disposition.enqueue_state_known || disposition.device_lost ||
      disposition.acquired_image_recovery_required) {
    log_error("Bindless Vulkan could not enqueue cancelled-frame presentation");
    renderer->terminal_failure = true_v;
  }
  slot->acquired_window_image = false_v;
  slot->reacquired_presented_image = false_v;
  return true_v;
}

void vkr_bindless_vulkan_renderer_cancel_frame(
    VkrBindlessVulkanRenderer *renderer) {
  if (!renderer)
    return;
  VkrBindlessVkFrameSlot *slot =
      &renderer->frame_slots[renderer->active_frame_slot];
  const bool8_t submitted =
      slot->acquired_window_image
          ? vkr_bindless_vk_present_cancelled_frame(renderer, slot)
          : false_v;
  if (!submitted)
    vkr_gpu_submit_ring_cancel(&renderer->command_ring,
                               renderer->active_command_slice);
  if (slot->acquired_window_image && !submitted)
    renderer->terminal_failure = true_v;
  renderer->frame_active = false_v;
  vkr_rg_end_frame(renderer->graph);
}

bool8_t vkr_bindless_vulkan_renderer_resize(VkrBindlessVulkanRenderer *renderer,
                                            uint32_t width, uint32_t height,
                                            uint32_t image_count) {
  if (!renderer || renderer->frame_active || renderer->terminal_failure ||
      !width || !height || !image_count ||
      image_count > VKR_BINDLESS_VK_TARGET_IMAGE_MAX) {
    return false_v;
  }
  if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN) {
    renderer->config.width = width;
    renderer->config.height = height;
    renderer->config.image_count = image_count;
    renderer->target_dirty = true_v;
    return true_v;
  }
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_retired_targets(renderer, completed);
  VkrBindlessVkRetiredTargetSet *retired = NULL;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
    if (!renderer->retired_targets[i].occupied) {
      retired = &renderer->retired_targets[i];
      break;
    }
  }
  if (!retired) {
    return false_v;
  }
  VkrBindlessVkTargetSet replacement;
  if (!vkr_bindless_vk_create_target_set(renderer, width, height, image_count,
                                         &replacement)) {
    return false_v;
  }
  retired->targets = renderer->targets;
  retired->retire_value = renderer->submit_value;
  retired->occupied = true_v;
  renderer->targets = replacement;
  renderer->config.width = width;
  renderer->config.height = height;
  renderer->config.image_count = image_count;
  renderer->next_image_index = 0u;
  vkr_bindless_vk_collect_retired_targets(renderer, completed);
  return true_v;
}

bool8_t
vkr_bindless_vulkan_renderer_wait_idle(VkrBindlessVulkanRenderer *renderer) {
  if (!renderer || !renderer->timeline || renderer->submit_value == 0u) {
    return renderer != NULL;
  }
  const VkSemaphoreWaitInfo wait_info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
      .semaphoreCount = 1u,
      .pSemaphores = &renderer->timeline,
      .pValues = &renderer->submit_value,
  };
  if (vkWaitSemaphores(vkr_bindless_vk_renderer_device(renderer), &wait_info,
                       UINT64_MAX) != VK_SUCCESS) {
    return false_v;
  }
  vkr_bindless_vk_refresh_completed(renderer);
  if (!vkr_bindless_vk_collect_captures(renderer, renderer->completed_value))
    return false_v;
  vkr_bindless_vk_collect_retired_targets(renderer, renderer->completed_value);
  vkr_bindless_vk_collect_retired_window_targets(renderer,
                                                 renderer->completed_value);
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  return true_v;
}

uint64_t vkr_bindless_vulkan_renderer_submit_value(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer ? renderer->submit_value : 0u;
}

uint64_t vkr_bindless_vulkan_renderer_completed_value(
    const VkrBindlessVulkanRenderer *renderer) {
  if (!renderer) {
    return 0u;
  }
  return vkr_bindless_vk_refresh_completed(
      (VkrBindlessVulkanRenderer *)renderer);
}

bool8_t vkr_bindless_vulkan_renderer_get_and_reset_upload_wait_count(
    VkrBindlessVulkanRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count)
    return false_v;
  *out_wait_count = renderer->upload_wait_count;
  renderer->upload_wait_count = 0u;
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_get_and_reset_command_slot_wait_count(
    VkrBindlessVulkanRenderer *renderer, uint64_t *out_wait_count) {
  if (!renderer || !out_wait_count) {
    return false_v;
  }
  *out_wait_count = renderer->command_slot_wait_count;
  renderer->command_slot_wait_count = 0u;
  return true_v;
}

void vkr_bindless_vulkan_renderer_memory_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanMemoryMetrics *out_metrics) {
  if (!out_metrics)
    return;
  MemZero(out_metrics, sizeof(*out_metrics));
  if (!renderer)
    return;
  VkrBindlessVkMemoryPoolMetrics metrics = {0};
  vkr_bindless_vulkan_memory_pool_get_metrics(renderer->memory_pool, &metrics);
  out_metrics->physical_allocations_live = metrics.physical_allocations_live;
  out_metrics->physical_allocations_peak = metrics.physical_allocations_peak;
  out_metrics->physical_allocations_created =
      metrics.physical_allocations_created;
  out_metrics->physical_allocated_bytes = metrics.physical_allocated_bytes;
  out_metrics->physical_allocated_bytes_peak =
      metrics.physical_allocated_bytes_peak;
  out_metrics->block_capacity_failures = metrics.block_capacity_failures;
  out_metrics->aggregate = metrics.aggregate;
}

void vkr_bindless_vulkan_renderer_device_memory_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrDeviceMemoryStats *out_stats) {
  if (!out_stats)
    return;
  MemZero(out_stats, sizeof(*out_stats));
  if (!renderer || !renderer->device)
    return;
  VkrBindlessVulkanMemoryMetrics metrics = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &metrics);
  out_stats->live_allocation_count = metrics.physical_allocations_live;
  out_stats->peak_allocation_count = metrics.physical_allocations_peak;
  out_stats->total_allocation_count = metrics.physical_allocations_created;
  out_stats->max_allocation_count =
      vkr_bindless_vulkan_device_properties(renderer->device)
          ->properties.limits.maxMemoryAllocationCount;
  out_stats->live_bytes = metrics.physical_allocated_bytes;
  out_stats->peak_bytes = metrics.physical_allocated_bytes_peak;
  out_stats->live_totals_exact = true_v;

  const VkPhysicalDeviceMemoryProperties *memory =
      vkr_bindless_vulkan_device_memory_properties(renderer->device);
  out_stats->heap_count =
      Min(memory->memoryHeapCount, (uint32_t)VKR_DEVICE_MEMORY_HEAP_MAX);
  for (uint32_t heap = 0; heap < out_stats->heap_count; ++heap) {
    out_stats->heap_size_bytes[heap] = memory->memoryHeaps[heap].size;
    out_stats->heap_budget_bytes[heap] = memory->memoryHeaps[heap].size;
  }
}

void vkr_bindless_vulkan_renderer_heap_metrics(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanHeapMetrics *out_metrics) {
  if (!out_metrics)
    return;
  MemZero(out_metrics, sizeof(*out_metrics));
  if (!renderer)
    return;
  vkr_gpu_slot_table_get_metrics(renderer->sampled_image_slots,
                                 &out_metrics->sampled_images);
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &out_metrics->samplers);
  vkr_gpu_slot_table_get_metrics(renderer->storage_image_slots,
                                 &out_metrics->storage_images);
  vkr_gpu_slot_table_get_metrics(renderer->material_slots,
                                 &out_metrics->materials);
}

void vkr_bindless_vulkan_renderer_wsi_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanWsiStats *out_stats) {
  if (!out_stats)
    return;
  MemZero(out_stats, sizeof(*out_stats));
  if (!renderer)
    return;
  out_stats->reacquire_proofs = renderer->wsi_reacquire_proofs;
  out_stats->retired_swapchains = renderer->wsi_retired_swapchains;
  out_stats->retired_swapchains_collected =
      renderer->wsi_retired_swapchains_collected;
  for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets); ++i) {
    if (renderer->retired_window_targets[i].occupied)
      out_stats->retired_swapchains_live++;
  }
}

uint32_t vkr_bindless_vulkan_renderer_frame_slot(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer ? renderer->active_frame_slot : 0u;
}

bool8_t vkr_bindless_vulkan_renderer_shader_abi_validated(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer && renderer->shader_abi_validated;
}

const VkrBindlessVkCapabilityProfile *vkr_bindless_vulkan_renderer_profile(
    const VkrBindlessVulkanRenderer *renderer) {
  return renderer ? vkr_bindless_vulkan_device_profile(renderer->device) : NULL;
}

VkrAllocator *
vkr_bindless_vulkan_renderer_allocator(VkrBindlessVulkanRenderer *renderer) {
  return renderer ? renderer->allocator : NULL;
}

static bool8_t vkr_bindless_vk_publish_sampled_view(
    VkrBindlessVulkanRenderer *renderer, VkImageView view,
    VkImageLayout image_layout, VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkDescriptorImageInfo image_info = {
      .imageView = view,
      .imageLayout = image_layout,
  };
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
      .data.pSampledImage = &image_info,
  };
  vkr_bindless_vulkan_device_get_descriptor(renderer->device)(
      vkr_bindless_vk_renderer_device(renderer), &get_info,
      properties->sampledImageDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampled_image_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  if (vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                 &renderer->resource_descriptors,
                                 layout->sampled_image_offset +
                                     (VkDeviceSize)out_handle->index *
                                         properties->sampledImageDescriptorSize,
                                 properties->sampledImageDescriptorSize))
    return true_v;
  (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots, *out_handle,
                                  renderer->completed_value);
  (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots,
                                   renderer->completed_value, NULL);
  *out_handle = (VkrGpuSlotHandle){0};
  return false_v;
}

static bool8_t
vkr_bindless_vk_publish_storage_view(VkrBindlessVulkanRenderer *renderer,
                                     VkImageView view,
                                     VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *layout =
      vkr_bindless_vulkan_device_resource_layout(renderer->device);
  const VkDescriptorImageInfo image_info = {
      .imageView = view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
  };
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
      .data.pStorageImage = &image_info,
  };
  vkr_bindless_vulkan_device_get_descriptor(renderer->device)(
      vkr_bindless_vk_renderer_device(renderer), &get_info,
      properties->storageImageDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->storage_image_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  if (vkr_bindless_vk_mark_dirty(&renderer->resource_descriptor_dirty,
                                 &renderer->resource_descriptors,
                                 layout->storage_image_offset +
                                     (VkDeviceSize)out_handle->index *
                                         properties->storageImageDescriptorSize,
                                 properties->storageImageDescriptorSize))
    return true_v;
  (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots, *out_handle,
                                  renderer->completed_value);
  (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots,
                                   renderer->completed_value, NULL);
  *out_handle = (VkrGpuSlotHandle){0};
  return false_v;
}

static bool8_t
vkr_bindless_vk_publish_sampler(VkrBindlessVulkanRenderer *renderer,
                                VkSampler sampler,
                                VkrGpuSlotHandle *out_handle) {
  const VkPhysicalDeviceDescriptorBufferPropertiesEXT *properties =
      vkr_bindless_vulkan_device_descriptor_properties(renderer->device);
  const VkrBindlessVulkanDescriptorLayout *layout =
      vkr_bindless_vulkan_device_sampler_layout(renderer->device);
  const VkDescriptorGetInfoEXT get_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
      .type = VK_DESCRIPTOR_TYPE_SAMPLER,
      .data.pSampler = &sampler,
  };
  vkr_bindless_vulkan_device_get_descriptor(renderer->device)(
      vkr_bindless_vk_renderer_device(renderer), &get_info,
      properties->samplerDescriptorSize, renderer->descriptor_scratch);
  if (vkr_gpu_slot_table_publish(renderer->sampler_slots,
                                 renderer->descriptor_scratch,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->sampler_descriptor_dirty, &renderer->sampler_descriptors,
      layout->sampler_offset +
          (VkDeviceSize)out_handle->index * properties->samplerDescriptorSize,
      properties->samplerDescriptorSize);
}

static bool8_t
vkr_bindless_vk_publish_material_gpu_row(VkrBindlessVulkanRenderer *renderer,
                                         const VkrBindlessVkMaterialGpuRow *row,
                                         VkrGpuSlotHandle *out_handle) {
  if (vkr_gpu_slot_table_publish(renderer->material_slots, row, out_handle) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->material_dirty, &renderer->materials,
      (VkDeviceSize)out_handle->index * sizeof(*row), sizeof(*row));
}

static bool8_t vkr_bindless_vk_replace_material_gpu_row(
    VkrBindlessVulkanRenderer *renderer, VkrGpuSlotHandle old_handle,
    const VkrBindlessVkMaterialGpuRow *row, uint64_t old_last_use_submit_value,
    VkrGpuSlotHandle *out_handle) {
  if (vkr_gpu_slot_table_replace(renderer->material_slots, old_handle, row,
                                 old_last_use_submit_value,
                                 out_handle) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  return vkr_bindless_vk_mark_dirty(
      &renderer->material_dirty, &renderer->materials,
      (VkDeviceSize)out_handle->index * sizeof(*row), sizeof(*row));
}

static bool8_t vkr_bindless_vk_publish_material_row(
    VkrBindlessVulkanRenderer *renderer, uint32_t texture_index,
    uint32_t sampler_index, uint32_t material_id,
    VkrGpuSlotHandle *out_handle) {
  const VkrBindlessVkMaterialGpuRow row = {
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
      .base_color_texture = texture_index,
      .normal_texture = texture_index,
      .orm_texture = texture_index,
      .emissive_texture = texture_index,
      .base_color_sampler = sampler_index,
      .normal_sampler = sampler_index,
      .orm_sampler = sampler_index,
      .emissive_sampler = sampler_index,
      .material_id = material_id,
  };
  return vkr_bindless_vk_publish_material_gpu_row(renderer, &row, out_handle);
}

static VkSamplerAddressMode
vkr_bindless_vk_sampler_address_mode(VkrTextureRepeatMode mode) {
  switch (mode) {
  case VKR_TEXTURE_REPEAT_MODE_MIRRORED_REPEAT:
    return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  case VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_BORDER:
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  default:
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
  }
}

static bool8_t vkr_bindless_vk_create_published_sampler(
    VkrBindlessVulkanRenderer *renderer,
    const VkrTextureDescription *description, uint32_t mip_levels,
    VkSampler *out_sampler) {
  const VkSamplerCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = description->mag_filter == VKR_FILTER_LINEAR
                       ? VK_FILTER_LINEAR
                       : VK_FILTER_NEAREST,
      .minFilter = description->min_filter == VKR_FILTER_LINEAR
                       ? VK_FILTER_LINEAR
                       : VK_FILTER_NEAREST,
      .mipmapMode = description->mip_filter == VKR_MIP_FILTER_LINEAR
                        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                        : VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU =
          vkr_bindless_vk_sampler_address_mode(description->u_repeat_mode),
      .addressModeV =
          vkr_bindless_vk_sampler_address_mode(description->v_repeat_mode),
      .addressModeW =
          vkr_bindless_vk_sampler_address_mode(description->w_repeat_mode),
      .maxLod = description->mip_filter == VKR_MIP_FILTER_NONE
                    ? 0.0f
                    : (float32_t)(mip_levels - 1u),
      .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
  };
  return vkCreateSampler(vkr_bindless_vk_renderer_device(renderer), &info, NULL,
                         out_sampler) == VK_SUCCESS;
}

static bool8_t vkr_bindless_vk_sampler_description_equal(
    const VkrTextureDescription *a, uint32_t a_mip_levels,
    const VkrTextureDescription *b, uint32_t b_mip_levels) {
  if (!a || !b)
    return false_v;
  const uint32_t a_sampled_mip_levels =
      a->mip_filter == VKR_MIP_FILTER_NONE ? 1u : a_mip_levels;
  const uint32_t b_sampled_mip_levels =
      b->mip_filter == VKR_MIP_FILTER_NONE ? 1u : b_mip_levels;
  return a->min_filter == b->min_filter && a->mag_filter == b->mag_filter &&
         a->mip_filter == b->mip_filter &&
         a->u_repeat_mode == b->u_repeat_mode &&
         a->v_repeat_mode == b->v_repeat_mode &&
         a->w_repeat_mode == b->w_repeat_mode &&
         a_sampled_mip_levels == b_sampled_mip_levels;
}

static uint32_t vkr_bindless_vk_mip_count(uint32_t width, uint32_t height) {
  uint32_t levels = 1u;
  for (uint32_t extent = Max(width, height); extent > 1u; extent >>= 1u)
    levels++;
  return levels;
}

static bool8_t
vkr_bindless_vk_acquire_sampler(VkrBindlessVulkanRenderer *renderer,
                                const VkrTextureDescription *description,
                                uint32_t mip_levels,
                                uint32_t *out_record_index) {
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    VkrBindlessVkPublishedSampler *record = &renderer->published_samplers[i];
    if (record->live && vkr_bindless_vk_sampler_description_equal(
                            &record->description, record->mip_levels,
                            description, mip_levels)) {
      record->reference_count++;
      *out_record_index = i;
      return true_v;
    }
  }
  uint32_t free_index = UINT32_MAX;
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    if (!renderer->published_samplers[i].live &&
        !renderer->published_samplers[i].pending_retire) {
      free_index = i;
      break;
    }
  }
  if (free_index == UINT32_MAX)
    return false_v;
  VkSampler sampler = VK_NULL_HANDLE;
  VkrGpuSlotHandle slot = {0};
  if (!vkr_bindless_vk_create_published_sampler(renderer, description,
                                                mip_levels, &sampler) ||
      !vkr_bindless_vk_publish_sampler(renderer, sampler, &slot)) {
    if (sampler)
      vkDestroySampler(vkr_bindless_vk_renderer_device(renderer), sampler,
                       NULL);
    return false_v;
  }
  renderer->published_samplers[free_index] = (VkrBindlessVkPublishedSampler){
      .description = *description,
      .sampler = sampler,
      .slot = slot,
      .mip_levels = mip_levels,
      .reference_count = 1u,
      .live = true_v,
  };
  *out_record_index = free_index;
  return true_v;
}

static bool8_t
vkr_bindless_vk_release_sampler(VkrBindlessVulkanRenderer *renderer,
                                uint32_t record_index,
                                uint64_t last_use_submit_value) {
  if (record_index >= renderer->config.sampler_capacity)
    return false_v;
  VkrBindlessVkPublishedSampler *record =
      &renderer->published_samplers[record_index];
  if (!record->live || !record->reference_count)
    return false_v;
  if (record->reference_count > 1u) {
    record->reference_count--;
    record->last_use_submit_value =
        Max(record->last_use_submit_value, last_use_submit_value);
    return true_v;
  }
  last_use_submit_value =
      Max(record->last_use_submit_value, last_use_submit_value);
  if (vkr_gpu_slot_table_retire(renderer->sampler_slots, record->slot,
                                last_use_submit_value) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  record->reference_count = 0u;
  record->last_use_submit_value = last_use_submit_value;
  record->live = false_v;
  record->pending_retire = true_v;
  return true_v;
}

static void
vkr_bindless_vk_collect_samplers(VkrBindlessVulkanRenderer *renderer,
                                 uint64_t completed) {
  (void)vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL);
  for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
    VkrBindlessVkPublishedSampler *record = &renderer->published_samplers[i];
    if (!record->pending_retire || record->last_use_submit_value > completed)
      continue;
    vkDestroySampler(vkr_bindless_vk_renderer_device(renderer), record->sampler,
                     NULL);
    MemZero(record, sizeof(*record));
  }
}

static void vkr_bindless_vk_prepare_writable_initialization(
    VkrTextureHandle texture, VkrBindlessVkImage *image,
    VkrBindlessVkPendingTextureInitialization *out_initialization) {
  *out_initialization = (VkrBindlessVkPendingTextureInitialization){
      .texture = texture,
      .writable = true_v,
  };
  image->layout = VK_IMAGE_LAYOUT_GENERAL;
}

static VkrBindlessVkPublishedTexture *
vkr_bindless_vk_published_texture(VkrBindlessVulkanRenderer *renderer,
                                  VkrTextureHandle handle,
                                  uint32_t *out_index) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity)
    return NULL;
  const uint32_t index = handle.id - 1u;
  VkrBindlessVkPublishedTexture *texture = &renderer->published_textures[index];
  if (!texture->live || texture->handle.generation != handle.generation)
    return NULL;
  if (out_index)
    *out_index = index;
  return texture;
}

static VkrBindlessVkPublishedTexture *
vkr_bindless_vk_texture_publication(VkrBindlessVulkanRenderer *renderer,
                                    VkrTextureHandle handle) {
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity)
    return NULL;
  VkrBindlessVkPublishedTexture *active =
      &renderer->published_textures[handle.id - 1u];
  if ((active->live || active->pending_retire) &&
      active->handle.id == handle.id &&
      active->handle.generation == handle.generation)
    return active;
  for (uint32_t i = 0u; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedTexture *retired = &renderer->retired_textures[i];
    if (retired->pending_retire && retired->handle.id == handle.id &&
        retired->handle.generation == handle.generation)
      return retired;
  }
  return NULL;
}

bool8_t vkr_bindless_vulkan_renderer_hdr_ibl_limits(
    const VkrBindlessVulkanRenderer *renderer, uint32_t *out_max_cube_extent,
    uint32_t *out_max_mip_levels) {
  if (out_max_cube_extent)
    *out_max_cube_extent = 0u;
  if (out_max_mip_levels)
    *out_max_mip_levels = 0u;
  if (!renderer || !renderer->device)
    return false_v;
  VkFormatProperties3 properties3 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,
  };
  VkFormatProperties2 properties2 = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &properties3,
  };
  vkGetPhysicalDeviceFormatProperties2(
      vkr_bindless_vulkan_device_physical(renderer->device),
      VK_FORMAT_R16G16B16A16_SFLOAT, &properties2);
  const VkFormatFeatureFlags2 required =
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_2_BLIT_SRC_BIT |
      VK_FORMAT_FEATURE_2_BLIT_DST_BIT |
      VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((properties3.optimalTilingFeatures & required) != required)
    return false_v;
  const VkPhysicalDeviceProperties2 *device_properties =
      vkr_bindless_vulkan_device_properties(renderer->device);
  if (!device_properties)
    return false_v;
  const uint32_t extent =
      Min(device_properties->properties.limits.maxImageDimensionCube,
          (uint32_t)VKR_IBL_PREFILTER_SIZE);
  if (!extent)
    return false_v;
  uint32_t mip_count = 1u;
  for (uint32_t size = extent; size > 1u; size >>= 1u)
    mip_count++;
  mip_count = Min(mip_count, (uint32_t)VKR_IBL_PREFILTER_MIP_COUNT);
  if (out_max_cube_extent)
    *out_max_cube_extent = extent;
  if (out_max_mip_levels)
    *out_max_mip_levels = mip_count;
  return true_v;
}

VkrRendererError vkr_bindless_vulkan_renderer_get_pixel_readback_result(
    VkrBindlessVulkanRenderer *renderer, VkrPixelReadbackResult *out_result) {
  if (!renderer || !out_result)
    return VKR_RENDERER_ERROR_INVALID_PARAMETER;
  *out_result = (VkrPixelReadbackResult){.status = VKR_READBACK_STATUS_IDLE};
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  VkrBindlessVkFrameSlot *best = NULL;
  for (uint32_t i = 0u; i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->picking_readback_pending &&
        (!best || slot->retire_value < best->retire_value))
      best = slot;
  }
  if (!best)
    return VKR_RENDERER_ERROR_NONE;
  out_result->x = best->picking_x;
  out_result->y = best->picking_y;
  if (best->retire_value > completed) {
    out_result->status = VKR_READBACK_STATUS_PENDING;
    return VKR_RENDERER_ERROR_NONE;
  }
  if (!vkr_bindless_vk_invalidate(renderer, &best->readback.allocation, 0u,
                                  sizeof(uint32_t))) {
    out_result->status = VKR_READBACK_STATUS_ERROR;
    return VKR_RENDERER_ERROR_DEVICE_ERROR;
  }
  MemCopy(&out_result->data, best->readback.allocation.mapped,
          sizeof(out_result->data));
  out_result->valid = true_v;
  out_result->status = VKR_READBACK_STATUS_READY;
  best->picking_readback_pending = false_v;
  return VKR_RENDERER_ERROR_NONE;
}

bool8_t vkr_bindless_vulkan_renderer_texture_format_supported(
    const VkrBindlessVulkanRenderer *renderer, VkrTextureFormat format) {
  if (!renderer || !renderer->device)
    return false_v;
  const VkFormat native_format = vkr_bindless_vk_texture_format(format);
  if (native_format == VK_FORMAT_UNDEFINED)
    return false_v;
  VkFormatProperties properties = {0};
  vkGetPhysicalDeviceFormatProperties(
      vkr_bindless_vulkan_device_physical(renderer->device), native_format,
      &properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return (properties.optimalTilingFeatures & required) == required;
}

bool8_t vkr_bindless_vulkan_renderer_graph_resource_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrRenderGraphResourceStats *out_stats) {
  if (!renderer || !renderer->graph || !out_stats)
    return false_v;
  MemZero(out_stats, sizeof(*out_stats));
  for (uint32_t i = 0u; i < renderer->config.max_graph_images; ++i) {
    const VkrBindlessVkGraphImage *image = &renderer->graph_images[i];
    if (!image->live || image->external_swapchain)
      continue;
    VkrTextureFormatInfo format = {0};
    if (!vkr_texture_format_get_info(image->desc.format, &format) ||
        format.block_width != 1u || format.block_height != 1u)
      continue;
    uint64_t texels = 0u;
    for (uint32_t mip = 0u; mip < Max(image->desc.mip_levels, 1u); ++mip) {
      texels += (uint64_t)Max(image->desc.width >> mip, 1u) *
                Max(image->desc.height >> mip, 1u);
    }
    const uint64_t bytes_per_image = texels * Max(image->desc.layers, 1u) *
                                     Max(image->desc.samples, 1u) *
                                     format.bytes_per_block;
    out_stats->live_image_textures += image->instance_count;
    out_stats->live_image_bytes += bytes_per_image * image->instance_count;
  }
  out_stats->peak_image_textures = out_stats->live_image_textures;
  out_stats->peak_image_bytes = out_stats->live_image_bytes;
  return true_v;
}

void vkr_bindless_vulkan_renderer_target_information(
    const VkrBindlessVulkanRenderer *renderer, VkrPresentMode *out_present_mode,
    VkrSurfaceColorFormat *out_color_format,
    VkrSurfaceDepthFormat *out_depth_format,
    VkrSurfaceColorSpace *out_color_space, float32_t *out_max_anisotropy) {
  if (!renderer)
    return;
  const bool8_t offscreen =
      renderer->config.target_kind == VKR_PRESENT_TARGET_OFFSCREEN;
  const VkFormat format =
      offscreen ? VK_FORMAT_R8G8B8A8_UNORM : renderer->window_target.format;
  if (out_present_mode) {
    *out_present_mode =
        offscreen ? VKR_PRESENT_MODE_DEFAULT
        : renderer->window_target.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR
            ? VKR_PRESENT_MODE_IMMEDIATE
        : renderer->window_target.present_mode == VK_PRESENT_MODE_MAILBOX_KHR
            ? VKR_PRESENT_MODE_MAILBOX
            : VKR_PRESENT_MODE_FIFO;
  }
  if (out_color_format) {
    *out_color_format = format == VK_FORMAT_B8G8R8A8_SRGB
                            ? VKR_SURFACE_COLOR_FORMAT_BGRA8_SRGB
                        : format == VK_FORMAT_R8G8B8A8_SRGB
                            ? VKR_SURFACE_COLOR_FORMAT_RGBA8_SRGB
                        : format == VK_FORMAT_B8G8R8A8_UNORM
                            ? VKR_SURFACE_COLOR_FORMAT_BGRA8_UNORM
                            : VKR_SURFACE_COLOR_FORMAT_RGBA8_UNORM;
  }
  if (out_depth_format)
    *out_depth_format = VKR_SURFACE_DEPTH_FORMAT_D32_SFLOAT;
  if (out_color_space)
    *out_color_space = VKR_SURFACE_COLOR_SPACE_SRGB_NONLINEAR;
  if (out_max_anisotropy) {
    const VkPhysicalDeviceProperties2 *properties =
        vkr_bindless_vulkan_device_properties(renderer->device);
    *out_max_anisotropy =
        properties ? properties->properties.limits.maxSamplerAnisotropy : 1.0f;
  }
}

static void vkr_bindless_vk_destroy_texture_storage_views(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkPublishedTexture *texture) {
  const VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (texture->storage_views[i])
      vkDestroyImageView(device, texture->storage_views[i], NULL);
    texture->storage_views[i] = VK_NULL_HANDLE;
  }
  texture->storage_slot_count = 0u;
}

static bool8_t vkr_bindless_vk_retire_unreferenced_texture(
    VkrBindlessVulkanRenderer *renderer, VkrBindlessVkPublishedTexture *texture,
    uint64_t completed) {
  if (!texture->pending_retire || texture->material_reference_count != 0u ||
      texture->ibl_reference_count != 0u || texture->initialization_pending ||
      texture->last_use_submit_value > completed)
    return true_v;

  VkrBindlessVkPublishedSampler *sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  bool8_t storage_can_retire = true_v;
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (vkr_gpu_slot_table_can_retire(renderer->storage_image_slots,
                                      texture->storage_slots[i]) !=
        VKR_GPU_SLOT_STATUS_OK) {
      storage_can_retire = false_v;
      break;
    }
  }
  if (vkr_gpu_slot_table_can_retire(renderer->sampled_image_slots,
                                    texture->sampled_slot) !=
          VKR_GPU_SLOT_STATUS_OK ||
      !storage_can_retire ||
      (sampler->reference_count == 1u &&
       vkr_gpu_slot_table_can_retire(renderer->sampler_slots, sampler->slot) !=
           VKR_GPU_SLOT_STATUS_OK)) {
    log_error("Bindless Vulkan texture retirement could not reserve every "
              "descriptor retirement; preserving the complete publication");
    return false_v;
  }

  if (vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                texture->sampled_slot,
                                completed) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                  texture->storage_slots[i],
                                  completed) != VKR_GPU_SLOT_STATUS_OK)
      log_fatal("Bindless Vulkan lost a validated storage-image retirement");
  }
  if (!vkr_bindless_vk_release_sampler(renderer, texture->sampler_record_index,
                                       completed) ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK) {
    log_error("Bindless Vulkan failed to retire a validated texture "
              "publication; preserving the native texture");
    return false_v;
  }
  if (!vkr_bindless_vk_retire_allocation(renderer, &texture->image.allocation,
                                         completed))
    log_fatal("Bindless Vulkan failed to retire completed texture memory");
  vkr_bindless_vk_destroy_texture_storage_views(renderer, texture);
  vkr_bindless_vk_destroy_image(renderer, &texture->image);
  MemZero(texture, sizeof(*texture));
  return true_v;
}

static VkrBindlessVkPublishedTexture *
vkr_bindless_vk_reserve_retired_texture(VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedTexture *retired = &renderer->retired_textures[i];
    if (!retired->pending_retire)
      return retired;
  }
  return NULL;
}

static void
vkr_bindless_vk_collect_asset_publications(VkrBindlessVulkanRenderer *renderer,
                                           uint64_t completed) {
  (void)vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL);
  vkr_bindless_vk_collect_samplers(renderer, completed);
  for (uint32_t i = 0; i < renderer->retired_staging_buffer_capacity; ++i) {
    VkrBindlessVkRetiredStagingBuffer *retired =
        &renderer->retired_staging_buffers[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    vkr_bindless_vk_destroy_buffer(renderer, &retired->buffer);
    renderer->staging_buffer_count--;
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrBindlessVkRetiredMaterial *retired = &renderer->retired_materials[i];
    if (!retired->occupied || retired->retire_value > completed)
      continue;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (retired->texture_record_indices[texture_slot] == UINT32_MAX)
        continue;
      VkrBindlessVkPublishedTexture *texture =
          &renderer->published_textures
               [retired->texture_record_indices[texture_slot]];
      if (texture->material_reference_count > 0u)
        texture->material_reference_count--;
    }
    MemZero(retired, sizeof(*retired));
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[i];
    if (!geometry->pending_retire ||
        geometry->last_use_submit_value > completed)
      continue;
    vkr_bindless_vk_destroy_buffer(renderer, &geometry->indices);
    vkr_bindless_vk_destroy_buffer(renderer, &geometry->vertices);
    if (geometry->submeshes)
      vkr_allocator_free(renderer->allocator, geometry->submeshes,
                         geometry->submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(geometry, sizeof(*geometry));
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedGeometry *geometry = &renderer->retired_geometries[i];
    if (!geometry->pending_retire ||
        geometry->last_use_submit_value > completed)
      continue;
    vkr_bindless_vk_destroy_buffer(renderer, &geometry->indices);
    vkr_bindless_vk_destroy_buffer(renderer, &geometry->vertices);
    if (geometry->submeshes)
      vkr_allocator_free(renderer->allocator, geometry->submeshes,
                         geometry->submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    MemZero(geometry, sizeof(*geometry));
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i)
    vkr_bindless_vk_retire_unreferenced_texture(
        renderer, &renderer->published_textures[i], completed);
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i)
    vkr_bindless_vk_retire_unreferenced_texture(
        renderer, &renderer->retired_textures[i], completed);
  vkr_bindless_vk_collect_samplers(renderer, completed);
}

static VkrBindlessVkRetiredMaterial *
vkr_bindless_vk_reserve_material_retirement(
    VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    if (!renderer->retired_materials[i].occupied)
      return &renderer->retired_materials[i];
  }
  return NULL;
}

static bool8_t vkr_bindless_vk_create_published_buffer(
    VkrBindlessVulkanRenderer *renderer, const void *data, uint64_t size,
    VkBufferUsageFlags usage, VkPipelineStageFlags2 destination_stage,
    VkAccessFlags2 destination_access, uint32_t geometry_record_index,
    VkrBindlessVkBuffer *out_buffer,
    VkrBindlessVkPendingBufferInitialization *out_initialization) {
  if (!data || !size || !out_buffer || !out_initialization ||
      !vkr_bindless_vk_create_buffer(
          renderer, VKR_BINDLESS_VK_MEMORY_CLASS_DEVICE, size,
          usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, out_buffer))
    return false_v;
  uint8_t *upload_data =
      vkr_bindless_vk_publication_source_alloc(renderer, size);
  if (!upload_data) {
    log_error(
        "Bindless Vulkan failed to retain %llu geometry bytes "
        "(free=%llu, committed=%llu, reserve=%llu)",
        (unsigned long long)size,
        (unsigned long long)vkr_dmemory_get_free_space(
            &renderer->publication_staging_memory),
        (unsigned long long)renderer->publication_staging_memory.total_size,
        (unsigned long long)renderer->publication_staging_memory.reserve_size);
    vkr_bindless_vk_destroy_buffer(renderer, out_buffer);
    return false_v;
  }
  MemCopy(upload_data, data, size);
  *out_initialization = (VkrBindlessVkPendingBufferInitialization){
      .destination = out_buffer->handle,
      .upload_data = upload_data,
      .size = size,
      .destination_stage = destination_stage,
      .destination_access = destination_access,
      .geometry_record_index = geometry_record_index,
  };
  return true_v;
}

static bool8_t vkr_bindless_vk_asset_publish_geometry_internal(
    void *state, VkrGeometryHandle handle, const VkrGeometryConfig *geometry,
    const VkrMeshLoaderSubmeshRange *submeshes, uint32_t submesh_count) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !geometry || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity ||
      !geometry->vertices || !geometry->indices || !geometry->vertex_count ||
      !geometry->index_count || !submeshes || !submesh_count ||
      renderer->pending_buffer_initialization_count >
          renderer->pending_buffer_initialization_capacity - 2u ||
      renderer->staging_buffer_count >
          renderer->retired_staging_buffer_capacity - 2u ||
      (geometry->vertex_size != sizeof(VkrVertex3d) &&
       geometry->vertex_size != sizeof(VkrVertex2d)) ||
      (geometry->index_size != sizeof(uint16_t) &&
       geometry->index_size != sizeof(uint32_t)))
    return false_v;
  for (uint32_t i = 0; i < submesh_count; ++i) {
    if (!submeshes[i].index_count ||
        submeshes[i].first_index > geometry->index_count ||
        submeshes[i].index_count >
            geometry->index_count - submeshes[i].first_index)
      return false_v;
  }
  VkrBindlessVkPublishedGeometry *record =
      &renderer->published_geometries[handle.id - 1u];
  const VkIndexType index_type = geometry->index_size == sizeof(uint16_t)
                                     ? VK_INDEX_TYPE_UINT16
                                     : VK_INDEX_TYPE_UINT32;
  const uint64_t submeshes_size =
      (uint64_t)submesh_count * sizeof(VkrBindlessVkSubmeshRange);
  if (record->live) {
    if (record->handle.generation != handle.generation ||
        record->vertex_count != geometry->vertex_count ||
        record->index_count != geometry->index_count ||
        record->index_type != index_type) {
      log_error("Bindless Vulkan geometry %u:%u conflicts with %u:%u",
                handle.id, handle.generation, record->handle.id,
                record->handle.generation);
      return false_v;
    }
    if (record->submeshes_size != submeshes_size) {
      VkrBindlessVkSubmeshRange *replacement =
          vkr_allocator_alloc(renderer->allocator, submeshes_size,
                              VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      if (!replacement)
        return false_v;
      vkr_allocator_free(renderer->allocator, record->submeshes,
                         record->submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      record->submeshes = replacement;
      record->submeshes_size = submeshes_size;
    }
    for (uint32_t i = 0u; i < submesh_count; ++i) {
      record->submeshes[i] = (VkrBindlessVkSubmeshRange){
          .first_index = submeshes[i].first_index,
          .index_count = submeshes[i].index_count,
          .vertex_offset = submeshes[i].vertex_offset,
      };
    }
    record->submesh_count = submesh_count;
    return true_v;
  }
  if (record->pending_retire) {
    log_error("Bindless Vulkan geometry %u:%u conflicts with %u:%u "
              "(live=%u, pending_retire=%u)",
              handle.id, handle.generation, record->handle.id,
              record->handle.generation, record->live, record->pending_retire);
    return false_v;
  }
  const uint64_t vertex_size =
      (uint64_t)sizeof(VkrVertex3d) * geometry->vertex_count;
  const uint64_t index_size =
      (uint64_t)geometry->index_size * geometry->index_count;
  VkrBindlessVkPublishedGeometry pending = {
      .handle = handle,
      .vertex_count = geometry->vertex_count,
      .index_count = geometry->index_count,
      .index_type = index_type,
      .submeshes_size = submeshes_size,
      .submesh_count = submesh_count,
  };
  pending.submeshes =
      vkr_allocator_alloc(renderer->allocator, pending.submeshes_size,
                          VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!pending.submeshes)
    return false_v;
  for (uint32_t i = 0; i < submesh_count; ++i) {
    pending.submeshes[i] = (VkrBindlessVkSubmeshRange){
        .first_index = submeshes[i].first_index,
        .index_count = submeshes[i].index_count,
        .vertex_offset = submeshes[i].vertex_offset,
    };
  }
  VkrBindlessVkPendingBufferInitialization initializations[2] = {0};
  const VkrVertex3d *vertices = geometry->vertices;
  VkrVertex3d *converted = NULL;
  if (geometry->vertex_size == sizeof(VkrVertex2d)) {
    converted = vkr_allocator_alloc(renderer->allocator, vertex_size,
                                    VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    if (!converted) {
      vkr_allocator_free(renderer->allocator, pending.submeshes,
                         pending.submeshes_size,
                         VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
      return false_v;
    }
    const VkrVertex2d *source = geometry->vertices;
    for (uint32_t i = 0; i < geometry->vertex_count; ++i) {
      converted[i] = (VkrVertex3d){
          .position = {source[i].position.x, source[i].position.y, 0.0f},
          .normal = {0.0f, 0.0f, 1.0f},
          .texcoord = source[i].texcoord,
          .colour = {1.0f, 1.0f, 1.0f, 1.0f},
          .tangent = {1.0f, 0.0f, 0.0f, 1.0f},
      };
    }
    vertices = converted;
  }
  const bool8_t created =
      vkr_bindless_vk_create_published_buffer(
          renderer, vertices, vertex_size,
          VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
          VK_ACCESS_2_SHADER_STORAGE_READ_BIT, handle.id - 1u,
          &pending.vertices, &initializations[0]) &&
      vkr_bindless_vk_create_published_buffer(
          renderer, geometry->indices, index_size,
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
          VK_ACCESS_2_INDEX_READ_BIT, handle.id - 1u, &pending.indices,
          &initializations[1]);
  if (converted)
    vkr_allocator_free(renderer->allocator, converted, vertex_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  if (!created) {
    vkr_bindless_vk_release_buffer_initialization(renderer,
                                                  &initializations[1]);
    vkr_bindless_vk_release_buffer_initialization(renderer,
                                                  &initializations[0]);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.indices);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.vertices);
    vkr_allocator_free(renderer->allocator, pending.submeshes,
                       pending.submeshes_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  if (renderer->pending_buffer_initialization_count >
          renderer->pending_buffer_initialization_capacity - 2u ||
      renderer->staging_buffer_count >
          renderer->retired_staging_buffer_capacity - 2u) {
    vkr_bindless_vk_release_buffer_initialization(renderer,
                                                  &initializations[1]);
    vkr_bindless_vk_release_buffer_initialization(renderer,
                                                  &initializations[0]);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.indices);
    vkr_bindless_vk_destroy_buffer(renderer, &pending.vertices);
    vkr_allocator_free(renderer->allocator, pending.submeshes,
                       pending.submeshes_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
    return false_v;
  }
  pending.pending_initialization_count = 2u;
  pending.live = true_v;
  *record = pending;
  renderer->pending_buffer_initializations
      [renderer->pending_buffer_initialization_count++] = initializations[0];
  renderer->pending_buffer_initializations
      [renderer->pending_buffer_initialization_count++] = initializations[1];
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_geometry(void *state, VkrGeometryHandle handle,
                                       const VkrGeometryConfig *geometry) {
  if (!geometry)
    return false_v;
  const VkrMeshLoaderSubmeshRange submesh = {
      .index_count = geometry->index_count,
  };
  return vkr_bindless_vk_asset_publish_geometry_internal(
      state, handle, geometry, &submesh, 1u);
}

static bool8_t vkr_bindless_vk_publish_writable_storage_views(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkPublishedTexture *texture) {
  if (texture->image.mip_levels > VKR_BINDLESS_VK_TEXTURE_MIP_MAX)
    return false_v;
  VkDevice device = vkr_bindless_vk_renderer_device(renderer);
  for (uint32_t mip = 0u; mip < texture->image.mip_levels; ++mip) {
    const VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = texture->image.handle,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = texture->image.format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = mip,
                .levelCount = 1u,
                .baseArrayLayer = 0u,
                .layerCount = texture->image.array_layers,
            },
    };
    if (vkCreateImageView(device, &view_info, NULL,
                          &texture->storage_views[mip]) != VK_SUCCESS)
      return false_v;
    texture->storage_slot_count++;
    if (!vkr_bindless_vk_publish_storage_view(renderer,
                                              texture->storage_views[mip],
                                              &texture->storage_slots[mip]))
      return false_v;
  }
  return true_v;
}

static void vkr_bindless_vk_discard_writable_storage_views(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVkPublishedTexture *texture) {
  for (uint32_t i = 0u; i < texture->storage_slot_count; ++i) {
    if (texture->storage_slots[i].generation) {
      (void)vkr_gpu_slot_table_retire(renderer->storage_image_slots,
                                      texture->storage_slots[i],
                                      renderer->completed_value);
    }
  }
  (void)vkr_gpu_slot_table_collect(renderer->storage_image_slots,
                                   renderer->completed_value, NULL);
  vkr_bindless_vk_destroy_texture_storage_views(renderer, texture);
}

static bool8_t vkr_bindless_vk_asset_publish_writable_texture(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !description || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity ||
      renderer->pending_texture_initialization_count >=
          renderer->config.sampled_image_capacity ||
      description->id != handle.id ||
      description->generation != handle.generation || !description->width ||
      !description->height ||
      (description->sample_count != 0u &&
       description->sample_count != VKR_SAMPLE_COUNT_1) ||
      (description->type != VKR_TEXTURE_TYPE_2D &&
       description->type != VKR_TEXTURE_TYPE_CUBE_MAP) ||
      (description->type == VKR_TEXTURE_TYPE_CUBE_MAP &&
       description->width != description->height)) {
    log_error("Bindless Vulkan rejected writable texture metadata "
              "(handle=%u:%u, size=%ux%u, type=%u, format=%u, pending=%u)",
              handle.id, handle.generation,
              description ? description->width : 0u,
              description ? description->height : 0u,
              description ? description->type : 0u,
              description ? description->format : 0u,
              renderer ? renderer->pending_texture_initialization_count : 0u);
    return false_v;
  }
  VkrBindlessVkPublishedTexture *record =
      &renderer->published_textures[handle.id - 1u];
  if (record->live || record->pending_retire) {
    log_error("Bindless Vulkan writable texture %u:%u collides with a %s "
              "publication %u:%u",
              handle.id, handle.generation, record->live ? "live" : "retiring",
              record->handle.id, record->handle.generation);
    return false_v;
  }
  const VkFormat format = vkr_bindless_vk_texture_format(description->format);
  if (format == VK_FORMAT_UNDEFINED) {
    log_error("Bindless Vulkan writable texture %u:%u has unmapped format %u",
              handle.id, handle.generation, description->format);
    return false_v;
  }
  VkFormatProperties properties = {0};
  vkGetPhysicalDeviceFormatProperties(
      vkr_bindless_vulkan_device_physical(renderer->device), format,
      &properties);
  const VkFormatFeatureFlags required =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  if ((properties.optimalTilingFeatures & required) != required) {
    log_error("Bindless Vulkan writable texture %u:%u format %u lacks "
              "sampled/storage features",
              handle.id, handle.generation, format);
    return false_v;
  }
  const bool8_t cube = description->type == VKR_TEXTURE_TYPE_CUBE_MAP;
  const uint32_t mip_levels =
      description->mip_filter == VKR_MIP_FILTER_NONE
          ? 1u
          : vkr_bindless_vk_mip_count(description->width, description->height);
  VkrBindlessVkPublishedTexture pending = {.handle = handle};
  VkrBindlessVkPendingTextureInitialization initialization = {0};
  const bool8_t image_created = vkr_bindless_vk_create_image_ex(
      renderer, description->width, description->height, mip_levels,
      cube ? 6u : 1u, format, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
      cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      &pending.image);
  const bool8_t sampler_acquired =
      image_created &&
      vkr_bindless_vk_acquire_sampler(renderer, description, mip_levels,
                                      &pending.sampler_record_index);
  const bool8_t sampled_published =
      sampler_acquired && vkr_bindless_vk_publish_sampled_view(
                              renderer, pending.image.view,
                              VK_IMAGE_LAYOUT_GENERAL, &pending.sampled_slot);
  const bool8_t storage_published =
      sampled_published &&
      vkr_bindless_vk_publish_writable_storage_views(renderer, &pending);
  if (storage_published)
    vkr_bindless_vk_prepare_writable_initialization(handle, &pending.image,
                                                    &initialization);
  const bool8_t initialization_queued =
      storage_published &&
      vkr_bindless_vk_enqueue_texture_initialization(renderer, &initialization);
  if (!initialization_queued) {
    log_error("Bindless Vulkan writable texture %u:%u failed at %s "
              "(%ux%u, mips=%u, layers=%u, format=%u)",
              handle.id, handle.generation,
              !image_created       ? "image creation"
              : !sampler_acquired  ? "sampler publication"
              : !sampled_published ? "sampled descriptor publication"
              : !storage_published ? "storage descriptor publication"
                                   : "initialization queue",
              description->width, description->height, mip_levels,
              cube ? 6u : 1u, format);
    if (pending.sampled_slot.generation) {
      (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                      pending.sampled_slot,
                                      renderer->completed_value);
      (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots,
                                       renderer->completed_value, NULL);
    }
    vkr_bindless_vk_discard_writable_storage_views(renderer, &pending);
    if (sampler_acquired) {
      (void)vkr_bindless_vk_release_sampler(
          renderer, pending.sampler_record_index, renderer->completed_value);
      vkr_bindless_vk_collect_samplers(renderer, renderer->completed_value);
    }
    vkr_bindless_vk_destroy_image(renderer, &pending.image);
    return false_v;
  }
  pending.initialization_pending = true_v;
  pending.live = true_v;
  *record = pending;
  return true_v;
}

static void
vkr_bindless_vk_material_row_set_sampler(VkrBindlessVkMaterialGpuRow *row,
                                         uint32_t texture_slot,
                                         uint32_t sampler_index) {
  switch (texture_slot) {
  case 0u:
    row->base_color_sampler = sampler_index;
    break;
  case 1u:
    row->normal_sampler = sampler_index;
    break;
  case 2u:
    row->orm_sampler = sampler_index;
    break;
  default:
    row->emissive_sampler = sampler_index;
    break;
  }
}

static bool8_t vkr_bindless_vk_asset_update_texture_sampler(
    void *state, VkrTextureHandle handle,
    const VkrTextureDescription *description) {
  VkrBindlessVulkanRenderer *renderer = state;
  uint32_t texture_record_index = 0u;
  VkrBindlessVkPublishedTexture *texture = vkr_bindless_vk_published_texture(
      renderer, handle, &texture_record_index);
  if (!texture || !description || description->id != handle.id ||
      description->generation != handle.generation)
    return false_v;
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  VkrBindlessVkPublishedSampler *old_sampler =
      &renderer->published_samplers[texture->sampler_record_index];
  if (vkr_bindless_vk_sampler_description_equal(
          &old_sampler->description, old_sampler->mip_levels, description,
          texture->image.mip_levels))
    return true_v;

  uint32_t dependent_material_count = 0u;
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    const VkrBindlessVkPublishedMaterial *material =
        &renderer->published_materials[i];
    if (!material->live)
      continue;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (material->texture_record_indices[texture_slot] ==
          texture_record_index) {
        dependent_material_count++;
        break;
      }
    }
  }
  VkrGpuSlotTableMetrics material_metrics = {0};
  vkr_gpu_slot_table_get_metrics(renderer->material_slots, &material_metrics);
  if (material_metrics.slots_live + material_metrics.slots_retired +
              dependent_material_count >
          material_metrics.slots_capacity ||
      material_metrics.slots_retired + dependent_material_count >
          material_metrics.slots_capacity ||
      (old_sampler->reference_count == 1u &&
       vkr_gpu_slot_table_can_retire(renderer->sampler_slots,
                                     old_sampler->slot) !=
           VKR_GPU_SLOT_STATUS_OK))
    return false_v;

  uint32_t replacement_sampler_index = UINT32_MAX;
  if (!vkr_bindless_vk_acquire_sampler(renderer, description,
                                       texture->image.mip_levels,
                                       &replacement_sampler_index))
    return false_v;
  const uint32_t replacement_slot =
      renderer->published_samplers[replacement_sampler_index].slot.index;
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrBindlessVkPublishedMaterial *material =
        &renderer->published_materials[i];
    if (!material->live)
      continue;
    VkrBindlessVkMaterialGpuRow replacement_row = material->row;
    bool8_t dependent = false_v;
    for (uint32_t texture_slot = 0; texture_slot < 4u; ++texture_slot) {
      if (material->texture_record_indices[texture_slot] !=
          texture_record_index)
        continue;
      vkr_bindless_vk_material_row_set_sampler(&replacement_row, texture_slot,
                                               replacement_slot);
      dependent = true_v;
    }
    if (!dependent)
      continue;
    VkrGpuSlotHandle replacement_material_slot = {0};
    if (!vkr_bindless_vk_replace_material_gpu_row(
            renderer, material->slot, &replacement_row, renderer->submit_value,
            &replacement_material_slot)) {
      log_error("Bindless Vulkan failed a preflighted dependent material "
                "sampler republication");
      renderer->terminal_failure = true_v;
      return false_v;
    }
    material->slot = replacement_material_slot;
    material->row = replacement_row;
  }
  const uint32_t old_sampler_index = texture->sampler_record_index;
  texture->sampler_record_index = replacement_sampler_index;
  if (!vkr_bindless_vk_release_sampler(renderer, old_sampler_index,
                                       renderer->submit_value)) {
    log_error("Bindless Vulkan failed a preflighted sampler retirement");
    renderer->terminal_failure = true_v;
    return false_v;
  }
  vkr_bindless_vk_collect_samplers(renderer, completed);
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_loaded_mesh(void *state, VkrGeometryHandle handle,
                                          const VkrMeshLoaderResult *mesh) {
  if (!mesh || !mesh->has_mesh_buffer || !mesh->submeshes.data ||
      !mesh->submeshes.length ||
      mesh->mesh_buffer.vertex_size != sizeof(VkrVertex3d) ||
      (mesh->mesh_buffer.index_size != sizeof(uint16_t) &&
       mesh->mesh_buffer.index_size != sizeof(uint32_t)))
    return false_v;
  for (uint64_t i = 0; i < mesh->submeshes.length; ++i) {
    const VkrMeshLoaderSubmeshRange *range = &mesh->submeshes.data[i];
    if (!range->index_count ||
        range->first_index > mesh->mesh_buffer.index_count ||
        range->index_count > mesh->mesh_buffer.index_count - range->first_index)
      return false_v;
  }
  const VkrGeometryConfig geometry = {
      .vertex_size = mesh->mesh_buffer.vertex_size,
      .vertex_count = mesh->mesh_buffer.vertex_count,
      .vertices = mesh->mesh_buffer.vertices,
      .index_size = mesh->mesh_buffer.index_size,
      .index_count = mesh->mesh_buffer.index_count,
      .indices = mesh->mesh_buffer.indices,
  };
  return vkr_bindless_vk_asset_publish_geometry_internal(
      state, handle, &geometry, mesh->submeshes.data,
      (uint32_t)mesh->submeshes.length);
}

static bool8_t
vkr_bindless_vk_activate_geometry(VkrBindlessVulkanRenderer *renderer,
                                  VkrGeometryHandle handle) {
  if (!renderer || renderer->frame_active ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  VkDeviceAddress vertices =
      renderer->upload.address + VKR_BINDLESS_VK_VERTEX_OFFSET;
  renderer->active_geometry = false_v;
  renderer->active_geometry_record_index = 0u;
  if (handle.id) {
    if (handle.id > renderer->config.sampled_image_capacity)
      return false_v;
    const uint32_t index = handle.id - 1u;
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[index];
    if (!geometry->live || geometry->handle.generation != handle.generation)
      return false_v;
    vertices = geometry->vertices.address;
    renderer->active_geometry = true_v;
    renderer->active_geometry_record_index = index;
  }
  VkrBindlessVkDrawRoot *root =
      (VkrBindlessVkDrawRoot *)((uint8_t *)renderer->upload.allocation.mapped +
                                VKR_BINDLESS_VK_ROOT_OFFSET);
  root->vertices = vertices;
  return vkr_bindless_vk_flush(renderer, &renderer->upload.allocation,
                               VKR_BINDLESS_VK_ROOT_OFFSET, sizeof(*root));
}

static VkrBindlessVkPublishedGeometry *
vkr_bindless_vk_reserve_retired_geometry(VkrBindlessVulkanRenderer *renderer) {
  for (uint32_t i = 0u; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedGeometry *retired = &renderer->retired_geometries[i];
    if (!retired->pending_retire)
      return retired;
  }
  return NULL;
}

static bool8_t
vkr_bindless_vk_asset_unpublish_geometry(void *state,
                                         VkrGeometryHandle handle) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity)
    return false_v;
  VkrBindlessVkPublishedGeometry *record =
      &renderer->published_geometries[handle.id - 1u];
  if (!record->live || record->handle.generation != handle.generation)
    return false_v;
  if (record->pending_initialization_count)
    vkr_bindless_vk_discard_geometry_initializations(renderer, handle.id - 1u);
  if (!vkr_bindless_vk_retire_buffer(renderer, &record->vertices,
                                     renderer->submit_value) ||
      !vkr_bindless_vk_retire_buffer(renderer, &record->indices,
                                     renderer->submit_value))
    log_fatal("Bindless Vulkan failed to retire geometry memory");
  record->live = false_v;
  record->pending_retire = true_v;
  record->last_use_submit_value = renderer->submit_value;
  vkr_bindless_vk_collect_asset_publications(
      renderer, vkr_bindless_vk_refresh_completed(renderer));
  if (!record->pending_retire)
    return true_v;
  VkrBindlessVkPublishedGeometry *retired =
      vkr_bindless_vk_reserve_retired_geometry(renderer);
  if (!retired) {
    log_error("Bindless Vulkan retired-geometry capacity exhausted");
    return false_v;
  }
  *retired = *record;
  MemZero(record, sizeof(*record));
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_texture(void *state, VkrTextureHandle handle,
                                      const VkrTexturePreparedLoad *prepared) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !prepared || handle.id == 0u ||
      handle.id > renderer->config.sampled_image_capacity ||
      renderer->pending_texture_initialization_count >=
          renderer->config.sampled_image_capacity ||
      renderer->staging_buffer_count >=
          renderer->retired_staging_buffer_capacity)
    return false_v;
  VkrBindlessVkPublishedTexture *record =
      &renderer->published_textures[handle.id - 1u];
  if (record->live || record->pending_retire) {
    log_error("Bindless Vulkan texture %u:%u is already published", handle.id,
              handle.generation);
    return false_v;
  }
  VkrBindlessVkPublishedTexture pending = {.handle = handle};
  VkrBindlessVkPendingTextureInitialization initialization = {0};
  const bool8_t image_uploaded = vkr_bindless_vk_upload_prepared_texture(
      renderer, prepared, handle, &pending.image, &initialization);
  const bool8_t sampler_acquired =
      image_uploaded &&
      vkr_bindless_vk_acquire_sampler(renderer, &prepared->description,
                                      prepared->upload_mip_levels,
                                      &pending.sampler_record_index);
  const bool8_t sampled_published =
      sampler_acquired &&
      vkr_bindless_vk_publish_sampled_view(
          renderer, pending.image.view,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &pending.sampled_slot);
  const bool8_t initialization_queued =
      sampled_published &&
      vkr_bindless_vk_enqueue_texture_initialization(renderer, &initialization);
  if (!initialization_queued) {
    log_error("Bindless Vulkan texture %u:%u publication failed at %s",
              handle.id, handle.generation,
              !image_uploaded      ? "image upload"
              : !sampler_acquired  ? "sampler publication"
              : !sampled_published ? "sampled descriptor"
                                   : "initialization queue");
    if (pending.sampled_slot.generation) {
      (void)vkr_gpu_slot_table_retire(renderer->sampled_image_slots,
                                      pending.sampled_slot,
                                      renderer->completed_value);
      (void)vkr_gpu_slot_table_collect(renderer->sampled_image_slots,
                                       renderer->completed_value, NULL);
    }
    if (sampler_acquired) {
      (void)vkr_bindless_vk_release_sampler(
          renderer, pending.sampler_record_index, renderer->completed_value);
      vkr_bindless_vk_collect_samplers(renderer, renderer->completed_value);
    }
    if (image_uploaded)
      vkr_bindless_vk_release_texture_initialization(renderer, &initialization);
    vkr_bindless_vk_destroy_image(renderer, &pending.image);
    return false_v;
  }
  pending.initialization_pending = true_v;
  pending.live = true_v;
  *record = pending;
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_unpublish_texture(void *state, VkrTextureHandle handle) {
  VkrBindlessVulkanRenderer *renderer = state;
  VkrBindlessVkPublishedTexture *texture =
      vkr_bindless_vk_published_texture(renderer, handle, NULL);
  if (!texture)
    return false_v;
  if (texture->ibl_reference_count) {
    if (!texture->material_reference_count) {
      VkrBindlessVkPublishedTexture *retired =
          vkr_bindless_vk_reserve_retired_texture(renderer);
      if (!retired) {
        log_error("Bindless Vulkan retired-texture capacity exhausted");
        return false_v;
      }
      texture->live = false_v;
      texture->pending_retire = true_v;
      texture->last_use_submit_value =
          Max(texture->last_use_submit_value, renderer->submit_value);
      *retired = *texture;
      MemZero(texture, sizeof(*texture));
      vkr_bindless_vk_collect_asset_publications(
          renderer, vkr_bindless_vk_refresh_completed(renderer));
      return true_v;
    }
    texture->unpublish_requested = true_v;
    return true_v;
  }
  if (texture->initialization_pending)
    vkr_bindless_vk_cancel_texture_initialization(renderer, handle);
  texture->live = false_v;
  texture->pending_retire = true_v;
  texture->last_use_submit_value = renderer->submit_value;
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  if (!texture->pending_retire)
    return true_v;
  if (texture->material_reference_count)
    return true_v;
  VkrBindlessVkPublishedTexture *retired =
      vkr_bindless_vk_reserve_retired_texture(renderer);
  if (!retired) {
    log_error("Bindless Vulkan retired-texture capacity exhausted");
    return false_v;
  }
  *retired = *texture;
  MemZero(texture, sizeof(*texture));
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_publish_material(void *state, VkrMaterialHandle handle,
                                       const VkrMaterial *material) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || !material || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity ||
      material->id != handle.id || material->generation != handle.generation)
    return false_v;
  VkrBindlessVkPublishedMaterial *record =
      &renderer->published_materials[handle.id - 1u];
  if (record->live && record->handle.generation != handle.generation)
    return false_v;
  VkrBindlessVkRetiredMaterial *retirement =
      record->live ? vkr_bindless_vk_reserve_material_retirement(renderer)
                   : NULL;
  if (record->live && !retirement)
    return false_v;

  static const VkrTextureSlot row_slots[4] = {
      VKR_TEXTURE_SLOT_DIFFUSE,
      VKR_TEXTURE_SLOT_NORMAL,
      VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS,
      VKR_TEXTURE_SLOT_EMISSION,
  };
  uint32_t texture_indices[4] = {0};
  uint32_t sampler_indices[4] = {0};
  uint32_t texture_record_indices[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                        UINT32_MAX};
  uint32_t material_flags = 0u;
  for (uint32_t i = 0; i < ArrayCount(row_slots); ++i) {
    const VkrMaterialTexture *source = &material->textures[row_slots[i]];
    if (!source->enabled)
      continue;
    uint32_t record_index = 0u;
    VkrBindlessVkPublishedTexture *texture = vkr_bindless_vk_published_texture(
        renderer, source->handle, &record_index);
    if (!texture)
      return false_v;
    texture_indices[i] = texture->sampled_slot.index;
    sampler_indices[i] =
        renderer->published_samplers[texture->sampler_record_index].slot.index;
    texture_record_indices[i] = record_index;
  }
  if (material->textures[VKR_TEXTURE_SLOT_NORMAL].enabled)
    material_flags |= VKR_BINDLESS_VK_MATERIAL_TEXTURE_NORMAL;
  if (material->textures[VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS].enabled)
    material_flags |= VKR_BINDLESS_VK_MATERIAL_TEXTURE_ORM;
  if (material->textures[VKR_TEXTURE_SLOT_EMISSION].enabled)
    material_flags |= VKR_BINDLESS_VK_MATERIAL_TEXTURE_EMISSIVE;
  const Vec4 tint = material->material_type == VKR_MATERIAL_TYPE_PBR
                        ? material->pbr.base_color
                        : material->phong.diffuse_color;
  const VkrBindlessVkMaterialGpuRow row = {
      .tint = {tint.x, tint.y, tint.z, tint.w},
      .base_color_texture = texture_indices[0],
      .normal_texture = texture_indices[1],
      .orm_texture = texture_indices[2],
      .emissive_texture = texture_indices[3],
      .base_color_sampler = sampler_indices[0],
      .normal_sampler = sampler_indices[1],
      .orm_sampler = sampler_indices[2],
      .emissive_sampler = sampler_indices[3],
      .material_id = handle.id,
      .flags = material_flags,
  };
  VkrGpuSlotHandle new_slot = {0};
  const bool8_t row_published =
      record->live
          ? vkr_bindless_vk_replace_material_gpu_row(
                renderer, record->slot, &row, renderer->submit_value, &new_slot)
          : vkr_bindless_vk_publish_material_gpu_row(renderer, &row, &new_slot);
  if (!row_published)
    return false_v;
  for (uint32_t i = 0; i < 4u; ++i) {
    if (texture_record_indices[i] != UINT32_MAX)
      renderer->published_textures[texture_record_indices[i]]
          .material_reference_count++;
  }
  if (record->live) {
    *retirement = (VkrBindlessVkRetiredMaterial){
        .retire_value = renderer->submit_value,
        .occupied = true_v,
    };
    MemCopy(retirement->texture_record_indices, record->texture_record_indices,
            sizeof(retirement->texture_record_indices));
  }
  *record = (VkrBindlessVkPublishedMaterial){
      .handle = handle,
      .slot = new_slot,
      .row = row,
      .pbr = material->pbr,
      .alpha_cutoff = material->alpha_cutoff,
      .alpha_mode = material->alpha_mode,
      .double_sided = material->double_sided,
      .live = true_v,
  };
  MemCopy(record->texture_record_indices, texture_record_indices,
          sizeof(record->texture_record_indices));
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_unpublish_material(void *state,
                                         VkrMaterialHandle handle) {
  VkrBindlessVulkanRenderer *renderer = state;
  if (!renderer || handle.id == 0u ||
      handle.id > renderer->config.material_record_capacity)
    return false_v;
  VkrBindlessVkPublishedMaterial *record =
      &renderer->published_materials[handle.id - 1u];
  if (!record->live || record->handle.generation != handle.generation)
    return false_v;
  VkrBindlessVkRetiredMaterial *retirement =
      vkr_bindless_vk_reserve_material_retirement(renderer);
  if (!retirement || vkr_gpu_slot_table_retire(
                         renderer->material_slots, record->slot,
                         renderer->submit_value) != VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  *retirement = (VkrBindlessVkRetiredMaterial){
      .retire_value = renderer->submit_value,
      .occupied = true_v,
  };
  MemCopy(retirement->texture_record_indices, record->texture_record_indices,
          sizeof(retirement->texture_record_indices));
  record->live = false_v;
  return true_v;
}

static bool8_t vkr_bindless_vk_queue_ibl_bake(
    VkrBindlessVulkanRenderer *renderer, VkrTextureHandle equirect,
    VkrTextureHandle source, VkrTextureHandle irradiance,
    VkrTextureHandle prefilter, bool8_t convert_equirect) {
  if (!renderer ||
      renderer->pending_ibl_bake_count >= VKR_BINDLESS_VK_PENDING_IBL_BAKE_MAX)
    return false_v;
  VkrBindlessVkPublishedTexture *source_texture =
      vkr_bindless_vk_published_texture(renderer, source, NULL);
  VkrBindlessVkPublishedTexture *irradiance_texture =
      vkr_bindless_vk_published_texture(renderer, irradiance, NULL);
  VkrBindlessVkPublishedTexture *prefilter_texture =
      vkr_bindless_vk_published_texture(renderer, prefilter, NULL);
  VkrBindlessVkPublishedTexture *equirect_texture =
      convert_equirect
          ? vkr_bindless_vk_published_texture(renderer, equirect, NULL)
          : NULL;
  if (!source_texture || !irradiance_texture || !prefilter_texture ||
      (convert_equirect && !equirect_texture) ||
      source_texture->image.array_layers != 6u ||
      irradiance_texture->image.array_layers != 6u ||
      prefilter_texture->image.array_layers != 6u ||
      source_texture->image.width != source_texture->image.height ||
      irradiance_texture->image.width != irradiance_texture->image.height ||
      prefilter_texture->image.width != prefilter_texture->image.height ||
      irradiance_texture->image.mip_levels != 1u ||
      prefilter_texture->storage_slot_count !=
          prefilter_texture->image.mip_levels ||
      irradiance_texture->storage_slot_count != 1u ||
      irradiance_texture->image.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
      prefilter_texture->image.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
      (convert_equirect &&
       (source_texture->image.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
        equirect_texture->image.array_layers != 1u ||
        equirect_texture->image.width != equirect_texture->image.height * 2u ||
        source_texture->storage_slot_count !=
            source_texture->image.mip_levels)))
    return false_v;
  VkrBindlessVkPublishedTexture *referenced[] = {
      equirect_texture, source_texture, irradiance_texture, prefilter_texture};
  for (uint32_t i = convert_equirect ? 0u : 1u; i < ArrayCount(referenced); ++i)
    referenced[i]->ibl_reference_count++;
  source_texture->ibl_irradiance = irradiance;
  source_texture->ibl_prefilter = prefilter;
  renderer->pending_ibl_bakes[renderer->pending_ibl_bake_count++] =
      (VkrBindlessVkPendingIblBake){
          .equirect = equirect,
          .source = source,
          .irradiance = irradiance,
          .prefilter = prefilter,
          .convert_equirect = convert_equirect,
      };
  return true_v;
}

static bool8_t
vkr_bindless_vk_asset_bake_ibl_cubemap(void *state, VkrTextureHandle source,
                                       VkrTextureHandle irradiance,
                                       VkrTextureHandle prefilter) {
  return vkr_bindless_vk_queue_ibl_bake(state, VKR_TEXTURE_HANDLE_INVALID,
                                        source, irradiance, prefilter, false_v);
}

static bool8_t vkr_bindless_vk_asset_bake_hdr_environment(
    void *state, VkrTextureHandle equirect, VkrTextureHandle source,
    VkrTextureHandle irradiance, VkrTextureHandle prefilter) {
  return vkr_bindless_vk_queue_ibl_bake(state, equirect, source, irradiance,
                                        prefilter, true_v);
}

static bool8_t vkr_bindless_vk_asset_publications_idle(void *state) {
  const VkrBindlessVulkanRenderer *renderer = state;
  return renderer && !renderer->pending_texture_initialization_count &&
         !renderer->pending_buffer_initialization_count &&
         !renderer->pending_ibl_bake_count;
}

void vkr_bindless_vulkan_renderer_get_asset_publisher(
    VkrBindlessVulkanRenderer *renderer, VkrAssetPublisher *out_publisher) {
  if (!out_publisher)
    return;
  *out_publisher = renderer
                       ? (VkrAssetPublisher){
                             .state = renderer,
                             .publications_idle =
                                 vkr_bindless_vk_asset_publications_idle,
                             .publish_geometry =
                                  vkr_bindless_vk_asset_publish_geometry,
                              .publish_loaded_mesh =
                                  vkr_bindless_vk_asset_publish_loaded_mesh,
                              .unpublish_geometry =
                                  vkr_bindless_vk_asset_unpublish_geometry,
                              .publish_texture =
                                  vkr_bindless_vk_asset_publish_texture,
                              .publish_writable_texture =
                                  vkr_bindless_vk_asset_publish_writable_texture,
                              .update_texture_sampler =
                                  vkr_bindless_vk_asset_update_texture_sampler,
                              .bake_ibl_cubemap =
                                  vkr_bindless_vk_asset_bake_ibl_cubemap,
                              .bake_hdr_environment =
                                  vkr_bindless_vk_asset_bake_hdr_environment,
                             .unpublish_texture =
                                 vkr_bindless_vk_asset_unpublish_texture,
                             .publish_material =
                                 vkr_bindless_vk_asset_publish_material,
                             .unpublish_material =
                                 vkr_bindless_vk_asset_unpublish_material,
                         }
                       : (VkrAssetPublisher){0};
}

static void
vkr_bindless_vk_drain_asset_publications(VkrBindlessVulkanRenderer *renderer) {
  const uint64_t completed = vkr_bindless_vk_refresh_completed(renderer);
  for (uint32_t i = 0; i < renderer->config.material_record_capacity; ++i) {
    VkrBindlessVkPublishedMaterial *material =
        &renderer->published_materials[i];
    if (material->live)
      (void)vkr_bindless_vk_asset_unpublish_material(renderer,
                                                     material->handle);
  }
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedTexture *texture = &renderer->published_textures[i];
    if (texture->live)
      (void)vkr_bindless_vk_asset_unpublish_texture(renderer, texture->handle);
  }
  for (uint32_t i = 0; i < renderer->config.sampled_image_capacity; ++i) {
    VkrBindlessVkPublishedGeometry *geometry =
        &renderer->published_geometries[i];
    if (geometry->live)
      (void)vkr_bindless_vk_asset_unpublish_geometry(renderer,
                                                     geometry->handle);
  }
  vkr_bindless_vk_collect_asset_publications(renderer, completed);
}

static bool8_t vkr_bindless_vk_result_is_empty_graph_clear(
    const VkrBindlessVulkanResult *result) {
  return result->readback_ready && result->color[0] == 0u &&
         result->color[1] == 0u && result->color[2] == 0u &&
         result->color[3] == 255u && result->identifier == 0xff000000u;
}

static bool8_t vkr_bindless_vk_run_asset_publisher_fixture(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanPublicationTestResult *out_result) {
  const uint8_t pixel[4] = {37u, 91u, 173u, 255u};
  VkrTextureUploadRegion region = {
      .width = 1u,
      .height = 1u,
      .depth = 1u,
      .byte_size = sizeof(pixel),
  };
  VkrTexturePreparedLoad prepared = {
      .description = {.width = 1u,
                      .height = 1u,
                      .channels = 4u,
                      .type = VKR_TEXTURE_TYPE_2D,
                      .format = VKR_TEXTURE_FORMAT_R8G8B8A8_UNORM,
                      .sample_count = VKR_SAMPLE_COUNT_1,
                      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .min_filter = VKR_FILTER_NEAREST,
                      .mag_filter = VKR_FILTER_NEAREST,
                      .mip_filter = VKR_MIP_FILTER_NONE},
      .upload_data = (uint8_t *)pixel,
      .upload_data_size = sizeof(pixel),
      .upload_regions = &region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
  };
  const VkrTextureHandle shared_texture = {
      .id = renderer->config.sampled_image_capacity - 1u, .generation = 1u};
  const VkrTextureHandle replacement_texture = {
      .id = renderer->config.sampled_image_capacity, .generation = 1u};
  const VkrTextureHandle writable_texture = {
      .id = renderer->config.sampled_image_capacity - 2u, .generation = 1u};
  const VkrGeometryHandle loaded_geometry = {
      .id = renderer->config.sampled_image_capacity, .generation = 1u};
  const VkrMaterialHandle material_a = {
      .id = renderer->config.material_record_capacity - 1u, .generation = 1u};
  const VkrMaterialHandle material_b = {
      .id = renderer->config.material_record_capacity, .generation = 1u};
  VkrMaterial material = {
      .material_type = VKR_MATERIAL_TYPE_PBR,
      .pbr.base_color = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
  };
  const VkrTextureSlot material_texture_slots[] = {
      VKR_TEXTURE_SLOT_DIFFUSE,
      VKR_TEXTURE_SLOT_NORMAL,
      VKR_TEXTURE_SLOT_METALLIC_ROUGHNESS,
      VKR_TEXTURE_SLOT_EMISSION,
  };
  for (uint32_t i = 0; i < ArrayCount(material_texture_slots); ++i) {
    const VkrTextureSlot slot = material_texture_slots[i];
    material.textures[slot] = (VkrMaterialTexture){
        .handle = shared_texture,
        .slot = slot,
        .enabled = true_v,
    };
  }
  const VkrVertex3d vertices[] = {
      {.position = {-1.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {3.0f, -1.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 2.0f, .y = 0.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
      {.position = {-1.0f, 3.0f, 0.0f},
       .normal = {0.0f, 0.0f, 1.0f},
       .texcoord = {.x = 0.0f, .y = 2.0f},
       .colour = {.x = 1.0f, .y = 1.0f, .z = 1.0f, .w = 1.0f},
       .tangent = {.x = 1.0f, .w = 1.0f}},
  };
  const uint32_t indices[] = {0u, 1u, 2u};
  VkrMeshLoaderSubmeshRange submesh = {.index_count = ArrayCount(indices)};
  const VkrMeshLoaderResult mesh = {
      .has_mesh_buffer = true_v,
      .mesh_buffer = {.vertex_size = sizeof(vertices[0]),
                      .vertex_count = ArrayCount(vertices),
                      .vertices = (void *)vertices,
                      .index_size = sizeof(indices[0]),
                      .index_count = ArrayCount(indices),
                      .indices = (void *)indices},
      .submeshes = {.length = 1u, .data = &submesh},
  };
  VkrTextureDescription writable_description = prepared.description;
  writable_description.id = writable_texture.id;
  writable_description.generation = writable_texture.generation;
  writable_description.width = 4u;
  writable_description.height = 4u;
  if (!vkr_bindless_vk_asset_publish_loaded_mesh(renderer, loaded_geometry,
                                                 &mesh) ||
      !vkr_bindless_vk_activate_geometry(renderer, loaded_geometry) ||
      !vkr_bindless_vk_asset_publish_writable_texture(
          renderer, writable_texture, &writable_description))
    return false_v;
  writable_description.u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT;
  if (!vkr_bindless_vk_asset_update_texture_sampler(renderer, writable_texture,
                                                    &writable_description))
    return false_v;
  VkrGpuSlotTableMetrics sampler_after_update = {0};
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &sampler_after_update);
  if (!vkr_bindless_vk_asset_update_texture_sampler(renderer, writable_texture,
                                                    &writable_description))
    return false_v;
  VkrGpuSlotTableMetrics sampler_after_duplicate = {0};
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &sampler_after_duplicate);
  if (sampler_after_duplicate.slots_published !=
          sampler_after_update.slots_published ||
      !vkr_bindless_vk_asset_publish_texture(renderer, shared_texture,
                                             &prepared))
    return false_v;

  VkrMaterial material_a_data = material;
  material_a_data.id = material_a.id;
  material_a_data.generation = material_a.generation;
  VkrMaterial invalid_material = material_a_data;
  invalid_material.textures[VKR_TEXTURE_SLOT_NORMAL].handle.generation++;
  if (vkr_bindless_vk_asset_publish_material(renderer, material_a,
                                             &invalid_material) ||
      !vkr_bindless_vk_asset_publish_material(renderer, material_a,
                                              &material_a_data))
    return false_v;
  VkrMaterial material_b_data = material;
  material_b_data.id = material_b.id;
  material_b_data.generation = material_b.generation;
  if (!vkr_bindless_vk_asset_publish_material(renderer, material_b,
                                              &material_b_data))
    return false_v;
  const uint32_t expected_material_flags =
      VKR_BINDLESS_VK_MATERIAL_TEXTURE_NORMAL |
      VKR_BINDLESS_VK_MATERIAL_TEXTURE_ORM |
      VKR_BINDLESS_VK_MATERIAL_TEXTURE_EMISSIVE;
  if (renderer->published_materials[material_a.id - 1u].row.flags !=
          expected_material_flags ||
      renderer->published_materials[material_b.id - 1u].row.flags !=
          expected_material_flags)
    return false_v;

  const VkrGpuSlotHandle material_a_before_sampler_update =
      renderer->published_materials[material_a.id - 1u].slot;
  const VkrGpuSlotHandle material_b_before_sampler_update =
      renderer->published_materials[material_b.id - 1u].slot;
  VkrTextureDescription shared_sampler_description = prepared.description;
  shared_sampler_description.id = shared_texture.id;
  shared_sampler_description.generation = shared_texture.generation;
  shared_sampler_description.u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_REPEAT;
  if (!vkr_bindless_vk_asset_update_texture_sampler(
          renderer, shared_texture, &shared_sampler_description))
    return false_v;
  const VkrBindlessVkPublishedTexture *shared_texture_record =
      &renderer->published_textures[shared_texture.id - 1u];
  const uint32_t shared_sampler_slot =
      renderer->published_samplers[shared_texture_record->sampler_record_index]
          .slot.index;
  out_result->shared_sampler_reused =
      shared_texture_record->sampler_record_index ==
      renderer->published_textures[writable_texture.id - 1u]
          .sampler_record_index;
  out_result->dependent_materials_republished =
      (renderer->published_materials[material_a.id - 1u].slot.index !=
           material_a_before_sampler_update.index ||
       renderer->published_materials[material_a.id - 1u].slot.generation !=
           material_a_before_sampler_update.generation) &&
      (renderer->published_materials[material_b.id - 1u].slot.index !=
           material_b_before_sampler_update.index ||
       renderer->published_materials[material_b.id - 1u].slot.generation !=
           material_b_before_sampler_update.generation) &&
      renderer->published_materials[material_a.id - 1u]
              .row.base_color_sampler == shared_sampler_slot &&
      renderer->published_materials[material_b.id - 1u]
              .row.base_color_sampler == shared_sampler_slot;
  if (!out_result->shared_sampler_reused ||
      !out_result->dependent_materials_republished)
    return false_v;

  VkrFrameSetup setup = {0};
  VkrRenderPacket packet = {.packet_version = VKR_RENDER_PACKET_VERSION};
  VkrBindlessVulkanResult submitted[3] = {0};
  renderer->active_material_index =
      renderer->published_materials[material_a.id - 1u].slot.index;
  uint64_t expected_staging_retirements =
      renderer->pending_buffer_initialization_count ? 1u : 0u;
  VkrBindlessVulkanMemoryMetrics memory_before_submit = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_before_submit);
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 2001u, 2048u, 1u,
                                                  &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[0]))
    return false_v;
  VkrBindlessVulkanMemoryMetrics memory_after_submit = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_after_submit);
  out_result->staging_retired_at_submit =
      expected_staging_retirements > 0u &&
      memory_after_submit.aggregate.retired_allocations ==
          memory_before_submit.aggregate.retired_allocations +
              expected_staging_retirements;
  if (!out_result->staging_retired_at_submit ||
      !vkr_bindless_vk_asset_unpublish_material(renderer, material_a) ||
      !vkr_bindless_vk_asset_unpublish_texture(renderer, shared_texture))
    return false_v;

  renderer->active_material_index =
      renderer->published_materials[material_b.id - 1u].slot.index;
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 2002u, 2048u, 1u,
                                                  &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[1]))
    return false_v;
  out_result->shared_resource_survived =
      renderer->published_textures[shared_texture.id - 1u]
              .material_reference_count > 0u &&
      renderer->published_textures[shared_texture.id - 1u].pending_retire;

  if (!vkr_bindless_vk_asset_publish_texture(renderer, replacement_texture,
                                             &prepared))
    return false_v;
  for (uint32_t i = 0; i < ArrayCount(material_texture_slots); ++i)
    material_b_data.textures[material_texture_slots[i]].handle =
        replacement_texture;
  if (!vkr_bindless_vk_asset_publish_material(renderer, material_b,
                                              &material_b_data))
    return false_v;
  renderer->active_material_index =
      renderer->published_materials[material_b.id - 1u].slot.index;
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 2003u, 2048u, 1u,
                                                  &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[2]) ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;

  uint64_t after_submit = submitted[0].submit_value - 1u;
  for (uint32_t i = 0; i < ArrayCount(submitted); ++i) {
    VkrBindlessVulkanResult completed = {0};
    if (!vkr_bindless_vulkan_renderer_poll_result(renderer, after_submit,
                                                  &completed) ||
        !vkr_bindless_vk_result_is_empty_graph_clear(&completed))
      return false_v;
    after_submit = completed.submit_value;
    out_result->exact_draw_count++;
  }
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  out_result->replacement_survived =
      !renderer->published_textures[shared_texture.id - 1u].pending_retire &&
      renderer->published_textures[replacement_texture.id - 1u].live &&
      renderer->published_materials[material_b.id - 1u].live;
  if (!out_result->shared_resource_survived ||
      !out_result->replacement_survived ||
      !vkr_bindless_vk_asset_unpublish_material(renderer, material_b))
    return false_v;
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  if (!vkr_bindless_vk_asset_unpublish_texture(renderer, replacement_texture))
    return false_v;
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  renderer->active_material_index = 0u;
  if (!vkr_bindless_vk_activate_geometry(renderer, (VkrGeometryHandle){0}) ||
      !vkr_bindless_vk_asset_unpublish_geometry(renderer, loaded_geometry) ||
      !vkr_bindless_vk_asset_unpublish_texture(renderer, writable_texture))
    return false_v;
  vkr_bindless_vk_collect_asset_publications(renderer,
                                             renderer->completed_value);
  uint64_t upload_wait_count = 0u;
  if (!vkr_bindless_vulkan_renderer_get_and_reset_upload_wait_count(
          renderer, &upload_wait_count))
    return false_v;
  out_result->upload_wait_free = upload_wait_count == 0u;
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_run_publication_test(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanPublicationTestResult *out_result) {
  if (!renderer || !out_result || renderer->frame_active ||
      renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  MemZero(out_result, sizeof(*out_result));
  if (renderer->pending_buffer_initialization_count ||
      renderer->pending_texture_initialization_count) {
    VkrFrameSetup setup = {0};
    const VkrRenderPacket packet = {
        .packet_version = VKR_RENDER_PACKET_VERSION,
    };
    VkrBindlessVulkanResult submitted = {0};
    if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 999u, 2048u, 1u,
                                                    &setup) ||
        !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                    &submitted) ||
        !vkr_bindless_vulkan_renderer_wait_idle(renderer))
      return false_v;
    VkrBindlessVulkanResult completed = {0};
    if (!vkr_bindless_vulkan_renderer_poll_result(
            renderer, submitted.submit_value - 1u, &completed) ||
        !vkr_bindless_vk_result_is_empty_graph_clear(&completed))
      return false_v;
  }
  VkrBindlessVulkanHeapMetrics baseline = {0};
  VkrBindlessVulkanMemoryMetrics memory_baseline = {0};
  vkr_bindless_vulkan_renderer_heap_metrics(renderer, &baseline);
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_baseline);
  if (!vkr_bindless_vk_run_asset_publisher_fixture(renderer, out_result))
    return false_v;
  VkrGpuSlotHandle shared_texture = {0}, shared_storage = {0};
  VkrGpuSlotHandle shared_sampler = {0};
  VkrGpuSlotHandle replacement_texture = {0}, replacement_storage = {0};
  VkrGpuSlotHandle replacement_sampler = {0};
  VkrGpuSlotHandle material_a = {0}, material_b = {0}, replacement = {0};
  if (!vkr_bindless_vk_publish_sampled_view(
          renderer, renderer->sentinel_image.view,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &shared_texture) ||
      !vkr_bindless_vk_publish_storage_view(
          renderer, renderer->sentinel_image.view, &shared_storage) ||
      !vkr_bindless_vk_publish_sampler(renderer, renderer->sentinel_sampler,
                                       &shared_sampler) ||
      !vkr_bindless_vk_publish_material_row(renderer, shared_texture.index,
                                            shared_sampler.index, 0x1001u,
                                            &material_a) ||
      !vkr_bindless_vk_publish_material_row(renderer, shared_texture.index,
                                            shared_sampler.index, 0x1002u,
                                            &material_b))
    return false_v;

  const uint64_t before_submit = renderer->submit_value;
  VkrFrameSetup setup = {0};
  VkrRenderPacket packet = {.packet_version = VKR_RENDER_PACKET_VERSION};
  VkrBindlessVulkanResult submitted[3] = {0};
  renderer->active_material_index = material_a.index;
  if (!vkr_bindless_vulkan_renderer_prepare_frame(renderer, 1001u, 2048u, 1u,
                                                  &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[0]))
    return false_v;
  if (vkr_gpu_slot_table_retire(renderer->material_slots, material_a,
                                submitted[0].submit_value) !=
      VKR_GPU_SLOT_STATUS_OK)
    return false_v;

  uint32_t survivor_index = UINT32_MAX;
  uint32_t shared_texture_index = UINT32_MAX;
  uint32_t shared_storage_index = UINT32_MAX;
  uint32_t shared_sampler_index = UINT32_MAX;
  out_result->shared_resource_survived =
      vkr_gpu_slot_table_resolve(renderer->material_slots, material_b,
                                 &survivor_index) == VKR_GPU_SLOT_STATUS_OK &&
      vkr_gpu_slot_table_resolve(renderer->sampled_image_slots, shared_texture,
                                 &shared_texture_index) ==
          VKR_GPU_SLOT_STATUS_OK &&
      vkr_gpu_slot_table_resolve(renderer->storage_image_slots, shared_storage,
                                 &shared_storage_index) ==
          VKR_GPU_SLOT_STATUS_OK &&
      vkr_gpu_slot_table_resolve(renderer->sampler_slots, shared_sampler,
                                 &shared_sampler_index) ==
          VKR_GPU_SLOT_STATUS_OK &&
      shared_texture_index == shared_texture.index &&
      shared_storage_index == shared_storage.index &&
      shared_sampler_index == shared_sampler.index;
  renderer->active_material_index = survivor_index;
  if (!out_result->shared_resource_survived ||
      !vkr_bindless_vulkan_renderer_prepare_frame(renderer, 1002u, 2048u, 1u,
                                                  &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[1]))
    return false_v;

  if (!vkr_bindless_vk_publish_sampled_view(
          renderer, renderer->sentinel_image.view,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &replacement_texture) ||
      !vkr_bindless_vk_publish_storage_view(
          renderer, renderer->sentinel_image.view, &replacement_storage) ||
      !vkr_bindless_vk_publish_sampler(renderer, renderer->sentinel_sampler,
                                       &replacement_sampler) ||
      !vkr_bindless_vk_publish_material_row(renderer, replacement_texture.index,
                                            replacement_sampler.index, 0x2002u,
                                            &replacement) ||
      vkr_gpu_slot_table_retire(renderer->material_slots, material_b,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->sampled_image_slots, shared_texture,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->storage_image_slots, shared_storage,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->sampler_slots, shared_sampler,
                                submitted[1].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  uint32_t replacement_index = UINT32_MAX;
  out_result->replacement_survived =
      vkr_gpu_slot_table_resolve(renderer->material_slots, replacement,
                                 &replacement_index) == VKR_GPU_SLOT_STATUS_OK;
  renderer->active_material_index = replacement_index;
  if (!out_result->replacement_survived ||
      !vkr_bindless_vulkan_renderer_prepare_frame(renderer, 1003u, 2048u, 1u,
                                                  &setup) ||
      !vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                  &submitted[2]) ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;

  uint64_t after_submit = before_submit;
  for (uint32_t i = 0; i < ArrayCount(submitted); ++i) {
    VkrBindlessVulkanResult completed = {0};
    if (!vkr_bindless_vulkan_renderer_poll_result(renderer, after_submit,
                                                  &completed) ||
        !vkr_bindless_vk_result_is_empty_graph_clear(&completed))
      return false_v;
    after_submit = completed.submit_value;
    out_result->exact_draw_count++;
  }
  const uint64_t completed = renderer->completed_value;
  if (vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->material_slots, replacement,
                                submitted[2].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(
          renderer->sampled_image_slots, replacement_texture,
          submitted[2].submit_value) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(
          renderer->storage_image_slots, replacement_storage,
          submitted[2].submit_value) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_retire(renderer->sampler_slots, replacement_sampler,
                                submitted[2].submit_value) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->material_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampled_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->storage_image_slots, completed,
                                 NULL) != VKR_GPU_SLOT_STATUS_OK ||
      vkr_gpu_slot_table_collect(renderer->sampler_slots, completed, NULL) !=
          VKR_GPU_SLOT_STATUS_OK)
    return false_v;
  renderer->active_material_index = 0u;
  vkr_gpu_slot_table_get_metrics(renderer->sampled_image_slots,
                                 &out_result->sampled_images);
  vkr_gpu_slot_table_get_metrics(renderer->storage_image_slots,
                                 &out_result->storage_images);
  vkr_gpu_slot_table_get_metrics(renderer->sampler_slots,
                                 &out_result->samplers);
  vkr_gpu_slot_table_get_metrics(renderer->material_slots,
                                 &out_result->materials);
  VkrBindlessVulkanMemoryMetrics memory_after = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &memory_after);
  out_result->memory_returned_to_baseline =
      memory_after.aggregate.live_allocations ==
          memory_baseline.aggregate.live_allocations &&
      memory_after.aggregate.retired_allocations ==
          memory_baseline.aggregate.retired_allocations &&
      memory_after.aggregate.live_requested_bytes ==
          memory_baseline.aggregate.live_requested_bytes &&
      memory_after.aggregate.live_reserved_bytes ==
          memory_baseline.aggregate.live_reserved_bytes &&
      memory_after.aggregate.retired_requested_bytes ==
          memory_baseline.aggregate.retired_requested_bytes &&
      memory_after.aggregate.retired_reserved_bytes ==
          memory_baseline.aggregate.retired_reserved_bytes;
  if (!out_result->memory_returned_to_baseline)
    log_debug(
        "Bindless Vulkan V4 memory baseline drift: live %llu->%llu "
        "requested %llu->%llu reserved %llu->%llu retired %llu->%llu",
        (unsigned long long)memory_baseline.aggregate.live_allocations,
        (unsigned long long)memory_after.aggregate.live_allocations,
        (unsigned long long)memory_baseline.aggregate.live_requested_bytes,
        (unsigned long long)memory_after.aggregate.live_requested_bytes,
        (unsigned long long)memory_baseline.aggregate.live_reserved_bytes,
        (unsigned long long)memory_after.aggregate.live_reserved_bytes,
        (unsigned long long)memory_baseline.aggregate.retired_allocations,
        (unsigned long long)memory_after.aggregate.retired_allocations);
  return out_result->exact_draw_count == 6u &&
         out_result->shared_sampler_reused &&
         out_result->dependent_materials_republished &&
         out_result->upload_wait_free &&
         out_result->staging_retired_at_submit &&
         out_result->memory_returned_to_baseline &&
         out_result->sampled_images.slots_live ==
             baseline.sampled_images.slots_live &&
         out_result->sampled_images.slots_retired ==
             baseline.sampled_images.slots_retired &&
         out_result->storage_images.slots_live ==
             baseline.storage_images.slots_live &&
         out_result->storage_images.slots_retired ==
             baseline.storage_images.slots_retired &&
         out_result->samplers.slots_live == baseline.samplers.slots_live &&
         out_result->samplers.slots_retired ==
             baseline.samplers.slots_retired &&
         out_result->materials.slots_live == baseline.materials.slots_live &&
         out_result->materials.slots_retired ==
             baseline.materials.slots_retired;
}

static bool8_t vkr_bindless_vk_half4_matches(const uint16_t value[4],
                                             const uint16_t expected[4]) {
  for (uint32_t channel = 0u; channel < 4u; ++channel) {
    const uint16_t difference = value[channel] > expected[channel]
                                    ? value[channel] - expected[channel]
                                    : expected[channel] - value[channel];
    if (difference > 2u)
      return false_v;
  }
  return true_v;
}

bool8_t vkr_bindless_vulkan_renderer_run_ibl_test(
    VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanIblTestResult *out_result) {
  if (!renderer || !out_result || renderer->frame_active ||
      renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN ||
      renderer->config.sampled_image_capacity < 24u ||
      !vkr_bindless_vulkan_renderer_wait_idle(renderer))
    return false_v;
  MemZero(out_result, sizeof(*out_result));
  const uint16_t expected[4] = {0x3400u, 0x3800u, 0x3a00u, 0x3c00u};
  MemCopy(out_result->expected, expected, sizeof(expected));
  uint16_t pixels[8u * 4u * 4u] = {0};
  for (uint32_t pixel = 0u; pixel < 8u * 4u; ++pixel)
    MemCopy(&pixels[pixel * 4u], expected, sizeof(expected));
  VkrTextureUploadRegion upload_region = {
      .width = 8u,
      .height = 4u,
      .depth = 1u,
      .byte_size = sizeof(pixels),
  };
  const uint32_t base_id = renderer->config.sampled_image_capacity - 20u;
  const VkrTextureHandle equirect = {.id = base_id, .generation = 1u};
  const VkrTextureHandle replacement_equirect = {
      .id = base_id,
      .generation = 2u,
  };
  const VkrTextureHandle source = {.id = base_id + 1u, .generation = 1u};
  const VkrTextureHandle irradiance = {.id = base_id + 2u, .generation = 1u};
  const VkrTextureHandle prefilter = {.id = base_id + 3u, .generation = 1u};
  VkrTexturePreparedLoad prepared = {
      .description = {.id = equirect.id,
                      .generation = equirect.generation,
                      .width = 8u,
                      .height = 4u,
                      .channels = 4u,
                      .type = VKR_TEXTURE_TYPE_2D,
                      .format = VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT,
                      .sample_count = VKR_SAMPLE_COUNT_1,
                      .u_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .v_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .w_repeat_mode = VKR_TEXTURE_REPEAT_MODE_CLAMP_TO_EDGE,
                      .min_filter = VKR_FILTER_LINEAR,
                      .mag_filter = VKR_FILTER_LINEAR,
                      .mip_filter = VKR_MIP_FILTER_NONE},
      .upload_data = (uint8_t *)pixels,
      .upload_data_size = sizeof(pixels),
      .upload_regions = &upload_region,
      .upload_region_count = 1u,
      .upload_mip_levels = 1u,
      .upload_array_layers = 1u,
  };
  VkrTextureDescription source_desc = prepared.description;
  source_desc.id = source.id;
  source_desc.generation = source.generation;
  source_desc.width = source_desc.height = 4u;
  source_desc.type = VKR_TEXTURE_TYPE_CUBE_MAP;
  source_desc.mip_filter = VKR_MIP_FILTER_LINEAR;
  VkrTextureDescription irradiance_desc = source_desc;
  irradiance_desc.id = irradiance.id;
  irradiance_desc.generation = irradiance.generation;
  irradiance_desc.width = irradiance_desc.height = VKR_IBL_IRRADIANCE_SIZE;
  irradiance_desc.mip_filter = VKR_MIP_FILTER_NONE;
  VkrTextureDescription prefilter_desc = source_desc;
  prefilter_desc.id = prefilter.id;
  prefilter_desc.generation = prefilter.generation;
  prefilter_desc.width = prefilter_desc.height = VKR_IBL_PREFILTER_SIZE;

  VkrBindlessVulkanMemoryMetrics baseline = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &baseline);
  bool8_t passed =
      vkr_bindless_vk_asset_publish_texture(renderer, equirect, &prepared);
  if (!passed)
    log_error("Bindless Vulkan V5 IBL test failed to publish equirect texture");
  if (passed && !vkr_bindless_vk_asset_publish_writable_texture(
                    renderer, source, &source_desc)) {
    log_error("Bindless Vulkan V5 IBL test failed to publish source cube");
    passed = false_v;
  }
  if (passed && !vkr_bindless_vk_asset_publish_writable_texture(
                    renderer, irradiance, &irradiance_desc)) {
    log_error("Bindless Vulkan V5 IBL test failed to publish irradiance cube");
    passed = false_v;
  }
  if (passed && !vkr_bindless_vk_asset_publish_writable_texture(
                    renderer, prefilter, &prefilter_desc)) {
    log_error("Bindless Vulkan V5 IBL test failed to publish prefilter cube");
    passed = false_v;
  }
  if (passed &&
      !vkr_bindless_vk_queue_ibl_bake(renderer, equirect, source, irradiance,
                                      prefilter, true_v)) {
    log_error("Bindless Vulkan V5 IBL test failed to queue bake");
    passed = false_v;
  }
  if (passed && !vkr_bindless_vk_asset_unpublish_texture(renderer, equirect)) {
    log_error("Bindless Vulkan V5 IBL test failed to release delivery texture");
    passed = false_v;
  }
  VkrTexturePreparedLoad replacement_prepared = prepared;
  replacement_prepared.description.id = replacement_equirect.id;
  replacement_prepared.description.generation = replacement_equirect.generation;
  if (passed && !vkr_bindless_vk_asset_publish_texture(
                    renderer, replacement_equirect, &replacement_prepared)) {
    log_error(
        "Bindless Vulkan V5 IBL test failed immediate texture-slot reuse");
    passed = false_v;
  }
  VkrBindlessVulkanResult submitted = {0};
  for (uint32_t frame = 0u;
       passed && renderer->pending_ibl_bake_count && frame < 8u; ++frame) {
    VkrFrameSetup setup = {0};
    const VkrRenderPacket packet = {
        .packet_version = VKR_RENDER_PACKET_VERSION,
    };
    passed = vkr_bindless_vulkan_renderer_prepare_frame(renderer, 3001u + frame,
                                                        2048u, 1u, &setup) &&
             vkr_bindless_vulkan_renderer_submit_packet(renderer, &packet,
                                                        &submitted) &&
             vkr_bindless_vulkan_renderer_wait_idle(renderer);
    if (!passed)
      log_error("Bindless Vulkan V5 IBL test frame %u failed", frame);
  }
  if (passed && renderer->pending_ibl_bake_count)
    log_error("Bindless Vulkan V5 IBL test exhausted publication frames");
  passed = passed && renderer->pending_ibl_bake_count == 0u;
  VkrBindlessVkFrameSlot *capture = NULL;
  for (uint32_t i = 0u; passed && i < VKR_BINDLESS_VK_FRAME_SLOT_COUNT; ++i) {
    VkrBindlessVkFrameSlot *slot = &renderer->frame_slots[i];
    if (slot->ibl_capture_pending &&
        (!capture || slot->retire_value > capture->retire_value))
      capture = slot;
  }
  if (passed && !capture)
    log_error("Bindless Vulkan V5 IBL test produced no capture slot");
  const uint64_t capture_bytes =
      VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET +
      (1u + VKR_IBL_PREFILTER_MIP_COUNT) * VKR_BINDLESS_VK_IBL_TEXEL_BYTES;
  passed = passed && capture &&
           capture->ibl_capture_mip_count == VKR_IBL_PREFILTER_MIP_COUNT &&
           vkr_bindless_vk_invalidate(renderer, &capture->readback.allocation,
                                      0u, capture_bytes);
  if (passed) {
    const uint8_t *bytes = capture->readback.allocation.mapped;
    MemCopy(out_result->irradiance, bytes + VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET,
            VKR_BINDLESS_VK_IBL_TEXEL_BYTES);
    out_result->prefilter_mip_count = capture->ibl_capture_mip_count;
    for (uint32_t mip = 0u; mip < out_result->prefilter_mip_count; ++mip) {
      MemCopy(out_result->prefilter[mip],
              bytes + VKR_BINDLESS_VK_IBL_CAPTURE_OFFSET +
                  (uint64_t)(mip + 1u) * VKR_BINDLESS_VK_IBL_TEXEL_BYTES,
              VKR_BINDLESS_VK_IBL_TEXEL_BYTES);
    }
    out_result->irradiance_matches =
        vkr_bindless_vk_half4_matches(out_result->irradiance, expected);
    out_result->all_prefilter_mips_match = true_v;
    for (uint32_t mip = 0u; mip < out_result->prefilter_mip_count; ++mip)
      out_result->all_prefilter_mips_match &=
          vkr_bindless_vk_half4_matches(out_result->prefilter[mip], expected);
    capture->ibl_capture_pending = false_v;
  }

  const VkrTextureHandle handles[] = {prefilter, irradiance, source,
                                      replacement_equirect};
  for (uint32_t i = 0u; i < ArrayCount(handles); ++i) {
    VkrBindlessVkPublishedTexture *texture =
        vkr_bindless_vk_published_texture(renderer, handles[i], NULL);
    if (texture)
      passed &= vkr_bindless_vk_asset_unpublish_texture(renderer, handles[i]);
  }
  vkr_bindless_vk_collect_asset_publications(
      renderer, vkr_bindless_vk_refresh_completed(renderer));
  VkrBindlessVulkanMemoryMetrics after = {0};
  vkr_bindless_vulkan_renderer_memory_metrics(renderer, &after);
  out_result->memory_returned_to_baseline =
      after.aggregate.live_allocations == baseline.aggregate.live_allocations &&
      after.aggregate.retired_allocations ==
          baseline.aggregate.retired_allocations &&
      after.aggregate.live_requested_bytes ==
          baseline.aggregate.live_requested_bytes &&
      after.aggregate.live_reserved_bytes ==
          baseline.aggregate.live_reserved_bytes;
  return passed && out_result->irradiance_matches &&
         out_result->all_prefilter_mips_match &&
         out_result->memory_returned_to_baseline;
}

void vkr_bindless_vulkan_renderer_validation_stats(
    const VkrBindlessVulkanRenderer *renderer,
    VkrBindlessVulkanValidationStats *out_stats) {
  vkr_bindless_vulkan_device_validation_stats(
      renderer ? renderer->device : NULL, out_stats);
}

void vkr_bindless_vulkan_renderer_destroy(VkrBindlessVulkanRenderer *renderer) {
  if (!renderer) {
    return;
  }
  VkrAllocator *allocator = renderer->allocator;
  VkDevice device = renderer->device
                        ? vkr_bindless_vulkan_device_handle(renderer->device)
                        : VK_NULL_HANDLE;
  if (device) {
    vkr_bindless_vulkan_renderer_wait_idle(renderer);
    if (renderer->config.target_kind != VKR_PRESENT_TARGET_OFFSCREEN)
      (void)vkDeviceWaitIdle(device);
    vkr_bindless_vk_discard_buffer_initializations(renderer);
    vkr_bindless_vk_discard_texture_initializations(renderer);
    vkr_bindless_vk_drain_asset_publications(renderer);
    for (uint32_t i = 0; i < ArrayCount(renderer->retired_window_targets);
         ++i) {
      if (renderer->retired_window_targets[i].occupied) {
        (void)vkr_bindless_vk_window_presents_complete(
            renderer, &renderer->retired_window_targets[i].target, true_v);
        vkr_bindless_vk_destroy_window_target(
            renderer, &renderer->retired_window_targets[i].target);
      }
    }
    (void)vkr_bindless_vk_window_presents_complete(
        renderer, &renderer->window_target, true_v);
    vkr_bindless_vk_destroy_window_target(renderer, &renderer->window_target);
    for (uint32_t i = 0; i < ArrayCount(renderer->acquire_semaphores); ++i) {
      if (renderer->acquire_semaphores[i])
        vkDestroySemaphore(device, renderer->acquire_semaphores[i], NULL);
    }
    for (uint32_t i = 0; i < ArrayCount(renderer->retired_targets); ++i) {
      if (renderer->retired_targets[i].occupied) {
        vkr_bindless_vk_destroy_target_set(
            renderer, &renderer->retired_targets[i].targets);
      }
    }
    for (uint32_t i = 0; i < renderer->config.max_graph_images; ++i) {
      if (renderer->graph_images && renderer->graph_images[i].live)
        vkr_bindless_vk_destroy_graph_image(renderer,
                                            &renderer->graph_images[i]);
    }
    vkr_bindless_vk_destroy_target_set(renderer, &renderer->targets);
    vkr_bindless_vk_destroy_frame_slots(renderer);
    for (uint32_t i = 0u; i < VKR_BINDLESS_VK_PACKET_PIPELINE_COUNT; ++i) {
      if (renderer->packet_pipelines[i])
        vkDestroyPipeline(device, renderer->packet_pipelines[i], NULL);
    }
    for (uint32_t i = 0u; i < VKR_BINDLESS_VK_IBL_PIPELINE_COUNT; ++i) {
      if (renderer->ibl_pipelines[i])
        vkDestroyPipeline(device, renderer->ibl_pipelines[i], NULL);
    }
    if (renderer->pipeline) {
      vkDestroyPipeline(device, renderer->pipeline, NULL);
    }
    if (renderer->pipeline_layout) {
      vkDestroyPipelineLayout(device, renderer->pipeline_layout, NULL);
    }
    if (renderer->vertex_shader) {
      vkDestroyShaderModule(device, renderer->vertex_shader, NULL);
    }
    if (renderer->fragment_shader) {
      vkDestroyShaderModule(device, renderer->fragment_shader, NULL);
    }
    for (uint32_t i = 0u; i < VKR_BINDLESS_VK_PACKET_SHADER_COUNT; ++i) {
      if (renderer->packet_shaders[i])
        vkDestroyShaderModule(device, renderer->packet_shaders[i], NULL);
    }
    for (uint32_t i = 0u; i < VKR_BINDLESS_VK_IBL_PIPELINE_COUNT; ++i) {
      if (renderer->ibl_shaders[i])
        vkDestroyShaderModule(device, renderer->ibl_shaders[i], NULL);
    }
    if (renderer->sentinel_sampler) {
      vkDestroySampler(device, renderer->sentinel_sampler, NULL);
    }
    for (uint32_t i = 0; i < renderer->config.sampler_capacity; ++i) {
      if (renderer->published_samplers &&
          renderer->published_samplers[i].sampler)
        vkDestroySampler(device, renderer->published_samplers[i].sampler, NULL);
    }
    for (uint32_t i = 0; i < renderer->retired_staging_buffer_capacity; ++i) {
      if (renderer->retired_staging_buffers &&
          renderer->retired_staging_buffers[i].occupied)
        vkr_bindless_vk_destroy_buffer(
            renderer, &renderer->retired_staging_buffers[i].buffer);
    }
    vkr_bindless_vk_destroy_image(renderer, &renderer->sentinel_image);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->materials);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->upload);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->sampler_descriptors);
    vkr_bindless_vk_destroy_buffer(renderer, &renderer->resource_descriptors);
    vkr_bindless_vulkan_memory_pool_destroy(renderer->memory_pool);
    renderer->memory_pool = NULL;
    if (renderer->timeline) {
      vkDestroySemaphore(device, renderer->timeline, NULL);
    }
    vkr_bindless_vk_pipeline_cache_shutdown(renderer);
  }
  if (renderer->descriptor_scratch) {
    vkr_allocator_free(allocator, renderer->descriptor_scratch,
                       renderer->descriptor_scratch_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_materials) {
    vkr_allocator_free(allocator, renderer->retired_materials,
                       renderer->retired_materials_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_staging_buffers) {
    vkr_allocator_free(allocator, renderer->retired_staging_buffers,
                       renderer->retired_staging_buffers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->pending_texture_initializations) {
    vkr_allocator_free(allocator, renderer->pending_texture_initializations,
                       renderer->pending_texture_initializations_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->pending_buffer_initializations) {
    vkr_allocator_free(allocator, renderer->pending_buffer_initializations,
                       renderer->pending_buffer_initializations_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_materials) {
    vkr_allocator_free(allocator, renderer->published_materials,
                       renderer->published_materials_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_textures) {
    vkr_allocator_free(allocator, renderer->published_textures,
                       renderer->published_textures_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_textures) {
    vkr_allocator_free(allocator, renderer->retired_textures,
                       renderer->retired_textures_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_samplers) {
    vkr_allocator_free(allocator, renderer->published_samplers,
                       renderer->published_samplers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->published_geometries) {
    vkr_allocator_free(allocator, renderer->published_geometries,
                       renderer->published_geometries_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->retired_geometries) {
    vkr_allocator_free(allocator, renderer->retired_geometries,
                       renderer->retired_geometries_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->sampler_slot_storage) {
    vkr_allocator_free(allocator, renderer->sampler_slot_storage,
                       renderer->sampler_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->storage_image_slot_storage) {
    vkr_allocator_free(allocator, renderer->storage_image_slot_storage,
                       renderer->storage_image_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->material_slot_storage) {
    vkr_allocator_free(allocator, renderer->material_slot_storage,
                       renderer->material_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->sampled_image_slot_storage) {
    vkr_allocator_free(allocator, renderer->sampled_image_slot_storage,
                       renderer->sampled_image_slot_storage_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->capture_storage_memory.base_memory) {
    vkr_dmemory_destroy(&renderer->capture_storage_memory);
  }
  if (renderer->graph_buffer_barriers) {
    vkr_allocator_free(allocator, renderer->graph_buffer_barriers,
                       renderer->graph_buffer_barriers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->graph_image_barriers) {
    vkr_allocator_free(allocator, renderer->graph_image_barriers,
                       renderer->graph_image_barriers_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  if (renderer->graph_images) {
    vkr_allocator_free(allocator, renderer->graph_images,
                       renderer->graph_images_size,
                       VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
  }
  vkr_rg_destroy(renderer->graph);
  arena_destroy(renderer->graph_frame_arena);
  vkr_rg_json_destroy(&renderer->json_graph);
  vkr_rg_executor_registry_destroy(&renderer->executors);
  vkr_bindless_vulkan_device_destroy(renderer->device);
  if (renderer->publication_staging_memory.base_memory)
    vkr_dmemory_destroy(&renderer->publication_staging_memory);
  vkr_allocator_free(allocator, renderer, sizeof(*renderer),
                     VKR_ALLOCATOR_MEMORY_TAG_RENDERER);
}
