#pragma once
#include "vkr_renderer_metrics.h"
struct VkrRenderAssets;
struct VkrUiSystem;
struct VkrLightingSystem;

VkrApplicationMetricsSnapshot
vkr_application_metrics_snapshot(const struct VkrRenderAssets *assets,
                                 const struct VkrUiSystem *ui,
                                 const struct VkrLightingSystem *lighting);
