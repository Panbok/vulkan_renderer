#include "camera.h"

void camera_perspective_create(Camera *camera, InputState *input_state,
                               Window *window, float32_t target_frame_rate,
                               float32_t zoom, float32_t near_clip,
                               float32_t far_clip) {
  assert_log(input_state != NULL, "Input state is NULL");
  assert_log(window != NULL, "Window is NULL");

  camera->input_state = input_state;
  camera->target_frame_rate = target_frame_rate;
  camera->window = window;

  camera->type = CAMERA_TYPE_PERSPECTIVE;

  camera->speed = DEFAULT_CAMERA_SPEED;
  camera->sensitivity = DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = DEFAULT_CAMERA_YAW;
  camera->pitch = DEFAULT_CAMERA_PITCH;

  camera->position = DEFAULT_CAMERA_POSITION;
  camera->forward = DEFAULT_CAMERA_FORWARD;
  camera->up = DEFAULT_CAMERA_UP;
  camera->right = DEFAULT_CAMERA_RIGHT;
  camera->world_up = DEFAULT_CAMERA_WORLD_UP;

  camera->near_clip = near_clip;
  camera->far_clip = far_clip;
  camera->zoom = zoom;

  int8_t wheel_delta;
  input_get_mouse_wheel(camera->input_state, &wheel_delta);
  camera->previous_wheel_delta = wheel_delta;
}

void camera_orthographic_create(Camera *camera, InputState *input_state,
                                Window *window, float32_t target_frame_rate,
                                float32_t left, float32_t right,
                                float32_t bottom, float32_t top,
                                float32_t near_clip, float32_t far_clip) {
  assert_log(input_state != NULL, "Input state is NULL");
  assert_log(window != NULL, "Window is NULL");

  camera->input_state = input_state;
  camera->window = window;

  camera->type = CAMERA_TYPE_ORTHOGRAPHIC;
  camera->target_frame_rate = target_frame_rate;
  camera->zoom = DEFAULT_CAMERA_ZOOM;

  camera->speed = DEFAULT_CAMERA_SPEED;
  camera->sensitivity = DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = DEFAULT_CAMERA_YAW;
  camera->pitch = DEFAULT_CAMERA_PITCH;

  camera->position = DEFAULT_CAMERA_POSITION;
  camera->forward = DEFAULT_CAMERA_FORWARD;
  camera->up = DEFAULT_CAMERA_UP;
  camera->right = DEFAULT_CAMERA_RIGHT;
  camera->world_up = DEFAULT_CAMERA_WORLD_UP;

  camera->near_clip = near_clip;
  camera->far_clip = far_clip;
  camera->left_clip = left;
  camera->right_clip = right;
  camera->bottom_clip = bottom;
  camera->top_clip = top;
}

// todo: impl setting dirty flag for skipping view and projection matrix
// recalculation
void camera_update(Camera *camera, float32_t delta_time) {
  assert_log(camera->input_state != NULL, "Input state is NULL");
  assert_log(camera->type != CAMERA_TYPE_NONE, "Camera type is NONE");

  if (input_is_key_down(camera->input_state, KEY_TAB) &&
      input_was_key_up(camera->input_state, KEY_TAB)) {
    window_set_mouse_capture(camera->window,
                             !window_is_mouse_captured(camera->window));
  }

  if (input_is_button_down(camera->input_state, BUTTON_GAMEPAD_A) &&
      input_was_button_up(camera->input_state, BUTTON_GAMEPAD_A)) {
    window_set_mouse_capture(camera->window,
                             !window_is_mouse_captured(camera->window));
    camera->should_use_gamepad = !camera->should_use_gamepad;
  }

  if (!window_is_mouse_captured(camera->window)) {
    return;
  }

  float32_t x_offset;
  float32_t y_offset;
  if (!camera->should_use_gamepad) {

    float32_t velocity = camera->speed * delta_time;
    if (input_is_key_down(camera->input_state, KEY_W)) {
      camera->position =
          vec3_sub(camera->position, vec3_scale(camera->forward, velocity));
    }

    if (input_is_key_down(camera->input_state, KEY_S)) {
      camera->position =
          vec3_add(camera->position, vec3_scale(camera->forward, velocity));
    }

    if (input_is_key_down(camera->input_state, KEY_A)) {
      camera->position =
          vec3_sub(camera->position, vec3_scale(camera->right, velocity));
    }

    if (input_is_key_down(camera->input_state, KEY_D)) {
      camera->position =
          vec3_add(camera->position, vec3_scale(camera->right, velocity));
    }

    int8_t wheel_delta;
    input_get_mouse_wheel(camera->input_state, &wheel_delta);

    if (wheel_delta != camera->previous_wheel_delta) {
      camera->zoom -= wheel_delta * 0.1f;
      if (camera->zoom < 1.0f)
        camera->zoom = 1.0f;
      if (camera->zoom > 45.0f)
        camera->zoom = 45.0f;

      camera->previous_wheel_delta = wheel_delta;
    }

    int32_t x, y;
    input_get_mouse_position(camera->input_state, &x, &y);

    int32_t last_x, last_y;
    input_get_previous_mouse_position(camera->input_state, &last_x, &last_y);

    if ((x == last_x && y == last_y) || (x == 0 && y == 0) ||
        (last_x == 0 && last_y == 0)) {
      return;
    }

    x_offset = x - last_x;
    y_offset = last_y - y;
  } else {
    // Gamepad mode: Use right thumbstick for camera movement
    float32_t velocity = camera->speed * delta_time;

    // Get right thumbstick values for movement
    float right_x, right_y;
    input_get_right_stick(camera->input_state, &right_x, &right_y);

    // Apply deadzone to prevent drift
    float deadzone = 0.1f;
    if (abs_f32(right_x) > deadzone || abs_f32(right_y) > deadzone) {
      // Forward/backward movement (Y-axis)
      if (right_y > deadzone) {
        camera->position =
            vec3_sub(camera->position,
                     vec3_scale(camera->forward, velocity * abs_f32(right_y)));
      } else if (right_y < -deadzone) {
        camera->position =
            vec3_add(camera->position,
                     vec3_scale(camera->forward, velocity * abs_f32(right_y)));
      }

      // Left/right movement (X-axis)
      if (right_x > deadzone) {
        camera->position =
            vec3_add(camera->position,
                     vec3_scale(camera->right, velocity * abs_f32(right_x)));
      } else if (right_x < -deadzone) {
        camera->position =
            vec3_sub(camera->position,
                     vec3_scale(camera->right, velocity * abs_f32(right_x)));
      }
    }

    // Use left stick for camera rotation (look around)
    float left_x, left_y;
    input_get_left_stick(camera->input_state, &left_x, &left_y);

    // Apply deadzone to prevent drift
    float rotation_deadzone = 0.1f;
    if (abs_f32(left_x) < rotation_deadzone)
      left_x = 0.0f;
    if (abs_f32(left_y) < rotation_deadzone)
      left_y = 0.0f;

    // Use direct stick values for rotation instead of deltas
    // This prevents the camera from following the stick back to center
    x_offset = left_x * 20.0f;  // Scale for sensitivity
    y_offset = -left_y * 20.0f; // Invert Y for natural camera movement
  }

  float32_t max_mouse_delta = MAX_MOUSE_DELTA / camera->sensitivity;
  x_offset = clamp_f32(x_offset, -max_mouse_delta, max_mouse_delta);
  y_offset = clamp_f32(y_offset, -max_mouse_delta, max_mouse_delta);

  float32_t frame_adjusted_sensitivity =
      camera->sensitivity * delta_time * camera->target_frame_rate;
  camera->yaw -= x_offset * frame_adjusted_sensitivity;
  camera->pitch += y_offset * frame_adjusted_sensitivity;

  // Done to prevent camera from flipping
  if (camera->pitch > 89.0f)
    camera->pitch = 89.0f;
  if (camera->pitch < -89.0f)
    camera->pitch = -89.0f;

  Vec3 front = vec3_new(
      cos_f32(to_radians(camera->yaw)) * cos_f32(to_radians(camera->pitch)),
      sin_f32(to_radians(camera->pitch)),
      sin_f32(to_radians(camera->yaw)) * cos_f32(to_radians(camera->pitch)));
  camera->forward = vec3_normalize(front);

  camera->right = vec3_normalize(vec3_cross(camera->forward, camera->world_up));
  camera->up = vec3_normalize(vec3_cross(camera->right, camera->forward));
}

Mat4 camera_get_view_matrix(const Camera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != CAMERA_TYPE_NONE, "Camera type is NONE");

  return mat4_look_at(camera->position,
                      vec3_add(camera->position, camera->forward), camera->up);
}

Mat4 camera_get_projection_matrix(const Camera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != CAMERA_TYPE_NONE, "Camera type is NONE");

  if (camera->type == CAMERA_TYPE_PERSPECTIVE) {
    WindowPixelSize window_size = window_get_pixel_size(camera->window);
    return mat4_perspective(to_radians(camera->zoom),
                            (float32_t)window_size.width /
                                (float32_t)window_size.height,
                            camera->near_clip, camera->far_clip);
  }

  if (camera->type == CAMERA_TYPE_ORTHOGRAPHIC) {
    return mat4_ortho(camera->left_clip, camera->right_clip,
                      camera->bottom_clip, camera->top_clip, camera->near_clip,
                      camera->far_clip);
  }

  return mat4_identity();
}