#pragma once

#include "defines.h"
#include "renderer/systems/vkr_camera.h"

#define VKR_GAMEPAD_MOVEMENT_DEADZONE 0.1f
#define VKR_GAMEPAD_ROTATION_SCALE 20.0f
/* Mouse-look degrees per pointer count at sensitivity 1. A count is a frame's
   displacement, not a rate, so look ignores frame time; 1/60 keeps the feel
   the former frame-time-scaled look had at 60 Hz. */
#define VKR_CAMERA_LOOK_DEGREES_PER_COUNT (1.0f / 60.0f)

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
  float32_t frame_look_yaw;
  float32_t frame_look_pitch;
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
 * @brief Adds yaw/pitch rates (pre-sensitivity, per second) for the current
 * frame, as a gamepad stick reports them; the update scales them by frame
 * time.
 * @param controller Controller to rotate
 * @param yaw_delta Yaw rate
 * @param pitch_delta Pitch rate
 */
void vkr_camera_controller_rotate(VkrCameraController *controller,
                                  float32_t yaw_delta, float32_t pitch_delta);

/**
 * @brief Adds mouse-look displacement in pointer counts for the current frame.
 * The update turns it by counts x sensitivity x
 * VKR_CAMERA_LOOK_DEGREES_PER_COUNT, independent of frame time.
 * @param controller Controller to rotate
 * @param yaw_counts Rightward pointer counts
 * @param pitch_counts Upward pointer counts
 */
void vkr_camera_controller_look(VkrCameraController *controller,
                                float32_t yaw_counts, float32_t pitch_counts);

/**
 * @brief Applies queued movement/rotation to the camera.
 * @param controller Controller to update
 * @param delta_time Time since last frame
 * @param input_blocked Clear queued input without applying it to the camera
 */
void vkr_camera_controller_update(VkrCameraController *controller,
                                  float64_t delta_time, bool8_t input_blocked);
