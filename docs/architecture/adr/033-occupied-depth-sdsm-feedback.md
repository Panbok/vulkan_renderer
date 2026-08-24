---
status: proposed
updated: 2026-08-24
authority: adr
---

# ADR-033: Occupied-depth SDSM through completed asynchronous feedback

## Status

**Proposed.** Nothing is implemented. Cascade splits remain fixed by
configuration.

Design source: section 10 of
[shadow-cpu-cost-and-csm-rewrite-spec.md](../../rendering/shadow-cpu-cost-and-csm-rewrite-spec.md).

This is a **quality and distribution** feature. It is not a CPU optimization and
no performance claim may be attached to it. It needs an ADR because it adds a
delayed GPU-to-CPU feedback edge into frontend-owned cascade fitting, which is
today a pure function of camera and configuration.

## Context

Cascade splits come entirely from configuration:
`lambda * logarithmic + (1 - lambda) * uniform`, clamped to
`max_shadow_distance`. At the shipped `VKR_SHADOW_CONFIG_HIGH` values — lambda
0.80, distance 200 — the distribution is heavily front-loaded. Working the
default config through with near 0.1 and a 45 degree vertical field of view,
cascade 3 spans roughly [54, 200]: about 146 units in one cascade.

That distribution is chosen without reference to what the camera can actually
see. A view down a corridor and a view across open ground get identical splits.
Sample-distribution shadow maps close that gap by fitting the working near/far
interval to the depth range of visible geometry.

The renderer already has the raw material: `VBuffer.Opaque` produces final
opaque depth plus a `uint2` visibility ID, and an HZB reduction chain already
runs over that depth.

### Pre-acceptance control result

The required lambda/map-size control experiment ran before this ADR was
accepted. Four local, non-authoritative Release offscreen captures held one
Bistro corridor camera, 16-tap PCF, and the early-out policy fixed while
varying one control at a time.
Lowering lambda from 0.80 to 0.25 strongly changed cascade assignment
(cascade-debug PSNR 15.97 dB) but produced no final-color change above the tiny
variation between identical controls. Raising map size from 2048 to 4096 kept
cascade assignment byte-identical and changed the shadow-factor diagnostic
(39.73 dB), but again showed no visible final-color improvement.

This result retains lambda 0.80 and map size 2048. It does not prove that SDSM
is worth its feedback edge: one view showed no final-color quality gap for SDSM
to close. Acceptance still requires broader far-field captures that demonstrate
a problem fixed splits cannot solve, followed by a measured reduction cost.

### Why this is not a small change

Three hazards make SDSM architectural rather than local.

**Background collapses a naive reduction.** Reducing raw depth as plain min/max
keeps the maximum pinned at the far plane in any view containing sky, which is
most exterior views. The reduction must distinguish shaded geometry from
clear-depth background.

**Delayed depth cannot be linearized with the current projection.** The result
arrives one or more frames late. Linearizing those device depths through the
projection in force when they are *consumed* silently corrupts the range
whenever the camera projection changed in between.

**Nothing may block on it.** The frame path must never wait on a fence,
semaphore, queue, or readback slot to obtain the sample. Missing feedback has to
be an ordinary, expected state rather than an error.

## Decision

### Occupied-depth reduction

Build the reduction from final opaque depth plus the visibility ID, not from raw
depth:

```c
typedef struct VkrSdsmReduceValue {
  float32_t min_device_z;
  float32_t max_device_z;
  uint32_t occupied_count;
} VkrSdsmReduceValue;
```

The base pass writes `occupied_count = 0` for background and 1 for a valid
visibility ID. A reduction step combines min and max **only** from children with
nonzero count, and sums the count with saturation. An empty tail is invalid, not
zero-valued.

Keep this chain separate from the existing R32 max HZB for the first
implementation. Merge them only if a GPU profile proves the wider shared
representation is cheaper; do not widen the HZB to RG32 merely to share it.

### Completed feedback record

The selected implementation writes one fixed-size result per frame slot:

```c
typedef struct VkrShadowDepthRangeSample {
  float32_t min_device_z;
  float32_t max_device_z;
  uint32_t occupied_count;
  uint32_t projection_convention;
  Vec4 source_depth_linearize;
  float32_t source_near;
  float32_t source_far;
  uint64_t source_frame_index;
  uint64_t source_projection_generation;
  uint64_t source_scene_generation;
  uint64_t submit_value;
  bool8_t valid;
} VkrShadowDepthRangeSample;
```

The source projection parameters travel **with** the result. The backend exposes
only the newest slot whose submit value is complete and whose stored frame
identity still matches the slot record. No frame path waits for anything.

### Frontend consumption

`vkr_shadow_system_update()` accepts an optional completed sample and rejects it
after target recreation, projection or scene generation change, non-finite or
unordered depths, zero occupied pixels, or a configured source-frame lag.
Camera pose change does **not** invalidate a sample: delayed visible depth is
precisely the input SDSM is designed to consume.

Accepted samples are linearized through the stored source coefficients, clamped
to the fixed shadow range, and temporally smoothed in linear view distance.

### Split and fallback policy

SDSM adjusts only the working near/far interval used to distribute the interior
splits. **The final cascade still reaches the configured fixed shadow
distance**, so a visible receiver never loses directional shadow merely because
it was absent from a delayed depth sample. Interval contraction is clamped per
frame and runs through the existing Phase 1 fit hysteresis.

Use the newest valid cached sample only up to `sdsm_max_source_lag_frames`.
During warmup or after invalidation, use fixed splits. Publish `warmup`,
`active`, `cached`, `empty`, `stale`, and `fixed_fallback` status, plus source
lag, occupied pixels, and the resolved linear range, in frame metrics and shadow
debug data.

## Consequences

- Cascade fitting stops being a pure function of camera and configuration. It
  gains an input whose availability depends on GPU completion, so the same
  camera can produce different splits depending on history. Exact-capture cases
  must pin the fallback path or tolerate that.
- A new reduction chain costs bandwidth every frame, whether or not the sample
  is consumed.
- Readback slots and their completion tracking join the frame-slot lifetime
  rules already used for command and upload storage.
- The status vocabulary above becomes part of the metrics surface, and every
  fallback must be observable — a silent fixed-split fallback would make an
  SDSM regression invisible.
- Unit tests own device-depth linearization, empty/background rejection, stale
  sample rejection, projection changes, smoothing clamps, and fixed-last-split
  coverage. Validation owns buffer barriers and completion. Exact captures own
  split stability.
- Interaction with [ADR-032](032-two-phase-confirmed-visibility.md): if
  two-phase visibility ships, "final opaque depth" means post-confirmation
  depth, and the reduction must be scheduled after step 7 of that topology.

## Alternatives Considered

**Plain min/max depth reduction.** Simplest, and wrong in any view containing
sky: clear depth pins the maximum at the far plane. Rejected.

**Share the existing R32 max HZB, widened to RG32.** Attractive because the
chain already exists, but it doubles that chain's bandwidth for every consumer
including the ones that only need max, and it still needs an occupancy signal to
avoid the background problem. Deferred: measure the separate chain first and
merge only on evidence.

**Synchronous readback of the depth range.** Removes all staleness handling and
the source-projection metadata. Rejected: it blocks the frame on GPU completion,
which is a defect in this renderer regardless of the quality benefit.

**Fit splits from scene bounds on the CPU.** Cheap and needs no feedback edge,
but scene bounds describe what exists rather than what is visible, which is the
quantity SDSM exists to track. It also duplicates the caster-depth input that
already feeds the cascade Z interval.

**Do nothing and tune `cascade_split_lambda` instead.** The cheapest option and
a genuine competitor. Lowering lambda toward 0.25 roughly doubles far-cascade
texel density for a one-line configuration change. It is already scoped as a
separate named quality experiment. The completed single-view control did not
show a final-color gain from lambda 0.25, so it neither selects the cheaper
change nor supplies the missing evidence needed to accept this feedback edge.

## Revisit When

- A controlled lambda and map-size sweep closes the far-field quality gap
  without feedback. Close this ADR.
- The occupied-depth reduction's measured GPU cost exceeds the quality benefit
  in matched profiles.
- Split-stability captures show visible crawling that contraction clamping and
  hysteresis cannot remove.
- A shared min/max representation is proven cheaper than the separate chain, at
  which point the reduction source is revisited but the feedback contract here
  stands.
- The frame topology changes such that final opaque depth is no longer available
  before cascade fitting for the next frame.
