#pragma once

#include "core/vkr_window.h"
#include "memory/vkr_allocator.h"

#include <vulkan/vulkan.h>

#if defined(PLATFORM_WINDOWS)
#include <vulkan/vulkan_win32.h>
#endif

enum {
  VKR_BINDLESS_VK_MAX_CANDIDATES = 16,
  VKR_BINDLESS_VK_MAX_REPORT_ENTRIES = 128,
  VKR_BINDLESS_VK_REPORT_NAME_CAPACITY = 80,
  VKR_BINDLESS_VK_REPORT_DETAIL_CAPACITY = 192,
};

typedef enum VkrBindlessVkReportKind {
  VKR_BINDLESS_VK_REPORT_API_VERSION = 0,
  VKR_BINDLESS_VK_REPORT_INSTANCE_EXTENSION,
  VKR_BINDLESS_VK_REPORT_DEVICE_EXTENSION,
  VKR_BINDLESS_VK_REPORT_FEATURE,
  VKR_BINDLESS_VK_REPORT_LIMIT,
  VKR_BINDLESS_VK_REPORT_QUEUE,
  VKR_BINDLESS_VK_REPORT_FORMAT,
  VKR_BINDLESS_VK_REPORT_DEVICE_CREATE,
  VKR_BINDLESS_VK_REPORT_LAYOUT,
} VkrBindlessVkReportKind;

typedef struct VkrBindlessVkReportEntry {
  VkrBindlessVkReportKind kind;
  bool8_t required;
  bool8_t present;
  char name[VKR_BINDLESS_VK_REPORT_NAME_CAPACITY];
  char detail[VKR_BINDLESS_VK_REPORT_DETAIL_CAPACITY];
} VkrBindlessVkReportEntry;

typedef struct VkrBindlessVkCandidateReport {
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
  VkrBindlessVkReportEntry entries[VKR_BINDLESS_VK_MAX_REPORT_ENTRIES];
} VkrBindlessVkCandidateReport;

typedef struct VkrBindlessVkCapabilityProfile {
  uint32_t candidate_count;
  uint32_t selected_candidate_index;
  bool8_t windowed;
  VkrBindlessVkCandidateReport candidates[VKR_BINDLESS_VK_MAX_CANDIDATES];
} VkrBindlessVkCapabilityProfile;

typedef struct VkrBindlessVulkanDeviceConfig {
  VkrAllocator *allocator;
  VkrWindow *window;
  uint32_t sampled_image_capacity;
  uint32_t storage_image_capacity;
  uint32_t sampler_capacity;
  uint32_t root_push_constant_size;
  bool8_t windowed;
  bool8_t enable_validation;
  bool8_t enable_synchronization_validation;
  bool8_t enable_gpu_assisted;
} VkrBindlessVulkanDeviceConfig;

typedef struct VkrBindlessVulkanDescriptorLayout {
  VkDescriptorSetLayout handle;
  VkDeviceSize size;
  VkDeviceSize sampled_image_offset;
  VkDeviceSize storage_image_offset;
  VkDeviceSize sampler_offset;
} VkrBindlessVulkanDescriptorLayout;

typedef struct VkrBindlessVulkanDevice VkrBindlessVulkanDevice;

/**
 * Creates and retains the highest-ranked device that passes the complete
 * immutable capability floor. A failed probe still returns an object when its
 * allocation succeeded so callers can publish the per-candidate rejection
 * report before destroying it.
 */
bool8_t
vkr_bindless_vulkan_device_create(const VkrBindlessVulkanDeviceConfig *config,
                                  VkrBindlessVulkanDevice **out_device);
void vkr_bindless_vulkan_device_destroy(VkrBindlessVulkanDevice *device);

bool8_t
vkr_bindless_vulkan_device_is_ready(const VkrBindlessVulkanDevice *device);
const VkrBindlessVkCapabilityProfile *
vkr_bindless_vulkan_device_profile(const VkrBindlessVulkanDevice *device);

VkInstance
vkr_bindless_vulkan_device_instance(const VkrBindlessVulkanDevice *device);
VkPhysicalDevice
vkr_bindless_vulkan_device_physical(const VkrBindlessVulkanDevice *device);
VkDevice
vkr_bindless_vulkan_device_handle(const VkrBindlessVulkanDevice *device);
VkQueue vkr_bindless_vulkan_device_queue(const VkrBindlessVulkanDevice *device);
VkSurfaceKHR
vkr_bindless_vulkan_device_surface(const VkrBindlessVulkanDevice *device);
bool8_t vkr_bindless_vulkan_device_present_fences_enabled(
    const VkrBindlessVulkanDevice *device);
uint32_t
vkr_bindless_vulkan_device_queue_family(const VkrBindlessVulkanDevice *device);
const VkPhysicalDeviceProperties2 *
vkr_bindless_vulkan_device_properties(const VkrBindlessVulkanDevice *device);
const VkPhysicalDeviceMemoryProperties *
vkr_bindless_vulkan_device_memory_properties(
    const VkrBindlessVulkanDevice *device);
const VkPhysicalDeviceDescriptorBufferPropertiesEXT *
vkr_bindless_vulkan_device_descriptor_properties(
    const VkrBindlessVulkanDevice *device);
const VkrBindlessVulkanDescriptorLayout *
vkr_bindless_vulkan_device_resource_layout(
    const VkrBindlessVulkanDevice *device);
const VkrBindlessVulkanDescriptorLayout *
vkr_bindless_vulkan_device_sampler_layout(
    const VkrBindlessVulkanDevice *device);

PFN_vkGetDescriptorEXT vkr_bindless_vulkan_device_get_descriptor(
    const VkrBindlessVulkanDevice *device);
PFN_vkCmdBindDescriptorBuffersEXT
vkr_bindless_vulkan_device_cmd_bind_descriptor_buffers(
    const VkrBindlessVulkanDevice *device);
PFN_vkCmdSetDescriptorBufferOffsetsEXT
vkr_bindless_vulkan_device_cmd_set_descriptor_offsets(
    const VkrBindlessVulkanDevice *device);
