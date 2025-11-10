#pragma once

#include "defines.h"
#include "renderer/systems/vkr_camera.h"

#define VKR_GAMEPAD_MOVEMENT_DEADZONE 0.1f
#define VKR_GAMEPAD_ROTATION_SCALE 20.0f

/**
 * @brief Queues frame-local movement/rotation and applies it to a camera.
 */
typedef struct VkrCameraController {
  VkrCamera *camera;
  float32_t target_frame_rate;
  float32_t move_speed;
  float32_t rotation_speed;

  float32_t frame_move_forward;
  float32_t frame_move_right;
  float32_t frame_move_world_up;
  float32_t frame_yaw_delta;
  float32_t frame_pitch_delta;
} VkrCameraController;

/**
 * @brief Creates a camera controller.
 * @param controller The camera controller to create
 * @param camera The camera to control
 * @param target_frame_rate The target frame rate to use
 */
void vkr_camera_controller_create(VkrCameraController *controller,
                                  VkrCamera *camera,
                                  float32_t target_frame_rate);

/**
 * @brief Accumulates local forward movement for the current frame.
 * @param controller Controller to move forward
 * @param amount Amount to move forward
 */
void vkr_camera_controller_move_forward(VkrCameraController *controller,
                                        float32_t amount);

/**
 * @brief Accumulates local right movement for the current frame.
 * @param controller Controller to move right
 * @param amount Amount to move right
 */
void vkr_camera_controller_move_right(VkrCameraController *controller,
                                      float32_t amount);

/**
 * @brief Accumulates world-up movement for the current frame.
 * @param controller Controller to move world up
 * @param amount Amount to move world up
 */
void vkr_camera_controller_move_world_up(VkrCameraController *controller,
                                         float32_t amount);

/**
 * @brief Adds yaw/pitch deltas (pre-sensitivity) for the current frame.
 * @param controller Controller to rotate
 * @param yaw_delta Yaw delta
 * @param pitch_delta Pitch delta
 */
void vkr_camera_controller_rotate(VkrCameraController *controller,
                                  float32_t yaw_delta, float32_t pitch_delta);

/**
 * @brief Applies queued movement/rotation to the camera.
 * @param controller Controller to update
 * @param delta_time Time since last frame
 */
void vkr_camera_controller_update(VkrCameraController *controller,
                                  float64_t delta_time);
