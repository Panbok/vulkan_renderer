#pragma once

#include "core/input.h"
#include "core/logger.h"
#include "math/mat.h"
#include "math/math.h"
#include "math/vec.h"
#include "platform/window.h"

#define MAX_MOUSE_DELTA 100.0f

typedef enum CameraType {
  CAMERA_TYPE_NONE,
  CAMERA_TYPE_PERSPECTIVE,
  CAMERA_TYPE_ORTHOGRAPHIC,
} CameraType;

typedef struct Camera {
  InputState *input_state;
  Window *window;
  float32_t target_frame_rate;

  CameraType type;

  Vec3 position;
  Vec3 forward;
  Vec3 up;
  Vec3 right;
  Vec3 world_up;

  float32_t yaw;
  float32_t pitch;

  float32_t speed;
  float32_t sensitivity;

  float32_t near_clip;
  float32_t far_clip;

  // Perspective
  float32_t zoom;
  float32_t previous_wheel_delta;

  // Orthographic
  float32_t left_clip;
  float32_t right_clip;
  float32_t bottom_clip;
  float32_t top_clip;
} Camera;

void camera_perspective_create(Camera *camera, InputState *input_state,
                               Window *window, float32_t target_frame_rate,
                               float32_t fov, float32_t near_clip,
                               float32_t far_clip);
void camera_orthographic_create(Camera *camera, InputState *input_state,
                                Window *window, float32_t target_frame_rate,
                                float32_t left, float32_t right,
                                float32_t bottom, float32_t top,
                                float32_t near_clip, float32_t far_clip);

void camera_update(Camera *camera, float32_t delta_time);

Mat4 camera_get_view_matrix(const Camera *camera);

Mat4 camera_get_projection_matrix(const Camera *camera);

void camera_destroy(Camera *camera);