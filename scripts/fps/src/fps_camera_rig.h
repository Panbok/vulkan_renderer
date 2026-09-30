#pragma once

#include "math/vec.h"

typedef enum FpsCameraRigMode {
  FPS_CAMERA_RIG_FIRST_PERSON,
  FPS_CAMERA_RIG_THIRD_PERSON,
  FPS_CAMERA_RIG_SHOULDER,
} FpsCameraRigMode;

typedef struct FpsCameraRigConfig {
  float32_t eye_height;
  float32_t distance;
  float32_t shoulder_offset;
  float32_t shoulder_height;
  float32_t sweep_radius;
  float32_t pitch_limit;
} FpsCameraRigConfig;

typedef struct FpsCameraRig {
  FpsCameraRigConfig config;
  FpsCameraRigMode mode;
  bool8_t shoulder_left;
} FpsCameraRig;

typedef struct FpsCameraRigPose {
  Vec3 position;
  Vec3 forward;
  Vec3 up;
  float32_t pitch;
} FpsCameraRigPose;

/* Return success and a finite fraction in [0,1]; 1 means unobstructed and 0
 * includes initial overlap. Sweep the supplied sphere from origin along the
 * whole displacement. Filtering the followed entity/attachments belongs to the
 * caller. No pointers may escape this call; the callback must not mutate rig
 * or output. A false return leaves the output pose unchanged. */
typedef bool8_t (*FpsCameraRigSweep)(Vec3 origin, Vec3 displacement,
                                     float32_t radius, void *context,
                                     float32_t *fraction);

/* Caller owns these values, with no allocation or retained pointers. Distances
 * and eye height are nonnegative; shoulder height may be signed. Radius is
 * positive; pitch_limit is radians, strictly between zero and pi/2. Invalid
 * initialization preserves rig. First-person mode is selected initially. */
bool8_t fps_camera_rig_initialize(FpsCameraRig *rig,
                                  const FpsCameraRigConfig *config);

/* Immediate mode/shoulder selection. Transition smoothing and camera history
 * reset on a switch/teleport belong to the presentation owner. */
bool8_t fps_camera_rig_set_mode(FpsCameraRig *rig, FpsCameraRigMode mode,
                                bool8_t shoulder_left);

/* Supply the interpolated foot position and latest render look separately from
 * authoritative simulation aim. Radians use the existing camera convention:
 * yaw zero faces +X, yaw -pi/2 faces -Z, positive pitch looks up; world up is
 * +Y. First-person uses the eye directly. Other modes sweep eye-to-camera when
 * a callback is supplied and retract immediately on obstruction. Forward
 * remains the requested look direction; shoulder aim convergence is caller
 * policy. Finite inputs and arithmetic are validated before publishing the
 * pose. */
bool8_t fps_camera_rig_evaluate(const FpsCameraRig *rig, Vec3 target_foot,
                                float32_t yaw, float32_t pitch,
                                FpsCameraRigSweep sweep, void *context,
                                FpsCameraRigPose *pose);
