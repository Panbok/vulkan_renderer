---
status: proposed
updated: 2026-10-16
authority: proposal
---
# Shadow quality

Sun shadows without stair-stepped edges, pop-in or an early end, on both
pipeline classes ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md)). The
implemented directional system is
[ADR-041](../adr/041-retained-cascaded-shadows.md); the local-light cache is
[ADR-019](../adr/019-bounded-forward-spatial-lighting.md), and its architecture
is owned by the [local shadow architecture](local-shadow-architecture.md)
proposal. This proposal fixes defects in both, and then layers the sun shadow
on each pipeline (owner decision 2026-10-16, [hybrid sun shadows](#hybrid-sun-shadows)):
the cascades stay the base, screen-space contact shadows add contacts on both
pipelines, the desktop pipeline adds ray-query penumbrae near the camera with a
sparse shadow-map clipmap as its fallback, and the tiled pipeline adds
signed-distance-field (SDF) shadows behind a prototype gate.

## Problem

The owner reports four symptoms: jagged edges, shadows that pop in and out,
shadows that end too close, and edges that are too hard. A source audit on
2026-10-16 (commit `3ccd29f7`) traced them to texel density, six defects in
the fit, cull and cache code, and the 200 m shadow distance. No build, capture
or timing was run for the audit; every cost below that is not marked measured
is an estimate.

## Current baseline

Both pipelines share the host fit, the cull kernel and the receiver kernel
(`renderer/src/shaders/shared/shadow_kernel.slangh`). The High preset
(`runtime/src/renderer/systems/vkr_shadow_system.h`, `VKR_SHADOW_CONFIG_HIGH`)
draws 4 cascades of 2048² D32F with a practical split of λ 0.80 from 0.1 m to
200 m: 10.6, 23.6, 53.9 and 200 m. Each cascade fits the smallest sphere around
its slice, rounds the extent up to a power-of-two bracket divided by 2048, and
snaps a world-anchored origin to texels. Receivers take 16 rotated Poisson
taps at 1.5 texels, PCSS on cascades 0 and 1, an 8% blend band, and a fade to
unshadowed over the last 20 m. Vulkan High adds EVSM on cascades 2 and 3; the
tiled pipeline has no EVSM. Lightmaps hold no direct sun
([ADR-088](../adr/088-baked-lightmap-sets.md)), so no surface past 200 m has
a sun shadow. Screen-space contact shadows exist only for local lights on the
desktop pipeline: an 8-step march over at most 0.25 m of camera depth in
`Shadow.LocalMask` (`renderer/src/shaders/shared/local_shadow.slangh`), on
when `contact_shadows` is set. The sun has none on either pipeline.

Texel density, computed from the High preset at 2560×1440 with a 60° vertical
field of view (estimate, not a capture):

| Cascade | Range | Rendered texel | Screen pixels per texel |
|---|---|---|---|
| 0 | 0.1–10.6 m | 1.39 cm | 17 at 1 m, 1.6 at 10.6 m |
| 1 | 10.6–23.6 m | 3.13 cm | 3.7 to 1.7 |
| 2 | 23.6–53.9 m | 7.22 cm | 3.8 to 1.7 |
| 3 | 53.9–200 m | 27.2 cm | 6.3 to 1.7 |

A texel wider than about 2 screen pixels shows its staircase through a
1.5-texel kernel, and the density drops 2.2× to 3.7× at each cascade border.

Measured costs, all local and non-authoritative:

| Item | Result | Configuration and source |
|---|---|---|
| Vulkan sun shadow sampling | 0.53 ms of a 3.50 ms `Lighting.Deferred` | RX 6700 XT, Bistro, 1920×1080, TAA, High ([lighting efficiency](lighting-efficiency.md)) |
| Vulkan `Shadow.LocalMask` and its reads | 2.82 ms and 0.73 ms | same |
| Vulkan cascade and moments work | 0.125 ms per frame; 146 / 0 / 42 / 60 redraws of cascades 0–3 in 600 frames | `bistro_shadow_orbit`, 2048², ADR-041 (`sha256:425eb1e72c5a`) |
| Vulkan cascade redraw | 0.30 / 0.33 / 0.17 ms for cascades 0 / 2 / 3 | [ADR-085](../adr/085-gpu-geometry-lod-and-terrain-geomorphing.md), with geometry levels |
| Vulkan TLAS refit and probe trace | `Volume.Build` 0.045 ms, `Volume.Trace` 0.811 ms p50 | RX 6700 XT, 2560×1440, `dynamic_volume_bistro_cafe_timing` ([ADR-119](../adr/119-dynamic-diffuse-volume-layer.md), `sha256:93fc987f…`) |
| Vulkan probe trace, 2048 probes × 128 rays | 3.06 ms trace, 0.51 ms build | same ADR, `sha256:833f66b2…` |
| Tiled cascade redraw | 1.4–2.1 ms | M1 Pro, 1440p, ADR-087 |
| Tiled frames with 0 / 1 / 2 redraws | 12.37 / 13.82 / 15.00 ms median, 220 / 76 / 4 of 300 | same, `sha256:0bc446fb…` |

Memory on Vulkan High is about 350 MiB for the sun (three 64 MiB targets, the
64 MiB static layer, 96 MiB of moments) and 384 MiB for the local atlas.

## Defects

Each row was read in source; the symptom column is inferred.

| # | Defect | Symptom | Location |
|---|---|---|---|
| 1 | A redrawn cascade uses `vkr_shadow_guarded_fit`: its centre moves by half the camera motion and its texel is extent / 2048, neither snapped nor quantized. Every redraw lands on a new sub-texel phase. `test_cascade_texel_grid_is_fixed_in_world` checks only the raw fit. | Edges jump at each redraw | `vkr_shadow_system.c` (`vkr_shadow_guarded_fit`), `tests/src/shadow_system_test.c` |
| 2 | Scatter and spline copies default to a 50 m shadow distance ([ADR-114](../adr/114-per-object-draw-distance.md)); High's cascade 3 starts at 53.9 m and drops them without a fade. | Tree and rock shadows end near 54 m | `lod_kernel.slangh` (`vkr_gpu_draw_shadow_culled`) |
| 3 | A caster under 1 texel is cut without a fade; the limit grows 2–4× at each border. | Small props lose their shadow at a border | `lod_kernel.slangh` (`vkr_gpu_cascade_caster_too_small`) |
| 4 | Shadow LOD is the coarsest level with at most 1 texel of error, so it changes at each border; geomorph is off in cascade views. | Shadow shapes change at borders | `gpu_draw.slangh` |
| 5 | Vulkan receivers pass the normal-mapped shading normal to the normal offset, slope bias and receiver-plane gradient; the G-buffer stores only that normal. Metal passes the geometric normal. | Speckle and ragged edges on bumpy surfaces | `deferred.slang` (sun term of the lighting kernel), `default.slang` (forward sun term) |
| 6 | The Vulkan kernel rotation uses noise index 0 every frame, although FSR 3.1 native antialiasing is the default. | Fixed grain that the temporal resolve cannot average | same call sites |
| 7 | A local light whose map is invalid and not drawn this frame gets strength 0 at once. | Local shadows blink off when lights move or resize past the face budget | `vkr_local_shadow_system.c` |
| 8 | Under budget, the atlas layer count follows the resident cells; a change replaces the atlas and clears every layer, which then refills at the face budget per frame. | Many local shadows vanish together | `vkr_local_shadow_system.c` |

Rows 7 and 8 block the local shadow architecture proposal's no-pop gate and
are listed here only so that one change list fixes every pop source.

A second audit on 2026-10-16, with Level Design Test and Bistro captures on
the tiled pipeline, found more defects; the code changes below fixed them
and defects 1–4, 7 and 8 in the shared host code, the shared cull kernel and
the tiled receivers. The decisions are recorded in ADR-019, ADR-041, ADR-087
and ADR-114.

| Defect | Fix |
|---|---|
| 1, the guarded fit's texel and origin | Quantized guarded extent, lead inside the band, edges on the guarded lattice; CPU test |
| 2, copies' 50 m shadow distance | No default; copies need 4 cascade texels instead (captures: tree and rock shadows past 54 m) |
| 3 and 4 | The guarded texel no longer varies, so the cut and the shadow level hold between redraws |
| 7 and 8 | Committed content fades over 1 s; the atlas never shrinks while the cache is valid; CPU tests |
| Near plane clamped at the light eye | Removed; ridges and backdrop terrain shadow the valley (captures) |
| Glass cast an opaque sun shadow | Cascades skip transmissive casters on both pipelines (owner decision) |
| A draw distance did not end the shadow | The shadow code takes the smaller of the two |
| Tiled 65,536-row bucket cap dropped casters in atomic order | Buckets pack one after another |
| Balanced kept no cascade | Its reuse controls are set and every adopted configuration is normalized |
| A refit composed cascade drew twice | It draws its static layer with the fresh fit in one frame; CPU test |
| Tiled screen-locked grain | Fixed bilinear grid; penumbra estimate on the three near cascades |
| Receivers outside their cascade shown lit | The next cascade samples them |
| Local and moving shadows offset along the normal-mapped normal | Geometric normal |
| Moving lamps drew a static square, a copy and a dynamic square | One pass; dynamic squares keep their incumbents |

T1 runs as the proactive refresh (one per frame, 32-texel margin) and T2b as
the far level (owner decision 2026-10-16: near cascades to 80 m, far level to
1 km), whose static layer is rebuilt one tile of an 8x8 grid a frame: its
most expensive tile took 1.11 ms on the Level Design Test, against 2.8–3.0 ms
for the whole layer and the 1.0 ms budget. A moving sun and a stale or
pending world reach the far level through the same tiles. Open on the tiled
pipeline: T3, which ADR-087 decision 11 settles for now; the near cascades
still draw whole while a publication is pending; a Bistro orbit and
capture-free timing against the budgets. The desktop pipeline compiles
every shared change. Its native evidence for the shared fixes is still
deferred (owner decision 2026-10-16). Defect 6 and the sun part of defect 5
are fixed on the desktop pipeline (see the
[hybrid prototype](#hybrid-prototype)).

## Desktop options

Costs are estimates for an RX 6700 XT at 2560×1440 in Bistro.

| Option | Jaggies | Pop-in | Distance | Softness | Cost | Memory |
|---|---|---|---|---|---|---|
| D0: fix defects 1–8 | small gain | fixed | none | small gain | under +0.1 ms | none |
| D1: D0, tighter light-space fits, 5–6 cascades, cached distant levels to about 2 km, sun screen-space shadows | medium gain | fixed | fixed | contact detail | +0.3–0.6 ms | +64–128 MiB |
| D2: D1, plus ray-query sun shadows near the camera, denoised | removed in ray range | fixed | from D1 | penumbra from the sun's size | +1–3 ms, see the gate | TLAS exists; +30–60 MiB of targets |
| D3: sparse shadow-map clipmap (virtual shadow maps), 16k virtual, 128² pages, sun clipmap and local lights | about 1 texel per pixel | no borders | clipmap to km | filtered | 2–3 ms cached, 4–6 ms after invalidation | 256 MiB pool replaces about 730 MiB |

Estimate sources: Bend Studio's screen-space shadows took 0.19 ms on PS5 at
1440p (SIGGRAPH 2023); Call of Duty MW traced and denoised one light for
2.55 ms at 1440p on an RTX 2070 (Digital Dragons 2020); NRD SIGMA denoises in
0.40 ms on an RTX 4080 at 1440p; Sakmary (CESCG 2025) measured Bistro virtual
shadow maps at 1.29 ms cached and 2.57 ms uncached on an RTX 4070 Ti SUPER,
scaled here by 0.4–0.5×.

D2 is preferred because the desktop already builds a BLAS per traced submesh
and refits a TLAS every frame for ADR-119, so the acceleration structure is
paid for. Rays trace only the pixels that the cascades mark as penumbra or
uncertain, within about 30–50 m, at a quarter of the pixel rate. Alpha-tested
casters, skinned meshes (not in the TLAS), froxel fog and the far range keep
the cascades, and GPUs without ray queries keep D1. D2 also removes the PCSS
search on cascades 0 and 1.

D3 fixes every symptom for the sun and the local lights in one system. Each
clipmap level covers twice the area of the one before around the camera, its
128² pages are anchored to the world, and a pixel samples the level whose
texel matches its footprint, so density stays near 1 texel per pixel at every
distance and camera motion draws only the pages that scroll in. It fits the
desktop pipeline: `VBuffer.Opaque` writes camera depth before
`Lighting.Deferred`, so the shadow passes can move after it and mark the pages
that visible pixels sample; RDNA2 and Ampere write depth into a physical page
pool through a software page table with 32-bit image atomics, so sparse
residency is not needed. Its costs:

- The engine has no cluster culling
  ([meshlet cluster culling](meshlet-cluster-culling.md)), so each dirty page
  draws whole instances.
- Moving casters invalidate pages every frame; static and dynamic page caches
  split them, as ADR-041's static layer and dynamic overlay do now.
- A moving sun invalidates every page. The sun direction would be held and
  refreshed through slices, as the tiled far level does.
- ADR-041's per-image retained cascades become one shared page cache with
  GPU-completion rules.

D3 is built if D2 misses its gate, or if the local shadow architecture pages
local lights in the same pool, where the gain is larger (6.3 ms for local
shadows against 1.1 ms for the sun).

Desktop SDF shadows are declined. The TLAS exists, and ray queries give exact
shadows for thin and alpha-tested casters, which an SDF loses below about two
voxels.

### Ray-query cost gate

The probe trace ran 262,144 rays in 3.06 ms, about 86 M rays per second. Those
rays are incoherent, 64 m long and return lighting, so they bound shadow rays
from below. Estimated cost of the ray pass alone:

| Rays | Count at 1440p | At the CoD rate | At the probe rate |
|---|---|---|---|
| 1 per pixel, whole screen | 3.7 M | about 2.5 ms | about 40 ms |
| 1 per 4 pixels, whole screen | 0.92 M | about 1 ms | about 10 ms |
| 1 per 4 pixels, near penumbra only | 0.2–0.4 M | about 0.5 ms | 2–4 ms |

The denoiser adds 0.4–1 ms. The gate: at or under 1.5 ms with denoising, D2
becomes the High preset; above 3 ms, D1 is the default and D2 an opt-in Ultra
setting.

### Prototype result

The gate passes at one ray per 2×2 block. An opt-in prototype,
`VKR_SUN_RAY_SHADOWS=1` (one ray per 2×2 block) or `=2` (one ray a pixel),
adds two passes:

- `Shadow.SunRay` traces one first-hit ray toward a point on the sun's disk
  (the cascades' PCSS angle) against the ADR-119 TLAS. Alpha-tested casters
  are tested at texture level 1. The traced pixel of each block turns with the
  frame.
- `Shadow.SunRayResolve` filters 3×3 trace texels with spatial, depth and
  normal weights into an RG8 mask: visibility, and its weight over the
  cascades.

Lighting takes the mask below 40 m view depth and blends to the cascades by
50 m; where the weight is 1, it samples no cascade. Plain Bistro has no
diffuse volume, so `Volume.Build` now also builds the TLAS for sun rays
(`ray_scene_active`). The sources are
[`sun_ray_shadow.slang`](../../renderer/src/shaders/vulkan/slang/world/sun_ray_shadow.slang)
and `vkr_vk_prepare_sun_ray_shadow` in
[`vkr_vulkan_deferred.c`](../../renderer/src/vulkan/vkr_vulkan_deferred.c).

Measured on 2026-10-16: RX 6700 XT, Release with validation unset, 2560×1440,
temporal AA (FSR native), High shadows, 5 repetitions × 300 frames,
`profile --profile tools/profiles/local-offscreen-gpu-repeated.json`. Values
are mean GPU ms per frame. Reports are "incomplete" only for a dirty tree and
an unstable transmission-coverage warm-up metric; all repetitions passed.
The two cases are `local.bistro.shadow.sun_ray_plaza_perf` (every visible
pixel within 50 m) and `local.bistro.shadow.sun_ray_street_perf`.

| Case, mode | Trace | Resolve | TLAS refit | Lighting | All GPU passes | Report |
|---|---|---|---|---|---|---|
| Plaza, off | — | — | — | 6.888 | 19.347 | `sha256:151801ba…` |
| Plaza, 1 ray / 2×2 | 0.847 | 0.433 | 0.062 | 5.878 | 19.725 | `sha256:256d4193…` |
| Plaza, 1 ray / pixel | 2.705 | 0.439 | 0.062 | 5.742 | 21.445 | `sha256:ac82bf58…` |
| Street, off | — | — | — | 7.913 | 20.927 | `sha256:7ecbbf7e…` |
| Street, 1 ray / 2×2 | 0.742 | 0.411 | 0.062 | 7.034 | 21.294 | `sha256:d20cff31…` |
| Street, 1 ray / pixel | 2.288 | 0.421 | 0.062 | 6.958 | 22.780 | `sha256:59aed509…` |

- At one ray per 2×2 block, the ray passes cost 1.34 ms (plaza) and 1.22 ms
  (street), within the 1.5 ms gate.
- Lighting gets 0.9–1.0 ms faster because it skips the cascade filter in ray
  range. The whole frame costs 0.38 ms (plaza) and 0.37 ms (street) more.
- One ray a pixel costs 3.2 ms (plaza), above the 3 ms limit, and looks
  nearly the same as one per block after filtering. The half rate is the
  High candidate.
- The full-rate trace, at most 3.7 M rays, took 2.7 ms. The probe-trace rate
  in the estimate table predicted about 40 ms.

A Debug run of the plaza at one ray per 2×2 block, with
`VK_LAYER_KHRONOS_validation` and synchronization checks, reported no
validation messages (`local.bistro.shadow.sun_ray_plaza_validation`,
`sha256:64f94aef…`).

In the `shadow_debug_factor` captures of the plaza, the rays give crisp
bollard and lamp-post shadows, hedge shadows with leaf detail, and contact
shadows in facade relief and paving joints. The cascades blur or lose all of
them.

The prototype leaves these items for D2:

- The filter is spatial only, and temporal AA averages its four rotating
  corners. A temporal denoiser (0.4–1 ms) and a motion capture that checks
  edges for ghosting and noise are still required.
- Skinned meshes are not in the TLAS, so within ray range they cast no sun
  shadow.
- The range ends at a fixed 50 m.
- Transmissive surfaces keep the cascades.

### Hybrid prototype

The hybrid prototype adds the hybrid layering to the ray prototype on the
desktop pipeline, behind the same `VKR_SUN_RAY_SHADOWS` switch (2026-10-16):

| Layer | Pass | Where it applies |
|---|---|---|
| Rays | `Shadow.SunRay`, `Shadow.SunRayResolve` | within 50 m, blended to the cascades from 40 m |
| Temporal denoise | `Shadow.SunRayTemporal`, `Shadow.SunRayHistory` | wherever the rays apply |
| Contact march | `Lighting.Deferred` | where the rays' weight is below 1 |
| Cascades | `Lighting.Deferred` | beyond the rays, and on GPUs without ray queries |

- **Temporal denoise.** `Shadow.SunRayTemporal` keeps an exponential
  average (weight 0.1) of the filtered visibility along `temporal_motion`.
  The history is RG16F (visibility, device depth). A bilinear history tap
  counts only when its depth is within 2% of the previous depth that
  `temporal_validity` records for the surface. The history is clipped to the
  current 3×3 mean ± 1.5 standard deviations. It starts over unless the
  previous submit wrote the same history image and the camera history is
  valid. `Shadow.SunRayHistory` copies the new history over the old one.
- **Complete TLAS.** The rays engage only when the frame's TLAS holds every
  eligible row (`tlas_missing_rows`). After a load, casters whose BLAS still
  waits for the per-frame build budget keep their cascade shadow.
- **Contact march.** `Lighting.Deferred` marches camera depth toward the
  sun with the tiled pipeline's shared kernel: `VKR_SUN_CONTACT_STEPS`
  samples, `vkr_sun_contact_length` and `vkr_sun_contact_strength`
  (`shadow_kernel.slangh`), and the local-light march's start offset, noise,
  occlusion test and end fade, through `vk_contact_shadow`. It runs where
  the rays' weight is below 1, because the rays find the contacts
  themselves. It follows the shadow settings' `contact_shadows` (the Epic
  preset), as the tiled pipeline does. `VKR_SUN_CONTACT_SHADOWS` set to `0`
  or `1` overrides the setting.
- **Defect 5, sun only.** The lighting, forward and transmission sun terms
  offset and bias the receiver along the geometric normal. Lighting
  reconstructs it from camera depth (`vkr_vk_depth_view_geometric_normal`) and
  already computed it for the horizon fade, so it costs no measured time.
  The ray origin uses the same normal. Local lights keep the shading normal:
  the reconstruction cost 0.29 ms in `Shadow.LocalMask` on the plaza (3.86 to
  4.14 ms), and 4.79 ms when deferred to the first shadowed light
  (`sha256:46c413a2…`, `2f99a298…`, `4561a079…`). The local shadow
  architecture proposal owns that fix.
- **Defect 6.** The sun's filter rotation and contact noise step through FSR
  3.1's 16 or SMAA T2x's 2 temporal phases, and stay fixed without a
  temporal resolve.

Measured as in the prototype result. "Hybrid" is rays with the contact
march (`VKR_SUN_CONTACT_SHADOWS=1`); "contact" is the march without rays, as
on a GPU without ray queries. The march took 12 steps then;
`VKR_SUN_CONTACT_STEPS` is now 8, so its rows are an upper bound:

| Case, mode | Trace | Resolve | Temporal + copy | TLAS refit | Lighting | All GPU passes | Report |
|---|---|---|---|---|---|---|---|
| Plaza, off | — | — | — | — | 6.848 | 19.334 | `sha256:ea74ec0a…` |
| Plaza, contact | — | — | — | — | 8.023 | 20.526 | `sha256:920cd1fd…` |
| Plaza, hybrid | 0.882 | 0.430 | 0.211 | 0.062 | 5.912 | 20.021 | `sha256:33c3b12d…` |
| Street, off | — | — | — | — | 7.898 | 20.937 | `sha256:a7c1ff03…` |
| Street, contact | — | — | — | — | 8.045 | 21.096 | `sha256:89fd8911…` |
| Street, hybrid | 0.767 | 0.413 | 0.206 | 0.062 | 7.066 | 21.618 | `sha256:dde69663…` |

- **Ray passes.** They cost 1.58 ms on the plaza, where every pixel is in
  range, and 1.45 ms on the street. That is around the 1.5 ms gate.
- **Frame cost.** The hybrid frame costs 0.69 ms (plaza) and 0.68 ms
  (street) more. Rays without the march measured 0.63 and 0.61 ms more
  (`sha256:d37f51d1…`, `10f97c9c…`).
- **Contact march without rays.** It costs 1.18 ms of lighting on the sunlit
  plaza and 0.15 ms on the shaded street, against the 0.2 ms budget. Inside
  the lighting kernel the march is too expensive for the no-ray path. A
  separate half-resolution pass that writes a contact mask is the next thing
  to measure. Under the rays it marches only past 40 m and costs about
  0.05 ms. Contacts show at curb edges, facade relief and hedges
  (`sha256:da3d9c9c…` against `d00312b3…`).
- **Defect fixes.** With rays off, the frame matches the prototype's
  baseline (19.347 and 20.927 ms), so the fixes add no measured cost.

`local.bistro.shadow.sun_ray_plaza_strafe_capture` strafes the plaza camera
2.5 m in 2 s and captures `shadow_debug_factor` at frames 30, 60 and 100
(hybrid `sha256:6c00ef0e…`, off `sha256:fbfe2a6c…`):

- The hybrid shows no trail behind the moving bollard and hedge shadows, and
  keeps their edges sharp; the cascades' edges blur.
- The rays darken the plaza tree's crown, whose alpha-tested leaves shadow
  each other; the cascades leave it lit.
- With rays off, the defect 5 fix removes the diagonal streaks the cascades
  drew on the normal-mapped facade and columns.

A Debug run of the plaza in hybrid mode, with `VK_LAYER_KHRONOS_validation`
and synchronization checks, ran all four ray passes and reported no
validation messages (`sha256:0595340c…`).

Production readiness: not yet. Each item below blocks the High preset:

| Item | Why it blocks | Work |
|---|---|---|
| Skinned casters | Not in the TLAS. Within 50 m characters cast no sun shadow, and the contact march is off where the rays apply. Bistro has none, so this is unmeasured. | A BLAS per skinned instance from `Animation.Skinning`'s output, refitted each frame; or the contact march under the rays as a stopgap |
| Settings | An environment switch. | A shadow-preset field (High on ray-query devices), the graphics settings, and the harness fingerprint |
| Worst-case cost | 1.58 ms on the plaza, above the 1.5 ms gate. | A half-size trace image (it is full size today); resolve and temporal in one pass; history images that alternate instead of the copy |
| Foliage-heavy levels | Alpha-tested candidates run the `Proceed` loop; only Bistro is measured. | Level Design Test forest timing |
| Moving sun | The history lags about ten frames behind a moving sun; the clip bounds the trail. | A time-of-day sweep capture |
| Range | Fixed at 50 m. | Tie the range to the cascade splits or a preset value |
| No-ray path | The contact march costs 1.18 ms in lighting on the sunlit plaza. | A half-resolution contact pass, timed against the 0.2 ms budget |


## Tiled options

Costs are estimates for an M1 Pro at 1440p.

| Option | Effect | Cost |
|---|---|---|
| T0: fix defects 1–4 and 7–8; replace the rotated Poisson kernel with a fixed tent of 9 bilinear compares, because the tiled pipeline has no temporal resolve | jaggies and grain lower, pop fixed | −0.3 to 0 ms |
| T1: at most one cascade redraw per frame, far cascades refreshed over several frames, layered rendering with vertex amplification when two redraw | removes the +1.45 ms redraw frames from the median | about 0 ms |
| T2a, fixed sun: 3 cascades to about 80 m; the bake stores sun visibility per sun key past them | denser near cascades; static shadows over the whole level | −0.3 to −0.7 ms; one BC4 plane per key |
| T2b, moving sun: 3 cascades to about 80 m and one cached far level refreshed over 8–16 frames | denser near cascades; shadows to about 1 km | about 0 ms |
| T3: local lights take a 16-texel gather-compare tent, cross-face reprojection and screen-size face resolution | softer, steadier local shadows | +0.1–0.3 ms for 4 lights |
| T4: sun contact shadows through a sun-term correction after `Tiled.Opaque` (implemented, Epic) | contacts and detail finer than a texel near casters | measured about +0.28 ms |
| T5: SDF sun shadows past the near cascades and in their penumbrae, behind a prototype gate | soft penumbrae that harden at contacts; static and rigid casters; a moving sun redraws nothing | +0.5–1.5 ms; 50–100 MiB |
| T6: dense clipmap of 2048² levels anchored to the world, each redrawing only the tiles that scroll in | uniform 2× borders and no refits; density about as now (1.2–2.4 screen pixels per texel at 1440p) | about 0 ms; about twice the sun memory |

T2a blends two sun keys, so a sun between keys shows two offset copies of a
baked shadow; it suits only a fixed sun. ADR-087 declined a per-frame redraw
cap for the p95 because the p95 follows opaque cost; T1 targets the median and
hitch frames. T4's estimate of 0.2–0.4 ms, scaled from Bend Studio's 0.19 ms
on PS5, proved low; its measured cost is in the T4 section. T5's estimate is
scaled from Unreal's distance field shadows and is not measured.

Not chosen for the tiled pipeline:

- Sparse shadow-map clipmap (virtual shadow maps). Its pages are chosen from
  camera depth before the shadow passes draw, and `Tiled.Opaque` produces depth
  in the pass that shades the sun. The previous frame's depth would show a
  coarse level for a frame wherever a surface is uncovered. Drawing into
  scattered pages takes fragment atomics, which turn off M1's hidden-surface
  removal, or a render pass per page. Placement sparse textures are mapped by
  CPU calls on `MTL4CommandQueue`, so page requests need a readback. Unreal
  ships virtual shadow maps on Apple silicon from M2 only, citing image atomics
  and forward-progress guarantees. Revisit for an M3-class tier, or if the
  tiled pipeline gains camera depth before its shadow passes.
- T6, for now: it does not raise density. Revisit it if borders remain the
  main complaint after T4 and T5.
- Ray tracing: M1 has no hardware ray tracing; M3 is the first Mac with it.
- EVSM: RGBA32F moment bandwidth.

The earlier reason for declining screen-space shadows on this pipeline, that
`Tiled.Opaque` keeps camera depth in memoryless tile storage, does not hold:
the pass resolves depth into `opaque_vbuffer_depth`, which `Tiled.Atmosphere`,
`HZB.BuildBase` and MetalFX read. The constraint is order, and T4 works
around it.

## Hybrid sun shadows

Owner decision 2026-10-16. Each technique covers the others' weaknesses:

| | Cascades | Contact shadows | Ray queries (desktop) | SDF (tiled) |
|---|---|---|---|---|
| Strong | every caster: skinned, alpha-tested, moving, foliage | contacts the cascade bias loses; detail finer than a texel | exact edges and penumbrae from the sun's size | soft penumbrae that harden at contacts; range at small cost; independent of the sun direction |
| Weak | texel density, bias leaks at contacts, one blur radius for every penumbra, redraw cost | on-screen casters only, guessed thickness, short reach | skinned meshes are not in the TLAS; cost per ray | static and rigid casters only; thin casters under about two voxels; needs a cooked SDF |

The layers by range and caster:

| Range or caster | Desktop | Tiled |
|---|---|---|
| Base, to the cascades' end | cascades | 3 near cascades to about 80 m |
| Contacts | sun contact march in `Lighting.Deferred` | T4 correction |
| Near penumbra | ray queries (D2) | cascade filter; T5 where the caster is static |
| Far range | D1 distant levels | far level to 1 km; T5 decides the static casters |
| Skinned, alpha-tested, foliage, thin casters | cascades | cascades and the far level |
| Glass | no sun shadow | no sun shadow |

### Contact shadows on both pipelines

One shared kernel in `renderer/src/shaders/shared/` marches camera depth
toward the sun with a thickness test and a start offset that grows with
camera distance, as the local-light march does. The desktop pipeline runs it
in `Lighting.Deferred`, which already reads depth. The tiled pipeline runs it
in the T4 correction. The step noise follows the frame index only while a
temporal resolve runs (MetalFX on the tiled pipeline, SMAA T2x or FSR 3.1 on
the desktop pipeline); otherwise it is fixed.

### T4: the tiled sun-term correction

Implemented on 2026-10-16 as
[ADR-087](../adr/087-gpu-class-graphics-pipelines.md) decision 6 records:
on sun contact frames, `Tiled.Opaque` writes the sun's term to `tiled_sun_light`.
`Tiled.Atmosphere` then marches the resolved depth and subtracts the occluded
part, and it also runs on frames without media, so no `Tiled.SunApply` pass
exists. With the march's visibility forced to one, the frame matches a frame
without it bit for bit. The Contact shadows setting enables it, so only the
Epic preset has it.

On the Bistro plaza at 1440p and 0.6 scale on the M1 Pro it adds about
0.09 ms to `Tiled.Opaque` and 0.19 ms to `Tiled.Atmosphere`, within its
0.4 ms budget in local runs
([measurement](../adr/087-gpu-class-graphics-pipelines.md#sun-contact-shadow-measurement)).
Keeping the sun's light as three values cost the opaque pass about 1.2 ms
through lost occupancy; packed in one register it costs almost nothing.
Remaining work:

- Authoritative windowed timing (`performance-windowed-gpu`) of the same
  views, and the Level Design Test lift, freight cars and mannequin captures.
- The desktop pipeline's march in `Lighting.Deferred`, which needs no
  second target, through the same shared helpers.

### T5: tiled SDF shadows, prototype and gate

The tiled pipeline's Distance field shadows setting, on the Epic preset,
runs it in the mode the owner chose after the gate: the trace adds to the
far level ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md) decision 6,
`vkr_metal_packet_sdf.inc`, `world/sun_shadow.metal`). Harness cases select
it with `shadow_sdf` (`add`, or the `replace_all` diagnostic, which uses the
trace alone at every distance to compare it with the cascades).

- Volumes: one unsigned narrow-band distance volume per static opaque
  submesh, R8 at most 64³ with voxels no smaller than 0.25 world metres
  (half the far level's texel). The GPU generates them from the resident
  triangles, four a frame, the first time a frame traces them, so the
  prototype needs no cooked data; Bakery is to own them.
  Casters under 0.25 m or over 120 m (terrain), cutout, skinned and moving
  casters keep the cascades.
- Culling: a ray toward the sun is one point in the plane across the sun,
  so a light-space grid of 128² cells over 800 m around the camera lists
  each cell's casters; the CPU rebuilds it each frame.
- Trace: `Tiled.SunShadow` sphere-traces at half resolution past where the
  far level starts, with a penumbra from the nearest miss over the sun's
  half angle (at least half a degree). The atmosphere draw takes the darker
  of the trace and the far level through the opaque pass's unshadowed sun
  light.

Gate results on the M1 Pro, 2026-10-16, local and not authoritative:

| Measure | Result | Gate |
|---|---|---|
| Trace, Level Design Test valley and freight views | 0.25 and 0.32 ms at 1146×595 in the headless editor, about 0.5 and 0.64 ms scaled by pixels to 1440p at 0.6 | 1.0 ms |
| Trace, Bistro plaza traced at every pixel (`replace_all`) | 0.55 ms median, 0.57 ms p95, at 1440p and 0.6 (`tiled_bistro_plaza_timing_sun_sdf_replace_all`) | 1.0 ms |
| SDF memory, Level Design Test | 41.5 MB: 13,943 volumes and 8 MB of generation scratch | 96 MiB |
| SDF memory, Bistro | 23.7 MiB with scratch | — |

At 64³ without the voxel floor the Level Design Test took 318 MB. Captures:
the trace reproduces Bistro's building shadows with softer edges when it
replaces the cascades everywhere. Past the near cascades on the Level
Design Test, the trace alone sharpens the viaduct piers' shadows but loses
the terrain's and the forest's, which have no volumes; adding it to the far
level keeps them and adds the piers' sharper shadows. Not done: a moving-sun sweep, the rigid
movers (lift, freight cars), authoritative 1440p timing of the Level
Design Test, Metal validation.

## Budgets

Targets for Bistro at High. Current 1440p desktop values are scaled from
1080p by pixel count, not measured.

| Budget | Now | Target |
|---|---|---|
| Desktop sun, all passes, median, 1440p | about 1.1 ms | 2.0 ms or less with D2 |
| Desktop sun, extra cost in any one frame | 0.17–0.33 ms per cascade redraw | 0.5 ms or less |
| Desktop local shadows, median, 1440p | about 6.3 ms | 3.0 ms or less (local shadow architecture) |
| Desktop shadow memory | about 730 MiB | 600 MiB or less |
| Tiled sun, all passes, median | not measured | 2.0 ms or less |
| Tiled cascade redraw in one frame | 1.4–2.1 ms; two in 1% of frames | 1.0 ms or less; never two |
| Tiled local shadows, 4 lights | +1.7 ms median, +5.0 ms p95 | 1.5 ms median, 2.5 ms p95 or less |
| Desktop sun contact shadows, median, 1440p | none | 0.2 ms or less |
| Tiled sun contact shadows (T4), median, 0.6 scale | about 0.28 ms, Bistro plaza, not authoritative | 0.4 ms or less |
| Tiled SDF trace (T5), median, 0.6 scale | none | 1.0 ms or less |
| Tiled SDF memory (T5), Level Design Test | none | 96 MiB or less |
| Visible pop | defects 1–4, 7, 8 | no shadow appears or vanishes in under 0.25 s in a Bistro orbit |

## Order

1. D0 and T0 on both pipelines.
2. A scoped Bistro case that orbits across all four cascade borders and past
   54 m, captured and timed on each backend.
3. Desktop: D1, with its distant levels and sun contact shadows, and the D2
   prototype gate, which passed (see the prototype result). Tiled: T1 and
   T2b, implemented.
4. Tiled: T4, implemented.
5. Desktop: D2 at one ray per 2×2 block. Tiled: the T5 prototype and gate.
6. Tiled: T3.

## Decisions

Settled by the owner on 2026-10-16:

- Tiled levels change the time of day at runtime, so the tiled pipeline uses
  T2b.
- Glass casts no sun shadow from the cascades.
- The hybrid layering above. Desktop: sun contact shadows, then the D2 gate,
  with D3 as the fallback; no SDF. Tiled: T4, then the T5 prototype; no
  sparse clipmap; T6 not now.
- After the T5 gate, which the prototype passes on cost and memory, SDF
  shadows add to the far level (the darker of the two) as the Distance field
  shadows setting, on the Epic preset: the trace alone loses the terrain's
  and foliage's far shadows.

Open:

- Replacing the far level for static casters, which softens their
  penumbrae, once the far level draws only the casters without volumes
  (terrain, foliage, moving and skinned casters).
- Bakery cooking the distance volumes, and the High preset once
  authoritative timing confirms the budget.

## Evidence required

- Defect fixes: CPU tests that a redrawn cascade's texel and origin stay on
  the raw fit's world grid, and that shadow-distance and small-caster cuts
  fade; a Bistro orbit on each backend with no shadow appearing or vanishing
  in under 0.25 s, using depth and `gbuffer_*` channels as the A/B oracle.
- Each option: capture-free Release before/after reports on its native
  backend (RX 6700 XT for Vulkan, an M1-class Mac for Metal) meeting the
  budgets above, with equivalent work and output.
- D2: the production items in the hybrid prototype's table, a capture
  with a skinned caster in ray range, and Vulkan validation of D2's final
  passes. The gate result, the strafe capture and the prototypes'
  validation runs are recorded above.
- Contact shadows: captures with contact shadows off and on at the Level
  Design Test's lift, freight cars and mannequin feet and at Bistro's chairs
  and tables, on each backend.
- T4: met on 2026-10-16, Bistro street and plaza on Metal: with the march
  forced to full visibility, the corrected frame matches the frame without
  contact shadows bit for bit. Local timing is within the budget. Still
  needed: authoritative timing and the Level Design Test captures.
- T5: the gate report above, and a Level Design Test sweep of the sun that
  shows no SDF work caused by the sun's motion.
- D3, if built: Vulkan validation and a page-pool residency report over the
  Bistro orbit.
