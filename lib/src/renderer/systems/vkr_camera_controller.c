#include "renderer/systems/vkr_camera_controller.h"

void vkr_camera_controller_create(VkrCameraController *controller,
                                  VkrCamera *camera,
                                  float32_t target_frame_rate) {
  assert_log(controller != NULL, "Camera controller is NULL");
  assert_log(camera != NULL, "Camera is NULL");
  // assert_log(target_frame_rate > 0.0f, "Target frame rate must be positive");

  controller->camera = camera;
  controller->target_frame_rate = target_frame_rate;
  controller->move_speed = camera->speed;
  controller->rotation_speed = camera->sensitivity;
  controller->frame_move_forward = 0.0f;
  controller->frame_move_right = 0.0f;
  controller->frame_move_world_up = 0.0f;
  controller->frame_yaw_delta = 0.0f;
  controller->frame_pitch_delta = 0.0f;
}

void vkr_camera_controller_move_forward(VkrCameraController *controller,
                                        float32_t amount) {
  assert_log(controller != NULL, "Camera controller is NULL");
  controller->frame_move_forward -= amount;
}

void vkr_camera_controller_move_right(VkrCameraController *controller,
                                      float32_t amount) {
  assert_log(controller != NULL, "Camera controller is NULL");
  controller->frame_move_right += amount;
}

void vkr_camera_controller_move_world_up(VkrCameraController *controller,
                                         float32_t amount) {
  assert_log(controller != NULL, "Camera controller is NULL");
  controller->frame_move_world_up += amount;
}

void vkr_camera_controller_rotate(VkrCameraController *controller,
                                  float32_t yaw_delta, float32_t pitch_delta) {
  assert_log(controller != NULL, "Camera controller is NULL");
  controller->frame_yaw_delta += yaw_delta;
  controller->frame_pitch_delta += pitch_delta;
}

void vkr_camera_controller_update(VkrCameraController *controller,
                                  float64_t delta_time) {
  assert_log(controller != NULL, "Camera controller is NULL");
  assert_log(controller->camera != NULL, "Camera is NULL");

  VkrCamera *camera = controller->camera;
  float32_t frame_delta = (float32_t)delta_time;
  if (frame_delta <= 0.0f) {
    controller->frame_move_forward = 0.0f;
    controller->frame_move_right = 0.0f;
    controller->frame_move_world_up = 0.0f;
    controller->frame_yaw_delta = 0.0f;
    controller->frame_pitch_delta = 0.0f;
    return;
  }

  float32_t move_speed =
      (camera->speed > 0.0f) ? camera->speed : controller->move_speed;
  float32_t velocity = move_speed * frame_delta;

  Vec3 movement = vec3_zero();
  if (controller->frame_move_forward != 0.0f) {
    movement = vec3_add(
        movement,
        vec3_scale(camera->forward, controller->frame_move_forward * velocity));
  }
  if (controller->frame_move_right != 0.0f) {
    movement =
        vec3_add(movement, vec3_scale(camera->right,
                                      controller->frame_move_right * velocity));
  }
  if (controller->frame_move_world_up != 0.0f) {
    movement = vec3_add(movement,
                        vec3_scale(camera->world_up,
                                   controller->frame_move_world_up * velocity));
  }

  if (movement.x != 0.0f || movement.y != 0.0f || movement.z != 0.0f) {
    vkr_camera_translate(camera, movement);
  }

  float32_t rotation_speed = (camera->sensitivity > 0.0f)
                                 ? camera->sensitivity
                                 : controller->rotation_speed;
  float32_t frame_adjusted_sensitivity = rotation_speed * frame_delta;

  float32_t yaw_delta =
      controller->frame_yaw_delta * frame_adjusted_sensitivity;
  float32_t pitch_delta =
      controller->frame_pitch_delta * frame_adjusted_sensitivity;

  if (yaw_delta != 0.0f || pitch_delta != 0.0f) {
    vkr_camera_rotate(camera, yaw_delta, pitch_delta);
  }

  controller->frame_move_forward = 0.0f;
  controller->frame_move_right = 0.0f;
  controller->frame_move_world_up = 0.0f;
  controller->frame_yaw_delta = 0.0f;
  controller->frame_pitch_delta = 0.0f;
}
