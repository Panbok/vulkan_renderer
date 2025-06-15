#pragma once

#include "containers/bitset.h"
#include "defines.h"
#include "platform/vulkan_platform.h"
#include "vulkan_types.h"
#include "vulkan_utils.h"

bool32_t vulkan_device_pick_physical_device(VulkanBackendState *state);

void vulkan_device_release_physical_device(VulkanBackendState *state);

bool32_t vulkan_device_create_logical_device(VulkanBackendState *state);

void vulkan_device_destroy_logical_device(VulkanBackendState *state);

void vulkan_device_get_information(VulkanBackendState *state,
                                   DeviceInformation *device_information,
                                   Arena *temp_arena);