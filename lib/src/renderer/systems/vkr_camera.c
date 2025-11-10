#include "vkr_camera.h"

vkr_internal Mat4 vkr_camera_calculate_view(const VkrCamera *camera) {
  return mat4_look_at(camera->position,
                      vec3_add(camera->position, camera->forward), camera->up);
}

vkr_internal Mat4 vkr_camera_calculate_projection(const VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
    VkrWindowPixelSize window_size = vkr_window_get_pixel_size(camera->window);
    assert_log(window_size.width > 0 && window_size.height > 0,
               "Window size invalid");

    float32_t aspect =
        (float32_t)window_size.width / (float32_t)window_size.height;
    return mat4_perspective(vkr_to_radians(camera->zoom), aspect,
                            camera->near_clip, camera->far_clip);
  }

  if (camera->type == VKR_CAMERA_TYPE_ORTHOGRAPHIC) {
    return mat4_ortho(camera->left_clip, camera->right_clip,
                      camera->bottom_clip, camera->top_clip, camera->near_clip,
                      camera->far_clip);
  }

  assert_log(false, "Unhandled camera type");
  return mat4_identity();
}

vkr_internal void vkr_camera_update_orientation(VkrCamera *camera) {
  Vec3 front = vec3_new(vkr_cos_f32(vkr_to_radians(camera->yaw)) *
                            vkr_cos_f32(vkr_to_radians(camera->pitch)),
                        vkr_sin_f32(vkr_to_radians(camera->pitch)),
                        vkr_sin_f32(vkr_to_radians(camera->yaw)) *
                            vkr_cos_f32(vkr_to_radians(camera->pitch)));
  camera->forward = vec3_normalize(front);
  camera->right = vec3_normalize(vec3_cross(camera->forward, camera->world_up));
  camera->up = vec3_normalize(vec3_cross(camera->right, camera->forward));
  camera->view_dirty = true_v;
}

vkr_internal float32_t vkr_camera_clamp_zoom(float32_t zoom) {
  return vkr_clamp_f32(zoom, VKR_MIN_CAMERA_ZOOM, VKR_MAX_CAMERA_ZOOM);
}

void vkr_camera_system_perspective_create(VkrCamera *camera, VkrWindow *window,
                                          float32_t zoom, float32_t near_clip,
                                          float32_t far_clip) {
  assert_log(window != NULL, "Window is NULL");

  camera->window = window;

  camera->type = VKR_CAMERA_TYPE_PERSPECTIVE;

  camera->speed = VKR_DEFAULT_CAMERA_SPEED;
  camera->sensitivity = VKR_DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = VKR_DEFAULT_CAMERA_YAW;
  camera->pitch = VKR_DEFAULT_CAMERA_PITCH;

  camera->position = VKR_DEFAULT_CAMERA_POSITION;
  camera->forward = VKR_DEFAULT_CAMERA_FORWARD;
  camera->up = VKR_DEFAULT_CAMERA_UP;
  camera->right = VKR_DEFAULT_CAMERA_RIGHT;
  camera->world_up = VKR_DEFAULT_CAMERA_WORLD_UP;

  camera->near_clip = near_clip;
  camera->far_clip = far_clip;
  camera->zoom = zoom;

  camera->view_dirty = true_v;
  camera->projection_dirty = true_v;

  vkr_camera_update_orientation(camera);

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(window);
  camera->cached_window_width = window_size.width;
  camera->cached_window_height = window_size.height;
  vkr_camera_system_update(camera);
}

void vkr_camera_system_orthographic_create(VkrCamera *camera, VkrWindow *window,
                                           float32_t left, float32_t right,
                                           float32_t bottom, float32_t top,
                                           float32_t near_clip,
                                           float32_t far_clip) {
  assert_log(window != NULL, "Window is NULL");

  camera->window = window;

  camera->type = VKR_CAMERA_TYPE_ORTHOGRAPHIC;
  camera->zoom = VKR_DEFAULT_CAMERA_ZOOM;

  camera->speed = VKR_DEFAULT_CAMERA_SPEED;
  camera->sensitivity = VKR_DEFAULT_CAMERA_SENSITIVITY;
  camera->yaw = VKR_DEFAULT_CAMERA_YAW;
  camera->pitch = VKR_DEFAULT_CAMERA_PITCH;

  camera->position = VKR_DEFAULT_CAMERA_POSITION;
  camera->forward = VKR_DEFAULT_CAMERA_FORWARD;
  camera->up = VKR_DEFAULT_CAMERA_UP;
  camera->right = VKR_DEFAULT_CAMERA_RIGHT;
  camera->world_up = VKR_DEFAULT_CAMERA_WORLD_UP;

  camera->near_clip = near_clip;
  camera->far_clip = far_clip;
  camera->left_clip = left;
  camera->right_clip = right;
  camera->bottom_clip = bottom;
  camera->top_clip = top;

  camera->view_dirty = true_v;
  camera->projection_dirty = true_v;

  vkr_camera_update_orientation(camera);

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(window);
  camera->cached_window_width = window_size.width;
  camera->cached_window_height = window_size.height;

  vkr_camera_system_update(camera);
}

void vkr_camera_system_update(VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->window != NULL, "Camera window is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  VkrWindowPixelSize window_size = vkr_window_get_pixel_size(camera->window);
  if (camera->cached_window_width != window_size.width ||
      camera->cached_window_height != window_size.height) {
    camera->cached_window_width = window_size.width;
    camera->cached_window_height = window_size.height;
    if (camera->type == VKR_CAMERA_TYPE_PERSPECTIVE) {
      camera->projection_dirty = true_v;
    }
  }

  if (camera->view_dirty) {
    camera->view = vkr_camera_calculate_view(camera);
    camera->view_dirty = false_v;
  }

  if (camera->projection_dirty) {
    camera->projection = vkr_camera_calculate_projection(camera);
    camera->projection_dirty = false_v;
  }
}

void vkr_camera_translate(VkrCamera *camera, Vec3 delta) {
  assert_log(camera != NULL, "Camera is NULL");
  camera->position = vec3_add(camera->position, delta);
  camera->view_dirty = true_v;
}

void vkr_camera_rotate(VkrCamera *camera, float32_t yaw_delta,
                       float32_t pitch_delta) {
  assert_log(camera != NULL, "Camera is NULL");

  camera->yaw += yaw_delta;
  camera->pitch = vkr_clamp_f32(camera->pitch + pitch_delta,
                                VKR_MIN_CAMERA_PITCH, VKR_MAX_CAMERA_PITCH);

  vkr_camera_update_orientation(camera);
}

void vkr_camera_zoom(VkrCamera *camera, float32_t zoom_delta) {
  assert_log(camera != NULL, "Camera is NULL");
  camera->zoom = vkr_camera_clamp_zoom(camera->zoom + zoom_delta);
  camera->projection_dirty = true_v;
}

Mat4 vkr_camera_system_get_view_matrix(const VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  assert(camera->view_dirty == false_v && "View matrix requested while dirty");
  return camera->view;
}

Mat4 vkr_camera_system_get_projection_matrix(const VkrCamera *camera) {
  assert_log(camera != NULL, "Camera is NULL");
  assert_log(camera->type != VKR_CAMERA_TYPE_NONE, "Camera type is NONE");

  assert(camera->projection_dirty == false_v &&
         "Projection matrix requested while dirty");
  return camera->projection;
}
