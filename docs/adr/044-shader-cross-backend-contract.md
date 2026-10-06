---
status: implemented
updated: 2026-10-06
authority: adr
---

# ADR-044: Portable shader semantics with native ABI validation

## Status

Accepted.

## Context

Sharing C frame inputs or shader source does not prove native binaries use the
same bindings, layouts, dispatches or numerical meaning. Resource references differ
between Metal and Vulkan. Since 2026-10-06 each backend runs one graphics
pipeline class ([ADR-087](087-gpu-class-graphics-pipelines.md)): Metal the
tiled pipeline and Vulkan the desktop pipeline. The kernels in
`renderer/src/shaders/shared/`, the shared host records and the passes both
render graphs declare still have consumers on both backends.

## Evidence rules

Each domain below names the backends that run it:

- **Shared**: both backends run it, Metal in the tiled class and Vulkan in the
  desktop class. This covers GPU culling, draw encoding and geometry LOD,
  compute skinning, shadow casting into the cascades and the local shadow
  atlas, the sky atmosphere, clouds, IBL and SH, exposure, bloom, tonemapping
  and display output, the editor passes, UI and text, and the material and
  light arithmetic that forward and deferred shading both call.
- **Desktop**: only Vulkan runs it. This covers the visibility buffer,
  G-buffer resolve, deferred lighting, the local shadow mask, transmission
  layers, TAA, FSR 3.1, SSR, SSGI, GTAO, depth of field, motion blur, froxel
  fog, profiled surface diffusion, FXAA and the clearcoat, sheen, anisotropy
  and thin-sheet diffuse transmission material layers.
- **Tiled**: only Metal runs it: the tiled forward, atmosphere, blend and
  picking passes (ADR-087).

A domain is **ALIGNED** only within a class that both backends implement,
when it has matching production semantics, applicable host and compiled
reflection, and non-degenerate native comparisons on both backends. No class
has two backends today, so no domain is ALIGNED. Each domain instead records
its native evidence on every backend that runs it: Vulkan runs for desktop
domains, Metal runs for tiled domains and runs on both for shared domains. The
two consumers of a shared domain render different classes, so their images
differ by design; ADR-087's art-level contract (decision 3) relates them, not
pixel parity. Shared source, compilation and cross-compilation are not native
evidence.

Metal ran the desktop pipeline until 2026-10-06. Metal runs recorded before
then remain evidence for shared passes whose Metal code is unchanged, such as
the atmosphere bake or UI, and are no evidence for the removed desktop passes
or for the tiled pipeline's own passes. Metal GPU shader validation supplies
no result: it crashed in MetalTools while decoding a buffer diagnostic, and
combined API and GPU validation aborts on the driver's assertion that
command-buffer residency sets exceed 32. Current missing native evidence is
summarized in [ARCHITECTURE](../ARCHITECTURE.md).

## Animation evidence state

Compute skinning and the independent animation preview are shared: both
graphs run `Animation.Skinning`, and the tiled vertex stage and the desktop
visibility and resolve passes read its output. Both production roots consume
decoded bind vertices, four influences and asset-space joint palettes. Current
output is a 32-byte record; prepared instances append current and compatible
previous deformation addresses. On the desktop pipeline the selected temporal
producer, instance/geometry generations and discontinuity determine
previous-position eligibility. [ADR-071](071-animation-bank-and-reference-pose.md)
owns the feature, capacity and editor behavior. Native evidence: the earlier
Metal validation and deformed color, normal and motion snapshots (ADR-071)
used the removed desktop passes; skinned captures on the tiled pipeline and
on Vulkan are pending.

## Anisotropy evidence state

Anisotropic GGX reflection is desktop-only; the tiled forward shader does not
draw it (ADR-087, decision 8). Vulkan material rows are 256 bytes and its
array-table reference records 16 bytes. Production Slang compilation and nine
affected SPIR-V module layout/validation checks pass, and the 2026-09-12
Windows sweep below passes a Debug synchronization-validation resize. Native
Vulkan output checks of rotated highlights are pending. Shared arithmetic and
compiled layouts do not establish native output.
[ADR-064](064-anisotropic-ggx-reflection.md) owns the accepted feature and budgets.

## Tiled pipeline evidence state

The tiled pipeline ([ADR-087](087-gpu-class-graphics-pipelines.md)) runs only
on Metal. It shares kernels and host records with the desktop pipeline:

- **Changed contracts.**
  - The 576-byte Metal frame root carries `lightmap` at byte 544, the address
    of a 96-byte `VkrMetalPacketLightmap`, `terrain_materials` at byte 552
    and `transmission_materials` at byte 560; the tiled sky root is 128
    bytes. Both are pinned in
    `vkr_metal_packet_abi.c` and checked against reflection at pipeline
    creation.
  - The prepared instance row carries the lightmap slot in
    `normal_column2.w`; every shader on both backends reads only its xyz.
  - The Metal sky background helpers, `vkr_metal_packet_sky_clear`,
    `vkr_metal_packet_sky_discs` and `vkr_metal_packet_sky_background`,
    serve the tiled sky and cloud draws.
  - The tiled blend fragment composes glass with the shared
    `vkr_transmission_compose` and returns the factor the destination keeps
    as its second, dual-source output; the Vulkan transmission kernels use
    the same composition.
  - The tiled shading calls the shared local-light loop and rectangle-light
    path without the coat and sheen layers. Its local shadows take one
    bilinear comparison and read no refractive layers
    (`vkr_metal_packet_local_shadow_sample<false, false>`), where the Vulkan
    receivers filter with the Poisson disk and read the transmission layers.
    The harder tiled shadow edge is a class difference the owner accepted
    (ADR-087, decision 11). Tiled rectangle lights skip rows below a
    contribution bound through the `ContributionCutoff` parameter of
    `vkr_metal_packet_layered_rectangle_lights`; a receiver that no row faces
    skips the LTC table reads, which changes no result.
  - The tiled class draws no FXAA; its alpha-tested surfaces use alpha to
    coverage and its opaque pass a tone-mapped tile resolve
    (`vkr_metal_tiled_resolve_tile`), a class difference the owner accepted
    (ADR-087, decision 6). Vulkan's FXAA and tonemap shaders are unchanged.
  - The Metal tonemap root's last 12 bytes carry `bloom_intensity` and the
    `bloom` texture; with `VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM` the pass adds
    that bloom level to its scene-linear samples, rounded to FP16 as
    `Post.Bloom.Combine` stores it. The desktop graph keeps
    `Post.Bloom.Combine` on Vulkan.
- **Metal evidence.** Pipeline creation validates the new layouts. The
  Bistro street view (`tiled_bistro_capture`), the café windows
  (`tiled_bistro_glass`) and a lightmap-baked Bistro
  (`tiled_bistro_baked_capture`) render. The tiled lighting variants render
  Bistro with no dynamic lights, unshadowed and shadowed point and spot
  lights, and a rectangle light; a night view compares the single-tap local
  shadow with the former filter (ADR-087,
  [Light shading variants](087-gpu-class-graphics-pipelines.md#light-shading-variants)).
- **Vulkan evidence.** Vulkan consumes the changed shared records without
  layout changes, and its sources build on macOS. No native Vulkan run has
  followed the changes.
- **Missing gates.** A native Vulkan run after the shared-record changes.

## Thin-sheet diffuse transmission evidence state

Thin-sheet diffuse transmission is desktop-only; the tiled forward shader does
not draw it (ADR-087, decision 8). Production Slang compilation and nine
affected SPIR-V layout/validation checks pass. Vulkan material rows are 272
bytes, deferred roots 192 bytes and SSGI composite roots 432 bytes; both roots
borrow the existing visible-draw buffer at graph binding 11, without new images.
[ADR-065](065-thin-sheet-diffuse-transmission.md) owns the split, input
restrictions and offline transport. The 2026-09-12 Windows sweep passes a Debug
synchronization-validation resize; Vulkan output checks of front/back energy,
tint, absorption, shadow occlusion and cutout coverage are pending.

## Terrain layer evidence state

The terrain material blend ([ADR-084](084-agent-channel-and-level-design-toolkit.md#terrain))
is shared. The Vulkan G-buffer resolve and the Metal tiled forward shader call
the shared `terrain_kernel.slangh` for weights, per-layer surfaces and the
blend; each samples its own terrain segment. Common material rows are
unchanged. The terrain segment rows are 192 bytes on Metal and 144 on Vulkan.
The Vulkan resolve root reuses its reserved address at byte 16, and the tiled
shader reads the rows through the Metal frame root's `terrain_materials`.
Native evidence: Vulkan execution passed on Windows (2026-10-04), and the four
Vulkan resolve modules pass `spirv-val`; on Metal, a 128 m terrain painted with
four layers in the headless editor shows the expected layer regions on the
tiled pipeline (ADR-087).

## Geometry LOD evidence state

Level selection, level encoding and terrain geomorphing
([ADR-085](085-gpu-geometry-lod-and-terrain-geomorphing.md)) are shared.
Native Vulkan execution passed on Windows (2026-10-04, and the 8 km terrain's
strips of audit A20 on 2026-10-05).
`vkr_gpu_geometry_lod_row` computes the LOD row's address in bytes, because
AMD's Vulkan driver offset a pointer cast of `records + lod_record` by the
row's stride. Both backends select through the shared `lod_kernel.slangh` and
morph through `vkr_gpu_terrain_morph` (shared Slang, mirrored in
`draw.metalh`) with `terrain_kernel.slangh`'s topology. The Metal culling
root carries the LOD views at byte 88, its former reserved field; the Vulkan
cull root carries them and the geometry rows at bytes 192 and 200, its former
reserved tail. Decode records keep 32 bytes, their last word now the LOD row's
record offset. Metal API validation of a 1 km Bistro terrain passes, and all
Vulkan modules pass `spirv-val`. The tiled pipeline's cooked mesh levels are
measured in ADR-085.

## Pre-exposure evidence state

Pre-exposure ([ADR-081](081-physical-night-sky.md)) is shared; its history
rescaling applies to the histories each class keeps.

- **Changed contracts.**
  - Frame, utility and Vulkan resolve roots carry `pre_exposure`, and the
    Metal tonemap root carries `inverse_pre_exposure`.
  - Bloom parameters carry `inverse_pre_exposure`.
  - The cloud trace root carries `history_pre_exposure_scale` on both
    backends; on Vulkan so do the TAA, SSR temporal, SSGI temporal, froxel
    inject and FSR stabilize roots, and TAA also carries `pre_exposure`.
- **Root sizes.** The Metal cloud trace root grows to 144 bytes and the Metal
  tonemap root to 64; the Vulkan TAA root grows to 160 bytes.
- **Metal evidence.** The forced P = 1, 8 and 1/64 Bistro runs used the
  removed desktop passes; the tiled pipeline has no forced-P run.
- **Vulkan evidence.** All production modules pass `spirv-val`, and their
  compiled offsets match the C roots. On Windows (RX 6700 XT, AMD 26.6.3),
  Release `local-offscreen` snapshots of the night (P = 1), moonlit-sky
  (P = 2^18), starry-sky (P = 2^19) and moonlit-cloud (P = 2^17) cases pass
  with finite HDR captures, and the night and moonlit-cloud cases run clean
  under Debug Vulkan validation with synchronization checks.
- **Missing gates.** Forced-P and night captures on the tiled pipeline.

## Moon and star evidence state

The moon, the atmosphere's second light, and the procedural star field
([ADR-081](081-physical-night-sky.md)) are shared. They change the sky
atmosphere, aerial perspective and volumetric cloud domains.

- **Changed contracts.**
  - Atmosphere parameters grow to 160 bytes with `moon` and `lunar`.
  - The sky record adds `key_light` and, for the star field, `star_pole` and
    `star_axis`: 432 bytes of parameters, a 496-byte Metal record and a
    480-byte Vulkan record.
  - The sky background, the Vulkan deferred background and the Metal tiled
    sky draw, draws the procedural star field from the shared
    `vkr_atmosphere_stars`.
  - The atmosphere bake roots grow to 224 bytes on Metal and 208 on Vulkan.
  - The sky-view image is 384×108, a sun table and a moon table.
- **Metal evidence.** The night, moonlit-sky, moonlit-cloud and starry-sky
  Bistro cases rendered and focused API validation runs were clean before
  2026-10-06; they cover the shared bake, sky-view and cloud kernels, not the
  tiled sky draw.
- **Vulkan evidence.** All production modules pass `spirv-val`, and their
  compiled atmosphere and sky offsets match the C asserts. The Windows runs
  under Pre-exposure render the moon disc and glow, the moonlit clouds and the
  star field natively, with clean Debug Vulkan validation.
- **Missing gates.** Night captures on the tiled pipeline.

## Editor inspection views

`VkrRenderMode` retains Lit (`DEFAULT`, 0) and Unlit (3), and adds Detail lighting
(10), Lighting only (11) and Wireframe (12). The existing render-mode word carries
these choices; no native root layout or resource binding changes. The views are
shared: the Vulkan forward and visibility/deferred consumers and the Metal
tiled inspection variant (ADR-087, decision 11) use
[`shared/editor_view.slangh`](../../renderer/src/shaders/shared/editor_view.slangh).
Detail lighting evaluates the scene lights and environment with neutral linear 0.5
albedo, dielectric F0 0.04, metallic 0 and perceptual roughness 0.5, retaining
mapped shading normals. Lighting only uses the same neutral material with
interpolated vertex normals and no normal map, matching the common editor split
between geometry-only and detail lighting; triangle face normals turned foliage
cards black in an earlier capture. Material emissive, sheen, clearcoat and
anisotropy do not color these neutral views. Wireframe computes visible
triangle-edge coverage from barycentric derivatives with a one-pixel edge and
one-pixel coverage ramp. It preserves scene visibility and alpha coverage;
hidden edges are not drawn through opaque surfaces. The forward paths
reconstruct barycentrics from the triangle or take them from the rasterizer;
the visibility path consumes its triangle reconstruction.

On the desktop pipeline the three inspection modes bypass bloom,
analytic/froxel fog, SSGI, SSR, subsurface diffusion, depth of field and motion
blur, and Wireframe also bypasses temporal reconstruction and GTAO.
Orthographic frames have a separate per-frame spatial capability boundary
recorded in [ADR-046](046-editor-viewport-mapping-and-picking.md).

Native evidence: on Windows (RX 6700 XT), the Release Vulkan snapshot
`smoke.bistro.editor.views` passes and captures all four channels, and the same
case runs clean under Debug Vulkan validation. On the M1 Pro, tiled-pipeline
captures of Unlit, Detail lighting, Lighting only and Wireframe render (ADR-087,
Unbaked lights, terrain, inspection and atmosphere). No frame-budget claim is
made.

## Decision

Keep portable arithmetic shared where both shader languages can consume it.
Keep native bindings, address spaces, sampling and resource references owned by
the backend. Metal native resource identifiers and Vulkan descriptor indices
need equivalent semantics in shared records, not identical root sizes.

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
source changes do not establish fresh native execution evidence.

The library split relocates application/runtime sources without moving production
shader sources from `renderer/src/shaders/`. It preserves shader-visible host
records, native entry points and source ABI layout, and adds no native evidence.

Match coordinate conventions, units, field order/types, basis signs, bounds,
edge behavior and dispatch coverage explicitly. World/view space is right-handed
with forward `-Z`; depth is `[0,1]` and projection/viewport Y lowering is backend
aware. Rounded dispatches retain guards required for valid edges. Material
normal decode reconstructs positive tangent Z; output transfer follows ADR-043.

The frame input's source instance is 96 bytes, including a CPU skinning index. Native publication/upload
lowers it once into a 144-byte `VkrPreparedInstanceGPU` with three prepared normal-transform
columns. Their common positive scale preserves inverse-transpose direction under
normalization; the first column's w carries model handedness for mirrored tangent
bases. The second column's w carries an outward-rounded conservative affine
sphere stretch. Current and previous deformation addresses occupy bytes 128 and
136 of the prepared record; zero selects static geometry.
Eight raster buckets partition material state by reflection parity; the shared
compaction record is 144 bytes. Direct draws preserve order through contiguous
parity runs. Shaders transform tangents with the model's linear part and normals with
the prepared columns. No per-pixel matrix inverse is required.

The Vulkan G-buffer root appends the sky reprojection matrix at byte 352 and
has a 416-byte size. The G-buffer owns sky motion for every temporal consumer;
portable resolve carries no duplicate reprojection matrix. The Vulkan temporal
resolve root carries current and previous pixel jitter at bytes 128 and 136
of its 144 bytes for canonical color reconstruction and raw metadata
validation, and the checked-scene flag at byte 124. It stores accumulation age
in the existing depth-history second channel and retains raw depth in the
first. Native content/revision eligibility and the portable signature gate the
static algorithm.

Vulkan FSR 3.1 ([ADR-052](052-vulkan-fsr31-upscaling.md)) is desktop-only. Its
prepare shader converts raw HDR, temporal validity and nearest transmission
depth into depth and mask inputs. The SDK consumes these with normalized
temporal motion and records the upscale.
Its 48-byte prepare root carries the submitted-history scene-stationary proof;
optical contrast contributes only to composition and is suppressed for matching
scenes. Reactive rejection retains authored reactivity and missing-motion protection. Its separate 48-byte stabilization root carries output
and render extents, raster jitter, sampled history/reactivity indices and the
same stationary proof. The output-resolution pass averages 128 stationary samples
and freezes completed pixels; changes or reactive footprints reset age. Age uses
private output alpha, removed by the FSR-only fullscreen opaque-alpha flag.
Production Vulkan compilation and reflection pass. Bounded Bistro static,
camera-translation and editor-resize runs pass native Vulkan diagnostics.
ADR-052 records the accepted capability boundary and evidence limits.

Presentation sharpening is shared: both production shader libraries use the
bounded cross-neighborhood arithmetic of `shared/sharpen_kernel.slangh`. Frame
version 32 carries `image_sharpness` in [0,1]; zero takes the existing path
without extra samples. The Vulkan utility root carries strength in
`point_light_grid_origin_cell_size.z` beside output extent in xy, with unchanged
native layout. Metal's tonemap root carries flags at byte 8 and float strength
at byte 12; exposure and extent remain at bytes 16 and 24, with the ABI
manifest updated. Vulkan Release captures cover FSR/TAA/native, both FXAA
states, zero/default/maximum strength and editor text. The enabled editor/text
Debug case passes Khronos synchronization validation with no API warnings or
errors; local cost and quality limits are in ADR-043. Native Metal sharpening
captures are not recorded.

The UI pass is shared and uses one pipeline per backend over a 96-byte vertex
whose `mode` selects flat quad, MTSDF text, bitmap text, SDF box or image. The
box mode shares one rounded-rectangle distance (per-corner radii, inner border,
and a feather that ramps coverage across twice its softness) in
`ui/default.metal` and `ui/default.slang`; the separate rounded-rectangle
pipeline is removed. Editor icons are Phosphor MTSDF glyphs through the text
mode. Host validation rejects glyph and image vertices in an untextured batch.
The UI root is 48 bytes on Metal (a nested `VkrMetalPacketUiVertex` record) and
64 bytes on Vulkan. Both native roots compile; the macOS Release editor renders
through Metal and the Windows Release editor through Vulkan.
Bootstrap MTSDF atlases use a 16-texel distance range at 64 texels/em.

Portable TAA is desktop-only. Bounded Vulkan Release Bistro profiling and
static/moving-camera snapshots pass on RX 6700 XT. A subsequent user-reported
blur/static-jitter regression required preceding-submission history ordering,
truthful center metadata, stationary coverage support and validated cubic
history reconstruction. Focused Vulkan Debug execution loaded the Khronos
validation layer and reported no synchronization hazards. All 71 measured pass
rows have valid GPU timestamps; captures were inspected without baseline
promotion. The original host freeze cause is unconfirmed. Cooker arena growth,
Vulkan upload fallback and harness report stack exhaustion were repaired during
validation. Checked static-scene accumulation subsequently passed the aligned
eight-phase Bistro capture and focused Vulkan validation; the matching
moving-camera replay remained visually unchanged. These local observations do
not close broader moving-image quality gates.

The editor retained-image path is shared and reuses the existing Tonemap
pipeline for resolve and composite. Resolve performs output processing once
into a swapchain-format image; composite samples that image with exposure 1,
tonemap disabled and FXAA disabled. Scene preparation optionally freezes this
retained image and selects bit 7 in each backend's existing post flags for
backdrop blur. The shared `scene_blur_kernel.slangh` defines normalized
binomial weights and offsets: 25 bilinear taps over a 5×5 grid, four output
pixels apart, clamped at source edges. The compositor averages
presentation-linear values, preserving output scale; UI draws afterward and
remains sharp. No shader root layout, binding, intermediate image or GPU
retirement contract changes. Normal frames retain the original sampling path.
Both native shader paths compile. A focused Bistro editor reload and
cancellation passes Metal API validation with visible backdrop blur; native
Vulkan evidence of the blur is not recorded.

Metal command-buffer demand growth uses the existing candidate count as its
per-view ICB command stride. Graph draw-table storage follows scene demand on
both backends through existing native capacity fields. Source capacity is
`C = pow2(max(candidate_count, 1))`; visible/classification/argument stride is
`V = 4 * min(C, 65536)`. The per-bucket limit remains sufficient for a concentrated
small scene and preserves the previous ceiling for larger scenes. Metal supplies
`V` to classification, raster and picking; Vulkan also uses `V / 4` for
indirect argument offsets and `maxDrawCount`. Native root layouts are unchanged.
The Metal command-capacity fixture with four shadow views and seven
transmission candidates passed API validation
(report SHA-256 `819dfb4ee766c027d61ee4144e01bd3e2112aa607304276e0476b419db8022a5`).
Its matched Release captures were byte-identical with SHA-256
`82510845bfe55d00ca57c4948579a0ebe367e8dd210f8f42fa48b9a2b49a7c28`.
Native Vulkan validation of these smaller argument ranges is not recorded.

Local shadow views use a shared 144-byte record: matrix at byte 0, light
position/near plane at 64, direction/far plane at 80, perspective footprint
and texel bias parameters at 96, the light's shadow strength and the face's
transmission layer plus one (zero without one) at 112, and the face's atlas
square and layer at 128. Both backends render the faces; the receivers
differ by class. Every receiver maps face UVs into the square through the
shared `vkr_local_shadow_atlas_uv`, reads the strength from the light's first
view, skips lookups at zero and blends through the shared
`vkr_local_shadow_apply_strength`. A point-light tap inside the receiver's face
takes its reference depth from the shared `vkr_local_shadow_in_face_depth`. The
tiled receiver takes one bilinear comparison and no transmission (ADR-087,
decision 11).

On Vulkan, `shadow_params.w` holds the light's source radius over twice the
face's tan(half FOV); the mask pass and the inline receivers apply
contact-hardening through the shared
`vkr_local_shadow_pcss_search_radius_texels`,
`vkr_local_shadow_pcss_filter_radius_texels`,
`vkr_local_shadow_forward_distance` and `vkr_local_shadow_atlas_texel`, with
raw depth `Load`s and the shared search tap counts. The receivers read
transmission from the view's layer, not from the view index.
`shadow_params.z` of one selects a single hardware-filtered tap and no contact
shadows. A transmission lookup stops after the first crossing depth when the
receiver lies in front of it, and an opaque-occluded tap skips transmission;
both are exact because the crossings are peeled in order. Native frame roots
append their local depth array reference and view pointer. Punctual row `p3.w`
stores first-view index plus one; zero means unshadowed. CPU point-face
orientation and shared face-ray reconstruction use the same canonical negative
projection-Y convention.

Vulkan runs the `Shadow.LocalMask` compute pass (`pass.local_shadow.mask`)
with its own 128-byte root: frame, G-buffer inputs, visible rows, inverse
view-projection, extent, the contact-shadow noise index at byte 112 and the
temporal filter flag at byte 116, nonzero under temporal reconstruction. The
208-byte deferred-lighting root reads the mask array at byte 168 and appends
the per-light contribution counters at byte 192 (null when not measured), into
which the deferred punctual loop adds, per wave and light, the pixels'
unshadowed contribution from the shared `vkr_local_light_contribution` for
local-shadow priority (ADR-019). Mask layers are per-pixel slots: the k-th
shadowed light in range of a pixel, in light traversal order, writes layer k
with alpha tagging its light index through the shared
`vkr_local_shadow_mask_tag`. The deferred-lighting kernels skip the lights
below the shared `VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF` that the mask gives no
slot, and filter inline when the slot is past `VKR_LOCAL_SHADOW_MASK_SLOT_COUNT`
or its tag names another light. Vulkan builds the mask kernel with and without
contact shadows and selects the variant from the payload's `contact_shadows`
(Ultra only). The shared `local_shadow.slangh` owns the contact-shadow step
count, length, noise, start offset, occlusion test and fade, and the full and
temporal tap counts, tap rotation and rotation of a Poisson tap. The receivers
take an optional rotation and tap count that default to the fixed nine taps;
only the mask passes the rotated four-tap kernel, and only when the temporal
flag is set. Forward and transmission shading pass an inline visibility source
(a Slang generic value parameter) to the shared punctual loop.
2026-09-26: all 31 Vulkan modules that declare the record, including the mask
and both deferred-lighting modules, pass `spirv-val` with offsets
0/64/80/96/112/128, and the emitted Vulkan mask root matches the host offsets.
Face passes draw into their atlas square: Metal clears it with a far-depth
triangle under its viewport and scissor, Vulkan with `vkCmdClearAttachments`.
On Windows, Debug `local_shadow_bistro_vulkan_street_ultra_validation` passes
with no VUID, synchronization hazard or error (2026-10-03,
[Windows/Vulkan checklist](../proposals/windows-vulkan-verification.md)).

## Local transmitting-shadow evidence

Local transmitting shadows are desktop-only. Vulkan point/spot shadows retain
two 512² D32/RGBA16F crossing prefixes and a third blocking depth, under
[ADR-019](019-bounded-forward-spatial-lighting.md). The shared coefficient and
prefix selector live in
[`local_shadow_transmission.slangh`](../../renderer/src/shaders/shared/local_shadow_transmission.slangh).
Vulkan applies the same RGB visibility per opaque PCF tap and in froxel
injection. Opaque/cutout and refractive local casters occupy separate cull views;
directional caster semantics are unchanged. Public frame-input version is 45.

Vulkan's frame root is 624 bytes, with the sampling pointer at byte 608, a
32-byte sampling record, an 80-byte shadow raster root and a 208-byte cull
root. Local cull views exist only for faces drawn this submission, one per
render slot, so the cull root carries no reused-face mask; bytes 192 to 207
are reserved. Native assertions and reflection pin these layouts.

The removed Metal desktop implementation passed all 18 independent receiver
ratios of the transmission fixture on 2026-09-12 (maximum RGB ratio error
0.000405 against tolerance 0.006): ordered prefixes, before/after overflow,
tint, absorption, Fresnel, zero-transmission texels, cutout holes, opaque and
thin-sheet blockers, mirrored sidedness and point-face seams. The receiver
checker is
[`check_local_shadow_transmission_fixture.py`](../../tools/checks/check_local_shadow_transmission_fixture.py);
fixture preparation is
[`prepare.py`](../../tests/fixtures/rendering/local_shadow_transmission/prepare.py).
The fixture has not run on Vulkan. Production builds and SPIR-V
validation/reflection pass for the affected raster, cull, deferred and froxel
entry points.

## Consequences

Editor overlay color and picking are shared and use packed geometry and an
unjittered MVP. The Metal root is 112 bytes and the Vulkan root 128 bytes, with
independently pinned layouts: Metal roots carry offset vertex/decode pointers,
while Vulkan roots carry base addresses and explicit vertex/decode indices.
Opaque linear color is written after tonemapping; picking writes the supplied
integer ID with identical primitive order and no depth test or culling. The
Windows Vulkan editor picks Bistro meshes by viewport click and draws the move,
rotate and scale gizmos with clean Debug Vulkan validation.

The selection outline ([ADR-046](046-editor-viewport-mapping-and-picking.md#selection-outline))
is shared. It reuses the overlay vertex path to write opaque white into an R8
mask, then blends the outline color from a twelve-tap edge test over the mask.
Both outline roots are 48 bytes: Metal carries a read-access texture reference,
Vulkan a bindless texture index, and each pins its layout by static assertion;
Vulkan also checks the reflected fragment root. The outline color goes through
the UI display-output transform. The tiled pipeline draws it in the Release
editor (ADR-087, Editor evidence); Vulkan SPIR-V compiles, passes `spirv-val`
and matches the host offsets. The Windows Release Vulkan editor draws the
outline around a picked Bistro mesh, and Debug Vulkan validation is clean.

The editor ground grid (ADR-027) is one shared full-screen pass over the Scene
image reading the opaque depth at binding 0. The shared kernel owns line coverage,
the tenfold level cross-fade, axis colors and distance fade; native entries own
the unjittered ray (Metal flips NDC Y and the clip matrix, Vulkan does not),
the depth read at the matching render-extent texel and the UI display-output
transform; the kernel's depth visibility widens its tolerance by the plane's
change across that texel so a floor on the plane cannot z-fight it. Both 128-byte roots pin their layouts by static assertion: Metal
carries a read-access depth reference, Vulkan a depth-table index, and Vulkan
checks the reflected fragment root. The Release editor renders it through Metal,
hidden behind a cube and drawn through it with the toggle; Vulkan SPIR-V passes
`spirv-val` and its reflected offsets match the host root. The Windows Release
Vulkan editor draws it in the top view through geometry with both axes, and
Debug Vulkan validation is clean for the XZ and ZY planes, depth-tested and
drawn-through. The shared kernel strengthens the finest level's sparse lines
toward major alpha when zoomed in past it; on Windows Vulkan this raised the
near-camera line contrast of the FPS Arena floor from 69 to 97 at 1.7 m and
from 44 to 68 at 0.25 m, and left 6.6 m unchanged. On 2026-10-03 the macOS
Release build compiled the Metal library with that change, and the windowed
Bistro editor drew the grid at 0.25 and 1.7 m above it with lines up to the
camera, hidden under the raised pavement; no contrast was measured on Metal.

Picking also returns the opaque device depth at the picked pixel, which the
editor's grid fit unprojects. On Vulkan the picking resolve root grows to 80
bytes (`depth_texture`, `depth_output` at 64) and writes the readback buffer at
offset 8 through its device address; Windows Vulkan fits Bistro's grid from
that depth. On Metal the tiled pipeline replays the opaque draws into the
picking target, and the readback copies the picked pixel's resolved depth
(ADR-087, decision 9); `grid.fit` on Bistro read 16.8432 m from it.

Metal and Vulkan reject geometry range counts that cannot fit the existing
32-bit temporal surface token before publication or narrowing loader counts.
Metal's per-geometry CPU range storage changes neither that encoding nor shader
roots. Native Vulkan validation of the publication guard remains unavailable.

Portable contracts remain reviewable without pretending native roots are
identical. A shared kernel or record change requires every consuming
source/lowering path on both backends to be inspected; output comparison
against a same-backend reference needs a case-specific tolerance rather than a
newly observed delta.

## Alternatives considered

Frontend `.shadercfg` layout/staging was retired. Manifest-only ABI checks miss
compiled layout drift. Requiring identical native resource bytes would erase
backend resource models without proving equivalent rendering. Requiring
pixel parity between the tiled and desktop classes would forbid the
techniques ADR-087 chose per GPU class.

## Revisit when

A new shader family, ABI, algorithm or native-only feature changes these
contracts, or a pipeline class gains a second backend.

## Implementation

Paths below are relative to [`renderer/src/shaders/`](../../renderer/src/shaders).
Native lowering lives in [`metal/`](../../renderer/src/metal) and
[`vulkan/`](../../renderer/src/vulkan). Class names the backends that run a
domain under the [evidence rules](#evidence-rules).

| Domain | Class | Shared source | Metal production | Vulkan production |
|---|---|---|---|---|
| Editor inspection views | Shared | `shared/editor_view.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Editor handles/color/picking | Shared | CPU `VkrEditorOverlayDraw` | `metal/msl/editor/overlay.metal`, `metal/slang/picking/world.slang` | `vulkan/slang/editor/overlay.slang`, `picking/default.slang` |
| Editor selection outline | Shared | CPU `VkrEditorOverlayDraw` mask draws | `metal/msl/editor/overlay.metal`, `selection.metal` | `vulkan/slang/editor/overlay.slang`, `selection.slang` |
| Editor ground grid | Shared | `shared/editor_grid_kernel.slangh` | `metal/msl/editor/grid.metal` | `vulkan/slang/editor/grid.slang` |
| Compute skinning | Shared | `shared/skinning_kernel.slangh` | `metal/msl/world/skinning.metal` | `vulkan/slang/world/skinning.slang` |
| Culling, draw encoding and geometry decode | Shared | `shared/gpu_draw.slangh` | `metal/msl/common/draw.metalh`, `metal/msl/world/gpu_draws.metal` | `vulkan/slang/common/`, `world/deferred.slang` |
| Geometry LOD and terrain geomorph | Shared | `shared/lod_kernel.slangh`, `terrain_kernel.slangh`, `gpu_draw.slangh` | `metal/msl/common/draw.metalh`, `metal/msl/world/gpu_draws.metal`, `metal/msl/world/tiled.metal`, `metal/slang/world/default.slang` | `vulkan/slang/world/deferred.slang`, `common/vertex.slangh` |
| Terrain layer blend | Shared | `shared/terrain_kernel.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/world/deferred.slang` |
| Tiled forward, atmosphere and blend | Tiled | shared material, light, fog, atmosphere and transmission kernels below | `metal/msl/world/tiled.metal`, `lighting.metalh`, `metal/msl/shadow/sampling.metalh` | — |
| Visibility buffer, G-buffer resolve and deferred lighting | Desktop | `shared/gpu_draw.slangh` and the kernels below | — | `vulkan/slang/world/deferred.slang`, `picking/default.slang` |
| Material/light math | Shared | `shared/normal_map_kernel.slangh`, `ggx_kernel.slangh`, `point_light.slangh`, `punctual_light_kernel.slangh` | `metal/msl/world/tiled.metal`, `lighting.metalh` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Clearcoat, sheen, anisotropy, thin-sheet diffuse transmission | Desktop | `shared/clearcoat_kernel.slangh`, `sheen_kernel.slangh`, `anisotropy_kernel.slangh` | — | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Transmission composition | Shared | `shared/transmission_kernel.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/world/deferred.slang` |
| Transmission layers | Desktop | `shared/transmission_kernel.slangh` | — | `vulkan/slang/world/deferred.slang` |
| Shadow receiver | Shared | `shared/shadow_kernel.slangh`, `local_shadow.slangh` | `metal/msl/shadow/sampling.metalh` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Local shadow mask and transmitting shadows | Desktop | `shared/local_shadow.slangh`, `local_shadow_transmission.slangh` | — | `vulkan/slang/world/deferred.slang`, `local_shadow_transmission.slang` |
| Baked diffuse volumes | Shared | `shared/diffuse_volume_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/world/lighting.metalh`, `tiled.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Rectangle LTC | Shared | `shared/ltc_kernel.slangh` | `metal/msl/world/lighting.metalh`, `tiled.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Analytic fog | Shared | `shared/fog_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/post/fog.slang`, `world/default.slang` |
| Froxel volumetric fog | Desktop | `shared/froxel_fog_kernel.slangh`, `fog_kernel.slangh`, `punctual_light_kernel.slangh` | — | `vulkan/slang/post/froxel_fog.slang` |
| IBL and SH | Shared | `shared/sh_l2_kernel.slangh`, `ggx_kernel.slangh` | `metal/msl/ibl/` | `vulkan/slang/ibl/` |
| Sky atmosphere and aerial perspective | Shared | `shared/atmosphere_kernel.slangh` | `metal/msl/ibl/atmosphere.metal`, `world/lighting.metalh`, `world/tiled.metal` | `vulkan/slang/ibl/atmosphere.slang`, `world/default.slang`, `deferred.slang`, `post/fog.slang`, `post/froxel_fog.slang` |
| Volumetric clouds | Shared | `shared/cloud_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/ibl/clouds.metal`, `ibl/sh_projection.metal`, `shadow/sampling.metalh`, `world/lighting.metalh`, `world/tiled.metal` | `vulkan/slang/ibl/clouds.slang`, `ibl/default.slang`, `world/default.slang`, `deferred.slang`, `post/froxel_fog.slang` |
| Opaque SSR | Desktop | `shared/ssr_kernel.slangh` | — | `vulkan/slang/world/deferred.slang` |
| Opaque SSGI | Desktop | `shared/ssgi_kernel.slangh` | — | `vulkan/slang/post/ssgi.slang` |
| Exposure/bloom | Shared | matching `shared/*_kernel.slangh` | `metal/msl/post/exposure.metal`, `bloom.metal` | `vulkan/slang/post/exposure.slang`, `bloom.slang` |
| GTAO | Desktop | `shared/gtao_kernel.slangh` | — | `vulkan/slang/post/gtao.slang` |
| Temporal resolve | Desktop | `shared/temporal_filter_kernel.slangh`; native visibility/identity helpers | — | `vulkan/slang/world/deferred.slang` |
| FSR 3.1 | Desktop | graph inputs and prepared temporal metadata | — | `vulkan/slang/post/fsr31.slang`, FSR SDK dispatch |
| Depth of field, motion blur, surface diffusion | Desktop | `shared/dof_kernel.slangh`, `motion_blur_kernel.slangh`, `subsurface_kernel.slangh` | — | `vulkan/slang/post/dof.slang`, `motion_blur.slang`, `subsurface.slang` |
| Tonemap/sharpening/display output | Shared | shared exposure state, `shared/sharpen_kernel.slangh`, `agx_kernel.slangh`, `display_output_kernel.slangh`, `color_grading_kernel.slangh` | `metal/msl/post/tonemap.metal` | `vulkan/slang/post/default.slang`, `tonemap.slangh` |
| FXAA | Desktop | — | — | `vulkan/slang/post/default.slang`, `tonemap.slangh` |
| Text/UI | Shared | native coverage; fixed MTSDF atlas sampling; per-vertex SDF box, text and image modes | `metal/msl/text/`, `ui/` | `vulkan/slang/text/`, `ui/` |

Metal also compiles `metal/slang/` support sources; native MSL geometry decode
mirrors the shared Slang record. Consult [`shared/README.md`](../../renderer/src/shaders/shared/README.md)
and build scripts for the exact entry-point inventory. This record replaces
former ADR-005's deleted reflection-driven frontend.

## Portable edge contracts and remaining differences

The renderer-features performance corrections reconstruct normal-map Z before
strength, preserve explicit glTF zero strength, and version paired cooked recipes.
Fog composition preserves scene HDR and ray reconstruction extrapolates beyond
raster far depth. Material planes are conditionally declared from an opaque
feature aggregate; the Vulkan path guards absent texture bindings. Deferred,
forward and transmission lighting share active lobe traversal. Deferred lighting
skips discarded environment diffuse, and SSGI excludes camera-directed specular
from its source. Vulkan splits deferred lighting into base and layered kernels
(a Slang generic value parameter) with 8x8 group classification; the lighting
root carries the split flag at byte 164. Both Vulkan modules pass `spirv-val`
with matching root offsets.

SSGI depth-base writes current-frame RG32UI receiver metadata with bit-preserved
32-bit depth and an exact local offset. Trace, temporal and composite reuse it.
Vulkan root sizes are 304/320/368/448 bytes for depth-base/trace/temporal/
composite; shared parameters remain 288 bytes. Source review and CPU
arithmetic do not establish execution; a native Vulkan run of this change is
pending.

Material normals transform through the explicit tangent/bitangent/normal basis;
Slang row constructors must not transpose that basis. Native model-matrix
indexing must preserve conservative affine culling bounds. Depth equality and odd
HZB mip edges follow ADR-028. Vulkan transmission compaction uses native subgroup
identity/count, independent of workgroup-local invocation numbering.

Direct lighting and IBL sampling PDFs share the GGX distribution. Its factored
denominator preserves the narrow supported specular lobe instead of flooring
the squared denominator. At or below `VKR_IBL_PREFILTER_MIRROR_ROUGHNESS`
(0.001, in `ggx_kernel.slangh`), the prefilter mip is a mirror: both backends
write one source-mip-zero fetch at the texel's normal, the value every one of
its 256 importance samples took. Changes to the distribution and
importance-sampling PDF must remain consistent. The Vulkan entry passes
`spirv-val` with the branch at the shared threshold; native Vulkan execution of
the branch is not recorded.

The material energy record uses correlated Smith visibility and one shared
RG16F DFG lookup per surface in both native implementations: the Metal frame
root holds the DFG texture at byte 472, and Vulkan uses sampled-image and
sampler indices at 488/492. Native manifests and Vulkan reflection include
these fields. [ADR-053](053-energy-compensated-ggx.md) owns the equations and
approximation. No furnace test has run on the tiled pipeline or on Vulkan; the
earlier Metal furnace used the removed deferred lighting.

Clearcoat is desktop-only and changes world material rows and resolve/deferred,
SSGI composite, and SSR trace/temporal/composite roots; Vulkan rows are 192
bytes. Production SPIR-V reflection passes all nine affected compute modules,
and the 2026-09-12 Windows sweep passes a Debug synchronization-validation
resize. Vulkan output checks are pending. [ADR-062](062-layered-clearcoat.md)
owns its independent-normal, layer allocation, graph storage and coat-priority
SSR policy.

Charlie sheen is desktop-only and extends the Vulkan material rows to 224
bytes. The shared layer and two-component rectangle kernels use a separate
immutable 32-byte table block addressed by the Vulkan frame root. Resolve
writes a sheen G-buffer; deferred lighting and SSGI/SSR composites consume it.
Actual SPIR-V reflection passes, and the 2026-09-12 Windows sweep passes a
Debug synchronization-validation resize. The implementation decodes
positive-orientation matrix parameters and clips rectangles to the receiver
horizon before the two fitted lobe integrals. Vulkan output checks are pending.
[ADR-063](063-charlie-sheen.md) owns the layer, numerical domain, table budget
and environment-filtering approximation.

Baked diffuse volumes are shared and share cell lookup and trilinear weights.
The Metal frame root keeps the texture at byte 480 and the 48-byte parameter
pointer at 488; origin, inverse spacing and dimensions remain at 0/16/32. The
576-byte Vulkan root keeps the corresponding texture/value offsets at
496/512/528/544. Both paths load an immutable RGBA32F texture and replace only
diffuse lighting inside validated same-room cells; the tiled pipeline uses them
for draws without a lightmap (ADR-087, decision 8). Actual deferred SPIR-V
reflection confirms all volume offsets in the 576-byte span; shader SHA-256 is
`dcbed9153947320b6163c397ec24361b39ca298f903a0c50f5cb617627f2d239`. Native
output evidence on the tiled pipeline and on Vulkan is not recorded.
[ADR-054](054-baked-diffuse-volumes.md) owns asset and sampling semantics.

Rectangle LTC is shared and appends a 32-byte light/table block after those
volume fields: the Metal frame pointer is at 496 and the Vulkan block pointer
at 560. Each block references 64-byte rectangle rows and two immutable 64×64
RGBA16F tables. Metal native MSL uses typed texture fields for sampling. The
separate Metal Slang vertex and shadow layout never samples LTC; its block
carries the row pointer plus raw 64-bit texture identifiers because nested
Slang texture fields cause the Metal-target compiler to fault. The source still
preserves the 32-byte block and frame-pointer offset, while native MSL ABI
manifests validate the typed consumer layout. Compiled Vulkan SPIR-V confirms a
576-byte frame root, 32-byte LTC block and 64-byte row. On the tiled pipeline,
night and daylight Bistro captures with a dynamic rectangle light changed by at
most 4 and 8 of 255 with the contribution cut-off (ADR-087,
[Light shading variants](087-gpu-class-graphics-pipelines.md#light-shading-variants));
native Vulkan output is not recorded. [ADR-056](056-rectangular-ltc-lights.md)
owns its authoring and transport policy.

Directional PCSS is shared and retains the 96-byte cascade record, with
`origin_inv_size_sun` at byte 80 and the sun half-angle tangent in its fourth
component. Every receiver shares the nearest-two-cascade gate and
world-to-texel radius equations, with eight raw blocker samples and at most
sixteen comparison-filter samples. ADR-041 records the authored units,
fallback and acceptance evidence.

Far-cascade EVSM shares its warp, tent weights and Chebyshev visibility through
`shadow_kernel.slangh` (`vkr_shadow_evsm_*`). Only the desktop graph runs the
moments passes (`pass.shadow.moments`), on Vulkan; the tiled pipeline keeps
depth PCF for every cascade and renders no moments (ADR-041). The passes use
a 32-byte root: depth and moments bindless indices at bytes 0 and 4, depth
and moments layers, and the two square sizes. The Vulkan deferred-lighting
root keeps its size and reuses bytes 172 for the
moments index and 200 for the linear-sampler slot; `UINT32_MAX` keeps PCF.
Vulkan's compiled-SPIR-V reflection test covers both roots. Native Vulkan
output of the far cascades is not recorded.

Both classify kernels skip a caster for any non-camera orthographic view when
the shared `vkr_gpu_cascade_caster_too_small` (`lod_kernel.slangh`) finds its
bounding-sphere diameter below one texel of the view's LOD scale. Native
Vulkan evidence of the skip is not recorded.

Directional GTAO is desktop-only and keeps the existing 192-byte parameter
record; Vulkan uses its existing 240-byte utility root. Raw and denoised graph
images are RGBA8, with world bent direction in RGB and visibility in alpha. The
shared kernel holds horizon moments, packing, multi-bounce diffuse and cone
specular arithmetic. Base deferred environment and SSR receiver weights use the
shared cone factor. Deferred coat lighting and matching SSR probe removal use
the coat-directed cone, with bent direction decoded against the coat normal and
packed coat roughness. SSR composite uses filtered coat roughness for incoming
reflection shading. This corrected Vulkan's omitted coat occlusion. Baked
volumes retain scalar AO. Production SPIR-V validation passes; native Vulkan
output checks are pending. ADR-042 owns the lighting policy.

Opaque SSR is desktop-only. [ADR-055](055-screen-space-reflections.md) owns its
accepted reflected-hit reprojection, incoming-radiance history, storage and
fallback policy. Shared `VkrSsrParams` stays 288 bytes. The temporal-only
camera record is 128 bytes, with inverse current view at 0 and exact selected
producer view at 64.

| Vulkan root | Size | Changed contract |
| --- | ---: | --- |
| SSR depth base | 304 B | Independent floor-half extent; receiver-coordinate output removed |
| SSR trace | 336 B | Full-source raw and uint4 hit; receiver-coordinate input removed |
| SSR temporal | 512 B | Camera record, selected transforms, paired identities and wider geometry; receiver input removed |
| SSR composite | 416 B | Same-pixel incoming history, current receiver shading |

Vulkan embeds the camera record at offset 288, then visible
rows/instances/prior transforms at 416/424/432, producer frame at 440, hit
texture at 460, and output color/geometry/identity at 476/480/484.
Specular/coat indices are 488/492 and tail padding begins at 496. Native
assertions and compiled reflection pin each layout. Existing 80-byte GPU
transform rows retain their layout; CPU graph-buffer history metadata adds an
owned camera view matrix, published only after successful submission.

Trace stores fractional hit UV, positive view depth and reflected visible-row
identity in full-source RGBA32_UINT. The full-resolution geometry history is
RGBA32F (receiver depth, virtual depth, selected view-normal octahedral components);
RGBA32_UINT identity stores receiver and reflected instance index/generation.
History color remains RGBA16F and now contains incoming radiance. The approved
full-resolution trial adds 42.1875 MiB with three slots at source 1280×720, or
112.5 MiB with eight, below the approved 64/169 MiB bounds. Including the earlier
reflected-hit history expansion, the increase is 140.625/316.40625 MiB. Alignment
and resize overlap are excluded. History instance count and completion ownership
are unchanged; the obsolete half-size RG32_UINT receiver image is removed.

The temporal entry gathers rough raw samples at source offsets {-2,0,2}, with
half-offset grid/bilateral weights preserving the former physical filter width.
Integer source coordinates give mirrors one nonzero center tap. The shared
ranking helper selects the largest weighted RGB component, breaking equal-energy
ties by covered weight. History geometry therefore follows the sample supplying
light, including bright lamps surrounded by more widely covered dark objects.
It reads that hit once and retains its visible row from the gather: one additional
texture read, below the approved nine. Cross-instance receiver correspondence
cannot seed or reuse history. Transported hit/receiver models and producer camera
supply virtual reflected-point motion; adding its UV delta preserves the current
full-resolution pixel's offset. Current jittered projection plus producer-minus-
current jitter supplies history UV; the previous ray/receiver-plane intersection
supplies expected receiver depth. Four independently validated taps require
receiver/hit identities, both depths and selected normals to match. Invalid or
absent correspondence uses current radiance/probes immediately. Shared affine,
projection, octahedral and acceptance math runs in the native entry. Curved
surfaces remain approximate.

The temporal source ceiling is 63 reads for rough base receivers and 60 for coat
receivers with mixed neighbors (52 for a fully coated footprint), or 23/20 on
mirrors, plus three writes. Former temporal material/LUT,
motion and validity reads are removed. Graph bindings 17/18 supply the hit image
and exact prior transform buffer. The graph declares these accesses; same-queue
barriers synchronize the selected producer, while submitted readers extend last
use. Output reuse still requires GPU completion.

Composite applies current base BRDF, sheen/anisotropy and indirect-specular GTAO
once. Coated pixels use filtered selected-normal roughness for new SSR and packed
roughness with the coat-directed GTAO cone for exact old-probe removal. Trace
keeps its one/five fractional linear-clamp source samples, full-resolution leaves
and 48-decision limit. It traces each source pixel, nominally four times the
former floor-half grid, without another per-ray source read. The depth pyramid
remains independently floor-half. Removed graph bindings are depth-base 3,
trace 5 and temporal 1; surviving bindings retain their numbers.
`ssr_reflection` version 5 denotes full-resolution incoming
radiance; earlier shaded versions cannot be compared numerically. Raw remains
version 2.

The shared supported RGB-retention cap and 128-frame SSR settling period remain.
Portable TAA caps ordinary history at 90% while settling, through the unchanged
144-byte Vulkan root, with its mode at offset 124. SSR-off stationary retention
and the following checked 128-sample integral are unchanged; FSR retains its
accepted policy. The reflected-hit change passed Release app/editor builds, 268
independent shared-math outputs, eleven SSR/SSGI/deferred SPIR-V modules and
Vulkan host syntax checks. Compiled temporal root/camera/transform strides are
512/128/80 bytes. The radiance-owner selection, the nine spaced rough taps and
full-resolution tracing were chosen from captures of the removed Metal desktop
implementation; the
[reflected-hit](../../assets/verification/renderer-features/ssr-reflected-hit.txt),
[radiance-owner](../../assets/verification/renderer-features/ssr-radiance-owner.txt)
and [full-resolution tracing](../../assets/verification/renderer-features/ssr-full-resolution-tracing.txt)
records keep their commands, visual and cost limits. Native Vulkan output
checks are pending (checklist).

Analytic fog shares `VkrFogParams`, three `float4` values (48 bytes), between
native passes: colour and density, height and distance limits, and sky lighting
with anisotropy. The frame roots address it at byte 504 on Metal and byte 568 on
Vulkan. Vulkan's `Fog.Apply` 8x8 compute root is 176 bytes and places the
parameter record, inverse view-projection, camera position, depth, target,
extent, sky record, aerial descriptor and frame root at bytes
0/48/112/128/132/136/144/152/160. The tiled pipeline applies the same kernel in
`Tiled.Atmosphere` and its blend fragment (ADR-087, decisions 6 and 8). The
shared kernel owns the Henyey-Greenstein phase and the sky-lit in-scatter;
native helpers derive the sun from the frame's directional light and the sky
light's average from the published SH. The shared froxel regression checks that
arithmetic on the CPU. Emitted SPIR-V reflection and Release compile-command C
syntax checks pass; the retained diagnostic is
[retained fog-spirv diagnostic](../../assets/verification/renderer-features/fog-spirv.txt).
On the tiled pipeline a Bistro capture with height fog shows the expected haze
and sky (ADR-087). Native Vulkan fog output is not recorded.

Extended-linear output is shared and implemented under
[ADR-061](061-extended-linear-display-output.md). The shared 16-byte display
record carries headroom, native output scale and the selected extended-linear
flag. Final, UI and editor-overlay roots are affected; world text remains
scene-linear. At that change the Metal roots were tonemap 48B, UI 48B and
overlay 112B, and the Vulkan roots fullscreen 576B, UI 64B and overlay 128B (UI
sizes as of the 2026-09-25 vertex change). Generated SPIR-V validates the 16B
parameter offsets 0/4/8/12 and root strides. Native Metal reflection, 36-swatch
FP16 output, one-time UI scaling, format transitions and API validation pass.
The editor composite preserves pre-encoded highlights without a second scale or
SDR clamp. Maximum numeric error is 0.003899. Native Vulkan EDR output needs an
HDR display on Windows and is not recorded.

SSGI is desktop-only. It shares a 288-byte parameter record and has five
phases: depth base/mips, trace, temporal, and composite. Bytes 280/284 carry
`history_jitter_uv_x/y` in the former unused tail. Deferred lighting writes the
isolated direct/emissive source only when SSGI is enabled; its root retains
existing solar fields and appends the direct-source identifier and enable flag,
producing a 160-byte Vulkan root.

The implementation uses a 256-phase Hammersley trace sequence and 3×3 raw
bilateral filter over nearest covered receivers. Valid misses remain zero samples
in that normalized average. History selection proves the exact
temporal-transform instance, submit, frame, scene and compatible retained tuple.
That same-queue predecessor may be in flight through existing barriers; no other
in-flight tuple is eligible. Reprojection adds the producer's previous-minus-
current raster jitter to unjittered motion on the raw grid. FSR uses its active
phase count and no-TAA keeps zero offsets.
Four bilinear color/depth/identity taps validate each depth and identity before
resolving RGB with bilinear × history-confidence weight. The four taps add nine
history texture accesses over the former single tap, with no new images or rays.

Release builds, all ten SSR/SSGI SPIR-V modules, compiled parameter offsets and
Vulkan host syntax pass. The
[correction record](../../assets/verification/renderer-features/ssgi-ssr-history-correction.txt)
retains the reported-view comparisons, made on the removed Metal desktop
implementation, and their limits. Native Vulkan evidence is the 2026-09-12
Release emission and Bistro runs and a Debug validation run
([ADR-060](060-screen-space-diffuse-indirect-lighting.md), which owns the
feature policy and acceptance evidence).

Froxel volumetric fog is desktop-only and uses a 944-byte `VkrFroxelFogParams`
record. Fields through byte 799 retain their existing offsets; unjittered
current view-projection and jittered inverse raster view-projection append at
bytes 800 and 864, and the sky-lighting vector at byte 928. The phase lane
carries the Henyey-Greenstein anisotropy that the injection pass applies per
light. The Vulkan frame root holds the parameter address, integrated
descriptor and sampler at bytes 576, 584 and 588. The graph defines
frame-slot-count plus two completion-gated RGBA16F 3D scattering-history
instances and one transient integrated volume per frame slot: four plus two in
the current renderer, five plus three in the approved three-slot budget.
History holds local scattering/extinction only; camera-integrated values are
current-frame data. Actual Vulkan SPIR-V reflection/validation and eight
affected host translation units compile successfully. Native Vulkan output is
not recorded. [ADR-059](059-froxel-volumetric-fog.md) records the implemented
scope and evidence.

Sky atmosphere is shared and uses shared 128-byte parameters and three cold
compute stages; the direct light's observer irradiance is a CPU integral, so no
stage writes a readback. Its native bake root is 192 bytes on Metal and 176
bytes on Vulkan, with the scalar tail at byte 168 and 152; the lookup texture
references name the generation's own textures. Both source stages add the
sunlit Lambertian ground for below-horizon rays and write no disc coverage.
Packet version 50 carries the published medium lit by the scene's current sun
and the published lookups; the
shared 352-byte `VkrSkyParams` record adds the camera altitude, the unjittered
view-projection pair, the camera position with kilometres per world unit, the
aerial constants and the 64-byte `VkrCloudParams` tail. Native sky records
embed it and append texture references: 416 bytes on Metal and 400 on Vulkan,
addressed from byte 536 of the Metal frame root and byte 616 of the Vulkan
frame root, whose sizes are unchanged. Vulkan sky-record slots name the
sampled heap; an aerial-perspective slot formerly taken from the storage heap
is corrected. Both sky builders use a 16-byte root. The Vulkan deferred
lighting root keeps its size and reuses the sky slot, flag and radiance vector
as the sky-view lookup, sky mode and constant radiance; the Vulkan fog roots
also address the aerial volume. Shared helpers own the sky-view mapping,
analytic disc, sun glow and aerial coordinates; native files own every sample
and write. The sky record carries the glow coefficient in
`atmosphere.mie_extinction.w`, a lane the bake leaves zero.
Actual Vulkan SPIR-V validation and layout reflection pass in
[the retained diagnostic](../../assets/verification/renderer-features/atmosphere-spirv.txt).
Metal startup reflection, API validation and Bistro captures pass, and the
sky-view lookup agrees with the removed cube sky within 0.65% over open sky.
Vulkan renders the sky natively (see
[Moon and star evidence state](#moon-and-star-evidence-state)).
[ADR-058](058-revision-baked-sky-atmosphere.md) owns the model.

Volumetric clouds are shared and share `shared/cloud_kernel.slangh`: tileable
noise synthesis, density shaping, the phase and multiple-scattering octaves,
step integration, the layer segment, screen and sun-projected coordinates and
the history blend. Native files own the march loops and every sample and
write. Noise synthesis uses one root (32 bytes on Metal, 16 on Vulkan), the
shadow builder the 16-byte sky builder root, and the trace a 128-byte root on
both backends with the previous canonical view-projection, sky and frame
addresses, depth, history and output textures, extents, frame index and
history flag. Metal startup reflection validates the three roots and the
embedded cloud record; Vulkan SPIR-V validation and layout reflection pass for
the five cloud modules and every sky-record consumer in
[the retained diagnostic](../../assets/verification/renderer-features/clouds-spirv.txt).
Metal API validation and Bistro captures pass, and Vulkan renders the moonlit
clouds natively. [ADR-074](074-volumetric-cloud-layer.md) owns the model.

The cloud-lit sky light adds march, projection and chain kernels on each
backend that share one root (64 bytes on Vulkan, 80 on Metal, whose source is
a cube texture reference). The shared kernel owns the composite formula
`vkr_cloud_sky_light_radiance`, the chain layout, the direction-to-face
inverse of the cube convention and the prefilter-to-chain level mapping.
Both backends project through one loop that also serves the revision bake
(`vk_sh_project`, `vkr_metal_sh_project`). The sky record gains the clear SH
slot, the chain's face size and its address: Vulkan grows to 496 bytes with
the address at 480, and Metal to 512 with the address at 504. Global
prefiltered specular composites the chain in `world/default.slang` and in
`world/lighting.metalh`. Vulkan Release execution and Debug synchronization
validation pass on the Windows host; native Metal execution of these kernels
is not recorded.

AgX and grading are shared production kernels. Metal's post root carries a
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
contract. The corrected filter passes Vulkan Release Bistro motion and
stationary captures with FSR and motion captures with portable TAA. The
bounded Debug static-reset case loads Khronos synchronization validation and
reports no API warnings or errors; ADR-052 records the small, mixed
motion-quality changes and local cost. Native Metal evidence of the filter on
the tiled pipeline is not recorded.

Exposure requires complete histogram groups and GTAO requires mip-selecting
depth sampling under ADR-042.

Direct-light helpers reject noncontributing light hemispheres before half-vector
construction. Shared GGX math defines zero contribution when the half-vector's
squared length is zero or subnormal; finite back-view diffuse behavior remains
unchanged. Local probe influence bounds remain active independently of parallax
projection. Bloom gain follows ADR-042's maximum-chain normalization.

Audit remediation changes visibility, HZB producer-grid metadata, Metal reset
ordering, optional resolve outputs and numerical edges. Its Vulkan evidence is
in [ARCHITECTURE](../ARCHITECTURE.md#remaining-implementation-and-evidence-boundaries);
analytical oracles do not substitute for native execution.

The 2026-09-24 shader audit changes the SSR/SSGI cell restart, layered light
traversal, sheen view-visibility preparation, GTAO denoise texel loads, Vulkan
resolve material-row reuse, motion-blur extent queries and the
surface-diffusion same-row path, all on the desktop pipeline. ADR-055 records
the restart's measured effect and ADR-062 the coat-shadow correction, both on
the removed Metal desktop implementation. Vulkan compiles and all 95 SPIR-V
modules validate; native Vulkan before/after captures are pending (checklist).

The display-linear post target is the default whenever FXAA or sharpening
filters the final draw ([ADR-043](043-presentation-dpi-and-color-transfer.md)).
FXAA filters only desktop frames without temporal reconstruction; the frontend
decides it once per frame, and the tiled class draws no FXAA. The Vulkan path
has compiled SPIR-V but no recorded native execution.

## Windows Vulkan native feature sweep

On 2026-09-12 an AMD Radeon RX 6700 XT, driver 26.6.3 and Vulkan API
1.4.315 completed focused Debug synchronization-validation resize profiles for
clearcoat, sheen, anisotropy, thin-sheet diffuse transmission, depth of field,
motion blur and profiled surface diffusion. Report IDs are
`20260912T104809.336Z-001bf5`, `20260912T104830.342Z-00367b`,
`20260912T104851.122Z-00302b`, `20260912T104911.922Z-003cff`,
`20260912T104932.786Z-0034d3`, `20260912T104951.620Z-002541` and
`20260912T111336.086Z-003163`. Child logs confirm Khronos validation with
synchronization checks and contain no API errors.

The sweep also passes Release portable-TAA reference and Vulkan FSR static and
motion snapshots. SSGI has separate Release emission/Bistro and Debug validation
witnesses recorded in ADR-060. These local dirty-tree runs establish bounded
native Vulkan execution; output checks of these features remain open on
Vulkan.

## Post-reconstruction depth of field

Depth of field is desktop-only. Release compilation and compiled SPIR-V
layout/validation pass, and the Windows sweep above passes a Debug resize.
Params are 48 bytes and Vulkan roots 80 bytes.
[ADR-066](066-post-reconstruction-depth-of-field.md) owns the image, quality
and transparency contract. Native Vulkan output checks are pending.

## Post-reconstruction motion blur

Motion blur is desktop-only. [ADR-067](067-post-reconstruction-motion-blur.md)
records the accepted 32-sample, 16-pixel-radius contract. Compiled Vulkan
root/binding and dispatch checks pass, and the Windows sweep above passes a
Debug resize. Native Vulkan output checks are pending.

## Profiled surface diffusion

Profiled surface diffusion is desktop-only.
[ADR-068](068-profiled-surface-diffusion.md) records the implemented 32-sample,
32-pixel-radius surface approximation and ownership. Reflection and `spirv-val`
pass for the actual gather, deferred and SSGI composite modules: parameters are
32 bytes, gather roots 192 bytes, material rows 288 bytes, deferred roots 192
bytes and SSGI roots 432 bytes. The Windows sweep above passes a Release
snapshot and a Debug resize. Its five numeric output fixtures and checker ran
on the removed Metal desktop implementation; Vulkan output checks are pending.
