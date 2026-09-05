---
status: partial
updated: 2026-09-05
authority: adr
---

# ADR-037: Portable scene-linear temporal antialiasing

## Status

Accepted (partial): rigid-motion production path; deformation and broader motion acceptance remain open.

## Context

Visibility raster alone does not suppress subpixel geometry and specular shimmer.
Temporal reconstruction needs stable identity, motion and completion-safe history,
including transparent composition.

## Decision

Metal and Vulkan share a same-resolution scene-linear temporal resolve with
renderer-owned Halton jitter, previous transforms and reset policy. History
selection uses the immediately preceding submitted frame, independently of
command slots. Vulkan orders same-queue history writes before compute/fragment
readers; Metal waits on the producer's GPU completion event before committing
its consumer. CPU completion still governs output reuse. If the fixed history
ring is busy, output acquisition waits for its oldest combined last use rather
than failing the frame or overwriting live storage. Arbitrary completed-frame
selection can split the eight jitter phases into independent accumulation chains.
Reset publication invalidates older history entries, so an in-flight reset frame
cannot leave pre-reset producers eligible for the next frame.
Projection jitter translates clip xy in proportion to clip w, so perspective
and orthographic views both receive the requested depth-independent pixel shift.

History identity is the temporal instance index/generation plus a stable mesh-local
submesh token, not a draw-local primitive number or compacted visible-row index.
Native candidate preparation and ordinary-blend emission encode `submesh_index + 1`
above bit 0 of the temporal flags. Ordinary blend requires the token to fit bits
1–17 so its overlay encoding preserves all 17 token bits; opaque history retains
the full token. Bit 0 elects the transform-history writer only. Readers of a valid
previous transform do not need the writer bit. Zero tokens, or tokens that do not
fit the blend overlay, disable the corresponding surface accumulation; external
direct instance producers must preserve this identity contract.

Portable resolve reconstructs current color onto the unjittered pixel grid with
a positive tent at `pixel + current_jitter`. Previous color lives on that same
grid and uses unjittered motion. Identity and depth remain samples of the raw
raster grid; they are not labels for the complete accumulated color. Each
canonical history color tap validates the raw footprint that contributed to its
reconstruction, offset by the previous jitter. Color coordinates clamp before
deriving that footprint, including at image borders.

Raw history samples normally require matching identity, submesh token and depth.
A nonreactive opaque/background sample may also retain different subpixel
coverage when that exact surface and expected previous depth are supported in
the current 3x3 neighborhood. Supporting motion must agree within half a pixel,
and its projected position must remain within the local footprint. This applies
to camera motion as well as stationary views. Blend/transmission composition
cannot use this fallback; zero non-sky tokens remain invalid. Weighted raw
support supplies confidence for both bilinear reconstruction and final history
retention, so a small accepted contributor cannot authorize a full-strength
mixed color. The inner support cache uses at most nine metadata samples.

A fully matching raw footprint permits Catmull-Rom history reconstruction using
nine bilinear color samples and at most 25 unique metadata samples. Only the
positive inner weights combine into hardware interpolation; negative outer
lobes stay separate. Cubic taps are never partially renormalized and remain
disabled where the canonical 4x4 footprint crosses an image border. A 3x3
current-color range clamps either reconstruction. Ordinary blend has no matching
written depth and retains identity-only bilinear reconstruction. Motion and
identity do not establish stationarity of animated alpha, materials or lighting.

For consecutive unchanged scenes, a checked accumulation path retains coverage
even when thin geometry disappears from the entire current neighborhood. One
allocation-free CPU scan hashes explicit packet semantics into two 64-bit words:
unjittered camera, geometry and transforms, materials, lighting, shadows and GTAO
controls. Native color history also records resource-radiance, candidate-publication
and graph revisions. Both eligible records must match; a camera-only match is
insufficient. Text and pending publication/upload/IBL writer frames are excluded.
The signature is a probabilistic content check, not collision-free equality.
It excludes jitter/noise phase and downstream exposure, bloom and UI. Disabled
portable TAA and MetalFX skip the scan.

That path reads canonical history at the same pixel without coverage rejection,
neighborhood clipping or contrast-derived glass reactivity. Authored material
reactivity and basic validity/identity exclusions still apply. The unused second
depth-history channel stores sample age: ordinary resolve writes zero, excluding
moving/rejected history from the next stationary integral. Checked accumulation
increments it up to 128 and uses `(age - 1) / age` retention. After 128 unchanged
samples, nonreactive pixels copy their completed history exactly. A capped
exponential average would continue oscillating with subpixel coverage indefinitely.
The finite sample count spans two complete GTAO noise cycles and sixteen raster
jitter cycles; it takes about 2.1 seconds at 60 Hz. Authored reactive pixels keep
updating. This convergence rule adds no image storage. Metadata
depth and identity remain raw raster samples. Camera, object, light or resource
changes return to the normal motion/rejection path, including moving objects
behind otherwise stationary glass. Native scene records publish only after a
successful submission and preserve the existing GPU lifetime rules.

Opaque and rigid transmission/blend paths publish current-to-previous motion.
Stationary transparency can accumulate, while moving composition and authored
material reactivity limit history, including with a stationary camera. The
G-buffer producer writes translation-free sky rotation motion for both portable
TAA and MetalFX. Native clip conventions remain backend-owned; sky validity uses
background depth one rather than a finite far-plane point's projected depth.
Extent, scene, camera and source discontinuities
reset accumulation. Invalid history uses a one-sample passthrough.

Exposure is applied after temporal resolve, so changing exposure does not change
stored history radiance. Output-space FXAA can run in the existing final draw.
Deferred lighting applies bounded normal-footprint roughness filtering before
temporal accumulation. The portable resolve works at the internal Scene extent;
ADR-040 selects a separate MetalFX consumer when enabled.

Deformation, procedural/particle motion and broader dynamic material signals
are not complete production motion contracts.

## Consequences

Cubic reconstruction reduces blur from repeated bilinear history resampling.
Canonical current reconstruction and motion-aware support add filtering and
metadata cost. Checked static-scene accumulation adds one CPU content scan and
small private records, while avoiding the normal history-filter work for eligible
pixels. In changing scenes, a feature absent from the entire current support
neighborhood can still lose coverage.
Motion remains a local approximation read at the raw center sample, so strong
depth motion can differ from the flow at the canonical pixel center.

The portable consumer has shared semantics, but image quality depends on identity,
reactivity and motion coverage. Source agreement and fixed-camera captures do
not establish moving-camera or animation acceptance.

## Alternatives considered

FXAA alone cannot reconstruct temporal detail. Blind history blending produces
ghosting; a stationary camera does not establish stationary geometry. Exact
triangle matching rejects coverage that a previous raster sample missed, while
unrestricted history acceptance loses disocclusion boundaries. The accepted
raw-footprint coverage support keeps surface/depth validation and requires
moving-image acceptance for its coverage/ghosting tradeoff.
MSAA remains a separate unimplemented proposal.

## Revisit when

New animation/motion producers or accepted moving-image fixtures expose missing
signals or unacceptable rejection/ghosting.

## Implementation

[`vkr_temporal.c`](../../lib/src/renderer/vkr_temporal.c),
[`vkr_vulkan_deferred.c`](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c),
[`gpu_draws.metal`](../../lib/src/renderer/shaders/metal/msl/world/gpu_draws.metal), and
[`deferred.slang`](../../lib/src/renderer/shaders/vulkan/slang/world/deferred.slang).
