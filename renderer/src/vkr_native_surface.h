#pragma once
#include "defines.h"

typedef struct VkrSurfaceSize {
  uint32_t width;
  uint32_t height;
} VkrSurfaceSize;

/** Borrowed presentation handles and size query. The caller keeps handles and
 * context alive until renderer destruction; pixel_size runs on the render
 * thread. Native handles are required only for the selected platform backend.
 */
typedef struct VkrNativeSurface {
  void *metal_layer;
  void *win32_instance;
  void *win32_window;
  void *context;
  VkrSurfaceSize (*pixel_size)(void *context);
} VkrNativeSurface;
