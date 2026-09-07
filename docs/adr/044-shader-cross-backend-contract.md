---
status: implemented
updated: 2026-09-07
authority: adr
---

# ADR-044: Portable shader semantics with native ABI validation

## Status

Accepted.

## Context

Sharing C frame inputs or shader source does not prove native binaries use the
same bindings, layouts, dispatches or numerical meaning. Resource references differ
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

The split between public `VkrFrameInput` and private `VkrPreparedFrame` changes
CPU preparation ownership, not the native root layouts or shader-visible instance
records. Moving scene/assets to application owners and preparing all pass families
before emission likewise preserve those native ABI and shader contracts. These
source changes do not establish fresh bilateral execution evidence.

The library split relocates application/runtime sources without moving production
shader sources from `renderer/src/shaders/`. It preserves shader-visible host
records, native entry points and source ABI layout. It adds no fresh native
cross-backend evidence; native Vulkan execution for the split remains unrun.

Match coordinate conventions, units, field order/types, basis signs, bounds,
edge behavior and dispatch coverage explicitly. World/view space is right-handed
with forward `-Z`; depth is `[0,1]` and projection/viewport Y lowering is backend
aware. Rounded dispatches retain guards required for valid edges. Material
normal decode reconstructs positive tangent Z; output transfer follows ADR-043.

The frame input's source instance remains 80 bytes. Native publication/upload
lowers it once into a 128-byte `VkrPreparedInstanceGPU` with three prepared normal-transform
columns. Their common positive scale preserves inverse-transpose direction under
normalization; the first column's w carries model handedness for mirrored tangent
bases. The second column's w carries an outward-rounded conservative affine
sphere stretch; source and prepared instance sizes remain 80 and 128 bytes.
Eight raster buckets partition material state by reflection parity; the shared
compaction record is 144 bytes. Direct draws preserve order through contiguous
parity runs. Shaders transform tangents with the model's linear part and normals with
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

Vulkan FSR 3.1 is an authorized backend-specific **UNALIGNED** exception under
ADR-052. Its prepare shader converts raw HDR, temporal validity and nearest
transmission depth into depth and mask inputs. The SDK consumes these with
normalized temporal motion and records the upscale. It has no Metal counterpart
or portable parity claim.
Its 48-byte prepare root carries the submitted-history scene-stationary proof;
optical contrast contributes only to composition and is suppressed for matching
scenes. Reactive rejection retains authored reactivity and missing-motion protection. Its separate 48-byte stabilization root carries output
and render extents, raster jitter, sampled history/reactivity indices and the
same stationary proof. The output-resolution pass averages 128 stationary samples
and freezes completed pixels; changes or reactive footprints reset age. Age uses
private output alpha, removed by the FSR-only fullscreen opaque-alpha flag.
Production Vulkan compilation and reflection pass. Bounded Bistro static,
camera-translation and editor-resize runs pass native Vulkan diagnostics; native Metal execution was
unavailable. ADR-052 records the accepted capability boundary and evidence limits.

Presentation sharpening shares bounded cross-neighborhood arithmetic in both
production shader libraries. Frame version 32 carries `image_sharpness` in [0,1];
zero takes the existing path without extra samples. The Vulkan utility root
carries strength in `point_light_grid_origin_cell_size.z` beside output extent
in xy, with unchanged native layout. Metal's 32-byte tonemap root replaces the
two reserved words with flags at byte 8 and float strength at byte 12; exposure
and extent remain at bytes 16 and 24, with the ABI manifest updated. This domain
is **UNALIGNED** until same-revision native Metal compilation, validation and
matched pixel evidence are available; Windows cannot execute those gates. Vulkan Release captures cover
FSR/TAA/native, both FXAA states, zero/default/maximum strength and editor text.
The enabled editor/text Debug case passes Khronos synchronization validation
with no API warnings or errors; local cost and quality limits are in ADR-043.

Current evidence state: **UNALIGNED** for every domain below. The production
source audit covers their counterparts; same-revision bilateral native
comparisons and runtime reflection checks remain incomplete.
UI icon coverage uses shared CPU-generated opaque/transparent polygon rings
through the existing 32-byte vertex format and native straight-alpha blending.
Bootstrap MTSDF atlases increase their distance range to 16 texels at 64 texels/em;
native coverage formulas and GPU roots are unchanged. Windows 100% scale captures
were inspected, and a focused Debug editor run with the Khronos layer loaded
reported no Vulkan validation errors. Matching Metal captures remain unavailable, so Text/UI stays
UNALIGNED.
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
gates. Native Metal validation and same-revision bilateral captures were
unavailable on that Windows host.

Metal's native library concatenates the shared temporal filter helper before
its MSL consumers. Shader size assertions match the existing 416-byte G-buffer
and 224-byte temporal host roots. Native Release startup/reflection and the
serial [candidate residency fixture](../../tools/cases/local/metal_candidate_residency_audit.case.json)
with TAA pass API/shader validation on M1 Pro. Static and moving Bistro
final-color/depth captures at native 1280×720 remain byte-identical across the
CPU preparation changes. This adds Metal execution evidence; same-revision
bilateral captures and full stationary-accumulation/moving-image quality gates
remain open.

The editor retained-image path reuses the existing Tonemap pipeline for resolve
and composite. Resolve performs output processing once into a swapchain-format
image; composite samples that image with exposure 1, tonemap disabled and FXAA
disabled. Both backend host paths implement the same lowering without changing
shader roots or source. Same-revision native Vulkan/Metal image comparison remains
unavailable for this change.

Metal command-buffer demand growth uses the existing candidate count as its
per-view ICB command stride. Graph draw-table storage now follows scene demand
on both backends through existing native capacity fields. Source capacity is
`C = pow2(max(candidate_count, 1))`; visible/classification/argument stride is
`V = 4 * min(C, 65536)`. The per-bucket limit remains sufficient for a concentrated
small scene and preserves the previous ceiling for larger scenes. Metal supplies
`V` to classification, raster, resolve and picking; Vulkan also uses `V / 4` for
indirect argument offsets and `maxDrawCount`. Native root layouts are unchanged.
Same-revision native Vulkan validation of these smaller argument ranges remains
unavailable; this domain remains **UNALIGNED**. The Metal command-capacity fixture
with four shadow views and seven transmission candidates passed API validation
(report SHA-256 `819dfb4ee766c027d61ee4144e01bd3e2112aa607304276e0476b419db8022a5`).
Its matched Release captures were byte-identical with SHA-256
`82510845bfe55d00ca57c4948579a0ebe367e8dd210f8f42fa48b9a2b49a7c28`.
This local Metal evidence does not close the bilateral UNALIGNED state.

Local shadow views use a shared 112-byte record: matrix at byte 0, light
position/near plane at 64, direction/far plane at 80, and perspective footprint
and texel bias parameters at 96. Native frame roots append their local depth
array reference and view pointer. Punctual row `p3.w` stores first-view index
plus one; zero means unshadowed. CPU point-face orientation and shared face-ray
reconstruction use the same canonical negative projection-Y convention.
Local shadow parity remains **UNALIGNED** until matched native Vulkan and Metal
captures and diagnostics pass. Production compilation does not close that gate.

2026-09-07 local evidence on Apple M1 Pro / Metal 4 / Darwin 25.6.0:
`./build_release.sh`, `./build_editor.sh Release`, and `./build_test.sh` pass.
Metal and Vulkan production shaders compile; native roots are pinned at 480 and
496 bytes respectively. CPU checks cover perspective depth, all six point-face
orientations, complete budget groups, and nested graph-use repetition.
Release `local_shadow_point_on/off` and `local_shadow_spot_on/off` smoke snapshots
show receiver occlusion; the point caster interior remains byte-identical to its
unshadowed control after setting normal offset to two texels. The full-pool
fixture executes sixteen local views, four directional cascades, and one deferred
lighting pass with seven selected lights. The directional `shadow_receiver_pcf9`
case passes all four caster-count assertions. These are focused synthetic
observations, with no accepted performance claim or transparent-receiver pixel
comparison.

API-only Metal validation of `local_shadow_point_on` passes (report SHA-256
`1e54c348964dbca060f22736f50022cab7d719c8f48f56b17566daacdbecb237`).
Combined API/GPU validation crashes in Apple MetalTools
`resolvedSharedPacketData` while decoding a buffer diagnostic; the directional-only
control reproduces that failure. The underlying diagnostic remains unreadable,
so shader validation is incomplete. Native Vulkan execution and cross-backend
pixel comparison remain unavailable on this host. Local reports, exact commands,
and retained capture paths are recorded in `.scratch/implement-local-shadows.md`.

## Consequences

Editor overlay color and picking share packed geometry and an unjittered MVP.
Both native roots are 112 bytes with independently pinned layouts: Metal roots
carry offset vertex/decode pointers, while Vulkan roots carry base addresses and
explicit vertex/decode indices. Opaque linear color is written after tonemapping;
picking writes the supplied integer ID with identical primitive order and no
depth test or culling. This new domain is **UNALIGNED** until same-revision native
Metal/Vulkan captures and reflection checks pass; native Vulkan is unavailable
on the current macOS host.

Metal and Vulkan reject geometry range counts that cannot fit the existing
32-bit temporal surface token before publication or narrowing loader counts.
Metal's per-geometry CPU range storage changes neither that encoding nor shader
roots. Native Vulkan validation of the publication guard remains unavailable.

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

Paths below are relative to [`renderer/src/shaders/`](../../renderer/src/shaders).
Native lowering lives in [`metal/`](../../renderer/src/metal) and
[`vulkan/`](../../renderer/src/vulkan).

| Domain | Shared source | Metal production | Vulkan production |
|---|---|---|---|
| Editor handles/color/picking | CPU `VkrEditorOverlayDraw` | `metal/msl/editor/overlay.metal` | `vulkan/slang/editor/overlay.slang` |
| Geometry/visibility/deferred/picking | `shared/gpu_draw.slangh` | `metal/msl/common/draw.metalh`, `metal/msl/world/gpu_draws.metal` | `vulkan/slang/common/`, `world/deferred.slang`, `picking/default.slang` |
| Material/light math | `shared/normal_map_kernel.slangh`, `ggx_kernel.slangh`, `point_light.slangh` | `metal/msl/world/default.metal`, `lighting.metalh`, `gpu_draws.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Transmission | `shared/transmission_kernel.slangh` | `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/deferred.slang` |
| Shadow receiver | `shared/shadow_kernel.slangh`, `local_shadow.slangh` | `metal/msl/shadow/sampling.metalh` | `vulkan/slang/world/default.slang` |
| IBL and SH | `shared/sh_l2_kernel.slangh`, `ggx_kernel.slangh` | `metal/msl/ibl/` | `vulkan/slang/ibl/` |
| Exposure/bloom/GTAO | matching `shared/*_kernel.slangh` | `metal/msl/post/` | `vulkan/slang/post/` |
| Temporal resolve | `shared/temporal_filter_kernel.slangh`; native visibility/identity helpers | `metal/msl/world/gpu_draws.metal` | `vulkan/slang/world/deferred.slang` |
| FSR 3.1 (UNALIGNED: authorized Vulkan-only feature) | graph inputs and prepared temporal metadata | — | `vulkan/slang/post/fsr31.slang`, FSR SDK dispatch |
| Tonemap/FXAA/sharpening (UNALIGNED) | shared exposure state, `shared/sharpen_kernel.slangh` | `metal/msl/post/tonemap.metal` | `vulkan/slang/post/default.slang`, `tonemap.slangh` |
| Text/UI (UNALIGNED: native comparison pending) | native coverage; fixed MTSDF atlas sampling | `metal/msl/text/`, `ui/` | `vulkan/slang/text/`, `ui/` |

Metal also compiles `metal/slang/` support sources; native MSL geometry decode
mirrors the shared Slang record. Consult [`shared/README.md`](../../renderer/src/shaders/shared/README.md)
and build scripts for the exact entry-point inventory. This record replaces
former ADR-005's deleted reflection-driven frontend.

## Portable edge contracts and remaining differences

Material normals transform through the explicit tangent/bitangent/normal basis;
Slang row constructors must not transpose that basis. Native model-matrix
indexing must preserve conservative affine culling bounds. Depth equality and odd
HZB mip edges follow ADR-028. Vulkan transmission compaction uses native subgroup
identity/count, independent of workgroup-local invocation numbering.

Direct lighting and IBL sampling PDFs share the GGX distribution. Its factored
denominator preserves the narrow supported specular lobe instead of flooring
the squared denominator. The zero-roughness prefilter retains its explicit
source-mip-zero behavior. Changes to the distribution and importance-sampling PDF
must remain consistent.

Forward and deferred material lighting share `vkr_ggx_filter_roughness` on both
backends. Material roughness is perceptual: GGX width is `alpha = roughness^2`.
The filter adds capped normal variance to `alpha^2` and takes a fourth root to
return perceptual roughness. The derivative coefficient and variance cap remain
0.25; neighboring pixels from another visible draw contribute zero variance in
deferred lighting. This corrects the former addition to `alpha` without changing
normal samples, resources or native roots. The variance domain follows the
less-conservative isotropic filter in
[Improved Geometric Specular Antialiasing, equation 5](https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf).
Material/light math remains **UNALIGNED**: the Windows host cannot compile or
execute native Metal, so a same-revision Metal build, focused diagnostic and
matched native pixel comparison remain required.
The corrected filter passes Vulkan Release Bistro motion and stationary captures
with FSR and motion captures with portable TAA. The bounded Debug static-reset
case loads Khronos synchronization validation and reports no API warnings or
errors; ADR-052 records the small, mixed motion-quality changes and local cost.

Exposure requires complete histogram groups and GTAO requires mip-selecting
depth sampling under ADR-042. Equirectangular HDR conversion wraps longitude
and clamps latitude, preserving the prepared source texture's pole behavior.

Direct-light helpers reject noncontributing light hemispheres before half-vector
construction. Shared GGX math defines zero contribution when the half-vector's
squared length is zero or subnormal; finite back-view diffuse behavior remains
unchanged. Local probe influence bounds remain active independently of parallax
projection. Bloom gain follows ADR-042's maximum-chain normalization.

Audit remediation changes visibility, HZB producer-grid metadata, Metal reset
ordering, optional resolve outputs and numerical edges. These domains remain
**UNALIGNED** until the same-revision Metal build, focused native validation and
bilateral capture gates pass. The available host is Windows/Vulkan; analytical
oracles do not substitute for Metal execution.

Two near-degenerate reconstruction policies still differ: Metal rejects
barycentric normalization sums at `1e-8`, Vulkan at `1e-12`; interpolated tangent
handedness exactly zero maps to zero on Metal and positive handedness on Vulkan.
These need a shared edge-case oracle before changing their thresholds or output.
