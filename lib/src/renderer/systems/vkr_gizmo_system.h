/**
 * @file vkr_gizmo_system.h
 * @brief Editor transform gizmo system.
 *
 * Provides a lightweight gizmo renderer that can be driven by higher-level
 * editor code. The system does not own selection logic; callers provide the
 * target transform each frame.
 */
#pragma once

#include "core/vkr_entity.h"
#include "defines.h"
#include "math/vec.h"
#include "math/vkr_quat.h"
#include "renderer/resources/vkr_resources.h"
#include "renderer/systems/vkr_picking_ids.h"
#include "renderer/vkr_renderer.h"

struct s_RendererFrontend;

typedef enum VkrGizmoMode {
  VKR_GIZMO_MODE_NONE = 0,      /**< Gizmo hidden. */
  VKR_GIZMO_MODE_TRANSLATE = 1, /**< Translation mode. */
  VKR_GIZMO_MODE_ROTATE = 2,    /**< Rotation mode. */
  VKR_GIZMO_MODE_SCALE = 3,     /**< Scale mode. */
} VkrGizmoMode;

typedef enum VkrGizmoSpace {
  VKR_GIZMO_SPACE_WORLD = 0, /**< World-aligned axes. */
  VKR_GIZMO_SPACE_LOCAL = 1, /**< Object-aligned axes (reserved). */
  VKR_GIZMO_SPACE_VIEW = 2,  /**< Camera-aligned axes (reserved). */
} VkrGizmoSpace;

typedef enum VkrGizmoHandle {
  VKR_GIZMO_HANDLE_NONE = 0,

  // Translation handles aligned to axes.
  VKR_GIZMO_HANDLE_TRANSLATE_X = 1,
  VKR_GIZMO_HANDLE_TRANSLATE_Y = 2,
  VKR_GIZMO_HANDLE_TRANSLATE_Z = 3,
  VKR_GIZMO_HANDLE_TRANSLATE_FREE = 4,

  // Rotation rings aligned to axes.
  VKR_GIZMO_HANDLE_ROTATE_X = 5,
  VKR_GIZMO_HANDLE_ROTATE_Y = 6,
  VKR_GIZMO_HANDLE_ROTATE_Z = 7,

  // Scale cubes aligned to axes (uniform scaling in current UX).
  VKR_GIZMO_HANDLE_SCALE_X = 8,
  VKR_GIZMO_HANDLE_SCALE_Y = 9,
  VKR_GIZMO_HANDLE_SCALE_Z = 10,
  VKR_GIZMO_HANDLE_SCALE_UNIFORM = 11,
} VkrGizmoHandle;

/**
 * @brief Encode a gizmo handle into a picking object id.
 * @param handle Gizmo handle to encode.
 * @return Encoded object id, or 0 if handle is invalid.
 */
vkr_internal inline uint32_t
vkr_gizmo_encode_picking_id(VkrGizmoHandle handle) {
  return vkr_picking_encode_id(VKR_PICKING_ID_KIND_GIZMO, (uint32_t)handle);
}

/**
 * @brief Decode a gizmo handle from a picking object id.
 * @param object_id Picking object id to decode.
 * @return Decoded handle or NONE if the id is not a gizmo handle.
 */
vkr_internal inline VkrGizmoHandle
vkr_gizmo_decode_picking_id(uint32_t object_id) {
  VkrPickingDecodedId decoded = vkr_picking_decode_id(object_id);
  if (!decoded.valid || decoded.kind != VKR_PICKING_ID_KIND_GIZMO) {
    return VKR_GIZMO_HANDLE_NONE;
  }
  return (VkrGizmoHandle)decoded.value;
}

/**
 * @brief Returns the transform mode implied by a gizmo handle.
 * @param handle Gizmo handle to inspect.
 * @return Transform mode, or NONE if the handle is invalid.
 */
vkr_internal inline VkrGizmoMode vkr_gizmo_handle_mode(VkrGizmoHandle handle) {
  switch (handle) {
  case VKR_GIZMO_HANDLE_TRANSLATE_X:
  case VKR_GIZMO_HANDLE_TRANSLATE_Y:
  case VKR_GIZMO_HANDLE_TRANSLATE_Z:
  case VKR_GIZMO_HANDLE_TRANSLATE_FREE:
    return VKR_GIZMO_MODE_TRANSLATE;
  case VKR_GIZMO_HANDLE_ROTATE_X:
  case VKR_GIZMO_HANDLE_ROTATE_Y:
  case VKR_GIZMO_HANDLE_ROTATE_Z:
    return VKR_GIZMO_MODE_ROTATE;
  case VKR_GIZMO_HANDLE_SCALE_X:
  case VKR_GIZMO_HANDLE_SCALE_Y:
  case VKR_GIZMO_HANDLE_SCALE_Z:
  case VKR_GIZMO_HANDLE_SCALE_UNIFORM:
    return VKR_GIZMO_MODE_SCALE;
  default:
    return VKR_GIZMO_MODE_NONE;
  }
}

/**
 * @brief Returns the axis for a handle (if any).
 * @param handle Gizmo handle to inspect.
 * @param out_axis Receives axis direction on success.
 * @return true when the handle implies an axis; false otherwise.
 */
vkr_internal inline bool8_t vkr_gizmo_handle_axis(VkrGizmoHandle handle,
                                                  Vec3 *out_axis) {
  if (!out_axis) {
    return false_v;
  }

  switch (handle) {
  case VKR_GIZMO_HANDLE_TRANSLATE_X:
  case VKR_GIZMO_HANDLE_ROTATE_X:
  case VKR_GIZMO_HANDLE_SCALE_X:
    *out_axis = vec3_right();
    return true_v;
  case VKR_GIZMO_HANDLE_TRANSLATE_Y:
  case VKR_GIZMO_HANDLE_ROTATE_Y:
  case VKR_GIZMO_HANDLE_SCALE_Y:
    *out_axis = vec3_up();
    return true_v;
  case VKR_GIZMO_HANDLE_TRANSLATE_Z:
  case VKR_GIZMO_HANDLE_ROTATE_Z:
  case VKR_GIZMO_HANDLE_SCALE_Z:
    *out_axis = vec3_back();
    return true_v;
  default:
    *out_axis = vec3_zero();
    return false_v;
  }
}

/**
 * @brief Returns axis index for handles that align to X/Y/Z.
 * @param handle Gizmo handle to inspect.
 * @return 0=X, 1=Y, 2=Z, or -1 when the handle has no axis.
 */
vkr_internal inline int32_t vkr_gizmo_handle_axis_index(VkrGizmoHandle handle) {
  switch (handle) {
  case VKR_GIZMO_HANDLE_TRANSLATE_X:
  case VKR_GIZMO_HANDLE_ROTATE_X:
  case VKR_GIZMO_HANDLE_SCALE_X:
    return 0;
  case VKR_GIZMO_HANDLE_TRANSLATE_Y:
  case VKR_GIZMO_HANDLE_ROTATE_Y:
  case VKR_GIZMO_HANDLE_SCALE_Y:
    return 1;
  case VKR_GIZMO_HANDLE_TRANSLATE_Z:
  case VKR_GIZMO_HANDLE_ROTATE_Z:
  case VKR_GIZMO_HANDLE_SCALE_Z:
    return 2;
  default:
    return -1;
  }
}

/**
 * @brief Returns true for screen-plane translation handles.
 */
vkr_internal inline bool8_t
vkr_gizmo_handle_is_free_translate(VkrGizmoHandle handle) {
  return handle == VKR_GIZMO_HANDLE_TRANSLATE_FREE;
}

/**
 * @brief Returns true for uniform scale handles (including axis cubes).
 *
 * Current UX treats all scale cubes as uniform scaling rather than per-axis.
 */
vkr_internal inline bool8_t
vkr_gizmo_handle_is_uniform_scale(VkrGizmoHandle handle) {
  return handle == VKR_GIZMO_HANDLE_SCALE_UNIFORM ||
         handle == VKR_GIZMO_HANDLE_SCALE_X ||
         handle == VKR_GIZMO_HANDLE_SCALE_Y ||
         handle == VKR_GIZMO_HANDLE_SCALE_Z;
}

/**
 * @brief Gizmo runtime configuration.
 */
typedef struct VkrGizmoConfig {
  float32_t screen_size; /**< Desired gizmo size in screen pixels. */
} VkrGizmoConfig;

#define VKR_GIZMO_CONFIG_DEFAULT ((VkrGizmoConfig){.screen_size = 150.0f})

/**
 * @brief Runtime state for the gizmo system.
 */
typedef struct VkrGizmoSystem {
  VkrGizmoConfig config;
  VkrGizmoMode mode;
  VkrGizmoSpace space;

  VkrEntityId selected_entity;
  Vec3 position;
  VkrQuat orientation;
  VkrGizmoHandle hot_handle;
  VkrGizmoHandle active_handle;

  uint32_t gizmo_mesh_index;
  VkrPipelineHandle pipeline;

  bool8_t visible;
  bool8_t initialized;
} VkrGizmoSystem;

/**
 * @brief Initialize gizmo resources (mesh/pipeline lookup).
 * @param system Gizmo system to initialize.
 * @param renderer Renderer frontend.
 * @param config Optional config override (NULL uses defaults).
 * @return true on success.
 */
bool8_t vkr_gizmo_system_init(VkrGizmoSystem *system,
                              struct s_RendererFrontend *renderer,
                              const VkrGizmoConfig *config);

/**
 * @brief Shutdown the gizmo system and release owned resources.
 * @param system Gizmo system to shutdown.
 * @param renderer Renderer frontend.
 */
void vkr_gizmo_system_shutdown(VkrGizmoSystem *system,
                               struct s_RendererFrontend *renderer);

/**
 * @brief Update the gizmo target transform for rendering.
 * @param system Gizmo system to update.
 * @param entity Selected entity (VKR_ENTITY_ID_INVALID hides the gizmo).
 * @param position World position of the gizmo.
 * @param orientation World orientation of the gizmo.
 */
void vkr_gizmo_system_set_target(VkrGizmoSystem *system, VkrEntityId entity,
                                 Vec3 position, VkrQuat orientation);

/**
 * @brief Clear the current gizmo selection and hide it.
 * @param system Gizmo system to update.
 */
void vkr_gizmo_system_clear_target(VkrGizmoSystem *system);

/**
 * @brief Set the currently hovered gizmo handle for highlight rendering.
 * @param system Gizmo system to update.
 * @param handle Handle under cursor (NONE clears hover).
 */
void vkr_gizmo_system_set_hot_handle(VkrGizmoSystem *system,
                                     VkrGizmoHandle handle);

/**
 * @brief Set the currently active gizmo handle for highlight rendering.
 * @param system Gizmo system to update.
 * @param handle Handle being manipulated (NONE clears active state).
 */
void vkr_gizmo_system_set_active_handle(VkrGizmoSystem *system,
                                        VkrGizmoHandle handle);

/**
 * @brief Render the gizmo in the current render pass.
 * @param system Gizmo system to render.
 * @param renderer Renderer frontend.
 * @param camera Active camera for screen-space scaling.
 * @param viewport_height Height of the render target in pixels.
 * @param pipeline_override Pipeline handle to use (invalid to use default).
 */
void vkr_gizmo_system_render(VkrGizmoSystem *system,
                             struct s_RendererFrontend *renderer,
                             const VkrCamera *camera, uint32_t viewport_height,
                             VkrPipelineHandle pipeline_override);

/**
 * @brief Render gizmo handles into an active picking pass.
 *
 * Assumes the picking shader and pipeline are already bound.
 *
 * @param system Gizmo system to render.
 * @param renderer Renderer frontend.
 * @param camera Active camera for screen-space scaling.
 * @param viewport_height Height of the picking target in pixels.
 */
void vkr_gizmo_system_render_picking(VkrGizmoSystem *system,
                                     struct s_RendererFrontend *renderer,
                                     const VkrCamera *camera,
                                     uint32_t viewport_height);
