---
status: proposed
updated: 2026-10-12
authority: proposal
---

# Short-history rendering

The owner decided on 2026-10-12 to stop hiding stochastic shading behind long
temporal history. This proposal records those decisions, the current baseline,
the work in order, the open choices and the Bistro evidence each step needs.
Section 1 is implemented (2026-10-12): [ADR-052](../adr/052-vulkan-fsr31-upscaling.md),
[ADR-055](../adr/055-screen-space-reflections.md) and
[ADR-060](../adr/060-screen-space-diffuse-indirect-lighting.md) record the
removal as declined. The other sections are not implemented.

## Owner decisions (2026-10-12)

| Area | Decision |
|---|---|
| SSR | Remove from the desktop pipeline. The tiled pipeline never had it. |
| SSGI | Remove from the desktop pipeline. |
| FSR 3.1 | Remove. Revisit when a PC release expects vendor upscalers, or the desktop pipeline misses its budget at native resolution. |
| Reflections | Baked, box-projected reflection probes for metal, weapons and general surfaces; planar reflections for water only. No voxel reflections. |
| Diffuse GI | Baked lightmaps and diffuse volumes on both pipelines; runtime DDGI-style volume updates on the desktop pipeline only (section 6). |
| Probe audit | Audit the baked reflection probes, bake and lookup, now that they replace SSR (section 5). |
| Anti-aliasing | Replace the long-history TAA with the original SMAA T2x: two jitter phases and one previous frame. |
| GTAO | Spatial only: fixed noise and more edge-aware denoise passes. |
| Tone mapping | Replace AgX with the Gran Turismo 7 operator on both pipelines, without an offline A/B comparison first. |
| Kept history | Clouds and froxel fog keep their temporal history; their signals are low frequency. |

Screen-space SSR and SSGI stayed noisy and produced temporal artifacts after
repeated fixes (ADR-037, ADR-055, ADR-060 record the settling caps, reflected
hit reprojection, no-hit holds and flicker evidence). The remaining effects
must produce a stable result from the current frame plus at most one
previous frame.

## Current baseline

- **TAA** ([ADR-037](../adr/037-portable-same-resolution-temporal-antialiasing.md)):
  same-resolution scene-linear resolve before the tonemap, eight Halton
  phases, history weight 0.9 for moving and 0.99 for stationary surfaces,
  static accumulation up to 128 samples
  ([`temporal_filter_kernel.slangh`](../../renderer/src/shaders/shared/temporal_filter_kernel.slangh)),
  identity, depth and raw-footprint history validation
  ([`vkr_temporal.c`](../../renderer/src/vkr_temporal.c)). FXAA runs on
  output when TAA does not ([ADR-043](../adr/043-presentation-dpi-and-color-transfer.md)).
- **SSR, SSGI and FSR 3.1** are removed (section 1). The desktop pipeline
  renders the Scene at unit scale.
- **GTAO** ([ADR-042](../adr/042-scene-linear-post-processing.md)): desktop
  only, XeGTAO High (3 slices × 3 steps), full resolution, one 3×3 denoise
  pass ([`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json)
  `pass.gtao.denoise`). Under TAA the noise cycles 64 Hilbert phases; without
  it `noise_index` is already 0 (`vkr_gtao_gpu_params` in
  [`vkr_gtao.c`](../../renderer/src/vkr_gtao.c)). Deferred shading applies it
  to indirect light only, with bent normals, cone specular occlusion and the
  multi-bounce fit, multiplied with material AO
  ([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)).
- **Specular AA**: screen-space normal variance broadens GGX roughness on both
  pipelines (`vkr_ggx_filter_roughness` in
  [`ggx_kernel.slangh`](../../renderer/src/shaders/shared/ggx_kernel.slangh),
  [ADR-012](../adr/012-texture-compression-pipeline.md)).
- **Tone mapping**: compact AgX in
  [`agx_kernel.slangh`](../../renderer/src/shaders/shared/agx_kernel.slangh),
  followed on extended-linear output by the scene-relative headroom shoulder in
  [`display_output_kernel.slangh`](../../renderer/src/shaders/shared/display_output_kernel.slangh)
  ([ADR-061](../adr/061-extended-linear-display-output.md)). The colour
  pipeline belongs to the shared art-level contract
  ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 3).

## 1. Remove SSR, SSGI and FSR 3.1

Implemented on 2026-10-12; the declined ADRs record what was removed and when
to revisit. Motion vectors, the temporal-transform tuple for motion blur
([ADR-067](../adr/067-post-reconstruction-motion-blur.md)), cloud and froxel
history, reflection probes and the diffuse-volume lookup stay. The signals the
removal leaves to other work:

| Removed | Replacement |
|---|---|
| SSR on glossy and metal surfaces | Box-projected reflection probes ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md), [ADR-103](../adr/103-probe-and-volume-authoring.md)), scaled by baked sky light as ADR-054 does today |
| SSR on water | Planar reflections ([Planar reflections](planar-reflections.md), water section of [Large-level rendering](large-level-rendering.md)) |
| Clearcoat SSR priority | Coat lobe reads the probe like the base lobe |
| SSGI | Lightmaps and diffuse volumes; later desktop DDGI updates |
| FSR 3.1 | Native resolution; a spatial render scale like the tiled pipeline's if a budget requires it |

Probe normalization by local baked irradiance and per-room probes
([Large-level rendering](large-level-rendering.md), section 4) are the follow-up
fixes for probe light in the wrong place (section 5).

## 2. SMAA T2x anti-aliasing

### Design

SMAA T2x (Jimenez et al. 2012) on the desktop pipeline replaces portable TAA
and FXAA:

1. Jitter alternates two sub-pixel offsets, (0.25, −0.25) and (−0.25, 0.25),
   matched to the SMAA `subsampleIndices` of each frame. The reference
   assumes y up; Vulkan's y-down clip space needs a sign check.
2. After the tonemap and before UI, the morphological passes run on the
   display-referred colour: edge detection, blend weights, neighbourhood
   blending. Velocity magnitude rides in alpha as in the reference.
3. The resolve blends the current and the previous SMAA output with
   `w = 0.5 · max(0, 1 − 30 · |v_current − v_previous|)`. The previous
   output is the only history.
4. The ADR-037 identity and depth checks also reject a history sample. The
   reference rejects by velocity only, so surfaces without motion vectors
   would ghost for one frame.
5. A texture LOD bias of about −0.5 compensates for the jitter's blur.

Removed with portable TAA: the eight-phase Halton sequence, the 0.9 and 0.99
history weights and the 128-sample static accumulation. Existing screen-space specular AA stays; two frames cannot hide
shimmer that long history used to average.

The tiled pipeline keeps multisampled forward rendering without history
(ADR-087, decision 7).

### Open choices

- **Area and search textures.** The reference ships them as C arrays
  (BC5 and BC4 compatible). Recommendation: embed them as a renderer-owned
  upload at device creation, so no cooked asset depends on the AA mode.
- **Blended surfaces.** Alpha-blended surfaces have no stored velocity.
  Recommendation: reject history where the ADR-037 overlay identity marks a
  blended surface, as the reference's responsive path does.

### Rejected alternatives

- **Filmic SMAA T2x** (Call of Duty, 2016) adds an exponential history filter
  after the two-frame resolve, which reintroduces long history.
- **Decima's FXAA plus two-frame TAA** publishes no rejection details, and
  FXAA is softer than SMAA.
- **Visibility-buffer MSAA** stays a separate proposal
  ([Visibility-buffer MSAA](visibility-buffer-msaa.md)).

### Evidence

- Bistro motion cases (translation, rotation, a moving object) captured with
  the old TAA and with SMAA T2x: no trail longer than one frame behind moving
  edges, and a still camera that converges in two frames.
- A Bistro still view with sub-pixel geometry (railings, foliage, wires) for
  edge and shimmer comparison.
- Release timing of the AA passes at 2560×1440 against the removed TAA resolve.
- The reference publishes 1.32 ms for T2x at 1080p on a GTX 470, of which
  0.3 ms is temporal. That figure is context, not a budget for RDNA 2.

## 3. Spatial GTAO

### Design

- `noise_index` stays 0 on every frame, independent of AA.
- Two denoise passes in place of one. XeGTAO v1.21 notes that one pass
  suffices only with TAA and that two new passes equal one old pass.
- Keep the High preset (3 × 3) at full resolution.
- Compare `min(material_ao, gtao)`, as Unity HDRP composes it, with the
  current product, which darkens occlusion that both terms contain.
- If fixed grain remains visible, evaluate the visibility bitmask
  (Therrien et al. 2023) in place of horizon angles before adding passes or
  slices. Its sampling cost on an RTX 2080 at 1080p is 0.51 ms against
  GTAO's 0.49 ms at 8 samples.

Shipped GTAO users keep AO out of the AA history: Call of Duty and Unity HDRP
give AO its own reprojected buffer, XeGTAO's mode without TAA uses static
noise and spatial passes, and Lumen applies its contact AO after its temporal
filter.

### Evidence

- Bistro interior and street stills with one, two and three denoise passes,
  and a camera move recorded as a clip.
- Two identical runs that produce byte-identical `gtao_visibility`.
- Release timing of `pass.gtao.*` for each pass count.

## 4. GT7 tone mapping

### Design

Port Polyphony Digital's MIT-licensed sample operator (SIGGRAPH 2025) into a
shared kernel replacing `agx_kernel.slangh`, used by both pipelines:

- Units: one frame-buffer unit is 100 cd/m². A calibration constant maps
  exposed scene-linear values to those units; start where the current
  mid-grey lands and tune by eye.
- Curve: toe, linear section to 0.444 × peak, exponential shoulder
  (`alpha` 0.25, `grayPoint` 0.538, `linearSection` 0.444, `toeStrength`
  1.280).
- Colour: per-channel curve blended 40/60 with a luminance-mapped result in
  ICtCp that keeps the original chroma, which fades between 0.98 and 1.16 of
  the target intensity.
- Gamut: convert sRGB scene-linear to Rec.2020 before the operator and back
  with a clamp for SDR, as the AgX kernel does today.
- SDR uses the 250 cd/m² paper white of `initializeAsSDR`. Extended-linear
  output passes the display peak in nits, derived from the OS headroom and
  SDR white, which replaces the scene-relative headroom shoulder in
  `display_output_kernel.slangh`.
- The source keeps Polyphony's MIT notice.

SMAA T2x runs after this operator, on the display-referred SDR result it
expects. The exposure false-colour view
([ADR-099](../adr/099-artist-views-and-luminance-queries.md)) moves with the
tonemap pass.

### Open choices

- **Analytic or LUT.** The sample costs about 33 to 36 transcendental calls
  per pixel, mostly in the ICtCp and PQ conversions. The tiled pipeline's p95
  is 0.1 ms above its M1 Pro budget, and its tonemap pass takes 1.75 ms
  (ADR-087). Recommendation: analytic on the desktop pipeline; on the tiled
  pipeline, a 3D LUT rebuilt when the display peak changes if the analytic
  pass costs measurable time.

### Evidence

- Every snapshot and baseline changes. Publishing new baselines needs the
  owner's authorization.
- Bistro day, night and interior captures on both pipelines, with native
  evidence from each backend.
- Release timing of the tonemap pass on the M1 Pro and on Vulkan.
- An extended-linear run on each OS to check peak handling.

## 5. Reflections: probes and planar water

### Baseline

Reflection probes are baked cubes with box projection, a blend band and
authored gains ([ADR-019](../adr/019-bounded-forward-spatial-lighting.md),
[ADR-103](../adr/103-probe-and-volume-authoring.md)). A surface with baked
light scales its environment or probe reflection by baked sky light,
`L = D + (E − D) · V` with `V = min(1, lum(D_sky) / lum(D_env))`
([ADR-054](../adr/054-baked-diffuse-volumes.md), "Baked sky in environment
reflections"), so an enclosed surface without a local probe reflects its
surroundings' mean radiance instead of the sky. The desktop pipeline blends
up to 16 probes per pixel; the tiled pipeline gives each opaque draw one
probe, chosen at its bounding-sphere centre (`vkr_metal_packet_draw_probe` in
[`gpu_draws.metal`](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal)).
A probe is captured by `vkr_bakery bake probe` as six 256² RGBA16F faces of
unexposed scene radiance at the box centre, then prefiltered at load into a
9-mip GGX cube and L2 SH
([`vkr_vulkan_ibl.c`](../../renderer/src/vulkan/vkr_vulkan_ibl.c)).

### Work

1. **Probe audit fixes.** The audit below lists defects to fix before probes
   carry every glossy surface.
2. **Tiled probe choice for large draws.** A draw that spans rooms takes the
   probe at its bounding-sphere centre (audit R10); split such draws or pick
   per pixel.
3. **Probe placement for rooms.** Per-room probes with portals
   ([Large-level rendering](large-level-rendering.md), section 4) keep a
   surface from reading another room's probe.
4. **Planar reflections for water on both pipelines.** The technique,
   reflector component and evidence stay in
   [Planar reflections](planar-reflections.md); the desktop pipeline renders
   the mirrored view as one more visibility-buffer view at reduced
   resolution. Water samples the reflection image with its normal-map
   offset and falls back to the probe where the offset leaves the image.
   [Large-level rendering](large-level-rendering.md) owns water planes and
   their baked plane-bound probe.

### Probe audit (2026-10-12)

A source review of the bake and of the lookup on both pipelines; nothing was
built or run. The faces are captured unexposed, cube faces are oriented
correctly, the prefilter's PDF and normalization are correct, the SH
projection uses exact solid angles, and bake and lookup share the linear
roughness-to-mip mapping over 9 mips. The defects, most severe first:

| # | Severity | Defect | Where | Fix |
|---|---|---|---|---|
| B1 | High | A project bake captures probes first, then the volume, then lightmaps, so probes show the previous bake's light, or none on a first bake; without a volume the capture also reflects unoccluded sky indoors. | `vkr_project_perform_bakes` in [`vkr_project_bake.c`](../../tools/bakery/project/vkr_project_bake.c) | Bake lightmaps and the volume first, then capture probes from the updated scene. |
| R1 | High | Sky visibility `V = lum(D_sky) / lum(D_env)` also scales local probes. `D_sky` holds no lamps but a probe's SH does, so a lamp-lit room's reflection collapses to the flat baked colour `D`. | `packet_environment_specular_radiance` in [`default.slang`](../../renderer/src/shaders/vulkan/slang/world/default.slang); [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal) | Apply `V` only to the global environment's share; a local probe's capture already holds its occlusion. Changes ADR-054. |
| R2 | High | Desktop: every probe has weight 1, overlapping probes average, and the blend band lies outside the box, so a room probe inside a hall probe ends 50/50 and a band crosses walls into the next room. Tiled lets the smaller box win, so the pipelines disagree. | `packet_probe_influence` in `default.slang`; `.weight = 1.0f` in [`vkr_standard_scene_runtime.c`](../../runtime/src/application/vkr_standard_scene_runtime.c) | Priority order (smaller volume first by default), accumulate `w = (1 − acc) · wᵢ`, band inside the box. Changes ADR-019. |
| R3 | High | The frame takes the first 16 enabled probes in scene order; later probes vanish whatever the camera sees. | `frame_ibl_probe_cap` loop in `vkr_standard_scene_runtime.c` | Rank by camera distance and frustum like the 128-light table; a clustered probe list later. |
| B2 | High | The capture's sky includes the sun and moon discs, glow and stars; the global IBL leaves them out (ADR-058). Metal shows the sun highlight twice, and the disc drives fireflies and SH ringing. | `deferred.slang` sky pass; capture case in [`vkr_bakery_bake.c`](../../tools/bakery/vkr_bakery_bake.c) | A capture setting that omits discs, glow and stars, as the IBL source does. |
| B3 | High | Probe sources have one mip, so filtered importance sampling reads mip 0: 256 samples of a sharp 256² face per texel alias on rough metal and catch fireflies. | [`vkr_hdr_cube_packer.cpp`](../../tools/vkr_hdr_cube_packer.cpp); `vkr_vk_record_ibl_source_mips` only for the atmosphere | Build the source mip chain for probes too. |
| R4 | Medium | Box projection switches off 1 cm outside the box while the band still weighs the probe: a seam where a floor runs through a doorway. Influence and projection share one axis-aligned box; no capture offset, no rotation. | `packet_box_project` in `default.slang`; `lighting.metalh` | Project from the position clamped to the box; separate projection box and capture origin. |
| R5 | Medium | No roughness fade of parallax correction and no dominant specular direction, so rough reflections smear off-centre. | `default.slang`, `tiled.metal` | `lerp(N, R, (1 − α)(√(1 − α) + α))` and a roughness fade toward the unprojected direction, in a shared kernel. |
| R6 | Medium | The environment's intensity and specular gains also scale local probes, whose captures already hold sun, lamps and sky. Bistro uses 1, so it is unaffected today. | `ibl_controls` in `default.slang` | Apply only the probe's own gains to probe terms. |
| B4 | Medium | One capture per probe at the World's start hour; changing the hour keeps the old sky and sun in probes while volumes follow sun keys. | `vkr_bake_probe_case` | Capture per sun key and blend like volume layers, or scale by the ratio of current to captured baked light. |
| B5 | Medium | The capture renders SSR, SSGI, fog, dynamic entities and blended effects, 8 frames per face, without AA. | Probe case in `vkr_bakery_bake.c` | SSR and SSGI disappear in step 1; exclude non-static entities and blended effects; supersample faces. |
| B6 | Medium | Probes bake with `sh_deringing = 0`; L2 of a bright window rings and the zero clamp shows black lobes. | [`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c) | A non-zero window for baked probes. |
| R7 | Medium | Desktop: a lightmapped surface the volume does not cover reflects the full environment; SSR hid this indoors. | `deferred.slang` environment branch | Probes in such rooms, or a sky share from lightmap sun-key layers. |
| R8 | Medium | Desktop deferred passes the shading normal as the horizon normal, so the horizon fade is always one and bumpy metal reflects below its surface. | `packet_environment_specular(..., normal, normal, ...)` in `deferred.slang` | Pass the geometric normal from the visibility buffer's triangle. |
| R9 | Medium, suspected | The GTAO cone multiplies all specular above roughness 0.09; spatial-only GTAO noise reaches glossy metal. | `vkr_gtao_cone_specular` | Check in the section 3 evidence; soften the cone if visible. |
| B7 | Medium, suspected | A box edit moves the projection centre without a recapture or a staleness mark. | Editor probe handles (ADR-103) | Store the capture origin; mark probes stale on edits. |
| B8 | Low | `--size` accepts 2048 but the prefilter is fixed at 256; a 2048 source SH projection risks a Windows TDR. | `vkr_bakery_bake.c`, `vkr_world_resources.c` | Limit to 256, or downsample with mips (B3). |
| R10 | Low | Tiled: one probe per draw at its bounding-sphere centre; the blend pass picks per pixel without the smaller-box tie-break. | `gpu_draws.metal`, `tiled.metal` | Work item 2; the same tie-break in both places. |
| R11 | Low, suspected | Specular occlusion receives perceptual roughness where the references use α. | `packet_specular_ao`, `lighting.metalh` | Check against the reference and convert. |
| B9 | Low | Bistro's interior probe is six 4 × 4 flat-colour 8-bit placeholders with specular gain 0, so it reflects nothing. | `assets/textures/bistro_indoor_probe_*.png`, `bistro.scene.json` | Replace with a real bake after B1 to B3; Bistro probe evidence needs specular gain 1. |

Owner decisions on the audit (2026-10-12):

- **R1:** sky visibility applies only to the global environment's share.
  Local probes keep their captured radiance. This supersedes the probe part
  of ADR-054's "Baked sky in environment reflections" when it lands.
- **R2:** probes blend in priority order, the smaller box first unless an
  authored priority says otherwise, with the blend band inside the box. This
  changes ADR-019's probe influence when it lands.
- **B4:** probes follow the time of day by a ratio: each probe's radiance
  scales by the current baked light at its capture point over the baked
  light captured with it, clamped. One capture per sun key (about 34 MB per
  probe for eight keys at 256²) is not planned.

Pipelines each fix touches:

| Scope | Findings |
|---|---|
| Both: shared bake, packer, loader and runtime probe list | B1, B4 to B9, R3 |
| Both: a change in each backend's shaders | B2 (each pipeline's sky pass in the capture), B3 (Metal also builds source mips only for the atmosphere, in `vkr_metal_packet_record.inc`), R1, R4, R5, R6, R11 |
| Both, different shape | R2: the desktop pipeline needs priority blending; the tiled pipeline already lets the smaller box win, and the inward band changes its influence test |
| Desktop only | R7, R8, R9 |
| Tiled only | R10 |

On the tiled pipeline, native evidence comes from the Mac. `Tiled.Opaque`
already runs 0.1 ms over its p95 budget and reacts to register pressure: in
ADR-054, computing the sky share before the cube sample cost 2.4 ms. Every
shader-side fix (R1, R4, R5, R11) needs a matched Release timing of
`tiled_bistro_baked_native` before and after.

### Evidence

- Bistro interior and street captures of glossy and metal surfaces with SSR
  removed: no sky in interior reflections, no seam where two probes blend,
  parallax-correct reflections of walls inside a probe's box.
- A Bistro water-plane case (added with [Large-level rendering](large-level-rendering.md),
  phase 0) for planar reflections on both pipelines, with Release timing.

## 6. DDGI-style volume updates on the desktop pipeline

### Baseline

The sparse diffuse volume ([ADR-054](../adr/054-baked-diffuse-volumes.md))
holds 4 × 4 × 4 probe bricks over three levels, distance moments, and one L1
SH set per sun key and per lamp group. `DiffuseVolume.Compose` weighs the
layers into a composed SH texture each time the weights move; the lookup
weighs eight corners by trilinear, back-face and Chebyshev visibility. The
Bistro volume holds 194,463 valid probes. The runtime renderer uses no ray
queries and builds no acceleration structures; only the bakers do
([ADR-083](../adr/083-supported-hardware-matrix.md): RDNA 2+ and Ampere+
support `VK_KHR_ray_query` in hardware).

### Design

Runtime ray-traced updates write into the existing volume so the lookup,
visibility and composition stay unchanged:

1. **Acceleration structures.** One BLAS per static mesh or brush batch
   built at scene activation, refitted or rebuilt for movers; one TLAS per
   frame. Build work runs outside per-draw loops and stays within a frame
   budget.
2. **Probe selection.** Each frame updates a bounded set of probes: those in
   cells the camera sees, those inside a changed light's range, then a
   round-robin of the rest. 194,463 probes cannot all update in one frame.
3. **Tracing.** Each selected probe traces a fixed number of rays with a
   rotated spherical Fibonacci set. A hit shades its surface: material
   albedo and emission through the bindless tables, direct light from the
   sun and the ranked local lights with a shadow ray each, and indirect light
   read from the volume itself for multi-bounce. A miss returns the sky.
4. **Storage.** Results project into L1 SH and blend into a dynamic layer
   with a hysteresis in probe space. History lives per probe, not per
   pixel: no reprojection, no motion vectors, no disocclusion. A light
   change shows as a short lag, not as a trail.
5. **Composition.** The dynamic layer joins `DiffuseVolume.Compose` as one
   more band. Distance moments stay baked unless geometry moved.

### Decisions (owner, 2026-10-12)

- **The dynamic layer first holds only light the bake lacks:** dynamic
  lights, emissive movers and changed lights, added to the baked layers. It
  traces less, leaves baked quality where nothing changed, and keeps the
  baked layers as the authority. A layer holding all light, replacing the
  baked layers for time of day without sun keys, follows once this one is
  stable.
- **Scenes without a bake** use a uniform runtime grid over the scene bounds
  with runtime relocation for blockout; the baked brick layout replaces it
  after a bake, because brick placement needs one.
- **Budget:** 1.0 ms of GPU time at 2560 × 1440 on an RX 6700 XT for build,
  trace and update together, measured on Bistro.

### Evidence

- A Bistro case with a moving dynamic light inside the café: its bounce
  appears on nearby walls within the hysteresis time and leaves no light
  after the light moves away.
- Bistro still captures with the dynamic layer on and no light change match
  the baked result within a stated tolerance.
- Release timing of the build, trace and update passes against the budget.
- A Vulkan validation run of the case with zero messages.

## Order

1. Remove SSR, SSGI and FSR 3.1 (section 1). Done 2026-10-12.
2. Probe audit fixes (section 5, item 1); probes become the only glossy
   reflection source in step 1.
3. Spatial GTAO (section 3); independent of AA once TAA no longer drives the
   noise.
4. GT7 tone mapping (section 4); it fixes the colour space SMAA T2x reads.
5. SMAA T2x replacing portable TAA and FXAA (section 2).
6. Tiled per-draw probes, room probes and planar water (section 5, items 2
   to 4).
7. DDGI-style updates (section 6).

## Documents to update when each step lands

| Step | Documents |
|---|---|
| 1 | Done 2026-10-12: ADR-052, ADR-055 and ADR-060 declined; the other ADRs, `ARCHITECTURE.md` and the proposals lost their SSR, SSGI and FSR claims |
| 2 | ADR-019, ADR-054, ADR-058, ADR-103; [Tiled graphics pipeline](tiled-pipeline.md) remaining-work item 1, which lists per-surface probes that already exist |
| 3 | ADR-042 (GTAO) |
| 4 | ADR-043, 044, 061 and 087 (colour pipeline), ADR-099 |
| 5 | ADR-037 rewritten for SMAA T2x; ADR-043 (FXAA) |
| 6 | ADR-087; [Tiled graphics pipeline](tiled-pipeline.md), [Planar reflections](planar-reflections.md), [Large-level rendering](large-level-rendering.md) |
| 7 | ADR-054 or a new ADR; [Sparse diffuse volumes](sparse-diffuse-volumes.md) item 2 |

## Sources

- [SMAA paper (Jimenez et al., 2012)](https://www.iryoku.com/smaa/downloads/SMAA-Enhanced-Subpixel-Morphological-Antialiasing.pdf)
  and [reference source](https://github.com/iryoku/smaa)
- [Filmic SMAA (SIGGRAPH 2016 Advances)](https://advances.realtimerendering.com/s2016/Filmic%20SMAA%20v7.pptx)
- [Decima Engine: Advances in Lighting and AA (SIGGRAPH 2017)](https://www.advances.realtimerendering.com/s2017/DecimaSiggraph2017.pdf)
- [Practical Realtime Strategies for Accurate Indirect Occlusion (GTAO, 2016)](https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf)
- [XeGTAO](https://github.com/GameTechDev/XeGTAO)
- [Screen Space Indirect Lighting with Visibility Bitmask (2023)](https://arxiv.org/pdf/2301.11376)
- [Unity HDRP source (GTAO and material AO composition)](https://github.com/Unity-Technologies/Graphics)
- [GT7 tone mapping sample code](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/supplemental/gt7_tone_mapping.cpp)
  and [SIGGRAPH 2025 slides](https://blog.selfshadow.com/publications/s2025-shading-course/pdi/s2025_pbs_pdi_slides.pdf)
