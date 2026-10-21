#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"
#include "vkr_renderer_impl.h"

#include <vulkan/vulkan.h>

/*
 * AMD hardware counters per graph pass through GPUPerfAPI (ADR-130).
 *
 * GPUPerfAPI's Vulkan library sits beside the executable and must be
 * initialized before the device and its queue exist, so the device creates
 * this on an AMD candidate with VK_AMD_gpa_interface and enables the
 * extensions it needs. The counter context opens on the first request.
 *
 * The counters form groups the hardware reads in one replay. A request
 * samples the frames recorded until its deadline in sessions of up to 30
 * frames, each with the next group: every graph pass of every frame is a
 * sample beside its timestamps, matched across frames by pass name, so a
 * graph that changes between frames only adds passes. Once the GPU finished
 * a session, its samples are read a batch per frame and each pass's mean
 * over its frames is published. Clocks keep the driver's own frequencies,
 * so the sampled frames still show real timing apart from the samples'
 * cost.
 */
typedef struct s_VkrVulkanAmdCounters VkrVulkanAmdCounters;

/**
 * @brief Loads GPUPerfAPI and initializes it; call before vkCreateDevice.
 * @return false when the library is missing or refuses the device, which
 * then runs without counters.
 */
bool8_t vkr_vulkan_amd_counters_create(VkrAllocator *allocator,
                                       VkrVulkanAmdCounters **out_counters);

/** Closes the counter context and uninitializes GPUPerfAPI; call before
 * vkDestroyDevice. NULL is ignored. */
void vkr_vulkan_amd_counters_destroy(VkrAllocator *allocator,
                                     VkrVulkanAmdCounters **counters);

/** The device the counter context opens on, once it exists. */
void vkr_vulkan_amd_counters_bind(VkrVulkanAmdCounters *counters,
                                  VkInstance instance,
                                  VkPhysicalDevice physical_device,
                                  VkDevice device);

/** Any thread: samples the frames recorded in the next `seconds`; zero
 * stops after the current session. */
void vkr_vulkan_amd_counters_request(VkrVulkanAmdCounters *counters,
                                     float64_t seconds);

/** Render thread, recording the frame's command buffer: begins the frame's
 * replay pass when a session samples it. `passes` are the graph passes the
 * frame records, in order. */
void vkr_vulkan_amd_counters_begin_frame(
    VkrVulkanAmdCounters *counters, VkCommandBuffer command,
    uint64_t frame_index, const VkrRendererImplPassTiming *passes,
    uint32_t pass_count);

/** Brackets graph pass `order` of a sampled frame, beside its timestamps. */
void vkr_vulkan_amd_counters_begin_pass(VkrVulkanAmdCounters *counters,
                                        uint32_t order);
void vkr_vulkan_amd_counters_end_pass(VkrVulkanAmdCounters *counters);

/** Ends the frame's replay pass, and the session after its last one. */
void vkr_vulkan_amd_counters_end_frame(VkrVulkanAmdCounters *counters);

/** Render thread, once a frame: reads a finished session's samples without
 * waiting for the GPU. */
void vkr_vulkan_amd_counters_collect(VkrVulkanAmdCounters *counters);

/**
 * @brief Any thread: copies the latest finished session into `out` once.
 * @return false when no session finished since the last take.
 */
bool8_t vkr_vulkan_amd_counters_take(VkrVulkanAmdCounters *counters,
                                     VkrRendererImplGpuCounters *out);
