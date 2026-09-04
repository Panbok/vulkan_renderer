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
selection uses compatible completed images independently of command slots.
Stable draw/primitive identity and depth constrain history acceptance; moving-
camera bilinear reconstruction masks the history footprint with those tests.

Opaque and rigid transmission/blend paths publish current-to-previous motion.
Stationary transparency can accumulate, while moving composition and authored
material reactivity limit history. Extent, scene, camera and source discontinuities
reset accumulation. Invalid history uses a one-sample passthrough.

Exposure is applied after temporal resolve, so changing exposure does not change
stored history radiance. Output-space FXAA can run in the existing final draw.
Deferred lighting applies bounded normal-footprint roughness filtering before
temporal accumulation. The portable resolve works at the internal Scene extent;
ADR-040 selects a separate MetalFX consumer when enabled.

Deformation, procedural/particle motion and broader dynamic material signals
are not complete production motion contracts.

## Consequences

The portable consumer has shared semantics, but image quality depends on identity,
reactivity and motion coverage. Source agreement and fixed-camera captures do
not establish moving-camera or animation acceptance.

## Alternatives considered

FXAA alone cannot reconstruct temporal detail. Blind history blending produces
ghosting; dropping identity/depth rejection for smoother output weakens correctness.
MSAA remains a separate unimplemented proposal.

## Revisit when

New animation/motion producers or accepted moving-image fixtures expose missing
signals or unacceptable rejection/ghosting.

## Implementation

[`vkr_temporal.c`](../../lib/src/renderer/vkr_temporal.c),
[`vkr_vulkan_deferred.c`](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c),
[`gpu_draws.metal`](../../lib/src/renderer/shaders/metal/msl/world/gpu_draws.metal), and
[`deferred.slang`](../../lib/src/renderer/shaders/vulkan/slang/world/deferred.slang).
