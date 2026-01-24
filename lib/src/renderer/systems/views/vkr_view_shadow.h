/**
 * @file vkr_view_shadow.h
 * @brief View layer that renders cascaded shadow maps.
 */
#pragma once

#include "defines.h"

struct s_RendererFrontend;

/**
 * @brief Register the shadow view layer.
 *
 * Creates a layer with one pass per cascade. Each pass renders into a
 * custom depth render target owned by the shadow system.
 */
bool32_t vkr_view_shadow_register(struct s_RendererFrontend *rf);

/**
 * @brief Unregister the shadow view layer.
 */
void vkr_view_shadow_unregister(struct s_RendererFrontend *rf);
