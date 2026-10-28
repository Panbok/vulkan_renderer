---
status: partial
updated: 2026-10-28
authority: adr
---

# ADR-136: Detail maps on Standard materials

## Status

Accepted (partial). Since 2026-10-28 an opaque or alpha-tested Standard
material can carry a tileable detail normal map and an optional grey detail
albedo overlay, sampled at a higher tiling of UV0 over its own maps. The
tiled pipeline shades them in Standard draw kinds of their own, so draws
without detail maps run the code they ran before. The desktop pipeline
takes a G-buffer resolve variant in frames whose scene has a detail
material. Metal has native evidence; Vulkan has compiled SPIR-V only.

## Context

Large props and unique-baked assets (one UV0 atlas over a whole object)
lose surface detail close to the camera: their maps run out of texels.
A detail map repeats a small tileable texture many times across the same
UV0 and adds its relief and albedo variation on top. The artist toolkit
lists one detail normal map in the Standard tier
([the artist toolkit proposal](../proposals/artist-toolkit.md), Part 3), and
[ADR-095](095-layered-standard-materials.md) left detail normals as
remaining work.

The owner approved the feature on 2026-10-28 with one constraint: draws
without detail maps pay nothing, so the feature is a per-draw compile-time
variant, never a runtime branch in the general tiled pipelines. On
2026-10-13 runtime-gated optional material code raised Metal tiled
`gpu.submission` p50 on Bistro at 2560×1440 from 28.7 to 33.0 ms with every
value at identity, from register pressure alone
([ADR-113](113-brdf-material-extensions.md#shading-variants)).

## Decision

### Material keys

| Key | Value | Default | Meaning |
|---|---|---|---|
| `detail_normal_texture` | texture path, class `normal_rg` | none | Tangent-space detail normal, glTF/OpenGL +Y like `normal_texture`; required by every other detail key |
| `detail_normal_scale` | [0, 8] | 1 | Its strength, as `normal_scale` scales the material's |
| `detail_uv_scale` | [0.01, 1024] | 8 | Repeats of both detail maps per UV0 unit |
| `detail_albedo_texture` | texture path, class `data_mask`, linear | none | Grey overlay about 0.5 |
| `detail_albedo_strength` | [0, 1] | 1 | The overlay's weight |
| `detail_mask` | `none` or `base_alpha` | `none` | `base_alpha`: the base colour's alpha weighs both maps; needs `alpha_mode=opaque` |

- The maps take texture slots `VKR_TEXTURE_SLOT_DETAIL_NORMAL` (24) and
  `VKR_TEXTURE_SLOT_DETAIL_ALBEDO` (25); the Custom slots move to 26 to 33.
  The scalar values live in `VkrPbrProperties`
  ([vkr_render_resources.h](../../renderer/src/vkr_render_resources.h)).
- The loader
  ([material_loader.c](../../runtime/src/renderer/resources/loaders/material_loader.c),
  `vkr_material_loader_parse_detail_key`, `vkr_material_loader_validate_detail`)
  refuses a misspelt `detail_` key, a value out of range, any detail key
  without `detail_normal_texture`, and detail maps on a blended (explicit or
  inferred), refracting, layered or Custom material. Terrain replacement
  drops layer 0's detail keys.
- `vkr_material_detail_active` decides whether a material shades with them:
  an opaque or alpha-tested PBR material without layers or refraction whose
  detail normal slot holds a map. The slot stays empty while its map
  streams in, so the material draws without detail until then. Publication
  checks the ranges again (`vkr_gpu_material_extensions_valid`).
- The lightmap baker accepts every `detail_` key and leaves the maps out:
  they add close-up variation about the material's own albedo and normal.

### Shading

The shared [normal_map_kernel.slangh](../../renderer/src/shaders/shared/normal_map_kernel.slangh)
owns both blends:

- `vkr_detail_normal_blend`: reoriented normal mapping
  (Barré-Brisebois and Hill 2012) of the decoded detail normal onto the
  material's tangent normal, in the same UV0 tangent frame. A flat detail
  returns the material's normal; a material without a normal map takes the
  detail normal.
- `vkr_detail_albedo`: `base × (1 + w(2d − 1))`, the overlay `d` scaling the
  base colour by `2d` at weight `w`; 0.5 changes nothing.

Both detail maps sample at `UV0 × detail_uv_scale` through their textures'
own samplers and mips, filtered like the material's maps, at the frame's LOD
bias. Under the
`base_alpha` mask the base colour's alpha scales the detail normal's strength
and the overlay's weight. Without an albedo map the overlay's weight is zero
and its sample is skipped (`vkr_packet_material_detail`).

### GPU rows

Each material row ends with the detail row (`material_detail`: UV scale,
normal strength, overlay weight, mask) and the two maps:

- Metal: 384 to 432 bytes, two `MTLResourceID`s and two samplers
  (`VkrMetalMaterialGpuRow`, `VkrMetalPacketMaterial` in
  [draw.metalh](../../renderer/src/shaders/metal/msl/common/draw.metalh)).
- Vulkan: 320 to 352 bytes, two bindless texture and sampler indices
  (`VkrVulkanMaterialGpuRow`, `VkrVulkanMaterial` in
  [resources.slangh](../../renderer/src/shaders/vulkan/slang/common/resources.slangh)),
  checked by SPIR-V reflection.

Material flag bit 13 marks a material with detail maps on both backends.
`VKR_WORLD_MATERIAL_FEATURE_DETAIL` (0x10) marks a frame whose opaque or
alpha-tested draws include one.

### Tiled pipeline (Metal): detail kinds

[ADR-129](129-tiled-standard-draw-kinds.md)'s Standard draw kinds gain two:

| Kind | Draws | Function constants |
|---|---|---|
| 2, detail | detail draws that can be lightmapped | 6 `detail`, 3 `no_terrain` |
| 3, unlightmapped detail | detail draws that cannot be lightmapped | 6 `detail`, 2 `no_lightmap`, 3 `no_terrain` |

- Function constant 6 (`vkr_tiled_detail` in
  [tiled.metal](../../renderer/src/shaders/metal/msl/world/tiled.metal))
  compiles `vkr_metal_tiled_detail` into the forward and alpha-tested
  fragments. The general pipelines, the blend pipelines and the Custom
  graphs' pipelines leave it undefined and kinds 0 and 1 set it false, so
  their code is the code before this change.
- A material with detail maps has no layers, so both detail kinds compile
  the terrain layers out. A detail draw goes to a detail kind whether or not
  it can be lightmapped; the classify kernel
  ([gpu_draws.metal](../../renderer/src/shaders/metal/msl/world/gpu_draws.metal),
  `vkr_metal_packet_standard_kind`) reads the material's flag bit 13.
- The kinds take compaction slots 35 and 36 in both BRDF planes; the table
  grows to 9,504 bytes. The frame's upload plan asks for them only while
  the world has `VKR_WORLD_MATERIAL_FEATURE_DETAIL`
  ([vkr_metal_packet_custom.inc](../../renderer/src/metal/internal/vkr_metal_packet_custom.inc),
  `vkr_metal_packet_standard_kind_mask`), and creates each shading variant
  in the background like the other kinds. Until it is READY a detail draw
  takes the general pipelines and draws without its detail maps; pending
  kinds count in `vkr_renderer_get_pipeline_stats`, so harness readiness
  waits for them.
- `VKR_STANDARD_KINDS_DISABLED=1` now disables kinds 0 and 1 only, since
  the detail kinds are the only pipelines that draw detail maps.

### Desktop pipeline (Vulkan): a resolve variant

`VKR_VULKAN_GBUFFER_DETAIL` builds four more G-buffer resolve modules, one
per existing resolve (`packet.gbuffer_resolve.detail_*.comp`). A frame whose
scene has a detail material (`detail_maps_enabled`) dispatches the detail
variant of its resolve
([vkr_vulkan_deferred.c](../../renderer/src/vulkan/vkr_vulkan_deferred.c));
within it only pixels whose material has flag bit 13 sample the maps
([deferred.slang](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
`vkr_vk_resolve_surface`). Other frames run the resolve modules they ran
before. The detail normal samples with the base normal map's footprint
scale. The forward path (blended draws, which take no detail maps, and
planar reflections) does not sample them, so a reflection shows a detail
material without its detail.

### Texture demand

Texture demand ([ADR-024](024-shared-bindless-gpu-cores.md)) measures each
material at UV0. A detail map's stream needs that extent times the detail
UV scale (`vkr_material_system_stream_required_extent`), so its mips stream
for the tiling it is sampled at.

### Material graphs

The Standard surface output gains four inputs, which lower to the keys above
and raise from them ([vkr_material_graph.c](../../runtime/src/assets/vkr_material_graph.c)):

- `detail_normal` (a normal map node: texture and strength);
- `detail_uv_scale` (scalar);
- `detail_albedo` (a texture's RGB, no constant);
- `detail_albedo_strength` (scalar).

The graph settings gain `detail_mask` (`none` or `base_alpha`), which
`material.patch` `settings` sets. A graph with detail inputs stays Standard
and adds no pipeline. Lowering names the output node when a detail input or
mask lacks the detail normal texture, sits beside layers or refraction, or
uses the base-alpha mask on a surface that is not opaque. A Custom graph
refuses detail inputs, as it refuses every input it has no Custom form for.
`VKR_MATERIAL_GRAPH_INPUT_MAX` rises from 30 to 34.

## Consequences

- A scene without detail materials requests no detail kinds and no detail
  resolve; its pipelines and shader code are unchanged. Every material row
  grows by 48 bytes on Metal and 32 on Vulkan, and the Metal compaction
  table by 512 bytes, whether or not a material has detail maps.
- On the tiled pipeline only detail draws pay for detail maps: two samples
  (one without an albedo map), the blend, and the kind's pipelines (56
  states per shading variant, built in the background). Detail draws that
  can be lightmapped keep the lightmap path but not the terrain layers.
- On the desktop pipeline a frame with a detail material resolves every
  pixel with the detail variant, so pixels without detail maps there carry
  its registers. Native Vulkan measurements have not run; see Revisit when.
- Terrain, layered, blended, refracting and Custom materials take no detail
  maps.
- A lightmap bakes without detail maps.

## Alternatives considered

- **A runtime branch in the general pipelines.** Rejected by the owner's
  constraint and by ADR-113's measurement of runtime-gated material code.
- **One detail kind with the lightmap path.** Rejected: unlightmapped detail
  draws would carry the lightmap code, which ADR-129 measured at 0.45 to
  0.7 ms of `Tiled.Opaque` p95 on the Level Design Test.
- **Detail maps on layered and terrain materials.** Not taken: the detail
  kinds would need the terrain layers, and layers share layer 0's UVs.
  Revisit with per-layer UV transforms.
- **Whiteout normal blending.** RNM keeps the detail's relief on the base
  map's slopes for a few more instructions, which only detail draws run.
- **Per-tile classification of the desktop resolve.** Not taken without a
  native Vulkan measurement that shows a cost.

## Revisit when

- A native Vulkan run is available: record a detail capture and time a
  Bistro frame with one detail material against the base resolve. If the
  detail variant costs pixels without detail maps measurably, classify
  resolve tiles by material feature.
- Layered materials need detail maps, or a second detail layer is asked for.
- Lightmaps need detail albedo.

## Evidence

Apple M1 Pro, macOS, Release, validation variables unset unless stated,
2026-10-28. "Base" is 4dd6c8da; "change" is the same tree with this
change's code, built in a separate worktree.

- `./build_release/tests/vulkan_renderer_tester --suite
  run_material_pbr_tests`: `test_material_detail_keys` parses every key into
  its value and slot (the texture keys do not fall into the base normal
  slot whose key their names contain) and refuses 15 files, each for its
  own reason: detail keys without the detail normal map (2), out-of-range or
  non-finite values (4), an unknown mask, a misspelt key, a blended
  (explicit and inferred), refracting, layered or Custom material (5) and
  the base-alpha mask without `alpha_mode=opaque` (2).
- `--suite run_material_graph_tests`: `test_graph_detail` raises a
  definition with every detail key to a graph that writes, reads and lowers
  back to the same material at the Standard tier with no function and three
  samples; a base-alpha mask on an alpha-tested surface, detail maps beside
  a layer blend and detail inputs without the detail normal texture each
  name the output node. `test_graph_round_trip` still round-trips the 1,241
  local materials.
- SPIR-V: the four detail resolve modules and the four existing ones pass
  `spirv-val --target-env vulkan1.4 --scalar-block-layout`;
  `packet.gbuffer_resolve.detail_none.comp.spv` is 130,912 bytes against
  128,108 for `none`.
- Unchanged output without detail maps: `vkr_harness snapshot --case
  tools/cases/local/tiled_bistro_baked_capture.case.json --profile
  tools/profiles/local-offscreen.json` with the base and the change gives
  byte-identical `final_color` (SHA-256 `342c1721…`) and `scene_color`
  (`dd7d0614…`).
- Detail maps shade: the same case with a hexagonal tile normal map
  (`detail_uv_scale=8`, `detail_normal_scale=1.5`) and a grey concrete
  overlay added locally to 18 Bistro wall and paving materials changes the
  image (PSNR 35.7 dB against the plain capture) and creates the detail
  kinds: four background pipeline builds against two without detail
  materials, all successful. A close-up of the paving with the maps on all
  233 eligible Bistro materials shows the hexagonal grid in the difference
  of the shading-normal view and the overlay in the difference of the unlit
  view. The material edits were local and are not committed.
- Metal API validation (`MTL_DEBUG_LAYER=1`, one process) on that close-up:
  0 errors, 0 warnings, with the detail kinds built.
- Timing: `vkr_harness profile --case
  tools/cases/performance/bistro_material_orbit_1440.case.json --profile
  tools/profiles/performance-windowed-gpu-submission.json` (2560×1440
  pixels, windowed hidden, immediate present, five children of 120 warmup
  and 300 measured frames), runs alternating base and change, no detail
  materials in the scene. All runs share the environment, workload and policy
  fingerprints (`2d32eb46…`, `dc00a41c…`, `7ba6b42f…`) and the work rows.
  `gpu.submission` in ms:

  | Run | Status | p50 | Child p50 range | Mean |
  |---|---|---|---|---|
  | Base 1 | incomplete: one invalid `draw.world.calls_issued` sample; child 1 disturbed (p95 208) | 25.66 | 24.73–25.54 without child 1 | 34.37 (28.04 without child 1) |
  | Change 1 | pass, authoritative (`sha256:0f6c6d79…`) | 25.47 | 25.21–25.63 | 28.22 |
  | Base 2 | incomplete: four invalid draw-count samples | 25.39 | 25.30–25.52 | 28.22 |
  | Change 2 | incomplete: one invalid draw-count sample | 25.54 | 25.39–25.81 | 28.39 |
  | Base 3 | pass, not authoritative: `power.low_power` in child 4 | 24.94 | 25.09–25.61 for children 0 to 3 | 28.18 for children 0 to 3 |

  The median child p50 is 25.37 ms for the 13 base children on mains power
  and 25.47 ms for the 10 change children, +0.1 ms (+0.4%); the two change
  runs differ from each other by 0.17 ms in mean. `perf-compare` of base 3
  against change 1 observes no change and is inconclusive for authority.
  The invalid draw-count samples occur on both builds. The Mac then ran on
  battery in Low Power Mode, so no authoritative pair was taken.
- Unavailable: native Vulkan execution, a Vulkan detail capture and Vulkan
  timing, which wait for a Windows host; an authoritative before/after pair
  (above); register counts of the detail kinds (no offline Metal compiler
  report).
