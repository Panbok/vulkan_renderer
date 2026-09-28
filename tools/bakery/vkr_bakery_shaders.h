#pragma once

#include "vkr_bakery_internal.h"

typedef struct VkrBakeryShaderToolchain {
  char slangc[VKR_BAKERY_PATH_CAPACITY];
  char slangc_version[128];
  char metal_version[256];
  bool8_t has_slangc;
  bool8_t has_metal;
} VkrBakeryShaderToolchain;

typedef struct VkrBakeryShaderRequest {
  bool8_t vulkan;
  bool8_t metal;
  /* Entry names to compile; zero compiles every entry and the manifests. */
  const char *const *entries;
  uint32_t entry_count;
  const char *output_directory;              /* Absolute catalog root. */
  const VkrBakeryShaderToolchain *toolchain; /* Resolved by the caller. */
  VkrBakeryPriority priority;
} VkrBakeryShaderRequest;

/** Finds slangc (config, VKR_SLANGC, VULKAN_SDK, build default, PATH) and
 * the Metal toolchain, recording their version strings for action keys.
 * `scratch` is a writable directory for capturing tool output. */
bool8_t vkr_bakery_shader_toolchain(VkrBakeryShaderToolchain *toolchain,
                                    const char *slangc, const char *scratch);
/** Adds shader actions for the requested backends to `graph`. The config's
 * slangc path must equal the request toolchain's resolved path. */
bool8_t vkr_bakery_plan_shaders(VkrBakeryGraph *graph,
                                const VkrBakeryShaderRequest *request);
