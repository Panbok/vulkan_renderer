#pragma once

#include "core/vkr_window.h"
#include "math/mat.h"
#include "math/vec.h"

#define VKR_MAX_MOUSE_DELTA 100.0f
#define VKR_DEFAULT_CAMERA_ZOOM 1.0f
#define VKR_DEFAULT_CAMERA_SPEED 7.5f
#define VKR_DEFAULT_CAMERA_SENSITIVITY 0.1f
#define VKR_DEFAULT_CAMERA_YAW -90.0f
#define VKR_DEFAULT_CAMERA_PITCH 0.0f

#define VKR_MIN_CAMERA_ZOOM 1.0f
#define VKR_MAX_CAMERA_ZOOM 45.0f
#define VKR_MAX_CAMERA_PITCH 89.0f
#define VKR_MIN_CAMERA_PITCH -89.0f

#define VKR_DEFAULT_CAMERA_POSITION vec3_new(-1.5f, 0.0f, -17.0f)
#define VKR_DEFAULT_CAMERA_FORWARD vec3_new(0.0f, 0.0f, -1.0f)
#define VKR_DEFAULT_CAMERA_UP vec3_new(0.0f, 1.0f, 0.0f)
#define VKR_DEFAULT_CAMERA_RIGHT vec3_new(1.0f, 0.0f, 0.0f)
#define VKR_DEFAULT_CAMERA_WORLD_UP vec3_new(0.0f, 1.0f, 0.0f)

/**
 * @brief Camera projection types.
 */
typedef enum VkrCameraType {
  VKR_CAMERA_TYPE_NONE,         /**< Uninitialized camera */
  VKR_CAMERA_TYPE_PERSPECTIVE,  /**< 3D perspective projection */
  VKR_CAMERA_TYPE_ORTHOGRAPHIC, /**< 2D/3D orthographic projection */
} CameraType;

/**
 * @brief 3D camera storing orientation and projection state.
 *
 * The camera keeps track of position/orientation vectors along with cached view
 * and projection matrices. Input devices are handled by controller systems that
 * mutate this data and mark matrices dirty before rendering.
 */
typedef struct VkrCamera {
  VkrWindow *window; /**< Window for input capture and aspect ratio */

  CameraType type; /**< Current projection type */

  Mat4 view;       /**< View matrix */
  Mat4 projection; /**< Projection matrix */

  Vec3 position; /**< Camera world position */
  Vec3 forward;  /**< Forward direction vector */
  Vec3 up;       /**< Up direction vector */
  Vec3 right;    /**< Right direction vector */
  Vec3 world_up; /**< World up reference (usually 0,1,0) */

  float32_t yaw;   /**< Horizontal rotation (degrees) */
  float32_t pitch; /**< Vertical rotation (degrees, clamped ±89°) */

  float32_t speed;       /**< Movement speed (units per second) */
  float32_t sensitivity; /**< Mouse sensitivity multiplier */

  float32_t near_clip; /**< Near clipping plane distance */
  float32_t far_clip;  /**< Far clipping plane distance */

  // Perspective projection
  float32_t zoom; /**< Field of view for perspective (degrees) */

  // Orthographic projection
  float32_t left_clip;   /**< Left boundary for orthographic */
  float32_t right_clip;  /**< Right boundary for orthographic */
  float32_t bottom_clip; /**< Bottom boundary for orthographic */
  float32_t top_clip;    /**< Top boundary for orthographic */

  bool8_t view_dirty;       /**< Requires view matrix recompute */
  bool8_t projection_dirty; /**< Requires projection matrix recompute */

  uint32_t cached_window_width;  /**< Last window width used for projection */
  uint32_t cached_window_height; /**< Last window height used for projection */
} VkrCamera;

/**
 * @brief Creates a perspective camera with 3D projection.
 * @param camera Camera to initialize
 * @param window Window for mouse capture and aspect ratio
 * @param zoom Initial zoom value (affects field of view)
 * @param near_clip Near clipping plane distance
 * @param far_clip Far clipping plane distance
 */
void vkr_camera_system_perspective_create(VkrCamera *camera, VkrWindow *window,
                                          float32_t zoom, float32_t near_clip,
                                          float32_t far_clip);

/**
 * @brief Creates an orthographic camera with 2D/3D projection.
 * @param camera Camera to initialize
 * @param window Window for mouse capture
 * @param left Left boundary of the orthographic volume
 * @param right Right boundary of the orthographic volume
 * @param bottom Bottom boundary of the orthographic volume
 * @param top Top boundary of the orthographic volume
 * @param near_clip Near clipping plane distance
 * @param far_clip Far clipping plane distance
 */
void vkr_camera_system_orthographic_create(VkrCamera *camera, VkrWindow *window,
                                           float32_t left, float32_t right,
                                           float32_t bottom, float32_t top,
                                           float32_t near_clip,
                                           float32_t far_clip);

/**
 * @brief Translates the camera by the supplied world-space delta.
 * @param camera Camera to translate
 * @param delta Delta
 */
void vkr_camera_translate(VkrCamera *camera, Vec3 delta);

/**
 * @brief Adds yaw/pitch deltas (in degrees) and refreshes orientation vectors.
 * @param camera Camera to rotate
 * @param yaw_delta Yaw delta
 * @param pitch_delta Pitch delta
 */
void vkr_camera_rotate(VkrCamera *camera, float32_t yaw_delta,
                       float32_t pitch_delta);

/**
 * @brief Adjusts zoom (perspective FOV) and marks projection dirty.
 * @param camera Camera to zoom
 * @param zoom_delta Zoom delta
 */
void vkr_camera_zoom(VkrCamera *camera, float32_t zoom_delta);

/**
 * @brief Recomputes camera matrices if marked dirty.
 *
 * @param camera Camera to update
 */
void vkr_camera_system_update(VkrCamera *camera);

/**
 * @brief Gets the view matrix for rendering.
 * @param camera Camera to get view matrix from
 * @return 4x4 view matrix for transforming world space to view space
 */
Mat4 vkr_camera_system_get_view_matrix(const VkrCamera *camera);

/**
 * @brief Gets the projection matrix for rendering.
 * @param camera Camera to get projection matrix from
 * @return 4x4 projection matrix for transforming view space to clip space
 */
Mat4 vkr_camera_system_get_projection_matrix(const VkrCamera *camera);
