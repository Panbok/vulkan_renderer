#pragma once

#include "defines.h"
#include "renderer/vkr_render_graph.h"

/** Registers graph-declared image copy executors. */
bool8_t vkr_pass_copy_register(VkrRgExecutorRegistry *registry);
