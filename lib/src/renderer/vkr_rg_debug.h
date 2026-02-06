/**
 * @file vkr_rg_debug.h
 * @brief Debug helpers for the render graph.
 */
#pragma once

#include "defines.h"
#include "renderer/vkr_render_graph.h"

typedef enum VkrRgDotExportFlags {
  VKR_RG_DOT_EXPORT_NONE = 0,
  VKR_RG_DOT_EXPORT_RESOURCES = 1 << 0,
  VKR_RG_DOT_EXPORT_PASS_EDGES = 1 << 1,
} VkrRgDotExportFlags;

#define VKR_RG_DOT_EXPORT_DEFAULT                                             \
  (VKR_RG_DOT_EXPORT_RESOURCES | VKR_RG_DOT_EXPORT_PASS_EDGES)

typedef struct VkrRgDotExportDesc {
  const char *path;
  VkrRgDotExportFlags flags;
} VkrRgDotExportDesc;

/**
 * @brief Export the render graph to a Graphviz DOT file.
 *
 * @param graph Render graph to export.
 * @param path Output path (relative to working directory unless absolute).
 * @return true on success, false on failure.
 */
bool8_t vkr_rg_export_dot(const VkrRenderGraph *graph, const char *path);

/**
 * @brief Export the render graph to DOT with configurable flags.
 *
 * @param graph Render graph to export.
 * @param desc Export options (path + flags).
 * @return true on success, false on failure.
 */
bool8_t vkr_rg_export_dot_ex(const VkrRenderGraph *graph,
                             const VkrRgDotExportDesc *desc);
