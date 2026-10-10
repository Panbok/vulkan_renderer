---
status: partial
updated: 2026-10-10
authority: adr
---

# ADR-104: Baked static lamps on the desktop pipeline

## Status

Accepted (partial). The desktop pipeline (Vulkan) bakes static lamps
into lamp-group lightmap layers with direction pages. It removes the lamps
from runtime lighting and shades the baked light per pixel on opaque and
transmissive surfaces. Moving receivers take lamp light from the sparse
volume, and moving casters shadow the two baked lamps whose light at them is
strongest. The shared payload, validation and atlas changes are unverified on
Metal, and no timing here is authoritative.

## Context

On the desktop pipeline, Bistro's static lamps ran through the point-light
list, the light grid, local shadow selection and `Shadow.LocalMask`. A local,
non-authoritative cost split on the RX 6700 XT (1920 × 1080, TAA, High) put
them at 4.63 ms of an 11 ms GPU frame
([lighting efficiency](../proposals/lighting-efficiency.md#vulkan-cost-split-2026-10-03)).
The tiled pipeline already bakes static lamps
([ADR-088](088-baked-lightmap-sets.md)), but desktop GPUs do not sample its
ASTC pages, and the bake needed Metal.

Owner decisions:
- **2026-10-07:**
  - Bake a static lamp's direct light, shadows and bounce fully; it changes
    only through its light group's factor ([ADR-090](090-time-of-day.md)).
  - Add a dominant-direction page per lamp layer.
  - Moving objects take lamp light from the sparse volume.
  - Moving casters shadow the nearest lamps, two at most.
- **2026-10-09:**
  - Windows bakes with a Vulkan ray-query gather.
  - Moving-caster shadows build on a shared static/dynamic split of the
    local shadow atlas for both pipeline classes
    ([ADR-019](019-bounded-forward-spatial-lighting.md)).
  - The two lamps are those whose light at the moving casters is strongest,
    not those nearest the camera: from the Bistro street camera the nearest
    lamps stood 8 to 12 m from the mannequin and its shadow barely showed.
  - Each platform is baked, packaged and tested on its own machine, so a bake
    writes only its host's planes and BC planes need not match across hosts.

## Decision

1. **Data.** Each lamp-group layer of a VKLM v4 set (ADR-088) carries:
   - its irradiance as BC6H, or RGB9E5 with `--desktop-encoding
     uncompressed`;
   - its direction page as BC7, or RGBA8: `0.5 d + 0.5` in RGB, with the
     directionality `|d|` in alpha, where `d` is the luminance-weighted
     mean incident direction.

   A bake on Windows or Linux writes only these planes, for the lamp
   groups alone; a Mac bake writes the tiled pipeline's ASTC planes
   ([ADR-088](088-baked-lightmap-sets.md)). Each platform is baked,
   packaged and tested on its own machine (owner decision, 2026-10-09), so
   no bake carries the other platform's data.
   The bake encodes BC on the GPU with DirectXTex's compute shaders
   ([`vkr_bake_gpu.h`](../../tools/bake/vkr_bake_gpu.h),
   [vendor note](../../vendor/directxtex.md)): BC6H tries modes 11 to 14,
   then 1 to 10, BC7 modes 4 to 6, then 1, 3 and 7. A device without
   `shaderIntegerDotProduct`, 16-bit types or BC sampling bakes
   uncompressed. BC bytes repeat on one GPU and are not required to match
   across GPUs, since only the platform that ships a package bakes it.
   A desktop bake keeps only lamp groups that hold light, lamps, emission
   or sky; a scene with none has nothing to bake, and `vkr_bakery bake
   lightmap` reports it as skipped (status 4), as it does a scene without
   lightmap UVs.
2. **Selection.** A scene whose set has desktop lamp planes, loaded on the
   desktop pipeline, marks its lamps baked (`lamps_baked`,
   [`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c)).
   `vkr_standard_scene_runtime_static_lights_baked` then drops its static
   lamps from the point lights, the light grid, contribution ranking and
   local shadows. A scene without such a set keeps runtime lamps.
3. **Static receivers.** For a draw with a lightmap slot, `pass.gbuffer.resolve`
   maps the decoded lightmap UV into the draw's rectangle and sums the
   active lamp layers at their weights. It writes the irradiance into
   `baked_lamp_irradiance` (RGBA16F) and the weighted direction and
   directionality into `baked_lamp_direction` (RGBA8). Alpha carries the
   facing scale `1 / max(n · d, 0.1)` and is zero without a lightmap
   ([`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
   `vkr_vk_baked_lamps`). Deferred lighting splits the irradiance:
   - the undirected share, `(1 − directionality) / π`, is plain diffuse
     light;
   - the directed share, scaled by the facing scale, is a light from the
     dominant direction through `packet_direct`, so normal maps modulate it
     and the BRDF gives its highlight.

   GTAO does not darken either share, since the bake holds the lamps'
   occlusion. SSGI does not bounce it again. Transmission shading samples
   the same lamp layers for a lightmapped transmissive draw through the
   shared `vkr_vk_baked_lamps` and adds both shares to its analytic light;
   its volume lookup then leaves out the lamp part, as below.
4. **Moving receivers.** The sparse volume keeps each lamp group's bounce in
   its layer and the group's direct light in a direct band
   ([ADR-054](054-baked-diffuse-volumes.md)). On a frame that samples baked
   lamps, `DiffuseVolume.Compose` writes the lamp groups and their direct
   bands into a second band. A pixel without a lightmap reads both bands; a
   lightmapped pixel reads only the first, so its lightmap's lamp light is
   not counted twice. With runtime lamps the direct bands stay out.
5. **Moving casters.** The lighting system keeps the static shadow-casting
   lamps it drops as baked candidates. Each frame the shadow system scores a
   candidate by its light at the moving casters it reaches, colour luminance
   × intensity × distance falloff × spot cone at the caster, over casters
   whose bounds meet its range on a face the camera may see, and selects the
   best two; an incumbent's score counts 1.15 times
   ([`vkr_shadow_system.c`](../../runtime/src/renderer/systems/vkr_shadow_system.c)).
   No lamp is selected while the moving casters are unknown or a publication
   is pending. A selected lamp's static squares take one atlas layer before
   the dynamic band, present only while the scene has baked lamps and moving
   casters, and its faces follow ADR-019's static and dynamic squares. It
   publishes two view blocks, the composite (the dynamic square where a
   caster reaches, the static one elsewhere) and the static, and enters
   neither the light table nor the light grid nor the mask's light slots.
   On lightmapped pixels, compile-time variants of `Shadow.LocalMask` take
   the same filter taps through both blocks and write the light the moving
   casters hide, `L · max(v_static − v_composite, 0)`; a face whose two
   views name the same square adds nothing and samples nothing. Deferred
   lighting subtracts it from the baked irradiance, clamped at zero, before
   the undirected and directed split. Without a selected lamp the graph drops
   the image, and the default mask kernels carry no baked-lamp code.

## Consequences

- Static lamps change only through their group factor. A lamp that moves,
  flickers or switches on its own must be `dynamic`.
- Baked lamp light is soft at 8 texels per metre: hard lantern and bracket
  shadows that runtime shadow maps draw on Bistro's walls come out softer or
  weaker. The directed share gives an approximate highlight, not one per lamp.
- A moving object takes lamp light at probe resolution (1 m) and has no lamp
  shadow on itself. Only two baked lamps cast its shadows; the others pass
  through it, and a tap that crosses into a face without a dynamic square
  can lose a sliver of a soft shadow edge.
- Bistro's desktop lamp data at 2,909 instances, three 4096 pages and one
  lamp group:
  - irradiance: 192 MiB as RGB9E5, or 48 MiB as BC6H;
  - direction: 192 MiB as RGBA8, or 48 MiB as BC7;
  - the two G-buffer images: 12 bytes per pixel, 44.2 MB at 2560 × 1440.

## Alternatives considered

- **Keep runtime lamps on the desktop pipeline.** Costs about 4.6 ms at
  1080p on Bistro.
- **Irradiance only, without direction.** Normal maps and highlights would
  lose the lamps.
- **Lighting reconstructs the lightmap UV itself.** That repeats the resolve's
  triangle and barycentric work; the resolve already holds both.
- **Nearest the camera.** The proposal's first rule; it picked lamps that
  barely shade the mover and skipped the one above it.
- **A moving-only face set per lamp.** It saves a copy per face but needs a
  second lookup per tap; the composite set gives the exact difference per
  tap and shares ADR-019's layout.
- **One set with both classes' planes, stripped at packaging.** Chosen on
  2026-10-07 and dropped on 2026-10-09: every bake paid for the other
  platform's planes (on Bistro about 75 s of ASTC encoding, eight sun-key
  layers and 450 MB) that its own machine re-bakes anyway.

## Evidence and remaining checks

Release, RX 6700 XT, Vulkan, 2026-10-09 and 10; captures and timings are
local and non-authoritative.

- **Fixture.** On the three-room leak fixture, baked lamps matched the
  runtime lamp within 4 to 9 % of HDR pixel values before direction pages
  were added. With GPU BC planes the final image differs from RGB9E5/RGBA8 by
  at most 4/255 and lit pixels by 0.28 % on average (DirectXTex's CPU codec,
  since removed: 3/255 and 0.22 %, in 57 s against 0.11 s on the GPU).
- **BC on Bistro** (three 4096 pages, one lamp group). The desktop bake takes
  128 to 136 s, of which the GPU gather is 31 s and scene load 69 s, against
  283 s for the nine-layer set with both classes' planes, and stores 101 MB
  against 906 MB; the uncompressed desktop set stores 403 MB. Per page, BC6H
  encodes in 0.4 to 0.6 s and BC7 in 1.6 to 2.9 s; two bakes write the same
  bytes. Page-wide errors (BC6H 4 to 8 % relative RMS, BC7 up to 219/255)
  sit mostly in the dilated texels between charts: the night view rendered
  with BC planes against uncompressed ones keeps exposure (5.270 against
  5.267), changes lit pixels by 0.78 % on average and only 0.47 % of pixels by
  more than 8/255, on thin edges and chart seams (runs
  `20261010T064117.080Z-003fad` and `20261010T064224.171Z-002856`). A
  validation-layer run of a BC bake reports no message. The AMD driver
  compiles the BC6H pipelines once, about 72 s, then caches them.
- **Bistro night captures** (`bistro_lamps_{current,lightmapped,baked}_night_capture`,
  1920 × 1080). After the bake's shadow clip, baked lamps light the street
  with automatic exposure 5.36 against 7.65 for runtime lamps; the baked
  view is brighter by the lamp bounce the runtime lacks. Before the clip,
  every Bistro lamp baked occluded by its own lantern and the frame needed
  exposure 397.
- **Transmission.** A capture of the baked night view before and after
  lightmap sampling in transmission shading keeps depth identical and
  changes only window and lantern glass (9 % of pixels, up to 135/255),
  which had lost the static lamps' light.
- **Moving casters.** Fixtures `bistro_lamps_baked_night_moving_caster_local`
  (the walking mannequin about 3 m from a lamp) and
  `bistro_lamps_baked_night_far_caster_local` (the mannequin out of range).
  From the street camera (`bistro_lamps_baked_night_moving_capture`) two of
  nine candidates are selected with five dynamic faces, and two shadows fall
  on the pavement; about 14,500 pixels change by more than 6/255 against the
  plain baked night, where nearest-camera ranking changed 6,400. A Debug run
  with the validation layer's synchronization checks reports no message.
  `test_local_shadow_baked_lamp_selection` covers the ranking, the incumbent
  bonus, both view blocks, validation, and nothing selected out of range or
  with unknown casters.
- **Cost.** `local-offscreen-gpu-repeated` (5 repetitions, 1,500 samples
  each) on an idle GPU, 2026-10-09 evening, after transmission sampling and
  the mask variants. Mean GPU times in ms:

  | Case | Frame | `Shadow.LocalMask` | `Lighting.Deferred` | `GBuffer.Resolve` | Transmission shading |
  |---|---:|---:|---:|---:|---:|
  | 1080p runtime lamps (`bistro_lamps_current_night_1080`) | 10.39 | 3.08 | 3.00 | 0.60 | 0.62 |
  | 1080p baked (`bistro_lamps_baked_night_1080`) | **5.66** | — | 1.54 | 0.73 | 0.25 |
  | 1440p runtime lamps | 18.02 | 5.07 | 5.50 | 1.11 | 0.95 |
  | 1440p baked | **9.95** | — | 2.73 | 1.30 | 0.37 |
  | 1080p baked, mannequin out of range | 6.44 | — | 1.58 | 0.75 | 0.26 |
  | 1080p baked, mannequin near a lamp | 8.08 | 1.34 | — | — | — |

  Baking saves 4.7 ms at 1080p and 8.1 ms at 2560 × 1440. The lightmapped
  model's 24 % more vertices cost nothing measurable (10.34 against 10.40 ms
  at 1080p in an earlier run), the resolve's lightmap sampling costs 0.13 to
  0.19 ms, and transmission sampling 0.05 to 0.08 ms. Out of range, no lamp is
  selected and the frame differs from the plain baked night by the
  mannequin's skinning alone (0.97 ms). Near a lamp, two lamps add the mask
  (1.34 ms), copies (0.10 ms) and dynamic draws (0.22 ms). An earlier build
  that compiled the baked-lamp work into the default mask kernel slowed the
  runtime-lamp mask by 13 % (3.05 to 3.46 ms at 1080p); the variants removed
  that. Reports: `20261009T202051.011Z-002e7a`, `20261009T202713.270Z-00045f`,
  `20261009T211159.780Z-000685`, `20261009T211927.415Z-00111e`,
  `20261009T205227.181Z-0036c4`, `20261009T212844.490Z-001d8b`.

Unavailable on this host:
- the tiled pipeline, which takes no baked-lamp data, and Metal's side of the
  shared payload, validation and atlas changes: `tiled_bistro_baked_native`
  has not run;
- the Metal shadow clip (uncompiled);
- an authoritative, clean-tree timing.

## Revisit when

- **Moving casters.** A scene needs more than two lamps' shadows on its
  movers, or the mask's 1.3 ms with two lamps selected must shrink, for
  example at half resolution.
- **BC quality.** A capture review finds block artefacts on lightmapped
  surfaces; BC7 modes 0 and 2, off as upstream ships them, or a padded chart
  layout are the next steps.
- **Texel density.** A capture review finds lamp shadows too soft at
  8 texels per metre; 16 texels per metre near lamps is the next step.

## Code evidence

- Bake: [lightmap baker](../../tools/vkr_lightmap_baker.cpp), [GPU gather](../../tools/bake/vkr_bake_lightmap.slang), [GPU BC encoder](../../tools/bake/vkr_bake_vulkan.cpp)
- Asset: [VKLM codec](../../runtime/src/assets/vkr_lightmap_set.h)
- Runtime: [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c), [scene lightmaps](../../runtime/src/renderer/systems/vkr_scene_lightmaps.c), [standard scene runtime](../../runtime/src/application/vkr_standard_scene_runtime.c), [lamp selection](../../runtime/src/renderer/systems/vkr_shadow_system.c), [local shadow cache](../../runtime/src/renderer/systems/vkr_local_shadow_system.c)
- Renderer: [Vulkan resolve and lighting](../../renderer/src/shaders/vulkan/slang/world/deferred.slang), [graph](../../assets/render_graphs/main.rendergraph.json)
