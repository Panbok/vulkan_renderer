#pragma once

#include "defines.h"

struct s_RendererFrontend;

/**
 * @brief Registers the skybox view layer with the renderer.
 *
 * The skybox layer renders before the world layer and draws a cube with a
 * cube map texture. It uses front-face culling to render the inside of the cube.
 *
 * @param rf The renderer frontend handle
 * @return true on success, false on failure
 */
bool32_t vkr_view_skybox_register(struct s_RendererFrontend *rf);

/**
 * @brief Unregisters the skybox view layer.
 *
 * @param rf The renderer frontend handle
 */
void vkr_view_skybox_unregister(struct s_RendererFrontend *rf);

