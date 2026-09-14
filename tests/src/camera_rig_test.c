#include "camera_rig_test.h"

#include "gameplay/vkr_camera_rig.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>

static void camera_rig_expect_vector(Vec3 actual, Vec3 expected) {
  assert(fabsf(actual.x - expected.x) < 1e-5f);
  assert(fabsf(actual.y - expected.y) < 1e-5f);
  assert(fabsf(actual.z - expected.z) < 1e-5f);
}

static bool8_t camera_rig_wall_sweep(Vec3 origin, Vec3 displacement,
                                     float32_t radius, void *context,
                                     float32_t *fraction) {
  uint32_t *calls = context;
  (*calls)++;
  camera_rig_expect_vector(origin, vec3_new(0, 1.5f, 0));
  assert(fabsf(radius - 0.25f) < 1e-5f);
  // Independent fixture: infinite wall at z=2, camera sphere on its near side.
  *fraction = (2.0f - radius - origin.z) / displacement.z;
  return true_v;
}

static bool8_t camera_rig_invalid_sweep(Vec3 origin, Vec3 displacement,
                                        float32_t radius, void *context,
                                        float32_t *fraction) {
  (void)origin;
  (void)displacement;
  (void)radius;
  *fraction = *(float32_t *)context;
  return true_v;
}

static bool8_t camera_rig_failed_sweep(Vec3 origin, Vec3 displacement,
                                       float32_t radius, void *context,
                                       float32_t *fraction) {
  (void)origin;
  (void)displacement;
  (void)radius;
  (void)context;
  (void)fraction;
  return false_v;
}

bool32_t run_camera_rig_tests(void) {
  const float32_t half_pi = 1.5707963267948966f;
  VkrCameraRigConfig config = {.eye_height = 1.5f,
                               .distance = 4.0f,
                               .shoulder_offset = 0.5f,
                               .shoulder_height = 0.25f,
                               .sweep_radius = 0.25f,
                               .pitch_limit = 0.5f};
  VkrCameraRig rig = {0};
  assert(vkr_camera_rig_initialize(&rig, &config));
  VkrCameraRigPose pose = {0};
  uint32_t sweep_calls = 0;
  assert(vkr_camera_rig_evaluate(&rig, vec3_new(2, 3, 4), -half_pi, 0,
                                 camera_rig_wall_sweep, &sweep_calls, &pose));
  camera_rig_expect_vector(pose.position, vec3_new(2, 4.5f, 4));
  camera_rig_expect_vector(pose.forward, vec3_new(0, 0, -1));
  camera_rig_expect_vector(pose.up, vec3_new(0, 1, 0));
  assert(sweep_calls == 0);

  assert(vkr_camera_rig_set_mode(&rig, VKR_CAMERA_RIG_THIRD_PERSON, false_v));
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), 0, 0, NULL, NULL, &pose));
  camera_rig_expect_vector(pose.position, vec3_new(-4, 1.5f, 0));
  camera_rig_expect_vector(pose.forward, vec3_new(1, 0, 0));
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), -half_pi, 0, NULL, NULL,
                                 &pose));
  camera_rig_expect_vector(pose.position, vec3_new(0, 1.5f, 4));
  assert(fabsf(vec3_length(vec3_sub(pose.position, vec3_new(0, 1.5f, 0))) -
               4.0f) < 1e-5f);
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), -half_pi, 0,
                                 camera_rig_wall_sweep, &sweep_calls, &pose));
  assert(sweep_calls == 1);
  camera_rig_expect_vector(pose.position, vec3_new(0, 1.5f, 1.75f));

  assert(vkr_camera_rig_set_mode(&rig, VKR_CAMERA_RIG_SHOULDER, false_v));
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), -half_pi, 0, NULL, NULL,
                                 &pose));
  camera_rig_expect_vector(pose.position, vec3_new(0.5f, 1.75f, 4));
  assert(vkr_camera_rig_set_mode(&rig, VKR_CAMERA_RIG_SHOULDER, true_v));
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), -half_pi, 0, NULL, NULL,
                                 &pose));
  camera_rig_expect_vector(pose.position, vec3_new(-0.5f, 1.75f, 4));

  assert(vkr_camera_rig_set_mode(&rig, VKR_CAMERA_RIG_FIRST_PERSON, false_v));
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), 0, 2, NULL, NULL, &pose));
  assert(pose.pitch == 0.5f);
  assert(fabsf(pose.forward.y - 0.47942554f) < 1e-5f);
  assert(vkr_camera_rig_evaluate(&rig, vec3_zero(), 0, -2, NULL, NULL, &pose));
  assert(pose.pitch == -0.5f);
  assert(fabsf(pose.forward.y + 0.47942554f) < 1e-5f);
  assert(fabsf(vec3_dot(pose.forward, pose.up)) < 1e-5f);

  const VkrCameraRigPose saved_pose = pose;
  assert(!vkr_camera_rig_evaluate(&rig, vec3_new(NAN, 0, 0), 0, 0, NULL, NULL,
                                  &pose));
  assert(!vkr_camera_rig_evaluate(&rig, vec3_zero(), INFINITY, 0, NULL, NULL,
                                  &pose));
  assert(
      !vkr_camera_rig_evaluate(&rig, vec3_zero(), 0, NAN, NULL, NULL, &pose));
  assert(MemCompare(&saved_pose, &pose, sizeof(pose)) == 0);
  VkrCameraRig saved_rig = rig;
  config.distance = -1;
  assert(!vkr_camera_rig_initialize(&rig, &config));
  assert(!vkr_camera_rig_set_mode(&rig, (VkrCameraRigMode)99, false_v));
  assert(MemCompare(&saved_rig, &rig, sizeof(rig)) == 0);
  config.distance = 4;
  config.eye_height = FLT_MAX;
  assert(vkr_camera_rig_initialize(&rig, &config));
  assert(!vkr_camera_rig_evaluate(&rig, vec3_new(0, FLT_MAX, 0), 0, 0, NULL,
                                  NULL, &pose));
  assert(MemCompare(&saved_pose, &pose, sizeof(pose)) == 0);

  rig = saved_rig;
  assert(vkr_camera_rig_set_mode(&rig, VKR_CAMERA_RIG_THIRD_PERSON, false_v));
  const float32_t invalid_fractions[] = {NAN, -0.1f, 1.1f};
  for (uint32_t i = 0; i < ArrayCount(invalid_fractions); ++i) {
    float32_t fraction = invalid_fractions[i];
    assert(!vkr_camera_rig_evaluate(
        &rig, vec3_zero(), 0, 0, camera_rig_invalid_sweep, &fraction, &pose));
    assert(MemCompare(&saved_pose, &pose, sizeof(pose)) == 0);
  }
  assert(!vkr_camera_rig_evaluate(&rig, vec3_zero(), 0, 0,
                                  camera_rig_failed_sweep, NULL, &pose));
  assert(MemCompare(&saved_pose, &pose, sizeof(pose)) == 0);
  printf("Camera rig tests passed\n");
  return true_v;
}
