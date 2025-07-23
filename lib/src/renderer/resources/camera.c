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

  camera->speed = DEFAULT_SPEED;
  camera->sensitivity = DEFAULT_SENSITIVITY;
  camera->yaw = DEFAULT_YAW;
  camera->pitch = DEFAULT_PITCH;

  camera->position = DEFAULT_POSITION;
  camera->forward = DEFAULT_FORWARD;
  camera->up = DEFAULT_UP;
  camera->right = DEFAULT_RIGHT;
  camera->world_up = DEFAULT_WORLD_UP;

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
  camera->zoom = DEFAULT_ZOOM;

  camera->speed = DEFAULT_SPEED;
  camera->sensitivity = DEFAULT_SENSITIVITY;
  camera->yaw = DEFAULT_YAW;
  camera->pitch = DEFAULT_PITCH;

  camera->position = DEFAULT_POSITION;
  camera->forward = DEFAULT_FORWARD;
  camera->up = DEFAULT_UP;
  camera->right = DEFAULT_RIGHT;
  camera->world_up = DEFAULT_WORLD_UP;

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

  if (!window_is_mouse_captured(camera->window)) {
    return;
  }

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

  float32_t x_offset = x - last_x;
  float32_t y_offset = last_y - y;

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