---
status: implemented
updated: 2026-10-09
authority: adr
---

# ADR-097: Look volumes

## Status

Implemented. A `look_volume` component overrides part of the scene's look
inside a box: exposure, grading, bloom, height fog and the sky light. The
CPU blends the volumes at the camera once a frame, so they add no pass and
apply on both pipelines.

## Context

Every environment setting is a scene singleton
([ADR-076](076-project-object-model.md)). Before this decision, a scene had
one look everywhere. The only spatial override was `fog_density_box`, which
affects volumetric fog on the desktop pipeline only. Interiors, tunnels and
districts needed their own exposure and grading.
[The artist toolkit proposal](../proposals/artist-toolkit.md) (Part 7)
specifies look volumes as a CPU blend that feeds the globals `post_process`
sets.

## Decision

1. **Component.** `look_volume` (`SceneLookVolume`) is a live world
   component. The entity's world transform places a box whose inside is
   [-0.5, 0.5] on each axis, as for decals
   ([ADR-092](092-projected-decals.md)). It has:
   - `enabled`, an integer `priority` and a `blend_distance` in metres;
   - a flag and a value for each overridable value:
     - exposure compensation;
     - the metering range, minimum and maximum EV;
     - white balance temperature and tint;
     - contrast;
     - saturation;
     - bloom intensity;
     - height fog colour;
     - height fog density;
     - sky light intensity.
2. **Weight.** [vkr_scene_look.c](../../runtime/src/renderer/systems/vkr_scene_look.c)
   weighs a volume at the camera:
   - 1 inside the box;
   - outside, 1 minus the distance to the box's nearest point over the blend
     distance, measured in world metres whatever the box's scale and
     rotation;
   - 0 at or past the blend distance, without a blend distance, for a
     disabled volume or a box with a zero-length axis.
3. **Blend.** Volumes apply in ascending priority. Equal priorities apply in
   the order the scene gathered them. Each overridden value moves from the
   current value toward the volume's by the weight, so a higher priority
   wins where both apply fully. Values no volume overrides keep the scene's.
4. **Inputs and outputs.** The runtime starts from the frame's resolved
   look and applies the blend to:
   - the `post_process` values or the runtime's base globals;
   - the resolved height fog;
   - the sky light intensity the frame lighting carries;
   - the renderer's metering range.

   Results go back to the same places `post_process` and the fog feed
   ([vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c)).
5. **Metering range.** Frame input version 53 adds `exposure_min_ev` and
   `exposure_max_ev` to `VkrFrameGlobals`. Equal values keep the renderer's
   `VkrExposureMeteringConfig` range. Otherwise `vkr_exposure_gpu_metering`
   clamps the metered EV to them, on both pipelines. Validation rejects
   non-finite values and a minimum above the maximum.
6. **Gathering.** World resolution records the directory indices of the
   scene's own look volumes, then the root World's, at most 64
   (`VKR_SCENE_LOOK_VOLUME_MAX`). Each frame reads their components and
   transforms live, skipping hidden or disabled ones. Volumes of additive
   scenes do not apply.
7. **Editor and agents.**
   - The Lighting palette and the Environment create menu add **Look
     volume**, an 8 x 4 x 8 m box standing on the snap point.
   - Details edits it as any component.
   - `look.volume` creates one between two world corners, with `rotation`,
     `priority`, `blend_distance` and a `look` object. Each key of `look`
     sets its value and turns its override on: `exposure_compensation_ev`,
     `metering` [min, max], `white_balance` [temperature, tint],
     `contrast`, `saturation`, `bloom_intensity`, `fog_color`,
     `fog_density` and `sky_light_intensity`. An unknown key or an empty
     `look` fails the operation.

## Consequences

- A scene without look volumes does the same work as before, plus one
  empty gather.
- Volumetric fog keeps its own density boxes; look volumes change only the
  analytic height fog.
- A selected volume draws its box and its blend box in the viewport
  ([ADR-100](100-lighting-tools.md)).
- Each frame copies up to 64 component values and inverts their boxes; the
  timing below measures eight.

## Evidence

- `./build_release/tests/vulkan_renderer_tester --suite
  run_look_volume_tests` (2026-10-09):
  - `test_look_volume_weight_at_boundaries`: a 4 x 2 x 2 m box turned 90
    degrees weighs 1 inside. One metre past a long face, a short face or a
    corner, it weighs 0.5 at a 2 m blend; 0 at the blend distance. A hard
    edge, a disabled volume and a flat box weigh as specified.
  - `test_look_volume_blend_priorities`: a higher priority wins regardless
    of array order, and equal priorities keep their order. A half weight
    moves exposure, the metering range, fog colour and sky light half way.
    Values no volume overrides, and a volume past its blend, change
    nothing.
- All 99 CPU suites pass. `run_local_socket_tests` was skipped; it fails the
  same way on `main`.
- On a headless Bistro editor (Release, Metal), `look.volume` created a
  10 x 6 x 10 m volume around a street camera: exposure -2 EV, saturation
  0.2, red fog, blend 4 m (`.scratch/artist/phase5a_check.py` in the
  working tree). Mean sRGB of the central image:

  | Camera | R | G | B |
  |---|---|---|---|
  | Before the volume | 108.8 | 117.0 | 118.4 |
  | Inside | 62.0 | 63.1 | 64.0 |
  | 2 m outside (half weight) | 87.3 | 91.8 | 94.0 |
  | 7 m outside | 111.7 | 120.3 | 123.1 |

  An empty `look` was refused.
- **Timing.** On commit `ba1f8a10`, `vkr_harness profile --profile
  tools/profiles/performance-windowed-gpu-submission.json` alternated two
  cases. `tools/cases/performance/bistro_look_volumes_orbit_1440.case.json`
  is the Bistro material orbit with eight 20 x 16 x 20 m volumes along the
  camera path (`assets/scenes/fixtures/bistro_look_volumes.scene.json`).
  Their overrides equal the defaults, so the image is unchanged and the
  difference is the per-frame gather and blend.
  - Configuration: Release, AppleClang 21, Apple M1 Pro, Metal 4,
    2560×1440 pixels, windowed hidden, immediate present; five children of
    120 warmup and 300 measured frames each run.
  - All four runs passed and are authoritative, with equal environment and
    policy fingerprints. The workloads differ by the scene, as intended.
  - `gpu.submission` mean, with volumes: 19.757 and 19.805 ms. Without:
    19.828 and 19.763 ms. Equal within the spread.
  - `cpu.update` mean, with volumes: 18.39 and 18.43 ms. Without: 18.45
    and 18.48 ms. The frame waits on the GPU, so the blend does not show.
  - World draw calls are equal (238.6).
  - Reports: with volumes
    `sha256:00187650f8721250e23ded3f8fd349430d6bfa6a8d145f6ee2fa1e05f60e5921`
    and `sha256:fbac90d7057bb51b5d2b314c8646fa8edc39d153feacf2dfe3536af7885f70a8`;
    without
    `sha256:07f87713b34c96a95d07f3580d62b665d6da5ebb8eb77c8696a35bc46b977068`
    and `sha256:aa83425035b490bc76f63eb3615498cecfe77022c6d66ab0e608dc7c9b1bf53d`.
- Unavailable: Vulkan native execution of the metering range. The desktop
  pipeline lowers it through the same `vkr_exposure_gpu_metering`.

## Alternatives considered

- **A GPU pass per volume.** Rejected (proposal, Part 7): the overridden
  values are frame constants, so a CPU blend at the camera costs nothing on
  the GPU.
- **Blending by volume order alone.** Rejected: artists nest a room inside a
  district and need the room to win regardless of which was made first.

## Revisit when

- Volumes need to override more than these values, such as volumetric fog
  or shadows.
- Additive scenes need their own look volumes.
- The viewport draws the boxes of volumes and other box components.
