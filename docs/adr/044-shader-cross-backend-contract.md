---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-044: Portable shader semantics with native ABI validation

## Status

Accepted.

## Context

Sharing C packets or shader source does not prove native binaries use the same
bindings, layouts, dispatches or numerical meaning. Resource references differ
between Metal and Vulkan.

## Decision

Keep portable arithmetic shared where both shader languages can consume it.
Keep native bindings, address spaces, sampling and resource references owned by
the backend. Metal native resource identifiers and Vulkan descriptor indices
need equivalent semantics, not identical root sizes.

Pin host records in `vkr_gpu_abi` and each native ABI. Vulkan recursively reflects
compiled SPIR-V physical-storage/root layouts at pipeline creation. Metal has
native packet ABI manifests and pipeline reflection checks. Build wrappers own
production shader compilation; shaders are not hot-reloaded. Native driver
pipeline caches/Metal archives are private to each implementation. There is no
frontend shader manifest or named-uniform pipeline system.

Match coordinate conventions, units, field order/types, basis signs, bounds,
edge behavior and dispatch coverage explicitly. World/view space is right-handed
with forward `-Z`; depth is `[0,1]` and projection/viewport Y lowering is backend
aware. Rounded dispatches retain guards required for valid edges. Material
normal decode reconstructs positive tangent Z; output transfer follows ADR-043.

The packet's source instance remains 80 bytes. Native publication/upload lowers
it once into a 128-byte `VkrPreparedInstanceGPU` with three prepared normal-transform
columns. Their common positive scale preserves inverse-transpose direction under
normalization; the first column's w carries model handedness for mirrored tangent
bases. Shaders transform tangents with the model's linear part and normals with
the prepared columns. No per-pixel matrix inverse is required.

Both G-buffer roots append the sky reprojection matrix at byte
352 and have a 416-byte native size. The G-buffer owns sky motion for every
temporal consumer; portable resolve carries no duplicate reprojection matrix.
Temporal resolve roots carry current and previous pixel jitter for canonical
color reconstruction and raw metadata validation. Vulkan offsets are 128/136
with a 144-byte root; Metal offsets are 200/208 with a 224-byte root.
The checked-scene flag occupies Vulkan byte 124 and Metal byte 216 without
growing those roots. Both implementations store accumulation age in the existing
depth-history second channel and retain raw depth in the first channel. Native
content/revision eligibility and the portable signature gate the same static
algorithm; unavailable Metal execution keeps this contract UNALIGNED.

A parity entry is ALIGNED only with matching production semantics, applicable
host/compiled reflection and non-degenerate native comparisons on both backends.
Missing or conflicting evidence is UNALIGNED. Shared source, compilation or a
single native run cannot establish bilateral parity. MetalFX is an explicit
backend-specific mode under ADR-040. Current missing native evidence is summarized
in [ARCHITECTURE](../ARCHITECTURE.md).

Current evidence state: **UNALIGNED** for every domain below. The production
source audit covers their counterparts; same-revision bilateral native
comparisons and runtime reflection checks remain incomplete.
Bounded Vulkan Release Bistro profiling and static/moving-camera snapshots pass
on RX 6700 XT. A subsequent user-reported blur/static-jitter regression required
preceding-submission history ordering, truthful center metadata, stationary
coverage support and validated cubic history reconstruction. Focused Vulkan
Debug execution loaded the Khronos validation layer and reported no synchronization
hazards; Metal native execution remains unavailable. All 71 measured pass rows have valid GPU timestamps; captures
were inspected without baseline promotion. The original host freeze cause is
unconfirmed. Cooker arena growth, Vulkan upload fallback and harness report
stack exhaustion were repaired during validation. Metal native execution is
unavailable on that Windows host. Checked static-scene accumulation subsequently
passed the aligned eight-phase Bistro capture and focused Vulkan validation;
the matching moving-camera replay remained visually unchanged. These local
observations do not close broader moving-image quality or bilateral comparison
gates. Native Metal validation and same-revision bilateral captures remain
unavailable.

## Consequences

Portable contracts remain reviewable without pretending native roots are identical.
A shader change requires both source/lowering paths to be inspected; output
comparison needs a case-specific tolerance rather than a newly observed delta.

## Alternatives considered

Frontend `.shadercfg` layout/staging was retired. Manifest-only ABI checks miss
compiled layout drift. Requiring identical native resource bytes would erase
backend resource models without proving equivalent rendering.

## Revisit when

A new shader family, ABI, algorithm or native-only feature changes these contracts.

## Implementation

Paths below are relative to [`lib/src/renderer/shaders/`](../../lib/src/renderer/shaders).
Native lowering lives in [`metal/`](../../lib/src/renderer/metal) and
[`vulkan/`](../../lib/src/renderer/vulkan).

| Domain | Shared source | Metal production | Vulkan production |
|---|---|---|---|
| Geometry/visibility/deferred/picking | `shared/gpu_draw.slangh` | `metal/msl/common/draw.metalh`, `metal/msl/world/gpu_draws.metal` | `vulkan/slang/common/`, `world/deferred.slang`, `picking/default.slang` |
| Material/light math | `shared/normal_map_kernel.slangh`, `ggx_kernel.slangh`, `point_light.slangh` | `metal/msl/world/default.metal`, `lighting.metalh`, `gpu_draws.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Transmission | `shared/transmission_kernel.slangh` | `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/deferred.slang` |
| Shadow receiver | `shared/shadow_kernel.slangh` | `metal/msl/shadow/sampling.metalh` | `vulkan/slang/world/default.slang` |
| IBL and SH | `shared/sh_l2_kernel.slangh`, `ggx_kernel.slangh` | `metal/msl/ibl/` | `vulkan/slang/ibl/` |
| Exposure/bloom/GTAO | matching `shared/*_kernel.slangh` | `metal/msl/post/` | `vulkan/slang/post/` |
| Temporal resolve | `shared/temporal_filter_kernel.slangh`; native visibility/identity helpers | `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/deferred.slang` |
| Tonemap/FXAA | shared exposure state | `metal/msl/post/tonemap.metal` | `vulkan/slang/post/default.slang`, `tonemap.slangh` |
| Text/UI (UNALIGNED: native comparison pending) | native coverage; fixed MTSDF atlas sampling | `metal/msl/text/`, `ui/` | `vulkan/slang/text/`, `ui/` |

Metal also compiles `metal/slang/` support sources; native MSL geometry decode
mirrors the shared Slang record. Consult [`shared/README.md`](../../lib/src/renderer/shaders/shared/README.md)
and build scripts for the exact entry-point inventory. This record replaces
former ADR-005's deleted reflection-driven frontend.

## Portable edge contracts and remaining differences

Material normals transform through the explicit tangent/bitangent/normal basis;
Slang row constructors must not transpose that basis. Native model-matrix
indexing must preserve transformed-axis culling bounds. Depth equality and odd
HZB mip edges follow ADR-028. Vulkan transmission compaction uses native subgroup
identity/count, independent of workgroup-local invocation numbering.

Direct lighting and IBL sampling PDFs share the GGX distribution. Its factored
denominator preserves the narrow supported specular lobe instead of flooring
the squared denominator. The zero-roughness prefilter retains its explicit
source-mip-zero behavior. Changes to the distribution and importance-sampling PDF
must remain consistent.

Exposure requires complete histogram groups and GTAO requires mip-selecting
depth sampling under ADR-042. Equirectangular HDR conversion wraps longitude
and clamps latitude, preserving the prepared source texture's pole behavior.

Two near-degenerate reconstruction policies still differ: Metal rejects
barycentric normalization sums at `1e-8`, Vulkan at `1e-12`; interpolated tangent
handedness exactly zero maps to zero on Metal and positive handedness on Vulkan.
These need a shared edge-case oracle before changing their thresholds or output.
