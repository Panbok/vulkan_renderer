#pragma once

#include "renderer/vkr_bloom.h"
#include "renderer/vkr_gtao.h"
#include "renderer/vkr_prepared_frame.h"
#include "renderer/vkr_render_graph.h"

/* Derive portable graph conditions and GTAO constants from prepared inputs.
 * Acquisition supplies the target extent. Native formats, resource instances,
 * history validity and completion state remain owned by the caller. */
void vkr_render_graph_prepare_frame(const VkrPreparedFrame *packet,
                                    const VkrBloomConfig *bloom_config,
                                    const VkrGtaoConfig *gtao_config,
                                    VkrRenderGraphFrameInfo *frame,
                                    VkrGtaoGpuParams *gtao_params);
