---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-059: Bounded froxel volumetric fog

## Status

Accepted. The portable graph, scene authoring, and both native implementations
are integrated. Metal checks pass; native Vulkan execution and bilateral image
comparison remain unavailable on this host.

## Context

Analytic fog cannot show shadowed light shafts or local density changes. A
camera-aligned volume must bound both shadow sampling and completion-safe history
storage, and preserve the ordered transparency policy in
[ADR-057](057-analytic-height-fog.md).

## Decision

Use 16-pixel XY cells and 64 logarithmic depth slices, giving 80×45×64 at a
1280×720 internal extent. Depth integration begins at the camera near plane;
samples beyond the authored maximum retain the terminal fog value. A scene authors one height medium and up to 16 density
boxes. Boxes add nonnegative density multipliers; color and phase remain global.
The initial phase is isotropic. Illumination comes from the directional sun and
at most two local lights that have complete shadow-view groups. Rank eligible
lights by their conservative illumination over the fog-frustum bounds, breaking
ties by source identity. Each froxel uses one shadow comparison per source.

The graph owns two RGBA16F 3D image families. Completion-gated history
instances hold local scattering RGB and extinction; one instance per frame slot
holds the current camera's integrated inscatter RGB and transmittance. History
uses the existing frame-slot count plus two. The approved three-slot ceiling
is five histories plus three integrated images: 14,745,600 bytes (14.063 MiB)
before native alignment. The current renderer has two frame slots and allocates
four plus two images, totaling 11,059,200 bytes (10.547 MiB); present-target image
count does not change the frame-slot count. Disabled
frames declare neither family; the graph may retain previously allocated capacity
until resize or teardown. History stores local coefficients because an integrated
path from an old camera cannot be reprojected to the current camera.

Injection selects compatible completed history, reprojects its local values and
rejects changed medium, projection, grid, lighting or shadow content. Integration
then traverses each current-camera column front to back. Opaque fog resolves
after SSR and before the opaque transmission pyramid, replacing analytic fog
while enabled. Transparent local radiance uses `T * local + S * (1 - W)` over
the already integrated feedback term `W * background`. Refraction retains the
straight camera-segment approximation.

The public frame carries scene settings; preparation validates and packs one
928-byte parameter record per frame slot. Existing graph allocation, history
completion and resource retirement own all device storage. Three-dimensional
images use one array layer; depth is a separate descriptor dimension.

## Consequences

Fog adds bounded per-froxel lighting, integration and temporal bandwidth.
Unselected local lights retain surface lighting but do not illuminate the volume.
Low resolution limits thin shafts and density boundaries. Isotropic scattering
does not model forward-scattering phase peaks.

## Alternatives considered

The user selected this budget over a coarser 64×36×48 volume. Reusing final TAA
history cannot establish validity for participating media. Reprojecting integrated
fog would retain a previous camera's light path.

## Revisit when

Measured frame cost, moving-light trails, phase anisotropy or refracted volume
transport requires a different quality or storage policy.

## Implementation

[Public settings and packed contract](../../renderer/src/vkr_froxel_fog.h),
[cold frame preparation](../../renderer/src/vkr_froxel_fog.c), and
[authored graph](../../assets/render_graphs/main.rendergraph.json).


## Verification

Release and editor wrappers compile the production shaders and host contracts.
Native Metal reflection and API validation pass. The lifecycle fixture checks
completed history selection, camera reprojection, medium/projection invalidation,
disable/re-enable without image churn, and resize with subsequent history reuse.
A separate native check confirms 13 valid shadow views, two chosen fog sources,
and history rejection/recovery after selected-light and caster changes.

Independent HDR checks cover homogeneous scattering, pure absorption, terminal
depth clamping, density-box interiors, and ordered glass/BLEND composition.
The largest homogeneous error is 0.001638; pure absorption stays below 0.000091.
Box interiors stay below 0.005406, excluding the one-cell trilinear transition
around the authored discontinuity. Clear glass and BLEND errors are below
0.001848 and 0.000423. Fog-disabled HDR remains byte-identical to the reference.

The local-light fixture retains identical sky HDR over 35,076 pixels when an
eligible dim third light is added. Removing the caster brightens 19,530 sky
pixels by more than 0.01 luminance, proving local shadows affect scattering.
The 1280×720 Bistro capture contains 921,600 finite HDR pixels. Invalid density
and a seventeenth box are rejected at scene load.

Actual Vulkan SPIR-V validation/reflection and eight affected host translation
units pass compilation checks. Native Vulkan commands and pixels remain unverified;
the shader contract stays **UNALIGNED** under
[ADR-044](044-shader-cross-backend-contract.md). Exact commands, report digests
and retained payloads are indexed in the
[local evidence ledger](../../assets/verification/renderer-features/froxel-evidence-digests.txt).

The corrected graph accounting reports exactly 11,059,200 added image bytes
for six 80×45×64 volumes in the current two-slot renderer.

A local Apple M1 Pro Release observation at 1280×720, three offscreen target
images and two frame slots used 24 warmup and 16 measured frames with GTAO, SSR
and TAA enabled. Mean fog GPU costs were 0.975151 ms injection, 0.043292 ms
integration and 0.084810 ms apply. Each had 16 valid samples. These dirty-tree,
single-process observations are non-authoritative and do not establish a speed
claim or a base-M1 frame budget. Commands use `vkr_harness profile`,
`tools/cases/local/froxel_bistro_{off,on}_local.case.json`, and
`tools/profiles/local-offscreen-gpu-single.json`; the
[cost record](../../assets/verification/renderer-features/froxel-cost-numeric.txt) retains exact report identities.
