---
status: implemented
updated: 2026-09-09
authority: adr
---

# ADR-044: Portable shader semantics with native ABI validation

## Status

Accepted.

## Context

Sharing C frame inputs or shader source does not prove native binaries use the
same bindings, layouts, dispatches or numerical meaning. Resource references differ
between Metal and Vulkan.

## Anisotropy evidence state

Anisotropic GGX reflection is **UNALIGNED** pending native Vulkan validation
and bilateral numeric comparison. Production Metal/Slang compilation and nine
affected SPIR-V module layout/validation checks pass. Metal rotated scalar/map
highlights, zero-strength comparison, layered SSR/SSGI and serial API-validated
resize pass. Material rows are 320 bytes on Metal and 256 on Vulkan; array-table
reference records are 32 and 16 bytes. Frame roots remain 528 and 608 bytes.
Shared arithmetic and compiled layouts do not establish native parity.
[ADR-064](064-anisotropic-ggx-reflection.md) owns the accepted feature and budgets.

## Thin-sheet diffuse transmission evidence state

Thin-sheet diffuse transmission is **UNALIGNED** pending native Vulkan and
bilateral comparison. Production Metal/Slang compilation and nine affected
SPIR-V layout/validation checks pass. Material rows are 336 bytes on Metal and
272 on Vulkan. Deferred roots are 240/192 bytes; SSGI composite roots are
496/432 bytes. Both borrow the existing visible-draw buffer at graph bindings
13/11, without new images. [ADR-065](065-thin-sheet-diffuse-transmission.md)
owns the split, input restrictions and offline transport. Native Metal checks
pass for front/back energy, tint, black absorption, sun/point/rectangle lighting,
shadow occlusion, cutout coverage, layered SSR/SSGI and API-validated resize.
The zero-strength witness preserves all eight captured channels byte-for-byte.

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
Metal and Vulkan production shaders compiled; at that checkpoint native roots
were 480 and 496 bytes respectively. ADR-054 subsequently extended them. CPU checks cover perspective depth, all six point-face
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
| Shadow receiver (UNALIGNED) | `shared/shadow_kernel.slangh`, `local_shadow.slangh` | `metal/msl/shadow/sampling.metalh` | `vulkan/slang/world/default.slang` |
| Baked diffuse volumes (UNALIGNED) | `shared/diffuse_volume_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/world/lighting.metalh`, `default.metal`, `gpu_draws.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Rectangle LTC (UNALIGNED) | `shared/ltc_kernel.slangh` | `metal/msl/world/lighting.metalh`, `default.metal`, `gpu_draws.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Analytic fog (UNALIGNED) | `shared/fog_kernel.slangh` | `metal/msl/post/fog.metal` | `vulkan/slang/post/fog.slang` |
| Froxel volumetric fog (UNALIGNED) | `shared/froxel_fog_kernel.slangh` | `metal/msl/post/froxel_fog.metal` | `vulkan/slang/post/default.slang` |
| IBL and SH | `shared/sh_l2_kernel.slangh`, `ggx_kernel.slangh` | `metal/msl/ibl/` | `vulkan/slang/ibl/` |
| Opaque SSR (UNALIGNED) | `shared/ssr_kernel.slangh` | `metal/msl/post/ssr.metal` | `vulkan/slang/world/deferred.slang` |
| Opaque SSGI (UNALIGNED) | `shared/ssgi_kernel.slangh` | `metal/msl/post/ssgi.metal` | `vulkan/slang/post/ssgi.slang` |
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

The new material energy record uses correlated Smith visibility and one shared
RG16F DFG lookup per surface in both native implementations. Metal adds the
DFG texture at byte 472; Vulkan uses sampled-image and sampler indices at
488/492. ADR-054 later extends the surrounding frame roots without moving DFG. Native
manifests and Vulkan reflection include these fields. [ADR-053](053-energy-compensated-ggx.md)
owns the equations and approximation. Native Metal Release furnace output and API
validation pass. Metal shader validation remains unresolved after a report-decoding
crash, and native Vulkan execution is unavailable on the current host. These domains
remain **UNALIGNED** pending those diagnostics and a same-revision comparison.

Clearcoat changes world material rows and resolve/deferred, SSGI composite,
and SSR trace/temporal/composite roots. Metal rows are 240 bytes; Vulkan rows
are 192 bytes. Production SPIR-V reflection passes all nine affected compute
modules, and Metal furnace, material/SSR and API-validation resize checks pass.
The domain remains **UNALIGNED** because native Vulkan execution and the
same-revision comparison are unavailable. [ADR-062](062-layered-clearcoat.md) owns its independent-normal,
layer allocation, graph storage and coat-priority SSR policy.

Charlie sheen extends those material rows to 288 bytes on Metal and 224 bytes
on Vulkan. The shared layer and two-component rectangle kernels use a separate
immutable table block (48 bytes on Metal, 32 bytes on Vulkan), addressed by the
528-byte Metal and 608-byte Vulkan frame roots. Resolve writes a sheen G-buffer;
deferred lighting and SSGI/SSR composites consume it. This domain is
**UNALIGNED** because native Vulkan execution and same-revision comparison are
unavailable. Actual SPIR-V reflection and Metal material, rectangle quadrature,
furnace, zero-color equivalence, editor and API-validation resize checks pass.
Both backends decode the same positive-orientation matrix parameters and clip
rectangles to the receiver horizon before the two fitted lobe integrals.
[ADR-063](063-charlie-sheen.md) owns the layer, numerical domain, table budget
and environment-filtering approximation.

Baked diffuse volumes share cell lookup and trilinear weights. Metal's 512-byte
frame root keeps the texture at 480 and the 48-byte parameter pointer at 488;
origin, inverse spacing and dimensions remain at 0/16/32. Vulkan's 576-byte root
keeps the corresponding texture/value offsets at 496/512/528/544. Both paths load
an immutable RGBA32F texture and replace only diffuse lighting inside validated
same-room cells. Native Metal reflection, opaque/BLEND HDR comparison (maximum
error 0.000330536), and API validation pass. Actual deferred SPIR-V reflection
confirms all volume offsets in the 576-byte span; shader SHA-256 is
`dcbed9153947320b6163c397ec24361b39ca298f903a0c50f5cb617627f2d239`. This domain
remains **UNALIGNED** because native Vulkan execution is unavailable.
[ADR-054](054-baked-diffuse-volumes.md) owns asset and sampling semantics.

Rectangle LTC appends a 32-byte light/table block after those volume fields: the
Metal frame pointer is at 496 and the Vulkan block pointer at 560. Each block
references 64-byte rectangle rows and two immutable 64×64 RGBA16F tables. Metal
native MSL uses typed texture fields for sampling. The separate Metal Slang
vertex/visibility/shadow layout never samples LTC; its block carries the row
pointer plus raw 64-bit texture identifiers because nested Slang texture fields
cause the Metal-target compiler to fault. The source still preserves the 32-byte
block and frame-pointer offset, while native MSL ABI manifests validate the typed
consumer layout. Compiled Vulkan SPIR-V confirms a 576-byte frame root, 32-byte
LTC block and 64-byte row. Metal GGX quadrature, zero-light/back-face, radiance
scaling, layered-path and API checks pass. Native Vulkan execution and bilateral
comparison remain unavailable, so this domain is **UNALIGNED**. [ADR-056](056-rectangular-ltc-lights.md)
owns its authoring and transport policy.

Directional PCSS retains the 96-byte cascade record, with `origin_inv_size_sun`
at byte 80 and the sun half-angle tangent in its fourth component. Both receivers
share the nearest-two-cascade gate and world-to-texel radius equations, with eight
raw blocker samples and at most sixteen comparison-filter samples. Metal
hard/default/wide-sun captures and API validation pass; native Vulkan receiver
execution and bilateral output comparison remain unavailable. ADR-041 records
the authored units, fallback and acceptance evidence.

Directional GTAO keeps the existing 192-byte parameter record and native roots:
Metal depth/evaluate/denoise are 224/256/240 bytes; Vulkan uses its existing
240-byte utility root. Raw and denoised graph images change from R8 to RGBA8,
with world bent direction in RGB and visibility in alpha. Both backends share
horizon moments, packing, multi-bounce diffuse and cone specular arithmetic.
Base deferred environment and SSR receiver weights use the shared cone factor.
Deferred coat lighting and matching SSR probe removal use the coat-directed cone
on both backends, with bent direction decoded against the coat normal and packed
coat roughness. Shaded SSR history uses its filtered coat roughness. This corrects
Metal's former base-cone approximation and Vulkan's omitted coat occlusion;
native Vulkan comparison is still missing. Baked volumes retain scalar AO. Metal's disabled output is byte-identical to
its prior HDR fixture, and the corner lighting oracle has maximum HDR error
0.000960297 across 34 samples. Native API validation and production SPIR-V
validation pass. Native Vulkan execution and bilateral comparison remain
unavailable, so the domain stays **UNALIGNED**. ADR-042 owns the lighting policy.

Opaque SSR is **UNALIGNED**. Its accepted full-source-resolution shaded-history
contract retains the shared 288-byte parameters, 464-byte Metal temporal root and
400-byte Vulkan temporal root. Composite roots shrink to 480 bytes on Metal and
416 on Vulkan after removing history-depth and trace-receiver bindings 8/9.
Vulkan push constants remain 16 bytes. Release builds, shared math, Vulkan host
syntax and ten SSR/SSGI SPIR-V modules pass. Compiled composite stride 416 and
coat/sheen/anisotropy offsets 404/408/412 match the host; native Metal root checks
pass. Native Vulkan execution and bilateral image comparison remain unavailable.
[ADR-055](055-screen-space-reflections.md) owns the storage, access and
probe-replacement contracts and separates current evidence from prior baselines.

Trace remains half resolution with at most 48 decisions and five hit-source taps.
Temporal now shades each full-resolution source receiver. It gathers at most nine
raw taps once and reuses them for current bounds: rough receivers use a 3×3
continuous tent of radius 1.5 trace pixels, mirrors the former composite's 2×2
footprint with radius 1. Four source-grid history taps each validate depth and
identity. Raw RGB remains incoming radiance; history RGB contains receiver-shaded
radiance, including selected-coat or base sheen/anisotropy energy and indirect
specular GTAO. Composite directly loads same-pixel shaded history, removes the
exact current deferred probe term and adds covered history without another
receiver multiplier. The former bilateral composite gather is removed.

The color/depth/identity image count and completion-safe history ownership do not
change. At source 1280×720 with three frame slots and five history instances,
logical history payload grows from 21.972656 to 87.890625 MiB (+65.917969 MiB,
excluding alignment and resize overlap). Temporal executes about four times as
many pixels, with a reviewed Metal ceiling of 80 texture reads plus three output
writes per temporal invocation. Local matched profiles at source 1025×577 record
SSR GPU time 1.992→2.631 ms; they are non-authoritative with unstable warmup.
Graph bindings 17–20 still supply albedo, conditional GTAO, sheen and anisotropy.
Existing frame/LUT ownership remains: Metal retains 1536 reserved upload bytes
per SSR frame (592 payload bytes) added by shaded-history frame/anisotropy records;
Vulkan reuses its frame root and retains the 32-byte temporal upload increase.

History selects the exact motion producer through existing GPU dependencies even
before CPU-observed completion; reuse waits for every reader. Source-pixel-center
reprojection adds motion and the producer's previous-minus-current raster jitter
from parameter offsets 280/284. Source extent controls coordinates and tap bounds.
Roughness continuously relaxes clamping, but at most half the normalized RGB
residual outside current bounds survives each update, including sparse support.
Motion-adaptive defaults rise from 0.85 to 0.95 on stationary rough receivers and
return at one source pixel per frame. Empty neighborhoods retain at most half
of validated coverage each frame; rejected history clears. The shared scene-static
proof waits for 128 matching submitted frames with SSR enabled before allowing
TAA/FSR's existing 128-sample static accumulation. Selected-producer CPU metadata
owns the capped counter, which resets on invalid history or changed inputs and
publishes only on successful submission. No roots, bindings or sampling ceilings
change; native Vulkan verification remains unavailable. Receiver depth tolerance
has a 2 cm minimum independent of ray thickness. Full-resolution trace leaves,
absolute crossings, earliest rear-slab
intersections and same-pixel validation keep the existing depth allocation.
SSGI retains its previous leaf, slab and receiver-minimum policies through shared
adapters and initializes its unused jitter offsets to zero.

Old coat subtraction matches deferred's coat-directed GTAO and packed roughness.
Both decode bent direction against the coat normal. Coated composite pixels skip
base normal-variance filtering; normalized GTAO sampling keeps the disabled 1×1
sentinel valid. Both trace implementations preserve fractional hit and cone-offset
coordinates with linear-clamp source sampling, replacing Vulkan's rounded point
loads. Vulkan reuses its renderer-owned linear-clamp sampler at trace-root offset
324; reserved padding begins at 328 and total root size remains 336 bytes. Mirrors
still sample once and rough receivers at most five times. Source corrections do
not establish native Vulkan or bilateral acceptance. The current fractional-filter
and coat-cone checks are recorded with [ADR-055](055-screen-space-reflections.md).

`ssr_reflection` version 4 identifies full-source-resolution shaded RGB; version 3
captures were half-resolution shaded RGB. Raw capture stays version 2. Earlier
captures predate the new history extent. Metal mirror, static/moving Bistro,
layered-material, combined SSR/SSGI and two API-resize checks pass. The subsequent
bounded-fade/settling correction passes 140 shared-math outputs, five SSR SPIR-V
modules, host syntax and one serial Metal API hold. The sampled portable-TAA bar
region freezes after settling and static accumulation; MetalFX shows no decisive
stability improvement. GPU shader validation previously crashed in MetalTools
with SSR disabled as well as enabled and supplied no result.
[ADR-055](055-screen-space-reflections.md#evidence-and-remaining-checks) links
current and prior evidence. The domain remains UNALIGNED.

Analytic fog shares `VkrFogParams`, two `float4` values (32 bytes), between
native passes. Packet version 38 appends its prepared fog pointer without
growing either frame root: Metal uses byte 504 of its 512-byte root and Vulkan
uses byte 568 of its 576-byte root. The Vulkan 8x8 compute root is 128 bytes:
the parameter record, inverse view-projection, camera position, depth texture,
target texture and extent begin at bytes 0/32/96/112/116/120. Emitted SPIR-V
reflection and Release compile-command C syntax checks pass; the retained
diagnostic is [retained fog-spirv diagnostic](../../assets/verification/renderer-features/fog-spirv.txt).
Metal Release and editor runs pass, including native MSL startup/API evidence
in [retained fog-api diagnostic](../../assets/verification/renderer-features/fog-api.txt). Independent opaque/sky
checks cover 98,304 pixels per case with maximum HDR error 0.0004891, and
disabled fog is byte-identical. Clear-glass feedback and BLEND composition
checks pass with maximum errors 0.0007009 and 0.0001494 respectively.
Native Vulkan execution and bilateral image comparison remain unavailable, so
analytic fog is **UNALIGNED**.

Extended-linear output is implemented under
[ADR-061](061-extended-linear-display-output.md). The shared 16-byte display
record carries headroom, native output scale and the selected extended-linear
flag. Final, UI and editor-overlay roots are affected; world text remains
scene-linear. Metal roots are tonemap 48B, UI 80B and overlay 112B; Vulkan
roots are fullscreen 576B, UI 80B and overlay 128B. Generated SPIR-V validates
the 16B parameter offsets 0/4/8/12 and root strides. Native Metal reflection,
36-swatch FP16 output, one-time UI scaling, format transitions and API validation
pass. The editor composite preserves pre-encoded highlights without a second
scale or SDR clamp. Maximum numeric error is 0.003899. Native Vulkan execution remains
unavailable, so affected presentation entries are **UNALIGNED**.

SSGI shares a 288-byte parameter record and has five phases: depth base/mips,
trace, temporal, and composite. Deferred lighting writes the isolated
direct/emissive source only when SSGI is enabled; its roots retain existing
solar fields and append the direct-source identifier and enable flag, producing
192-byte Metal and 160-byte Vulkan roots. Generated Vulkan SPIR-V validation and
reflection prove roots of 304/32/320/368/416 bytes. Generated Metal source
matches its 320/320/352/400/448-byte native roots; see
[the retained reflection review](../../assets/verification/renderer-features/ssgi-final-spirv.txt).

Both native paths use the same 256-phase Hammersley trace sequence and 3×3 raw
bilateral filter over nearest covered receivers. Valid misses remain zero samples
in that normalized average. SSGI accepts only completed history tuples;
when portable TAA or MetalFX's shared motion predecessor is still in flight, SSGI falls
back to current radiance without waiting. Metal API validation proves completion,
TAA-jitter reuse, disable/re-enable, and resize behavior in
[the lifecycle record](../../assets/verification/renderer-features/ssgi-lifecycle-api-completed.txt). Native source-isolation,
emissive-bounce, baked-volume exclusion, editor output and Bistro cost checks
pass. Native Vulkan execution and bilateral comparison remain unavailable, so
SSGI is **UNALIGNED**. [ADR-060](060-screen-space-diffuse-indirect-lighting.md)
owns the feature policy and acceptance evidence.

Froxel volumetric fog uses a 928-byte `VkrFroxelFogParams` record. Fields through
byte 799 retain their existing offsets; unjittered current view-projection and
jittered inverse raster view-projection append at bytes 800 and 864. Packet
version 40 binds the Metal 512-byte frame root's froxel parameter pointer and
integrated 3D texture at bytes 136 and 216. The Vulkan frame root is 592 bytes:
the parameter address, integrated descriptor and sampler occupy bytes 576, 584
and 588. The graph defines frame-slot-count plus two completion-gated RGBA16F
3D scattering-history instances and one transient integrated volume per frame
slot: four plus two in the current renderer, five plus three in the approved
three-slot budget. History holds local
scattering/extinction only; camera-integrated values are current-frame data.
Metal host layout and native reflection, API validation, lifecycle, and numeric
captures pass. Actual Vulkan SPIR-V reflection/validation and eight affected host
translation units compile successfully. Native Vulkan execution and bilateral
comparison remain unavailable, so froxel fog is **UNALIGNED**.
[ADR-059](059-froxel-volumetric-fog.md) records the implemented scope and evidence.

Sky atmosphere uses shared 128-byte parameters and four cold compute stages.
Its native bake root is 192 bytes on Metal and 176 bytes on Vulkan. The deferred
lighting root appends a solar-radiance vector at byte 160 on Metal (176 bytes
total) and byte 128 on Vulkan (144 total); common frame roots remain unchanged.
Packet version 39 carries that radiance in the sky payload and includes it in
temporal/SSR signatures. Actual Vulkan SPIR-V validation and reflection pass in
[the retained diagnostic](../../assets/verification/renderer-features/atmosphere-spirv.txt). Native Metal
startup and API validation pass. Independent direct-light comparison has maximum
HDR error 0.000987, the zero-density sky is black outside the disc, and integrated
visible disc irradiance differs from its authored value by 0.517% in the tested
view. Ground/horizon, daylight, high-altitude and Bistro captures are finite.
Native Vulkan execution and bilateral comparison remain unavailable: atmosphere
is **UNALIGNED**. [ADR-058](058-revision-baked-sky-atmosphere.md) owns the model.

AgX and grading share production kernels. Metal's post root is 48 bytes, with a
grading-block pointer at byte 32; Vulkan's utility root is 560 bytes, with the
pointer at byte 544. The 64-byte grading record contains three matrix rows followed
by contrast, saturation, enable and reserved controls. Native ABI manifests and
Vulkan typed fullscreen-root reflection validate the contract. ADR-043 owns the
operation order and native display evidence.

Forward and deferred material lighting share `vkr_ggx_filter_roughness` on both
backends. Material roughness is perceptual: GGX width is `alpha = roughness^2`.
The filter adds capped normal variance to `alpha^2` and takes a fourth root to
return perceptual roughness. The derivative coefficient and variance cap remain
0.25; neighboring pixels from another visible draw contribute zero variance in
deferred lighting. This corrects the former addition to `alpha` without changing
normal samples, resources or native roots. The variance domain follows the
less-conservative isotropic filter in
[Improved Geometric Specular Antialiasing, equation 5](https://www.jp.square-enix.com/tech/library/pdf/ImprovedGeometricSpecularAA.pdf).
Offline normal/roughness recipes use these existing shader inputs and GGX
roughness units without changing shader sources, bindings or host ABI. They
retain full normal moments, bake capped directional spread into roughness and
fold normal strength/roughness factor into the images; ADR-012 owns that asset
contract. A focused Release M1 Pro/Metal 4 fixture loaded two strength-specific
pairs, passed draw/G-buffer assertions and produced the expected broader
minified highlights. This is local output evidence. The paired material outputs
still require matched native Vulkan/Metal comparison; the current macOS host
cannot execute native Vulkan.

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

## Post-reconstruction depth of field

The six DoF entry points and native roots are **UNALIGNED** pending native
Vulkan execution and bilateral output comparison. Release compilation, compiled
SPIR-V layout/validation and Metal focus/blur/bypass, odd-size, TAA, spatial,
MetalFX and serial API-resize checks pass. Params are 48 bytes; Metal roots
are 112 bytes and Vulkan roots are 80. [ADR-066](066-post-reconstruction-depth-of-field.md)
owns the image, quality and transparency contract. Native Vulkan execution is
unavailable on the current Mac.

## Post-reconstruction motion blur

**UNALIGNED**. [ADR-067](067-post-reconstruction-motion-blur.md) records the
accepted 32-sample, 16-pixel-radius contract. Both production shader paths compile;
compiled Vulkan root/binding and dispatch checks pass. Selected native Metal output, scaling, editor and API resize checks pass.
Native Vulkan execution and bilateral comparison are unavailable.

## Profiled surface diffusion

**UNALIGNED**. [ADR-068](068-profiled-surface-diffusion.md) records the implemented
32-sample, 32-pixel-radius surface approximation and ownership. Both production
shader paths compile. Reflection and `spirv-val` pass for the actual gather,
deferred and SSGI composite modules: parameters are 32 bytes, gather roots are
208/192 bytes, material rows are 352/288 bytes, deferred roots are 240/192 bytes
and SSGI roots are 496/432 bytes (Metal/Vulkan).

Five native Metal output fixtures and their numeric checker pass, including
zero-strength identity, RGB shadow spreading, furnace allocation and mixed
SSR/SSGI/glass/coat. TAA, spatial scaling, MetalFX, combined editor composition
and a separate API-validation resize pass. Final compilation, three-module
reflection and the focused extreme-irradiance fixture pass after signed
half-range source saturation: 49,056 covered pixels remain finite, with original
and composed HDR exactly equal. The ordinary fixture and checker also pass again.
Native Vulkan execution and bilateral comparison remain unavailable on this host.
