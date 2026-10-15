#pragma once

#include "memory/vkr_allocator.h"
#include "vkr_native_surface.h"

#include <vulkan/vulkan.h>

enum {
  VKR_VULKAN_MAX_CANDIDATES = 16,
  VKR_VULKAN_MAX_REPORT_ENTRIES = 128,
  VKR_VULKAN_REPORT_NAME_CAPACITY = 80,
  VKR_VULKAN_REPORT_DETAIL_CAPACITY = 192,
};

typedef enum VkrVulkanReportKind {
  VKR_VULKAN_REPORT_API_VERSION = 0,
  VKR_VULKAN_REPORT_INSTANCE_EXTENSION,
  VKR_VULKAN_REPORT_DEVICE_EXTENSION,
  VKR_VULKAN_REPORT_FEATURE,
  VKR_VULKAN_REPORT_LIMIT,
  VKR_VULKAN_REPORT_QUEUE,
  VKR_VULKAN_REPORT_FORMAT,
  VKR_VULKAN_REPORT_DEVICE_CREATE,
  VKR_VULKAN_REPORT_LAYOUT,
} VkrVulkanReportKind;

typedef struct VkrVulkanReportEntry {
  VkrVulkanReportKind kind;
  bool8_t required;
  bool8_t present;
  char name[VKR_VULKAN_REPORT_NAME_CAPACITY];
  char detail[VKR_VULKAN_REPORT_DETAIL_CAPACITY];
} VkrVulkanReportEntry;

typedef struct VkrVulkanCandidateReport {
  char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
  char driver_name[VK_MAX_DRIVER_NAME_SIZE];
  char driver_info[VK_MAX_DRIVER_INFO_SIZE];
  uint32_t api_version;
  uint32_t driver_id;
  VkConformanceVersion conformance_version;
  uint32_t queue_family_index;
  bool8_t offscreen_viable;
  bool8_t window_viable;
  bool8_t overflowed;
  uint32_t entry_count;
  VkrVulkanReportEntry entries[VKR_VULKAN_MAX_REPORT_ENTRIES];
} VkrVulkanCandidateReport;

typedef struct VkrVulkanCapabilityProfile {
  uint32_t candidate_count;
  uint32_t selected_candidate_index;
  bool8_t windowed;
  VkrVulkanCandidateReport candidates[VKR_VULKAN_MAX_CANDIDATES];
} VkrVulkanCapabilityProfile;

typedef struct VkrVulkanDeviceConfig {
  VkrAllocator *allocator;
  VkrNativeSurface surface;
  uint32_t sampled_image_capacity;
  uint32_t storage_image_capacity;
  uint32_t sampler_capacity;
  uint32_t root_push_constant_size;
  bool8_t windowed;
  bool8_t enable_validation;
  bool8_t enable_synchronization_validation;
  bool8_t enable_gpu_assisted;
  /** Requests shaderStorageImageExtendedFormats for FSR 3.1; a device
      without it is still selected and reports FSR unavailable. */
  bool8_t fsr31_enabled;
} VkrVulkanDeviceConfig;

/* Acceleration-structure descriptors of the resource layout's binding 2,
   one per frame slot's TLAS, present only with ray queries (ADR-119). */
#define VKR_VULKAN_ACCELERATION_STRUCTURE_SLOTS 4u

typedef struct VkrVulkanDescriptorLayout {
  VkDescriptorSetLayout handle;
  VkDeviceSize size;
  VkDeviceSize sampled_image_offset;
  VkDeviceSize storage_image_offset;
  VkDeviceSize sampler_offset;
  /* Binding 2's offset; zero without ray queries. */
  VkDeviceSize acceleration_structure_offset;
} VkrVulkanDescriptorLayout;

typedef struct VkrVulkanDevice VkrVulkanDevice;

/**
 * Creates and retains the highest-ranked device that passes the complete
 * immutable capability floor. A failed probe still returns an object when its
 * allocation succeeded so callers can publish the per-candidate rejection
 * report before destroying it.
 */
bool8_t vkr_vulkan_device_create(const VkrVulkanDeviceConfig *config,
                                 VkrVulkanDevice **out_device);
void vkr_vulkan_device_destroy(VkrVulkanDevice *device);

bool8_t vkr_vulkan_device_is_ready(const VkrVulkanDevice *device);
const VkrVulkanCapabilityProfile *
vkr_vulkan_device_profile(const VkrVulkanDevice *device);

VkInstance vkr_vulkan_device_instance(const VkrVulkanDevice *device);
VkPhysicalDevice vkr_vulkan_device_physical(const VkrVulkanDevice *device);
/** True when FSR 3.1 was requested and the selected device enabled its
    storage-image formats. */
bool8_t vkr_vulkan_device_fsr31_supported(const VkrVulkanDevice *device);
VkDevice vkr_vulkan_device_handle(const VkrVulkanDevice *device);
VkQueue vkr_vulkan_device_queue(const VkrVulkanDevice *device);
VkSurfaceKHR vkr_vulkan_device_surface(const VkrVulkanDevice *device);
VkSurfaceFormatKHR vkr_vulkan_device_choose_surface_format(
    const VkSurfaceFormatKHR *formats, const bool8_t *format_usable,
    uint32_t count, bool8_t prefer_extended_linear);
bool8_t vkr_vulkan_device_present_fences_enabled(const VkrVulkanDevice *device);
/** True when the device enabled textureCompressionASTC_HDR. */
bool8_t vkr_vulkan_device_astc_hdr_enabled(const VkrVulkanDevice *device);
bool8_t vkr_vulkan_device_extended_linear_present_enabled(
    const VkrVulkanDevice *device);
uint32_t vkr_vulkan_device_queue_family(const VkrVulkanDevice *device);
const VkPhysicalDeviceProperties2 *
vkr_vulkan_device_properties(const VkrVulkanDevice *device);
float32_t vkr_vulkan_device_max_anisotropy(const VkrVulkanDevice *device);
const VkPhysicalDeviceMemoryProperties *
vkr_vulkan_device_memory_properties(const VkrVulkanDevice *device);
const VkPhysicalDeviceDescriptorBufferPropertiesEXT *
vkr_vulkan_device_descriptor_properties(const VkrVulkanDevice *device);
const VkrVulkanDescriptorLayout *
vkr_vulkan_device_resource_layout(const VkrVulkanDevice *device);
const VkrVulkanDescriptorLayout *
vkr_vulkan_device_sampler_layout(const VkrVulkanDevice *device);

PFN_vkGetDescriptorEXT
vkr_vulkan_device_get_descriptor(const VkrVulkanDevice *device);
PFN_vkCmdBindDescriptorBuffersEXT
vkr_vulkan_device_cmd_bind_descriptor_buffers(const VkrVulkanDevice *device);
PFN_vkCmdSetDescriptorBufferOffsetsEXT
vkr_vulkan_device_cmd_set_descriptor_offsets(const VkrVulkanDevice *device);

/** Acceleration-structure entry points of an enabled VK_KHR_ray_query. */
typedef struct VkrVulkanRayQueryFunctions {
  PFN_vkCreateAccelerationStructureKHR create_acceleration_structure;
  PFN_vkDestroyAccelerationStructureKHR destroy_acceleration_structure;
  PFN_vkGetAccelerationStructureBuildSizesKHR get_build_sizes;
  PFN_vkCmdBuildAccelerationStructuresKHR cmd_build;
  PFN_vkGetAccelerationStructureDeviceAddressKHR get_device_address;
  /* Compaction: a built structure's compacted size, and the copy into it. */
  PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmd_write_properties;
  PFN_vkCmdCopyAccelerationStructureKHR cmd_copy;
  /** minAccelerationStructureScratchOffsetAlignment, at least one. */
  uint32_t scratch_alignment;
  /** accelerationStructureDescriptorSize of the descriptor buffer. */
  uint32_t descriptor_size;
} VkrVulkanRayQueryFunctions;

/** Null unless the device enabled ray queries (ADR-083: an optional
 * capability; the desktop pipeline's volume stays baked without it). */
const VkrVulkanRayQueryFunctions *
vkr_vulkan_device_ray_query_functions(const VkrVulkanDevice *device);

/** Null when VK_EXT_debug_utils is absent; callers skip labelling. */
PFN_vkCmdBeginDebugUtilsLabelEXT
vkr_vulkan_device_cmd_begin_debug_label(const VkrVulkanDevice *device);
PFN_vkCmdEndDebugUtilsLabelEXT
vkr_vulkan_device_cmd_end_debug_label(const VkrVulkanDevice *device);
