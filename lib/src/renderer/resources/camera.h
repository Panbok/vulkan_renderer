#pragma once

#include "core/input.h"
#include "core/logger.h"
#include "core/vkr_window.h"
#include "math/mat.h"
#include "math/math.h"
#include "math/vec.h"

#define MAX_MOUSE_DELTA 100.0f
#define DEFAULT_CAMERA_ZOOM 1.0f
#define DEFAULT_CAMERA_SPEED 2.5f
#define DEFAULT_CAMERA_SENSITIVITY 0.1f
#define DEFAULT_CAMERA_YAW -90.0f
#define DEFAULT_CAMERA_PITCH 0.0f

#define DEFAULT_CAMERA_POSITION vec3_new(0.0f, 0.0f, -5.0f)
#define DEFAULT_CAMERA_FORWARD vec3_new(0.0f, 0.0f, -1.0f)
#define DEFAULT_CAMERA_UP vec3_new(0.0f, 1.0f, 0.0f)
#define DEFAULT_CAMERA_RIGHT vec3_new(1.0f, 0.0f, 0.0f)
#define DEFAULT_CAMERA_WORLD_UP vec3_new(0.0f, 1.0f, 0.0f)

/**
 * @brief Camera projection types.
 */
typedef enum CameraType {
  CAMERA_TYPE_NONE,         /**< Uninitialized camera */
  CAMERA_TYPE_PERSPECTIVE,  /**< 3D perspective projection */
  CAMERA_TYPE_ORTHOGRAPHIC, /**< 2D/3D orthographic projection */
} CameraType;

/**
 * @brief 3D camera with input handling and projection support.
 *
 * Supports both perspective and orthographic projections with mouse look,
 * WASD movement, and mouse wheel zoom. Handles input capture and frame-rate
 * independent movement. Also supports gamepad input with right thumbstick
 * for movement and left thumbstick for camera rotation.
 */
typedef struct Camera {
  InputState *input_state;     /**< Input system reference */
  VkrWindow *window;           /**< Window for input capture and aspect ratio */
  float32_t target_frame_rate; /**< Target FPS for frame-independent movement */

  CameraType type; /**< Current projection type */

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
  float32_t previous_wheel_delta; /**< Previous mouse wheel state */

  // Orthographic projection
  float32_t left_clip;   /**< Left boundary for orthographic */
  float32_t right_clip;  /**< Right boundary for orthographic */
  float32_t bottom_clip; /**< Bottom boundary for orthographic */
  float32_t top_clip;    /**< Top boundary for orthographic */

  // Gamepad
  bool8_t should_use_gamepad; /**< When true, uses right thumbstick for movement
                                 and left thumbstick for camera rotation */
} Camera;

/**
 * @brief Creates a perspective camera with 3D projection.
 * @param camera Camera to initialize
 * @param input_state Input system for handling user input
 * @param window Window for mouse capture and aspect ratio
 * @param target_frame_rate Target FPS for consistent movement speed
 * @param fov Initial field of view (degrees)
 * @param near_clip Near clipping plane distance
 * @param far_clip Far clipping plane distance
 */
void camera_perspective_create(Camera *camera, InputState *input_state,
                               VkrWindow *window, float32_t target_frame_rate,
                               float32_t fov, float32_t near_clip,
                               float32_t far_clip);

/**
 * @brief Creates an orthographic camera with 2D/3D projection.
 * @param camera Camera to initialize
 * @param input_state Input system for handling user input
 * @param window Window for mouse capture
 * @param target_frame_rate Target FPS for consistent movement speed
 * @param left Left boundary of the orthographic volume
 * @param right Right boundary of the orthographic volume
 * @param bottom Bottom boundary of the orthographic volume
 * @param top Top boundary of the orthographic volume
 * @param near_clip Near clipping plane distance
 * @param far_clip Far clipping plane distance
 */
void camera_orthographic_create(Camera *camera, InputState *input_state,
                                VkrWindow *window, float32_t target_frame_rate,
                                float32_t left, float32_t right,
                                float32_t bottom, float32_t top,
                                float32_t near_clip, float32_t far_clip);

/**
 * @brief Updates camera position and orientation based on input.
 *
 * Handles:
 * - WASD movement (frame-rate independent)
 * - Mouse look (with sensitivity and pitch clamping)
 * - Mouse wheel zoom
 * - TAB key for toggling mouse capture
 *
 * @param camera Camera to update
 * @param delta_time Time since last frame (seconds)
 */
void camera_update(Camera *camera, float32_t delta_time);

/**
 * @brief Gets the view matrix for rendering.
 * @param camera Camera to get view matrix from
 * @return 4x4 view matrix for transforming world space to view space
 */
Mat4 camera_get_view_matrix(const Camera *camera);

/**
 * @brief Gets the projection matrix for rendering.
 * @param camera Camera to get projection matrix from
 * @return 4x4 projection matrix for transforming view space to clip space
 */
Mat4 camera_get_projection_matrix(const Camera *camera);