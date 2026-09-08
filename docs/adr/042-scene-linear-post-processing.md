---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-042: Scene-linear exposure, bloom and ambient visibility

## Status

Accepted.

## Context

Post effects must preserve temporal radiance, expose independent bypasses and
modify the lighting terms they physically approximate.

## Decision

Keep temporal history scene-linear. Meter the post-temporal HDR source with a
256-bin log-luminance histogram and percentile exposure resolve. Renderer-owned
frame delta/discontinuities control completion-safe EV history. Each native
renderer advances an exposure clock only when an exposure output is submitted
and stores that clock with the output. Adaptation uses elapsed exposure-clock
time since the selected completed history, bounded by the shared hitch limit;
it does not apply only one frame's delta to an older state. Invalid history snaps
to target. Defaults lower exposure at 8 EV/s, raise it at 1 EV/s, and clamp the
target to [-8,+4] EV. The rate names describe displayed-image brightness.
Tonemap consumes GPU state
without synchronous CPU readback; delayed completed samples expose diagnostics.
Manual exposure remains an explicit alternative.
Histogram dispatches use complete 16x16 threadgroups on both backends: all 256
lanes initialize and merge bins, while edge lanes omit out-of-extent source reads.

Bloom prefilters scene-linear HDR with threshold/soft knee into a bounded
half-resolution chain, downsamples, accumulates deepest-first, and combines into
full-resolution HDR before exposure multiplication/tonemap. Separate graph
resources preserve read/write dependencies. Shared arithmetic handles non-finite
and extreme input and pins the knee/Karis behavior. Bloom can bypass independently.
Combine intensity is multiplied once by configured maximum mip count divided
by the actual contributing count. This preserves the usual full six-level look
and its constant-field gain on shorter chains. Fewer than two levels disables
bloom. Normalization changes amplitude, not the spatial spread of a shorter
chain; firefly saturation and boundaries remain nonlinear limits of the DC rule.

GTAO uses current-frame depth and normals before deferred lighting. A dedicated
positive-view-depth R16 pyramid feeds full-resolution three-slice/three-step
horizon evaluation and an edge-aware 3x3 denoise. Raw and denoised outputs are
RGBA8: RGB encodes a world-space bent normal, and alpha holds scalar visibility.
The existing R8 edge image remains separate. The graph owns these transient
images per in-flight image; replacing two R8 outputs adds 5.27 MiB at 1280x720,
or 15.82 MiB across three image sets. Pass scheduling and sample counts stay fixed.
Depth-pyramid horizon samples use nearest texel and nearest mip filtering.
Slice directions use the signed view-space pixel scale, including projection Y,
and the positive/negative horizon bounds share the integration sign convention.
The denoiser averages decoded directions and visibility before normalizing and
repacking. Lighting blends the bent direction toward the shading normal as
visibility approaches one. Full visibility preserves the original lighting,
including the disabled fallback, for every reflection direction.

Global/probe diffuse uses the bent normal and albedo-aware multi-bounce AO fit,
multiplied by independent material AO. This fit compensates local missing energy;
it does not compute scene-wide light transport or color bleeding. Baked diffuse
volumes retain their shading-normal SH lookup and scalar AO because their bake
already includes multiple bounces. Indirect specular, including SSR replacement,
uses a visibility cone derived from alpha and an approximate spherical-cap
intersection with the rough reflection lobe. The practical near-mirror fade
limits this approximation. Environment and SSR receiver weights apply the same
cone factor once; authored IBL gain remains specific to environment lighting.
Direct lighting keeps its shadow visibility. GTAO is not reused HZB history or
general wall visibility. Raw/denoised capture schema version 2 records the RGBA
directional tuple as data, without a color-transfer interpretation.

Automatic exposure, bloom and GTAO are enabled by production initialization.
Authored graph conditions and packet globals retain isolated bypasses and direct
intermediate capture channels. Final output follows ADR-043.

## Consequences

Effects have observable inputs and independent controls. Pixel equivalence,
quality acceptance and GPU cost still require matched cases on each backend;
enabling effects is a workload change.

The Release Metal corner fixture verifies that bent normals point away from a
nearby wall and that diffuse/specular lighting follows the captured tuple
(maximum HDR error 0.000960297 across 34 samples). Disabling GTAO preserves the
previous fixture's HDR bytes. A mirror fixture exercises non-neutral SSR cone
attenuation, with maximum HDR error 0.002351667 across 15 interior samples.
A directional-SH volume fixture verifies scalar AO across 201,629 covered
pixels (maximum HDR error 0.0002432). Bistro at 1280x720 completes with finite HDR output. Focused Metal API validation
and production Vulkan SPIR-V validation pass; native Vulkan execution and
bilateral output comparison remain unavailable. These checks establish local
behavior, not a frame-time claim.

## Alternatives considered

Pre-exposing temporal history couples adaptation to reconstruction. Scalar AO on all
lighting darkens direct lighting and cannot model directional specular occlusion. Reusing stale HZB as current
GTAO depth changes the input contract.

## Revisit when

Accepted image-quality fixtures require a different metering, bloom or AO model,
or matched Release cost justifies changing resolution/quality.

## Implementation

[`vkr_exposure.c`](../../renderer/src/vkr_exposure.c),
[`vkr_bloom.c`](../../renderer/src/vkr_bloom.c),
[`vkr_gtao.c`](../../renderer/src/vkr_gtao.c),
[`shared/`](../../renderer/src/shaders/shared), and
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
