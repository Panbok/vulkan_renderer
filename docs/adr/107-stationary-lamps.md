---
status: partial
updated: 2026-10-10
authority: adr
---

# ADR-107: Stationary lamps

## Status

Accepted (partial). A point or spot light can be `stationary`: the bake
keeps its bounce light and a shadow mask, and the runtime shades its direct
light. On the tiled pipeline (Metal) the nearest stationary lamps take
runtime shadow maps and the others shade through the baked mask. The
desktop pipeline (Vulkan) lights stationary lamps as dynamic lamps over
their baked bounce. The Metal bake, runtime and level conversion ran on the
M1 Pro. The Vulkan bake and the desktop runtime are compiled from source
only; no Windows run exists.

## Context

On the tiled pipeline a scene with a loaded lightmap set drops its static
lamps from runtime lighting ([ADR-087](087-gpu-class-graphics-pipelines.md)
section 11). Their direct light and shadows come from the lightmap, at 8
texels per metre (12.5 cm per texel), after smoothing and ASTC encoding
([ADR-088](088-baked-lightmap-sets.md)). In the Testbed project's "Level
Design Test" scene every visible shadow came from 153 static spot and point
lamps, and the scene's only sun has zero intensity. Shadows there were
blurred blobs with stepped edges at every shadow quality, because Shadow
quality drives only runtime shadow maps (user report, 2026-10-10). Bistro's
visible shadows come from the sun's runtime cascades.

Owner decision (2026-10-10): add stationary lamps, as Unreal Engine does,
instead of sharper baked shadows or a higher-density bake.

## Decision

### Mobility

`VkrLightMobility` gains `VKR_LIGHT_MOBILITY_STATIONARY`
([`vkr_scene_system.h`](../../runtime/src/renderer/systems/vkr_scene_system.h)).
Scene JSON writes `"stationary"`, the editor overlay writes 2, and the Lights
window cycles point lights through Static, Stationary and Dynamic
([`editor_lighting.c`](../../editor/src/editor_lighting.c)). Rectangle
lights cannot be stationary: their mobility names stop at dynamic and the
loader rejects `"stationary"` on them. A stationary light keeps its light
group: its bounce and its runtime direct light both scale by the group
factor.

### Bake

[`vkr_bake_stationary.cpp`](../../tools/bake/vkr_bake_stationary.cpp) plans
the enabled stationary lights of a tiled bake:

- **Keys.** A stationary light without a document id, a positive range or a
  point or spot kind is disabled for the bake with a warning. The runtime
  then finds no record for it and lights it as a dynamic light.
- **Channels.** The shadow mask has four channels. Two shadow-casting lamps
  conflict when their ranges overlap, both reach a common lightmapped
  instance and nothing blocks the segment between their centres. Lamps take
  channels in order of weight (colour luminance × intensity), each the
  first channel no conflicting lamp holds, else the channel whose holders
  cost least. A lamp that casts no shadow has no channel.
- **Ownership.** At a point, a channel belongs to the instance's candidate
  of that channel with the largest metric `weight × window(d, range) / d²`
  times the spot cone, the lower index on a tie
  (`vkr_lightmap_stationary_metric` in
  [`vkr_lightmap_set.c`](../../runtime/src/assets/vkr_lightmap_set.c)). The
  metric uses the bake's lamp terms, so an edited lamp keeps the bake's
  ownership.
- **Candidates.** The lamps whose range reaches an instance's world bounds
  may light it. Before each page gathers, the page's texel centres rank them
  per channel, and an instance's candidates are the lamps that own a channel
  at one of its texels at least, or have no channel and light one; past 16,
  those that own the most texels. A reaching lamp that is not a candidate
  bakes as a static lamp on that instance.
- **Texel direct.** The Metal gather leaves a stationary lamp's direct term
  out of a texel where the lamp is a candidate and owns its channel or has
  none (`stationary_at_runtime` in
  [`vkr_bake_metal.mm`](../../tools/bake/vkr_bake_metal.mm)); elsewhere it
  bakes the lamp's direct light as it bakes a static lamp's. Bounce paths
  keep every stationary lamp.
- **Mask.** A second kernel (`stationary_mask`) writes, per channel, the
  owner's visibility at the texel: the mean over the reaching points of the
  3×3 footprint grid that the texel direct term uses, of the luminance of
  the shadow transmittance toward the lamp; one where no lamp owns the
  channel. Buried texels fill from their neighbours, and the page encodes as
  ASTC 4×4 LDR.
- **Desktop sets.** A desktop bake (Vulkan, or Metal with
  `--pipeline desktop`) leaves every stationary lamp's direct term out of
  the texels and writes no mask.
- **Volumes.** The diffuse volume's lamp direct bands leave stationary lamps
  out; its layers keep their bounce.

### VKLM v5

[`vkr_lightmap_set.h`](../../runtime/src/assets/vkr_lightmap_set.h): after
the instance table, a set with stationary lamps holds 64-byte lamp records
(document id, position, range, unit direction, weight, cone cosines, kind,
channel), one candidate range per instance and the 16-bit candidate indices.
The header names their counts and offset in bytes 104 to 119. A
`VKR_LIGHTMAP_PLANE_SHADOW_MASK` plane on layer 0 holds the mask. Version 4
files, which have neither, still decode.

### Runtime

- **Records.** The loader copies the records and candidates and packs each
  instance's candidate range into its rectangle (`VkrLightmapRect::stationary`:
  first index in bits 0 to 26, count in bits 27 to 31). The mask uploads as
  a second 2D array, one slice per page
  ([`scene_loader.c`](../../runtime/src/renderer/resources/loaders/scene_loader.c)).
  A tiled set with stationary lamps and no usable mask fails to load.
- **Binding.** Lightmap binding matches each record to its light entity by
  document id ([`vkr_scene_lightmaps.c`](../../runtime/src/renderer/systems/vkr_scene_lightmaps.c)).
  The lighting system keeps bound stationary lamps out of the point light
  table, in record order with their live light
  ([`vkr_lighting_system.c`](../../runtime/src/renderer/systems/vkr_lighting_system.c)).
- **Runtime shadows.** `vkr_lighting_system_shadow_stationary` gives
  runtime shadow maps to the stationary casters nearest the camera, ranked
  as the dynamic-light limit ranks lights with its 1.15 incumbent bonus:
  6 at High, 3 at Balanced, none with shadows or local shadows off
  (`stationary_shadow_lamps` in
  [`vkr_shadow_system.h`](../../runtime/src/renderer/systems/vkr_shadow_system.h)).
  Their lights follow the point lights in the list the local shadow cache
  resolves, so they keep cached static faces and take dynamic squares where
  a moving caster reaches them ([ADR-019](019-bounded-forward-spatial-lighting.md)).
- **Shading.** Frames with stationary lamps take the
  `VKR_METAL_TILED_LIGHTING_STATIONARY` variant: every dynamic light as the
  ALL variant draws them, plus the stationary lamps; the inspection variant
  also draws them
  ([`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal),
  `vkr_metal_tiled_stationary`). A lightmapped surface ranks its instance's
  candidates per channel by the metric and shades each owner with the full
  BRDF. Its visibility is the bilinear mask channel, blended toward the
  runtime shadow map by the shadow's strength, so a lamp entering or leaving
  the shadowed set fades over the cache's fade time. A candidate without a
  channel shades unshadowed. A surface without a lightmap shades only the
  shadowed lamps, faded in by their strength. Frames without stationary
  lamps take the existing variants.
- **Desktop.** Desktop sets carry no records, so the lighting system keeps
  stationary lamps in its table as dynamic lamps with local shadows
  ([ADR-104](104-desktop-baked-lamps.md)).

## Consequences

- Near the camera, lamp shadows follow Shadow quality: a 16 m lamp's cube
  faces are 1024² at High and 512² at Balanced. Moving casters such as the
  player shadow the shadowed lamps.
- Farther lamps shade through the mask, which keeps the 12.5 cm texel but
  is not smoothed and keeps specular highlights.
- Where more than four lamps meet with conflicting channels, or a large
  instance has more than 16 owners, the extra lamps bake as static lamps
  there. Where ownership changes between texels of one channel, the
  bilinear mask mixes two lamps' visibility across about one texel.
- A surface without a lightmap gets no direct light from stationary lamps
  without runtime shadows; their bounce reaches it through the volume.
- The level's set grew from 101 MB to 151 MB for the mask pages.

## Alternatives considered

- **Sharper baked shadows** (a high-density visibility mask and bicubic
  sampling): Shadow quality would still change nothing and edges would stay
  soft. Not chosen (owner, 2026-10-10).
- **Bake again at 16 texels per metre**: four times the memory and bake
  time for half the step size. Not chosen.
- **Channels by range overlap alone** (Unreal Engine's rule): on the level's
  lamps, 7 to 28 m ranges, a greedy colouring by range spheres left about 40
  to 53 of 153 lamps without a channel. Line of sight between lamps and
  per-point ownership keep every lamp stationary.
- **Candidates as the nearest lamps to an instance's bounds**: every lamp
  above a large floor is at distance zero, so the cut kept the lowest
  indices and left the lamps over the camera baked.
- **One owner list per texel**: 8 more bytes per texel for the lamp
  indices; the runtime ranking reproduces them from the candidates.

## Evidence and remaining checks

Release, M1 Pro, Metal, 2026-10-10; local and non-authoritative.

- **CPU.** `vulkan_renderer_tester --suite run_lightmap_bake_tests`
  (`test_lightmap_set_stationary_round_trip_and_rejects`: records, ranges,
  candidates and the mask plane survive a round trip; a mask without lamps,
  a candidate past the lamps, descending candidates and a lamp without a
  range are rejected; the spot metric is zero outside its cone and range),
  `run_scene_loader_tests` (a stationary point light loads with its group, a
  stationary rectangle light is rejected), `run_scene_edit_tests`,
  `run_lighting_system_tests`, `run_metal_packet_abi_tests` and
  `run_metal_material_tests` pass in Debug.
- **Level Design Test bake.** The 153 shadow-casting static lamps became
  stationary through `component.set`; the 11 unshadowed lamps stayed
  static. `lighting.bake` (64 samples, volume spacing 1.5) planned 153
  lamps, all with a channel and 8 sharing one with a conflicting lamp; 14,850
  reaching lamp slots chose 9,081 candidates, and 6 instances were capped at
  16. The mask pages took 1.5 s of GPU time, the lightmap job 150 s and the
  volume job 76 s. The set is VKLM v5, 151 MB
  (`sha256:69a882bd8191b75a9f21f5e196a5e18e60d111d9cbfc6acbd330d30c2297b5df`).
- **Level runtime.** All 153 records bind. At High, 6 lamps take runtime
  shadows (36 cube faces). Lighting-only captures of the lobby and an office
  desk show hard chair, desk and stair shadows at High and Medium and the
  soft mask at Low, where local shadows are off; before the change both
  showed only the soft baked shadow.
- **GPU time.** Headless editor Scene view, `VKR_RG_GPU_TIMING=1`, median of
  40 frames, presets set live with `gfx.preset`, two runs each:

  | View | High: passes / `Tiled.Opaque` | Medium | Low |
  |---|---|---|---|
  | Lobby | 3.58 / 2.74 ms | 3.56 / 2.72 ms | 2.91 / 2.09 ms |
  | Desk | 3.32 / 2.45 ms | 3.40 / 2.56 ms | 2.91 / 2.06 ms |

  Low also changes other preset settings. No build without stationary lamps
  was measured on this level, so these numbers do not give the feature's
  cost. Bistro has no stationary lamps and takes the same variants and work
  as before; its timing was not measured.
- **Bistro.** `vkr_harness snapshot --case
  tools/cases/local/tiled_bistro_baked_capture.case.json --profile
  tools/profiles/local-offscreen.json` passes, and the v4 Bistro set loads
  with its baked lamps; no baseline exists, so the image was inspected only.
- **Validation unavailable.** The editor under `MTL_DEBUG_LAYER=1
  MTL_SHADER_VALIDATION=1` aborts with "command buffer residency set limit
  of 32 exceeded" on this scene and on the Testbed's other scene, which has
  no stationary lamps; no Metal validation of the new shader code ran.
- **Vulkan sources.** `slangc` compiles `lightmap_gather`, `probe_gather`
  and `lightmap_trace_benchmark` from `vkr_bake_lightmap.slang`;
  `vkr_bake_vulkan.cpp` passes a clang syntax check against the Vulkan SDK
  headers with stub SPIR-V arrays. Neither ran.

## Revisit when

- A level needs moving receivers lit by every stationary lamp: bake a
  per-probe stationary visibility into the diffuse volume.
- The desktop pipeline needs the mask, for example for scenes with more
  stationary lamps than its point light table holds.
