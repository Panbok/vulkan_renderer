---
status: implemented
updated: 2026-10-23
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
  layers, FSR 3.1 native antialiasing, SMAA 4x and T2x, GTAO, depth of field, motion blur, froxel
  fog, profiled surface diffusion and the clearcoat, sheen, anisotropy
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
  - The Metal frame root carries `lightmap` at byte 544, the address
    of a 320-byte `VkrMetalPacketLightmap` (its lamp-direct fields from byte
    96 and two 80-byte baked lamp rows from byte 160,
    [ADR-108](108-baked-lamp-direct-pages.md)), `terrain_materials` at byte 552
    and `transmission_materials` at byte 560; the tiled sky root is 224
    bytes, with the sun light at byte 184, the contact march's noise index,
    depth linearization and flags at bytes 192 to 207, and the SDF trace
    with its start and band at bytes 208 to 223. Both are pinned in
    `vkr_metal_packet_abi.c` and checked against reflection at pipeline
    creation.
  - The frame root carries `decals` at byte 568, its former tail padding:
    the address of a 48-byte `VkrMetalPacketDecals` with 80-byte
    `VkrMetalPacketDecal` rows and the decal grid's cell masks, or zero on
    frames without decals. MSL reads the rows; the Slang mirror keeps their
    address ([ADR-092](092-projected-decals.md)).
  - The frame root's `texture_lod_bias` at byte 444, formerly reserved,
    holds the tiled texture mip bias under MetalFX upscaling, zero
    otherwise. The MetalFX path adds four roots, pinned in
    `vkr_metal_packet_abi.c`: `VkrMetalTiledMotionRoot` (96 bytes),
    `VkrMetalPacketTemporalDrawState` (160 bytes) for the object motion
    draws, `VkrMetalPacketTemporalTransformRoot` (32 bytes) and
    `VkrMetalTiledStabilizeRoot` (64 bytes)
    ([ADR-121](121-tiled-temporal-upscaling.md)).
  - The frame root grew to 592 bytes for Custom material graphs
    ([ADR-096](096-custom-material-graphs.md)): `custom_materials` at byte
    576 addresses the 256-byte `VkrMetalPacketCustomMaterial` rows, and
    `custom_time` at byte 292 takes the first reserved point-light word. The
    GPU draw root's `custom_compaction` at byte 184, its former reserved
    word, addresses the frame's 4,240-byte `VkrMetalCustomCompaction`.
  - The GPU draw root grew to 240 bytes for two-phase camera occlusion
    ([ADR-128](128-tiled-two-phase-occlusion.md)): `late_view` at byte 224
    and `classify_view_base` at byte 228, then two reserved words. The late
    phase's root points `custom_compaction` at the late view's own table.
    Native evidence is in ADR-128; the kernels have no Vulkan consumer.
  - The Standard draw kinds ([ADR-129](129-tiled-standard-draw-kinds.md))
    add function constants 2 and 3 to the tiled forward fragments, which
    compile out the lightmap path and the terrain layers when set, and grow
    `VkrMetalCustomCompaction` to 35 slots per plane (8,992 bytes) with a
    `kind_ready_mask` in its former reserved word. Classification bit 6
    marks a kind draw. Native evidence is in ADR-129; no Vulkan consumer.
  - The tiled resolve kernel leaves in the resolved colour's alpha the share
    of each pixel's resolve weight that surfaces hold, as the sky writes
    alpha zero. The atmosphere draw reads it through programmable blending
    to split pixels mixing sky and surfaces and writes alpha one, and the
    Metal tonemap presents alpha one (ADR-087, decision 6). No Vulkan shader
    reads it.
  - The prepared instance row carries the lightmap slot in
    `normal_column2.w`; every shader on both backends reads only its xyz.
  - The Metal sky background helpers, `vkr_metal_packet_sky_clear`,
    `vkr_metal_packet_sky_discs` and `vkr_metal_packet_sky_background`,
    serve the tiled sky and cloud draws.
  - The tiled blend fragment composes glass with the shared
    `vkr_transmission_compose` and returns the factor the destination keeps
    as its second, dual-source output; the Vulkan transmission kernels use
    the same composition. A glass volume refracts, and glass whose material
    sets `VKR_METAL_PACKET_MATERIAL_ROUGH` blurs, through the shared
    `vkr_transmission_exit_point`, `vkr_transmission_project_uv` and
    `vkr_transmission_rough_lod`, sampling a half-resolution copy of the
    image through the frame root's `transmission_source` at byte 424, which
    is null on frames without one.
  - The tiled shading calls the shared local-light loop and rectangle-light
    path without the coat and sheen layers. Its local shadows take a
    four-comparison tent and read no refractive layers
    (`vkr_metal_packet_local_shadow_sample<false, false, true>`), where the Vulkan
    receivers filter with the Poisson disk and read the transmission layers.
    The harder tiled shadow edge is a class difference the owner accepted
    (ADR-087, decision 11). Its sun cascades filter with a fixed grid of
    bilinear compares (`vkr_metal_shadow_grid_kernel`) instead of the shared
    rotated Poisson table, because the class has no temporal resolve; the
    shared receiver-plane gradient and penumbra bounds keep their meaning.
    Local and moving-caster shadows offset along the geometric normal.
    Native evidence: Metal Release headless-editor captures of the Level
    Design Test and Bistro on 2026-10-16; no Metal validation run. Tiled
    rectangle lights skip rows below a
    contribution bound through the `ContributionCutoff` parameter of
    `vkr_metal_packet_layered_rectangle_lights`; a receiver that no row faces
    skips the LTC table reads, which changes no result.
  - The tiled `CASTERS` and `CASTERS_ALL` variants
    (`vkr_metal_tiled_moving_shadow`,
    [ADR-108](108-baked-lamp-direct-pages.md)) subtract from lightmapped
    irradiance the light that moving casters hide from the selected baked
    lamps, at most two, `L · max(v_static − v_composite, 0)`, as the Vulkan
    `Shadow.LocalMask` baked-lamp variants do (ADR-104), through the tiled
    tent instead of the Poisson disk. Both skip a face whose static and
    composite views name one square and stop where the static lookup is
    fully shadowed, as the mask does. Native evidence: Metal Release
    captures of the Level Design Test scene (a capture pair, and the
    warehouse before and after on 2026-10-15); no Metal validation run.
  - GPU culling applies the static/dynamic caster filter to every view on
    both backends (`deferred.slang` before applied it to local views only),
    so the cascades' dynamic-caster views of ADR-041 take only dynamic
    casters. Native evidence: Metal only; the Vulkan change compiles.
  - Frames that correct the sun's light (ADR-087, decision 6) specialize the
    tiled forward, sky and atmosphere entries with function constant 1
    (`vkr_tiled_sun_output`): the forward and sky entries return
    `VkrMetalTiledForwardOutput`, whose second output is the sun's light
    without the cascades and their visibility, and Custom graphs' generated
    entries return the same record (`vkr_material_codegen_msl_entries`). The
    SDF sun shadow prototype adds Metal-only kernels in
    `world/sun_shadow.metal` with two roots pinned in
    `vkr_metal_packet_abi.c`, `VkrMetalSdfGenerateRoot` (96 bytes) and
    `VkrMetalSunShadowRoot` (192 bytes), and 80-byte `VkrMetalSdfObject`
    rows. The march takes the shared
    `vkr_sun_contact_length` and `vkr_sun_contact_strength`
    (`shadow_kernel.slangh`) and the local-light march's start offset, noise,
    occlusion test and end fade (`local_shadow.slangh`). The Vulkan
    `Shadow.SunContact` pass marches with the same helpers once per 2x2
    block (`vk_sun_contact`, 128-byte `VkrVulkanSunContactRoot`) where
    ray-query sun shadows do not cover the pixel, and deferred lighting
    upsamples it by depth (`vk_deferred_sun_contact`); its start offset also
    follows the sun. Native evidence:
    Metal Release Bistro captures and the forced-visibility comparison on
    2026-10-16; no Metal validation run.
  - Sun shafts ([ADR-132](132-tiled-sun-shafts.md)) add the Metal-only
    kernels `vkr_metal_tiled_sun_shafts_source` and
    `vkr_metal_tiled_sun_shafts` (`world/sun_shafts.metal`), which share the
    176-byte `VkrMetalTiledSunShaftsRoot` pinned in
    `vkr_metal_packet_abi.c`, and grow `VkrMetalTiledSkyRoot` from 224 to
    256 bytes (`sun_shafts` at 224, density 232, sky share 236, tint and
    path cap 240). The atmosphere draw adds them only in its variant with
    function constant 4 (`vkr_tiled_sun_shafts`). The march takes the shared
    Henyey-Greenstein phase, `vkr_fog_phase_hg`, with the volumetric fog
    component's anisotropy, as froxel fog does on the desktop pipeline, its
    start offset from `vkr_local_shadow_contact_noise`, and the cloud shadow
    from `vkr_metal_packet_sky_cloud_shadow`, which
    `vkr_metal_packet_cloud_shadow` now calls with the frame's sky record.
    Native evidence: Metal Release Bistro captures and timing on
    2026-10-22, the fog-off capture identical to the one before the change;
    no Metal validation run.
  - The tiled class draws no SMAA; its alpha-tested surfaces use alpha to
    coverage and its opaque pass a tone-mapped tile resolve
    (`vkr_metal_tiled_resolve_tile`), a class difference the owner accepted
    (ADR-087, decision 6).
  - The Metal tonemap root's last 12 bytes carry `bloom_intensity` and the
    `bloom` texture; with `VKR_METAL_PACKET_TONEMAP_FLAG_BLOOM` the pass adds
    that bloom level to its scene-linear samples, rounded to FP16 as
    `Post.Bloom.Combine` stores it. The desktop graph keeps
    `Post.Bloom.Combine` on Vulkan.
- **Metal evidence.** Pipeline creation validates the new layouts. The
  Bistro street view (`tiled_bistro_capture`), the café windows
  (`tiled_bistro_glass`) and a lightmap-baked Bistro
  (`tiled_bistro_baked_capture`) render; the baked street view with
  projected decals (`tiled_bistro_decals_capture`) renders, also with Metal
  API validation. The tiled lighting variants render
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
bytes and deferred roots 192 bytes; the deferred root borrows the existing
visible-draw buffer at graph binding 11, without new images.
[ADR-065](065-thin-sheet-diffuse-transmission.md) owns the split, input
restrictions and offline transport. The 2026-09-12 Windows sweep passes a Debug
synchronization-validation resize; Vulkan output checks of front/back energy,
tint, absorption, shadow occlusion and cutout coverage are pending.

## Custom material graph evidence state

Custom material graphs ([ADR-096](096-custom-material-graphs.md)) run only
on the tiled pipeline. Their generated MSL instantiates the tiled forward
template with `VkrMetalCustomSurface`, and the GPU draw kernels bucket the
camera's Custom draws per graph. The host rows, the 592-byte frame root and
the compaction table are pinned in `vkr_metal_packet_abi.c` and
`vkr_metal_packet_abi.h`. On 2026-10-09 the Metal tiled pipeline:
- drew eight Custom graphs on Bistro brushes with no late draw after
  creation;
- matched a Standard graph bit for bit in the Unlit view;
- passed Metal API validation.

Vulkan draws each Custom material's Standard fallback. Its classification
and per-graph resolve wait for a Windows host.

## Terrain layer evidence state

The layer blend of terrain materials
([ADR-084](084-agent-channel-and-level-design-toolkit.md#terrain)) and
layered Standard materials ([ADR-095](095-layered-standard-materials.md)) is
shared. The Vulkan G-buffer resolve and the Metal tiled forward shader call
the shared `terrain_kernel.slangh` for the mask weights
(`vkr_layer_mask_weights`: vertex colour, mask texture, slope or height),
per-layer surfaces and the blend; each samples its own terrain segment.
Common material rows are unchanged. The terrain segment rows are 240 bytes on
Metal and 176 on Vulkan since ADR-095 added the mask texture, sampler,
source and range. On 2026-10-09 the Metal tiled pipeline showed the slope,
height and texture masks on Bistro brushes and passed Metal API validation;
Vulkan native execution and `spirv-val` of the changed resolve modules wait
for a Windows host.
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
  - Bloom parameters carry `karis_scale`, E/P, which takes pre-exposed
    luminance to exposed luminance for the Karis weight.
  - The cloud trace root carries `history_pre_exposure_scale` on both
    backends; on Vulkan so do the TAA and froxel inject roots, and TAA also
    carries `pre_exposure`.
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

## Reversed camera depth evidence state

[ADR-109](109-reversed-camera-depth.md) reverses camera depth in GPU
culling and the HZB, the editor grid and clouds (shared), the tiled
forward, atmosphere and picking passes (tiled), and fog, froxel fog, depth
of field, motion blur, surface diffusion, transmission peeling and TAA's sky
history (desktop). Shadow kernels are unchanged.

- **Metal (tiled).** Bistro snapshots before and after match within
  isolated silhouette pixels, with identical sky masks, linear depth and
  work counts; the HZB rejects the same draws over a 300-frame orbit; the
  editor grid and `grid.fit` match (ADR-109 Evidence).
- **Vulkan (desktop).** The changed modules compile and pass `spirv-val`.
  No native run exists. Missing gate: a Windows Release
  `bistro_snapshot` against a before run of the same build, plus a focused
  Debug validation run with fog, depth of field, motion blur and
  transmission on.

## Camera-relative sky ray evidence state

Sky, cloud and aerial-perspective view directions come from
`vkr_atmosphere_view_rays`: the inverse of the projection times the view's
rotation, without the camera translation. It maps the near-plane clip
position (x, y, 1, 1) to the unnormalized world direction from the eye in
xyz with a positive w; an orthographic projection maps every position to its
forward axis. Unprojecting the near plane, 0.1 m from the eye, through the
float32 world inverse and subtracting the eye lost the direction's
precision hundreds of metres from the origin, so the discs, stars, glows and
clouds jittered as the camera moved. The change is shared (clouds and aerial
perspective), tiled (the sky and atmosphere draws) and desktop (the deferred
background and TAA's sky history).

- **Changed contracts.**
  - The shared `VkrSkyParams` record replaces `inverse_view_projection` with
    `view_rays` at byte 224; its size stays 432 bytes.
    `vkr_cloud_view_direction` and both aerial-perspective kernels read it.
  - The Metal tiled sky root adds `view_rays` at byte 80 and grows to 192
    bytes; `inverse_view_projection` stays at byte 16 for the resolved
    surfaces' media.
  - The Vulkan deferred lighting root appends `view_rays` at byte 256 and
    grows to 320 bytes; `inverse_view_projection` still reconstructs
    surfaces.
  - `vkr_temporal_sky_reprojection` moves the current eye to the origin in
    float64 before inverting; its result is unchanged in exact arithmetic.
- **Missing gates.** Metal shader library compilation, startup reflection
  and a Bistro capture on the tiled pipeline; Vulkan module compilation,
  `spirv-val`, reflection and a Windows Release capture on the desktop
  pipeline.

## Cloud history read evidence state

The cloud trace read its history through one bilinear sample and rejected it
only when the filtered alpha exceeded one, so an untraced tap (alpha 2)
blended with traced taps passed and leaked its marker into radiance and
transmittance. The change is shared (`vkr_cloud_history_footprint`,
`vkr_cloud_history_resolve` and `VKR_CLOUD_TRACED_ALPHA_MAX` in
`shared/cloud_kernel.slangh`) and read by both trace entries.

- **Changed contracts.**
  - Each trace gathers the four channels at the footprint's shared corner,
    `gather` with a component on Metal and `GatherRed` through `GatherAlpha`
    on Vulkan. Both return the taps in the same order, and the corner fixes
    the footprint whatever the sampler's sub-texel precision.
  - The footprint takes the history texture's own extent. The texture is not
    required to match the output size.
  - Untraced taps get no weight, and the traced taps renormalise; a footprint
    without a traced tap returns the marker, and the blend rejects it.
  - Roots, bindings and samplers are unchanged.
- **Missing gates.** Metal shader library compilation and a Bistro capture on
  the tiled pipeline; Vulkan module compilation, `spirv-val` and a Windows
  Release capture on the desktop pipeline.

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

The artist views of [ADR-099](099-artist-views-and-luminance-queries.md)
(normals, base colour, roughness, metallic, material cost, texel density and
exposure false colour) add the mode predicates and the cost, density and
stop ramps to `editor_view.slangh`. Only the Metal tiled inspection variant
and tonemap consume them; the Vulkan build compiles the kernel, and its
views wait for a Windows host. On 2026-10-09 the tiled pipeline drew all
seven views on Bistro and passed Metal API validation.

On the desktop pipeline the three inspection modes bypass bloom,
analytic/froxel fog, subsurface diffusion, depth of field and motion blur,
and Wireframe also bypasses temporal reconstruction and GTAO.
Orthographic frames have a separate per-frame spatial capability boundary
recorded in [ADR-046](046-editor-viewport-mapping-and-picking.md).

Native evidence: on Windows (RX 6700 XT), the Release Vulkan snapshot
`smoke.bistro.editor.views` passes and captures all four channels, and the same
case runs clean under Debug Vulkan validation. On the M1 Pro, tiled-pipeline
captures of Unlit, Detail lighting, Lighting only and Wireframe render (ADR-087,
Unbaked lights, terrain, inspection and atmosphere). No frame-budget claim is
made.

### Shadow views and Pass output

The shadow views (`VkrGpuDebugPayload.shadow_debug_mode`: 1 the cascade that
shades a surface, 2 its shadow factor, 3 the map's stored depth) take their
colour from `vkr_editor_shadow_debug_color` in
[`shared/editor_view.slangh`](../../renderer/src/shaders/shared/editor_view.slangh);
the Vulkan forward and deferred consumers call it in place of their own
palette, which it reproduces. The Metal tiled inspection variant draws them
from `vkr_metal_packet_directional_shadow_sample` with the geometric normal,
as shading does, and the tonemap shows them as data views. The editor's
render menu and Cmd `view.mode` (`shadow-cascades`, `shadow-factor`,
`shadow-depth`) select them on both pipelines.

The Pass output view (`VkrGpuDebugPayload.pass_output`,
`vkr_pass_output_source`) draws one graph image over the editor's Scene image
in pass `Editor.PassOutput`, after `Editor.Resolve` in both graphs. The pass
reads `${pass_output_image}`, the frame's chosen image, whole; a frame whose
graph lacks it reads nothing and the pass draws dark stripes. The grid,
selection outline and overlay stay off while it shows.
[`shared/pass_output_kernel.slangh`](../../renderer/src/shaders/shared/pass_output_kernel.slangh)
owns the texel mapping (the source fitted at its own aspect) and the colour of
each `VkrPassOutputKind`: exposed and Reinhard-mapped radiance, stored colour,
octahedral normals, reversed camera depth and linear view depth on a squared
log scale to 1 km, shadow depth, vector magnitudes, occlusion alpha and
scalars. It also owns `vkr_octahedral_decode`, which the desktop G-buffer
decode calls. The Metal `editor/pass_output.metal` and Vulkan
`editor/pass_output.slang` entries own the reads and the display encoding.
The tiled pipeline keeps its G-buffer, occlusion and per-sample data in tile
memory, so it offers the images its graph stores: scene radiance, depth, the
four shadow cascades, the sun shadow trace, motion vectors and bloom.

Native evidence: on 2026-10-18 the tiled pipeline drew the three shadow views
and every tiled Pass output source on Bistro in a headless Release editor, live
and during a recording's playback, with no ABI mismatch; a source the frame
lacked (the sun shadow trace) drew stripes. The desktop build compiles both
Vulkan entries, `spirv-val` accepts `packet.pass_output.frag.spv`, and its
reflected root offsets match `VkrVulkanPassOutputRoot`. On the Windows host
(RX 6700 XT, Release at 69b3f530) the desktop pipeline drew the shadow views,
the seven desktop modes and the Pass output sources on Bistro with no Khronos
validation message. Under FSR 3.1 no pass writes `temporal_history_accept`,
so History accepted read an unwritten image; the image now exists only while
FSR is off (`!fsr31_enabled`), and that view draws stripes under FSR.

### View export

View streaming ([ADR-131](131-view-streaming.md)) has one kernel per
backend. They share the resample and the sRGB encode but not the output:
VideoToolbox takes a BGRA surface and converts it, and Vulkan Video takes
only 4:2:0 YCbCr, so the Vulkan kernel converts as well. Each is the only
consumer of its own source; nothing in `shaders/shared/` serves both.

On Metal, `vkr_metal_packet_view_export` in
[`editor/view_export.metal`](../../renderer/src/shaders/metal/msl/editor/view_export.metal)
resamples `editor_scene_image` into an
IOSurface-backed BGRA8 surface in the frame's readback encoder, after the
barrier that waits for the Scene image's last writer: one thread per surface
pixel in 8×8 groups, one bilinear sample at the pixel's centre (each Scene
pixel exactly when the sizes match), clamped to [0, 1] and sRGB-encoded
unless the Scene image is 8-bit UNORM and already holds display values; an
EDR Scene image loses its range above 1. `VkrMetalPacketViewExportRoot` (32
B: source, destination, extent, `source_encoded`) is checked against the
reflected root like every other record. One dispatch runs per Scene frame,
only while a client views.

Native evidence: on 2026-10-22 `tools/checks/check_view_stream.py` streamed
a headless Bistro Scene (RGBA8 sRGB, 1274×700) on the M1 Pro; the client's
decoded image was 34.4 dB PSNR from the server's own capture on the plaza
view and 43.1 dB on a wall, and Metal API validation reported nothing on the
server or the client. A windowed BGRA8 sRGB source and an RGBA16F EDR source
have not run.

On Vulkan, `vk_view_export` in
[`editor/view_export.slang`](../../renderer/src/shaders/vulkan/slang/editor/view_export.slang)
runs after the graph, which leaves the Scene image in its final layout; the
backend samples it in `SHADER_READ_ONLY_OPTIMAL` through its bindless slot
with the linear clamp-to-edge sampler and returns it to that layout. One
thread writes a 2×2 block of the encode picture, in 8×8 groups over half
the 16-pixel coded extent: four pixels sampled like the Metal kernel
(bilinear at each pixel's centre in the stream's even extent, clamped to
[0, 1], sRGB-encoded unless the source is 8-bit UNORM), stored as BT.709
video-range luma through the picture's R8 plane view, and the block's mean
R′G′B′ as Cb and Cr through its RG8 plane view. Pixels past the extent
repeat its last row and column. `VkrVkViewExportRoot` (48 B: source and
sampler slots, luma and chroma storage slots, extent, coded extent,
`source_encoded`) is pinned on the host by `_Static_assert` in
`vkr_vulkan_view.c`; it is not in the reflected-root checks.

Native evidence: on 2026-10-23 `tools/checks/check_view_stream.py --role
record` encoded a headless Bistro Scene (RGBA8 sRGB, 1275×700, streamed at
1274×700) on the RX 6700 XT, Release; PyAV decoded the recording, and the
frame nearest the server's capture was 41.5 dB PSNR from that capture
resampled to the stream as the kernel does (the last frame 33.9 dB, the
Metal plaza result's level). Runs under the Khronos validation layer, core
and synchronization, reported no message. A windowed or extended-linear
RGBA16F source has not run.

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

Vulkan compute entries read their dispatch root once with
`vkr_vk_uniform_root` (`common/resources.slangh`). It copies the root by value
and broadcasts each 32-bit word with `WaveReadLaneFirst`, so the fields stay in
scalar registers; helpers receive the root by value. Read through its
physical-storage pointer, the AMD compiler reloaded every root field with a
vector memory load at each use, including inside loops (driver ISA from
`VK_KHR_pipeline_executable_properties`, RX 6700 XT, driver 26.6.3). Slang's
`ConstBufferPointer` and `Ptr<T, Access.Read>` emit no `NonWritable`
decoration and do not change that code. Vertex and fragment entries keep
pointer reads: the same broadcast in the visibility vertex shader made
`VBuffer.Opaque` about 4% slower. On the Bistro orbit at 2560×1440 with TAA
(Release, local and non-authoritative, one run each, 2026-10-07), the GPU pass
sum fell from 16.68 to 14.98 ms and `Temporal.Resolve` from 3.41 to 2.03 ms
(reports `sha256:00023a09a58cd98c5837493ac487c4e06f1b0cf6765441329773129325b97c5e`,
`sha256:f3abae63cd90c291b3d24d909967556e73ff24445187acdc1547ac875b1a7ae5`).
With GTAO, SSR, SSGI, bloom, depth of field and motion blur also on (SSR and
SSGI were removed on 2026-10-12), it fell from 35.52 to 29.83 ms;
`SSR.Temporal` fell from 3.85 to 2.45 ms
(`sha256:b9c00e110fb02548e29bb7a753f3f740a14b5d569d9f95a2c031698e0994ce1b`,
`sha256:3d69e50a27b6b8afa2fda4717e81c36c662f566c87730528e92d9508502f75c3`).
Without SSR, moving-camera TAA captures of the old and new builds differed no
more than two runs of one build do. With SSR, the trace rounded differently:
its first captured frame differed in 69 pixels by at most 1.1e-4, and the SSR
and TAA histories spread that to about 1,000 final pixels by frame 30, 15 of
them above 2/255, within the harness capture thresholds.

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
with forward `-Z`; depth is `[0,1]`, reversed for cameras (near 1, far 0) and
forward for shadow maps ([ADR-109](109-reversed-camera-depth.md)), and
projection/viewport Y lowering is backend aware. Rounded dispatches retain guards required for valid edges. Material
normal decode reconstructs positive tangent Z; output transfer follows ADR-043.
A normal-mapped surface whose mesh tangent is zero or parallel to its normal
has no tangent frame. Since 2026-10-20 the desktop pipeline keeps the
interpolated normal there, and the clearcoat its geometric normal
(`vkr_vk_finish_surface_normal` and `vk_gbuffer_resolve` in
[`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)).
Before, the G-buffer resolve rejected the surface: one pixel of the High
Bistro street walk at 2560×1440 became background in frame 319 of every run,
and `visibility.gbuffer.resolve_invalid` read 1. Native evidence (Vulkan, RX
6700 XT, Release): the walk reads 0 and passes
(`sha256:367f17d723ceec73b79f55f45dc26c5fb476ebd1ee6dcbeb5dd1e2b3b54d0125`),
and `depth`, `gbuffer_normal`, `gbuffer_diffuse` and
`resolve_barycentric_lod` at its frame 79 match the build before the change
bit for bit. The tiled forward shader
([`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal))
and the shared `vkr_anisotropy_axis` still normalize such a tangent without
a guard; their result for that case has no native Metal evidence.

The frame input's source instance is 112 bytes, including a CPU skinning index. Native publication/upload
lowers it once into a 160-byte `VkrPreparedInstanceGPU` with three prepared normal-transform
columns. Their common positive scale preserves inverse-transpose direction under
normalization; the first column's w carries model handedness for mirrored tangent
bases. The second column's w carries an outward-rounded conservative affine
sphere stretch. Current and previous deformation addresses occupy bytes 128 and
136 of the prepared record; zero selects static geometry. Bytes 144 to 159
carry the lighting origin ([ADR-120](120-mover-lighting-origin.md)): a world
point with w one where a mover's draws take baked diffuse light and probes,
zero w for per-pixel lookups; the source instance carries it at byte 96.
Eight raster buckets partition material state by reflection parity; the shared
compaction record is 144 bytes. Direct draws preserve order through contiguous
parity runs. Shaders transform tangents with the model's linear part and normals with
the prepared columns. No per-pixel matrix inverse is required.

The Vulkan G-buffer root appends the sky reprojection matrix at byte 352 and
then the decal rows at byte 416 (ADR-092), the frame's
`VkrVulkanLightClusters` at byte 424 and the decal count at byte 448
(ADR-135), for a 464-byte size. The G-buffer owns sky motion for every temporal consumer;
portable resolve carries no duplicate reprojection matrix. The Vulkan temporal
resolve root carries current and previous pixel jitter at bytes 128 and 136
of its 160 bytes for canonical color reconstruction and raw metadata
validation, and the checked-scene flag at byte 124. It stores accumulation age
in the existing depth-history second channel and retains raw depth in the
first. Native content/revision eligibility and the portable signature gate the
static algorithm.

Vulkan FSR 3.1 ([ADR-052](052-vulkan-fsr31-upscaling.md)) was removed on
2026-10-12 with its prepare and stabilization roots and the SDK dispatch.
Vulkan renders the Scene at unit scale.

Presentation sharpening is shared: both production shader libraries use the
bounded cross-neighborhood arithmetic of `shared/sharpen_kernel.slangh`. Frame
version 32 carries `image_sharpness` in [0,1]; zero takes the existing path
without extra samples. The Vulkan utility root carries strength in
`point_light_grid_origin_cell_size.z` beside output extent in xy, with unchanged
native layout. Metal's tonemap root carries flags at byte 8 and float strength
at byte 12; exposure and extent remain at bytes 16 and 24, with the ABI
manifest updated. Vulkan Release captures covered FSR (removed on
2026-10-12), TAA and native presentation, both FXAA states,
zero/default/maximum strength and editor text. The enabled editor/text
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

Metal command-buffer demand growth places the views' commands one after
another in each ICB group's pool since 2026-10-18 (ADR-024): the prefix
kernel runs one thread per group, each view's commands start at its first
range (`execution_ranges[0].x`), which encode subtracts to index the view's
visible rows, and draws the pool cannot hold count as overflow. The root's
former `reserved_3` is `command_capacity`. Before, each view's commands
started at the view's index times the candidate count. Graph draw-table storage follows scene demand on
both backends through existing native capacity fields. Source capacity is
`C = pow2(max(candidate_count, 1))`. Metal's visible, classification and
argument stride is `V = 8 * min(C, 65536)`, which holds every candidate
(`VKR_GPU_DRAW_CANDIDATE_CAPACITY` is at most `8 * 65536`). Since 2026-10-16
its prefix gives each bucket its whole count after the previous bucket's
range, as the Custom ranges already did, instead of capping each bucket at
`V / 8` and dropping the draws that reached a full bucket last, which varied
with atomic order between frames. Metal supplies `V` to classification, raster and picking, and
its root layouts are unchanged. Since 2026-10-11 Vulkan strides each culling
view's tables by `C` and gives each bucket the slice of the frame's packed
candidates in that bucket: `VkrVulkanCullRoot.command_partitions` (first rows
and sizes, byte 208 of 272) feeds the prefix clamp, the encoded command
index, raster's argument offsets and `maxDrawCount`
([ADR-028](028-gpu-driven-deferred-visibility-buffer.md)).
The Metal command-capacity fixture with four shadow views and seven
transmission candidates passed API validation
(report SHA-256 `819dfb4ee766c027d61ee4144e01bd3e2112aa607304276e0476b419db8022a5`).
Its matched Release captures were byte-identical with SHA-256
`82510845bfe55d00ca57c4948579a0ebe367e8dd210f8f42fa48b9a2b49a7c28`.
On 2026-10-12 a Vulkan Release profile of a copy of `ssr_ghost_under_bar`
(an SSR case deleted with SSR the same day) under
`VK_LAYER_KHRONOS_validation`, with core and synchronization checks enabled by
a settings file, logged no message over the partitioned ranges
(`sha256:4c08ff5cb67021c8df1e94609156bf05e5486a33b681e4c45d6547187f6e971b`);
no layer status line confirmed that the synchronization checks ran.

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
tiled receiver takes a four-comparison tent and no transmission (ADR-087,
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
with its own 160-byte root: frame, G-buffer inputs, visible rows, inverse
view-projection, extent, the contact-shadow noise index at byte 112, the
temporal filter flag at byte 116 (nonzero under temporal reconstruction),
the baked-lamp shadow fields from byte 120 (ADR-104), the frame's overflow
tag at byte 140 and, at byte 144, `mask_lights`: bit i set for table light i
when it is shadowed and its range reaches the camera frustum (2026-10-11,
ADR-019). The kernel tests each light of the pixel's cluster list (ADR-135)
against it, so only those lights, all in the 128-row light head, take slots, and a pixel with none returns before its surface
loads unless the baked-lamp variant needs them. The 368-byte deferred-lighting
root reads the mask array at byte 160, the per-light contribution counters at
byte 184 (null when not measured), into which the deferred punctual loop adds,
per wave and light, the pixels' unshadowed contribution from the shared
`vkr_local_light_contribution` for local-shadow priority (ADR-019), and the
overflow tag at byte 352. Mask layers are per-pixel slots: the k-th shadowed
light in range of a pixel, in light traversal order, writes layer k with alpha
tagging its light index through the shared `vkr_local_shadow_mask_tag`. A
pixel with more such lights than the `VKR_LOCAL_SHADOW_MASK_SLOT_COUNT` slots
writes the frame's tag, 1 to 255 by frame, to the overflow layer past them
(`vkr_local_shadow_mask_overflow_mark`). The deferred-lighting kernels skip the
lights below the shared `VKR_LOCAL_LIGHT_CONTRIBUTION_CUTOFF` that the mask
gives no slot, and classify each 8×8 tile (2026-10-22): a tile with layer data
takes the layered kernel, one with an overflow mark but no layer data the
fallback kernel, both of which filter inline a light whose slot is past the
last or names another light, and every other tile the base kernel. The base
kernel compiles that inline filter out, since its registers alone cost 1.2 ms
of the 4.9 ms Bistro frame's lighting; it takes the slot that carries the
light's tag and shades a light no slot holds unshadowed, which only a range,
cone or cutoff test that differed at a boundary leaves. SMAA S2x's second
sample lights its listed pixels with the fallback filter in its own kernel.
Vulkan builds the mask kernel with and without
contact shadows and selects the variant from the payload's `contact_shadows`
(Ultra only). The shared `local_shadow.slangh` owns the contact-shadow step
count, length, noise, start offset, occlusion test and fade, the full and
temporal tap counts and the rotation of a Poisson tap; the mask's temporal
tap rotation is the per-pixel `vkr_shadow_kernel_rotation` of
`shadow_kernel.slangh`, which the directional receivers share. The receivers
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

The sampling pointer always names a record once the graph is prepared; a
frame without transmission layers gets one with extent zero and the
sentinel maps, which receivers skip. Adrenalin 26.10.1 on the RX 6700 XT
moved Froxel.Inject's loads of the record above its null test (the SPIR-V
loads it only after the test), so frames without layers faulted at VA 0x0
1.8k to 5.8k frames after the Level Design Test scene loaded (RGD,
2026-10-24). With the record always present, the same scene ran 58,177
frames in the Release editor without a fault.

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

Native entries pick the plane by the root's `plane`: 0 is XZ, 1 ZY and 2 XY
(front and back views), and the hosts translate the unprojection along that
plane's normal by the payload `height`, so side planes lie at the depth the
view is framed on. On 2026-10-08 the macOS Release editor drew the XY plane in
the Front view and the ZY plane in the Right view through Metal at about
x = 900 m and z = 400 m, and the XY plane through the origin with its red X
and green Y axes; the Vulkan `packet.editor_grid` stages pass `spirv-val`.
Native Vulkan execution of the XY plane and of the side-plane offset is
unavailable until a Windows Release run.
Since 2026-10-11 both native entries discard a pixel at or past the fade
end before the depth read, and a fully depth-hidden pixel before shading.
Both `fwidth` calls stay before every discard and the shared kernel is
unchanged, so no written pixel changes. The Vulkan stages pass `spirv-val`
(Slang lowers `discard` to `OpDemoteToHelperInvocation`, so any saving
depends on the driver). `grid.metal` has not been compiled since, and
neither backend has a capture comparison or a timing that isolates the
change.

Picking also returns the opaque device depth at the picked pixel, which the
editor's grid fit unprojects. On Vulkan the picking resolve root grows to 80
bytes (`depth_texture`, `depth_output` at 64) and writes the readback buffer at
offset 8 through its device address; Windows Vulkan fits Bistro's grid from
that depth. On 2026-10-11 every pick lost the device on an AMD Radeon RX 6700
XT (driver 26.6.3): the shader saw the `float *` address with its low word in
both halves, while the root's words held it correctly. `depth_output` now
points at a one-float struct; Release and Debug (validation clean) pick Bistro
meshes there with depths near 0.97 (forward depth, before
[ADR-109](109-reversed-camera-depth.md) reversed it; sky now reads 0). On Metal the tiled pipeline replays the opaque draws into the
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
| Terrain and layered material blend | Shared | `shared/terrain_kernel.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/world/deferred.slang` |
| Custom material graphs | Tiled | — | `metal/msl/world/tiled.metal`, `metal/msl/world/gpu_draws.metal`, generated `project_materials.metal` | — (draws the Standard fallback) |
| Tiled forward, atmosphere and blend | Tiled | shared material, light, fog, atmosphere and transmission kernels below | `metal/msl/world/tiled.metal`, `lighting.metalh`, `metal/msl/shadow/sampling.metalh` | — |
| Projected decals | Shared semantics, one implementation per backend | — | `metal/msl/world/tiled.metal` | `vulkan/slang/world/deferred.slang` |
| Light and decal frustum clusters (ADR-135) | Desktop | `shared/point_light.slangh` | — | `vulkan/slang/world/light_clusters.slang` (`vk_light_cluster_prepare`, `vk_light_cluster_bin`, `vkr_vk_light_cluster_header`), read by `world/default.slang` (`packet_punctual_layered`) and `deferred.slang` (`vk_local_shadow_mask`, `vkr_vk_apply_decals`); Vulkan native: Release captures of the Bistro lamp views and decal G-buffer byte-identical to the world grids it replaced |
| Visibility buffer, G-buffer resolve and deferred lighting | Desktop | `shared/gpu_draw.slangh` and the kernels below | — | `vulkan/slang/world/deferred.slang`, `picking/default.slang` |
| Texture demand ([ADR-024](024-shared-bindless-gpu-cores.md)): each material's required texture extent from its UV derivatives; one pixel of each 4x4 block every frame on the desktop pipeline, every pixel of one frame in 8 while the texture budget binds on the tiled pipeline | Shared | `shared/texture_demand_kernel.slangh` | `metal/msl/world/tiled.metal` (`vkr_metal_tiled_demand_word`, colour 2 of `VkrMetalTiledForwardOutput`, `vkr_metal_tiled_demand_tile`) | `vulkan/slang/world/deferred.slang` (`vk_gbuffer_resolve`, `VkrVkResolveRoot.texture_feedback`) |
| Planar reflection: mirrored view shaded forward, reflector lookup | Desktop | the material, light, shadow and probe kernels below | — | `vulkan/slang/world/deferred.slang` (`vk_reflection_vertex`, `vk_reflection_fragment`, `vk_planar_reflection_specular`), `world/default.slang` (`packet_world_shade_surface`) |
| Diffuse volume dynamic layer: BLAS decode, ray-query probe trace, runtime grid moments and relocation, lookup add, reflection probe relight ([ADR-119](119-dynamic-diffuse-volume-layer.md)) | Desktop, with ray queries | `shared/diffuse_volume_kernel.slangh`, `gpu_draw.slangh`, `punctual_light_kernel.slangh`, `sh_l2_kernel.slangh` | — | `vulkan/slang/world/volume_trace.slang` (`vk_volume_blas_decode`, `vk_volume_trace`, `vk_volume_clear`, `vk_volume_probe_relight`), `world/default.slang` (`packet_diffuse_volume_lookup`, `packet_probe_relight`); Vulkan native: Debug validation clean, Release capture and timing on Bistro; relight: Debug validation clean and a Release A/B against a relight-off catalog on Bistro |
| Ray-query sun shadows, the Ray traced shadows setting (`VKR_SUN_RAY_SHADOWS` and `VKR_SUN_CONTACT_SHADOWS` override): trace, filtered mask, temporal history, half-resolution sun contact march, lighting blend ([shadow quality](../proposals/shadow-quality.md#hybrid-prototype)) | Desktop, with ray queries; the contact march also without them | `shared/shadow_kernel.slangh`, `local_shadow.slangh` (contact helpers), `gpu_draw.slangh` | — | `vulkan/slang/world/sun_ray_shadow.slang` (`vk_sun_ray_shadow_trace`, `vk_sun_ray_shadow_resolve`, `vk_sun_ray_shadow_temporal`), `world/volume_trace.slang` (`vk_ray_triangle_uv`), `world/deferred.slang` (`vk_deferred_sun_shadow`, `vk_contact_shadow`, `vk_sun_contact`, `vk_deferred_sun_contact`, `vkr_vk_depth_view_geometric_normal`); Vulkan native: Debug validation (synchronization) clean on a Bistro plaza capture, Release captures, a strafe capture and timing on Bistro |
| Material/light math | Shared | `shared/normal_map_kernel.slangh`, `ggx_kernel.slangh`, `point_light.slangh`, `punctual_light_kernel.slangh` | `metal/msl/world/tiled.metal`, `lighting.metalh` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Clearcoat, sheen, anisotropy, thin-sheet diffuse transmission | Desktop | `shared/clearcoat_kernel.slangh`, `sheen_kernel.slangh`, `anisotropy_kernel.slangh` | — | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Transmission composition | Shared | `shared/transmission_kernel.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/world/deferred.slang` |
| Transmission layers | Desktop | `shared/transmission_kernel.slangh` | — | `vulkan/slang/world/deferred.slang` |
| Shadow receiver | Shared | `shared/shadow_kernel.slangh`, `local_shadow.slangh` | `metal/msl/shadow/sampling.metalh` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Local shadow mask and transmitting shadows | Desktop | `shared/local_shadow.slangh`, `local_shadow_transmission.slangh` | — | `vulkan/slang/world/deferred.slang`, `local_shadow_transmission.slang` |
| Baked diffuse volumes | Shared | `shared/diffuse_volume_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/world/lighting.metalh`, `tiled.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Reflection probe influence, priority share and lookup direction | Shared | `shared/reflection_probe_kernel.slangh` | `metal/msl/world/lighting.metalh`, `tiled.metal`, `gpu_draws.metal` | `vulkan/slang/world/default.slang` |
| Rectangle LTC | Shared | `shared/ltc_kernel.slangh` | `metal/msl/world/lighting.metalh`, `tiled.metal` | `vulkan/slang/world/default.slang`, `deferred.slang` |
| Analytic fog | Shared | `shared/fog_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/world/tiled.metal` | `vulkan/slang/post/fog.slang`, `world/default.slang` |
| Froxel volumetric fog | Desktop | `shared/froxel_fog_kernel.slangh`, `fog_kernel.slangh`, `punctual_light_kernel.slangh` | — | `vulkan/slang/post/froxel_fog.slang` |
| Screen-space sun shafts, the tiled realization of volumetric fog ([ADR-132](132-tiled-sun-shafts.md)) | Tiled | `shared/fog_kernel.slangh` (`vkr_fog_phase_hg`), `local_shadow.slangh` (march offset) | `metal/msl/world/sun_shafts.metal`, `world/tiled.metal`, `shadow/sampling.metalh` | — |
| IBL and SH | Shared | `shared/sh_l2_kernel.slangh`, `ggx_kernel.slangh` | `metal/msl/ibl/` | `vulkan/slang/ibl/` |
| Sky atmosphere and aerial perspective | Shared | `shared/atmosphere_kernel.slangh` | `metal/msl/ibl/atmosphere.metal`, `world/lighting.metalh`, `world/tiled.metal` | `vulkan/slang/ibl/atmosphere.slang`, `world/default.slang`, `deferred.slang`, `post/fog.slang`, `post/froxel_fog.slang` |
| Volumetric clouds | Shared | `shared/cloud_kernel.slangh`, `sh_l2_kernel.slangh` | `metal/msl/ibl/clouds.metal`, `ibl/sh_projection.metal`, `shadow/sampling.metalh`, `world/lighting.metalh`, `world/tiled.metal` | `vulkan/slang/ibl/clouds.slang`, `ibl/default.slang`, `world/default.slang`, `deferred.slang`, `post/froxel_fog.slang` |
| Exposure/bloom | Shared | matching `shared/*_kernel.slangh` | `metal/msl/post/exposure.metal`, `bloom.metal` | `vulkan/slang/post/exposure.slang`, `bloom.slang` |
| GTAO | Desktop | `shared/gtao_kernel.slangh` | — | `vulkan/slang/post/gtao.slang` |
| Temporal accept mask | Desktop | native visibility/identity helpers | — | `vulkan/slang/world/deferred.slang` |
| Depth of field, motion blur, surface diffusion | Desktop | `shared/dof_kernel.slangh`, `motion_blur_kernel.slangh`, `subsurface_kernel.slangh` | — | `vulkan/slang/post/dof.slang`, `motion_blur.slang`, `subsurface.slang` |
| Tonemap/sharpening/display output | Shared | shared exposure state, `shared/sharpen_kernel.slangh`, `gt7_kernel.slangh`, `display_output_kernel.slangh`, `color_grading_kernel.slangh` | `metal/msl/post/tonemap.metal` | `vulkan/slang/post/default.slang`, `tonemap.slangh` |
| SMAA 4x and T2x | Desktop | — | — | `vulkan/slang/post/smaa.slang`; S2x's visibility split, second-sample resolve and lighting, and compose in `world/deferred.slang` |
| FSR 3.1 native antialiasing inputs | Desktop | — | — | `vulkan/slang/post/fsr31.slang` (`vk_fsr31_prepare`); the reconstruction is the FidelityFX SDK's own shaders (ADR-052) |
| Text/UI | Shared | native coverage; fixed MTSDF atlas sampling; per-vertex SDF box, text and image modes | `metal/msl/text/`, `ui/` | `vulkan/slang/text/`, `ui/` |
| Draw census (pass `Cull.Census`, `VkrGpuDebugPayload.draw_census`): reason-coded rejected classification words, each camera candidate's state packed eight to a word and its object id | Shared | `shared/lod_kernel.slangh` (`vkr_gpu_draw_rejected`, `vkr_gpu_draw_camera_state`, `vkr_gpu_draw_census_put`); host mirror and CPU test in `vkr_gpu_abi.h`, `tests/src/draw_census_test.c` | `metal/msl/world/gpu_draws.metal` (classify, encode, `vkr_metal_packet_gpu_draw_census`); Metal native evidence pending | `vulkan/slang/world/deferred.slang` (`vk_gpu_draw_classify`, `vk_gpu_draw_encode`, `vk_gpu_draw_census`); compiles to SPIR-V, Vulkan native evidence unavailable |
| Debug GPU spike (pass `Debug.GpuSpike`, `VkrGpuDebugPayload.gpu_spike_iterations`): a dependent multiply-add chain for a GPU cost with no CPU cost | Shared semantics, one implementation per backend | — | `metal/msl/editor/gpu_spike.metal`; Metal native: on Bistro (M1 Pro, Release) 2,000,000 iterations ran about 60 ms on the frame that carried them | `vulkan/slang/editor/gpu_spike.slang` (`vk_gpu_spike`); compiles to SPIR-V, Vulkan native evidence unavailable |
| View export ([ADR-131](131-view-streaming.md)): the finished Scene image resampled for a video encoder | Shared resample and sRGB encode, one implementation per backend; the outputs differ by encoder input (BGRA surface, NV12 picture) | — | `metal/msl/editor/view_export.metal`; Metal native: Bistro plaza 34.4 dB client vs server (M1 Pro) | `vulkan/slang/editor/view_export.slang` (`vk_view_export`); Vulkan native: Bistro plaza 41.5 dB decoded vs resampled server capture at the capture's frame, no Khronos validation message (RX 6700 XT, 2026-10-23) |

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
skips discarded environment diffuse. Vulkan splits deferred lighting into base
and layered kernels (a Slang generic value parameter) with 8x8 group
classification; the lighting root carries the split flag at byte 156. Both
Vulkan modules pass `spirv-val`
with matching root offsets.

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
RGBA16F DFG lookup per surface in both native implementations: the Metal frame
root holds the DFG texture at byte 472, and Vulkan uses sampled-image and
sampler indices at 488/492. Native manifests and Vulkan reflection include
these fields. [ADR-053](053-energy-compensated-ggx.md) owns the equations and
approximation. No furnace test has run on the tiled pipeline or on Vulkan; the
earlier Metal furnace used the removed deferred lighting.

The BRDF material extensions ([ADR-113](113-brdf-material-extensions.md)) run
on both classes from the same shared kernel functions. They append two
`float4` fields to each material row: Vulkan rows grow to 320 bytes, Metal
rows to 384 bytes. The DFG table gains its C channel and both renderers and
both GPU lightmap bakers sample it as RGBA16F. On Metal, a Release Bistro
snapshot of the tiled pipeline at identity values is byte-identical to the
base revision. On Vulkan, the SPIR-V compiles; reflection at pipeline
creation and native execution are not recorded.

Clearcoat is desktop-only and changes world material rows and resolve/deferred
roots; Vulkan rows are 192 bytes. Production SPIR-V reflection passed all nine
compute modules it affected at the time, including the SSGI and SSR modules
removed on 2026-10-12,
and the 2026-09-12 Windows sweep passes a Debug synchronization-validation
resize. Vulkan output checks are pending. [ADR-062](062-layered-clearcoat.md)
owns its independent-normal, layer allocation and graph storage policy.

Charlie sheen is desktop-only and extends the Vulkan material rows to 224
bytes. The shared layer and two-component rectangle kernels use a separate
immutable 32-byte table block addressed by the Vulkan frame root. Resolve
writes a sheen G-buffer; deferred lighting consumes it.
Actual SPIR-V reflection passes, and the 2026-09-12 Windows sweep passes a
Debug synchronization-validation resize. The implementation decodes
positive-orientation matrix parameters and clips rectangles to the receiver
horizon before the two fitted lobe integrals. Vulkan output checks are pending.
[ADR-063](063-charlie-sheen.md) owns the layer, numerical domain, table budget
and environment-filtering approximation.

Baked diffuse volumes are shared: both pipeline classes take the brick
lookup, the back-face, Chebyshev and crush weights and the L1 evaluation from
`diffuse_volume_kernel.slangh`. The tiled pipeline calls it inline in
`Tiled.Opaque` for draws without a lightmap (ADR-087, decision 8); the
desktop pipeline at half resolution with an exact fallback. The Metal frame
root names the 80-byte `VkrMetalPacketDiffuseVolume` record at byte 488
(indirection, probes, moments and composed SH at 0/8/16/24, origin at 32,
parameters at 48, dimensions at 64), with byte 480 reserved. Each backend
composes the layers in its own kernel: Vulkan's `diffuse_volume_compose`
writes the active lamp groups into a second band whenever one is active
(2026-10-11), Metal's 128-byte compose root (active layers at 32,
weights at 64, direct bands at 96, bound at buffer 0) adds the direct bands
into its single sum while a lightmap is sampled. Native evidence: Vulkan
captures and timing on Bistro (2026-10-09), and on Metal the
`tiled_bistro_sparse_volume_{on,off}_capture` pair, the leak fixture and one
API validation run (2026-10-10), recorded in ADR-054.
[ADR-054](054-baked-diffuse-volumes.md) owns asset and sampling semantics.

Baked sky in environment reflections is shared:
`vkr_sh_l2_sky_visibility` and `vkr_sh_l2_occluded_radiance` in
`sh_l2_kernel.slangh` give both classes one meaning. Metal's compose root
names the active lamp groups in `lamp_mask` at byte 28 and writes the sun-key
luminance into the composed texture's second band, whose row
`VkrMetalPacketDiffuseVolume::dimensions.w` names; `VkrMetalPacketLightmap`
carries `active_lamp_mask` at byte 152. Vulkan's volume images hold the sky
share in alpha; the SSR composite root that also named the volume and baked
lamp images was removed with SSR on 2026-10-12. Native evidence: Metal
Bistro snapshots and Black Mesa captures (2026-10-11, ADR-054); Vulkan is
compiled and passes `spirv-val` only, and
has no native run of the lamp band or of lamp light in lightmapped
reflections.

A mover's draws look the volume and their reflection probes up at its
lighting origin ([ADR-120](120-mover-lighting-origin.md)), the prepared
instance's `lighting_origin` at byte 144, with their own normals. Tiled:
`vkr_metal_tiled_lighting_position` in `tiled.metal` feeds the volume lookup
and the probe box projection of `vkr_metal_tiled_shade`, the blend pass's
pixel probe, and `vkr_metal_packet_draw_probe` in `gpu_draws.metal` tests the
origin as a point. Desktop: `packet_lighting_position` in `default.slang`
feeds the forward blend path, the deferred lighting kernel's probes, the
transmission shade, and `vk_diffuse_volume_lookup`, whose 160-byte root adds
the camera `visible_rows` at byte 144 (binding 13 in the three
`DiffuseVolume` passes). The ADR-119 dynamic layer is read in the same lookup,
so a mover takes it at the origin too. Native evidence: none yet; Metal
library compilation, Vulkan module compilation with `spirv-val`, reflection
of the instance record and the sample root, and a Level Design Test tram
capture on each pipeline are pending.

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
world-to-texel radius equations with their 32 m and 8-texel bounds, with
eight raw blocker samples and at most sixteen comparison-filter samples.
Every receiver also shares the receiver-plane depth gradient
(`vkr_shadow_receiver_plane_gradient`, from the cascade's
`light_view_projection` rows), which each PCF and blocker tap adds to its
reference depth, and the per-pixel kernel rotation
(`vkr_shadow_kernel_rotation`). Desktop deferred lighting steps its noise
index with the temporal phase; the other receivers use index 0. Both
pipelines pass the geometric normal to the sun's cascades. The desktop
G-buffer keeps only the shading normal, so deferred lighting reconstructs
the geometric normal from camera depth
(`vkr_vk_depth_view_geometric_normal`). Desktop local-light receivers still
pass the shading normal. Since 2026-10-15 no shader reads the first two lanes of
`origin_inv_size_sun`, the light-space origin: the runtime still fills them,
frame-input validation still requires them finite, and both backends copy
them. ADR-041 records the authored units, fallback and acceptance evidence.

Far-cascade EVSM shares its warp, tent weights and Chebyshev visibility through
`shadow_kernel.slangh` (`vkr_shadow_evsm_*`). Only the desktop graph runs the
moments passes (`pass.shadow.moments`), on Vulkan; the tiled pipeline keeps
depth PCF for every cascade and renders no moments (ADR-041). The passes use
a 32-byte root: depth and moments bindless indices at bytes 0 and 4, depth
and moments layers, and the two square sizes. The Vulkan deferred-lighting
root keeps its size and reuses bytes 164 for the
moments index and 192 for the linear-sampler slot; `UINT32_MAX` keeps PCF.
Vulkan's compiled-SPIR-V reflection test covers both roots. Native Vulkan
output of the far cascades is not recorded.

Both classify kernels skip a caster for any non-camera orthographic view when
the shared `vkr_gpu_cascade_caster_too_small` (`lod_kernel.slangh`) finds its
bounding-sphere diameter below one texel of the view's LOD scale, or below
four for a candidate with the copy bit of its cull word. Native Vulkan
evidence of the skip is not recorded.

Both classify kernels write a rejected word as all ones in the four bucket
bits and the first failing test above them (`vkr_gpu_draw_rejected`:
filtered, frustum, distance, occluded, cascade caster size); a drawn word's
bucket bits hold the bucket on Vulkan and bucket + 1 on Metal, never 15, and
Metal's two-phase deferral stays `VKR_GPU_DRAW_DEFERRED`, every bit set. Both
encode kernels skip every word whose bucket bits are all ones. Pass
`Cull.Census` exists only while the debug payload asks for it; it maps the
camera word, and on Metal the late view's word of a deferred candidate,
through `vkr_gpu_draw_camera_state` and is read back with the culling
counters.

Both classify kernels also apply the per-object draw distance
([ADR-114](114-per-object-draw-distance.md)) through the shared
`vkr_gpu_draw_distance_culled` (camera) and `vkr_gpu_draw_shadow_culled`
(cascades) in `lod_kernel.slangh`, reading the cull word above the candidate
flags of `state_flags` and `VkrGpuLodView`'s former reserved words
`distance_scale` and `shadow_near` (bytes 36 and 40; the view keeps 48
bytes). Both encode kernels keep only the bucket and candidate flags of the
candidate state in the visible row. In the Draw reach view (render mode 19)
the Metal encode kernel writes the shared `vkr_gpu_draw_reach_code` into
the camera rows' probe bits instead of a probe, and the tiled inspection
variant colours it with `editor_view.slangh`'s `vkr_editor_reach_color`;
the desktop pipeline does not draw the view. The LOD view (mode 20) colours
the visible row's level and morph with `vkr_editor_lod_color`, and the light
count view (mode 21) the tiled shader's count of reaching local lights with
`vkr_editor_light_count_color`, and the overdraw view (mode 22) the sum of
`VKR_EDITOR_OVERDRAW_LAYER` the tiled overdraw entries add with
`vkr_editor_overdraw_color` in the tonemap; all three are tiled-only,
natively checked on Metal (ADR-127). A frozen culling camera
(`VkrWorldPassPayload.frozen_cull`) replaces the camera view's frustum and
`VkrGpuLodView` 0 on both backends; Metal tests occlusion against the
pyramid kept from the frozen frame, Vulkan turns it off, and the Vulkan path
is compiled only. The host mirror
`vkr_gpu_draw_distance_culled` in `vkr_gpu_abi.c` serves the blend list and
editor ring. Bit 20 of the cull word hides a candidate from the camera (rooms,
[ADR-115](115-rooms-and-portals.md); proxy cells,
[ADR-116](116-scatter-proxy-cells.md)) and bit 21 keeps it out of every
cascade; a cascade's `shadow_near` is at least 1 mm, other views' zero.
Metal ran it on Bistro and the large-level fixture (2026-10-13); Vulkan has
compiled SPIR-V only.

Directional GTAO is desktop-only and keeps the existing 192-byte parameter
record; Vulkan uses its existing 240-byte utility root. Raw and denoised graph
images are RGBA8, with world bent direction in RGB and visibility in alpha. The
shared kernel holds horizon moments, packing, multi-bounce diffuse and cone
specular arithmetic. Base deferred environment weights use the shared cone
factor. Deferred coat lighting uses the coat-directed cone, with bent direction
decoded against the coat normal and packed coat roughness. This corrected
Vulkan's omitted coat occlusion. Baked
volumes retain scalar AO. Production SPIR-V validation passes; native Vulkan
output checks are pending. ADR-042 owns the lighting policy.

Opaque SSR ([ADR-055](055-screen-space-reflections.md)) was removed from the
desktop pipeline on 2026-10-12 with its shared kernel, Vulkan roots, graph
passes and `ssr_depth`, `ssr_raw` and `ssr_reflection` capture channels; Git
history keeps its contracts and evidence. Desktop reflections come from the
global environment and box-projected reflection probes. Portable TAA lost its
SSR settling mode with it: it takes the checked 128-sample static integral
whenever the scene signature matches, continuing from the ordinary resolve's
effective sample count (ADR-037).

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

SSGI ([ADR-060](060-screen-space-diffuse-indirect-lighting.md)) was removed
from the desktop pipeline on 2026-10-12 with `ssgi_kernel.slangh`,
`ssgi_contract.slangh`, `post/ssgi.slang` and its graph passes. Deferred
lighting no longer writes the SSGI direct-light source: the lighting root lost
`direct_source_texture` and `ssgi_enabled`, keeps its 224-byte size and moves
the later fields down 8 bytes.

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
step integration, the layer segment, screen and sun-projected coordinates,
the history footprint, its traced-tap resolve and the history blend. Native
files own the march loops and every sample and
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
inverse of the cube convention, the bilinear footprint whose taps cross face
seams and the prefilter-to-chain level mapping.
Both backends project through one loop that also serves the revision bake
(`vk_sh_project`, `vkr_metal_sh_project`). The sky record gains the clear SH
slot, the chain's face size and its address: Vulkan grows to 496 bytes with
the address at 480, and Metal to 512 with the address at 504. Global
prefiltered specular composites the chain in `world/default.slang` and in
`world/lighting.metalh`. Vulkan Release execution and Debug synchronization
validation pass on the Windows host; native Metal execution of these kernels
is not recorded.

GT7 and grading are shared production kernels. Metal's post root carries a
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
contract. The corrected filter passed Vulkan Release Bistro motion and
stationary captures with FSR (removed on 2026-10-12) and motion captures with
portable TAA. The bounded Debug FSR static-reset case, since deleted, loaded
Khronos synchronization validation and reported no API warnings or errors;
ADR-052 records the small, mixed
motion-quality changes and local cost. Native Metal evidence of the filter on
the tiled pipeline is not recorded.

Exposure requires complete histogram groups and GTAO requires mip-selecting
depth sampling under ADR-042.

Direct-light helpers reject noncontributing light hemispheres before half-vector
construction. Shared GGX math defines zero contribution when the half-vector's
squared length is zero or subnormal; finite back-view diffuse behavior remains
unchanged. Local probe influence bounds remain active independently of parallax
projection. Box projection is the shared `vkr_reflection_probe_direction`: it
clamps the shaded point to the probe's box and aims from the probe record's
`capture_origin` on both backends, fading toward the plain reflection with
`α`; influence is zero more than 1 cm outside the box. Specular occlusion is
the shared `vkr_specular_occlusion` and takes `α`. Bloom gain follows ADR-042's
maximum-chain normalization.

Audit remediation changes visibility, HZB producer-grid metadata, Metal reset
ordering, optional resolve outputs and numerical edges. Its Vulkan evidence is
in [ARCHITECTURE](../ARCHITECTURE.md#remaining-implementation-and-evidence-boundaries);
analytical oracles do not substitute for native execution.

The 2026-09-24 shader audit changes the SSR/SSGI cell restart (removed with
SSR and SSGI on 2026-10-12), layered light traversal, sheen view-visibility
preparation, GTAO denoise texel loads, Vulkan resolve material-row reuse,
motion-blur extent queries and the
surface-diffusion same-row path, all on the desktop pipeline. ADR-055 records
the restart's measured effect and ADR-062 the coat-shadow correction, both on
the removed Metal desktop implementation. Vulkan compiles and all 95 SPIR-V
modules validate; native Vulkan before/after captures are pending (checklist).

The display-linear post target is the default whenever SMAA or sharpening
filters the final draw ([ADR-043](043-presentation-dpi-and-color-transfer.md)).
SMAA T2x runs on desktop frames in the default render mode, with opt-in S2x
(SMAA 4x, `VKR_MSAA_ENABLED`) unless depth of field or motion blur is
enabled; the frontend decides
both once per frame, and the tiled class draws no SMAA
([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)).

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

The sweep also passed Release portable-TAA reference and Vulkan FSR static and
motion snapshots, and ADR-060 records separate SSGI Release emission/Bistro and
Debug validation witnesses; FSR and SSGI were removed on 2026-10-12. These
local dirty-tree runs establish bounded native Vulkan execution; output checks
of these features remain open on
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
pass for the actual gather and deferred modules: parameters are 32 bytes,
gather roots 192 bytes, material rows 288 bytes and deferred roots 192 bytes.
The Windows sweep above passes a Release
snapshot and a Debug resize. Its five numeric output fixtures and checker ran
on the removed Metal desktop implementation; Vulkan output checks are pending.
