---
status: partial
updated: 2026-10-14
authority: adr
---

# ADR-113: BRDF material extensions

## Status

Accepted (partial). Both pipeline classes and both lightmap bakers implement
the four extensions. The tiled pipeline has native Metal evidence. The desktop
pipeline has compiled SPIR-V only; native Vulkan execution is not recorded.

## Context

The base model ([ADR-053](053-energy-compensated-ggx.md)) is Lambert diffuse
and energy-compensated GGX with Schlick Fresnel. A 2026-10-12 comparison with
the Callisto Protocol BRDF (Jimenez and Petersen, SIGGRAPH 2023 Advances)
found its base weaker than ours: it has no multiple-scattering compensation
and sums diffuse and specular without coupling. Four of its ideas, or a
physically based replacement for them, were worth taking as opt-in material
controls:

- a rough diffuse lobe (Callisto's Proxima did not ship; EON replaces it);
- the smooth shading terminator;
- a dual specular lobe;
- Fresnel control at grazing angles (F82-tint replaces Callisto's Schlick
  exponent, because the DFG table can integrate it).

The user chose identity defaults, a separate diffuse roughness, scalar
parameters without texture maps, and EON plus F82 in the lightmap bakers.

## Decision

### Parameters

The [PBR material](../../renderer/src/vkr_render_resources.h) adds six
values. `.mt` keys, material-graph pins and material instances carry them.
Every key selects PBR. Range checks run at parse and at every publication
boundary (`vkr_pbr_brdf_extensions_valid`).

| Key | Range | Identity | Meaning |
|---|---|---|---|
| `diffuse_roughness` | [0, 1] | 0 | EON roughness; 0 is Lambert |
| `smooth_terminator` | [0, 1] | 0 | Terminator amount |
| `smooth_terminator_length` | [0, 1] | 0.5 | Terminator width in N·L |
| `dual_specular_opacity` | [0, 1] | 0 | Mix weight of the second lobe |
| `dual_specular_roughness_scale` | [0, 16] | 2 | Second lobe roughness = `clamp(r * scale, 0.04, 1)` |
| `specular_f82_tint` | [0, 1] RGB | white | Reflectance at about 82° relative to Schlick |

Identity values reproduce ADR-053 exactly. Terrain materials reset the six
values to identity, because terrain shading reads layer 0's material row for
every layer. The graph pin for the roughness scale is `dual_specular_scale`,
because the full name exceeds the 31-byte node id limit.

### GPU rows

Each material row ends with two `float4` fields:

- `material_brdf`: diffuse roughness, terminator amount, terminator length,
  dual opacity;
- `material_specular_tint`: F82 tint RGB, dual roughness scale.

Vulkan rows grow from 288 to 320 bytes (offsets 288 and 304). Metal rows grow
from 352 to 384 bytes (offsets 352 and 368).
`vkr_packet_derive_material_constants` fills both. The Vulkan deferred
resolve reads the row through the visible row's material index; no G-buffer
channel is added.

### Math

[`ggx_kernel.slangh`](../../renderer/src/shaders/shared/ggx_kernel.slangh)
owns every equation. Backends sample tables and call the shared functions.

**F82 tint** (Hoffman 2023):
`F(μ) = Schlick(μ) − a·μ(1−μ)^6`, where
`a = Schlick(1/7)·(1 − tint) / ((1/7)(6/7)^6)`. Schlick keeps the ADR-053
`F90 = saturate(25·max F0)`. The DFG table gains a third channel
`C = ∫ D·V·μ(1−μ)^6`. Integrated reflectance becomes
`(F0·A + F90·B − a·C)·(1 + F0(1/(A+B) − 1))`. The compensation factor keeps
F0, not the F82-weighted average.

**Dual specular**: an isotropic second GGX lobe, mixed by opacity. Each lobe
has its own table sample and compensation, so the mixture keeps the ADR-053
energy bound. The material energy record keeps `reflectance` as the total.
`primary_reflectance` and `dual_reflectance` split environment specular:
the second lobe fetches the prefiltered environment again, at its roughness
and about its own dominant direction. The specular-AO term uses each lobe's
roughness; the desktop GTAO cone uses the primary lobe's.

**EON** (Portsmouth, Kutz and Hill 2024): the energy-preserving Oren-Nayar
model with the fitted single-scatter albedo `E_FON`. Preparation folds the
view albedo ratio `E(N·V)/ρ` into `diffuse_weight` after the
diffuse-transmission split. Therefore SH, lightmap, volume, ambient, LTC
rectangle and subsurface diffuse take EON as a uniform-light albedo without
per-site changes. Direct lights restore the exact angular shape through
`vkr_eon_direct_albedo`. With ρ = 1, the folded albedo is 1 at every N·V.

**Smooth terminator** (Callisto, slide 90):
`c = lerp(1, smoothstep(0, mask·length, N·L), mask·amount)`, where
`mask = (1 − (1 − L·H)^3)(1 − (1 − N·H)^3)`. The slide does not state the
angle convention; the cosine reading is the only one that keeps the grazing
and retroreflective configurations unchanged, as its notes require. The
amount range is [0, 1] instead of Callisto's [−1, 1], so the term never adds
energy. It scales the diffuse and specular of the sun, punctual lights and
the directed share of baked lamps. Rectangle lights and environment light do
not take it.

### Shading variants

With every value at identity, runtime-gated extension state cost the tiled
forward pass 15% of `gpu.submission` on Bistro (see Evidence): it kept about
twenty values live through the light loops and lowered occupancy. The
extension code therefore runs only where an extended material shades:

- `vkr_pbr_brdf_extensions_active` sets
  `VKR_WORLD_MATERIAL_FEATURE_BRDF_EXTENSION` in the world's material
  feature mask. The bit counts opaque, blended and transmissive sources;
  the other bits count opaque sources only.
- **Tiled (Metal), per draw.** The bool function constant
  `vkr_tiled_brdf_extension_constant` (index 0) gates the extension block
  in `vkr_metal_tiled_shade`. The forward, alpha-tested and blend pipelines
  gain a BRDF dimension. The first publication of an extended material
  starts creating the extended set as a background job, like a Custom graph
  ([ADR-096](096-custom-material-graphs.md)).
  `vkr_renderer_get_pipeline_stats` counts the pending set as one graph, so
  harness and runtime readiness wait for it; until it is READY, and after a
  failure, extended materials shade with the base model. A frame whose
  feature bit is set while the set is READY (`tiled_brdf_draws`) buckets
  draws by material:
  - The Custom compaction (ADR-096) has two planes by BRDF variant. The
    camera classify kernel sends a draw whose material row is extended to
    plane 1, marked with classification bit 5
    (`VKR_METAL_BRDF_CLASS_BIT`): a Standard draw to slot 0 while
    `brdf_ready` is set, a Custom draw to its graph's slot while that slot
    is in `brdf_ready_mask`. Prefix and encode place both planes like the
    graph slots.
  - The tiled opaque pass draws the Standard buckets with the base
    pipelines, plane 1's slot 0 with the Standard extension pipelines and a
    Custom slot with its graph's pipelines of the plane's variant, each of
    the frame's lighting, probe and decal variant. Picking and other camera
    passes draw every filled range with their own pipeline.
  - The blend pass draws each blended draw whose material is extended,
    Custom fallbacks included, with the extension pipeline, and the others
    with the base one.
- **Custom graphs (Metal).** The six BRDF inputs of a Custom graph's
  surface output take Scalar or Colour constants, which an instance can
  override; a varying value is refused, because the values are one per
  material. They stay out of the generated function, so a graph with and
  without them shares its function, slot and base pipelines, and lowering
  writes them as `.mt` keys. Each graph has a second pipeline set with the
  extension constant, created in the background when a published material
  of the graph first uses an extension; `pending_graphs` counts it.
- **Desktop (Vulkan).** `brdf_extensions_enabled` joins
  `lighting_layers_enabled`. Bit 1 of the lighting root's `layered_tiles`
  makes tile classification also give the layered kernel pixels whose
  material row is extended; only that kernel applies the extensions.
  Forward, transmission, subsurface and local-shadow transmission shading
  keep runtime branches.

### Consumers

| Path | Specular extensions | EON and terminator |
|---|---|---|
| Tiled forward (`tiled.metal`, `lighting.metalh`) | yes | yes |
| Desktop forward, deferred and transmission (`default.slang`, `deferred.slang`) | yes | yes |
| Desktop subsurface receiver | yes | EON |
| Local-shadow transmission | yes | no diffuse |
| CPU baker BSDF (`vkr_bake_bsdf.cpp`) | F82 | EON |
| GPU baker bounce albedo (`vkr_bake_lightmap.slang`, `vkr_bake_metal.mm`) | F82 | EON |

The bakers do not model the terminator or the dual lobe: bounce light holds
little specular, and the terminator is an artistic per-light control. GPU
baker material records grow from 96 to 112 bytes.

### Approximations

- Rectangle lights apply F82 to the LTC amplitude, with the LTC norm standing
  in for A + B. The dual lobe takes its own LTC sample and integrates in the
  view-aligned frame.
- An anisotropic primary lobe reads C from the isotropic table at its
  environment roughness. The dual lobe stays isotropic.
- EON under uniform-light paths ignores the angular distribution of the
  incoming light.

## Consequences

- The DFG table is RGBA16F, 512 KiB, in both renderers and both GPU bakers
  ([ADR-053](053-energy-compensated-ggx.md)).
- Only extended materials' draws pay the extended variant on the tiled
  pipeline, Custom graphs included, and only desktop deferred tiles that
  hold an extended pixel pay it there. A scene without an extended material
  shades as before.
- A Custom graph used by an extended material holds 40 more pipeline
  states, and its first extended material waits for their creation (9.5 s
  cold in the check below). The Custom compaction table doubles to two
  planes (8,480 bytes).
  Each enabled extension adds its own table samples: one for F82, one for
  the dual lobe, plus a second environment fetch and LTC sample when the
  dual lobe is active.
- The tiled pipeline holds up to twice its forward, alpha-tested and blend
  pipeline states. The extended set exists only after an extended material
  is published. While it is READY and a scene uses it, the camera's Custom
  compaction table exists every frame, even without Custom graphs.
- `.mt` keys that start with `smooth_terminator`, `dual_specular`,
  `specular_f82` or `diffuse_roughness` but are misspelt reject the material.

## Evidence

- An independent quadrature and a GGX importance-sampled integral agree with
  the cooked C channel within 5e-5 across the roughness and view grid
  (`.scratch` scripts, not tracked).
- Numeric integration of the EON formulas gives a white-albedo furnace of
  1.000 ± 0.002 (Monte Carlo noise). The fitted `E_FON` is within 3.1e-4 of
  the exact albedo.
- `vulkan_renderer_tester` passes, 808 tests, Release, macOS. New tests:
  `test_material_brdf_keys`, `test_bake_material_reads_brdf_extensions`, the
  packing in `test_packet_material_constants` and the graph round trip.
- Metal, M1 Pro, Release, `local.tiled.bistro_baked_capture` with
  `local-offscreen.json`: at identity values, `final_color`, `scene_color`
  and `depth` are byte-identical to an `origin/main` (d5eef0c3) build. Two
  reference runs differ from each other by at most 7/255 on 0.7% of pixels.
  With each extension enabled on every Bistro material, the image changes
  (mean 0.3 to 1.5/255) with no non-finite or black pixels.
- Metal timing, M1 Pro, Release, `performance.bistro.material_orbit_1440`
  (2560×1440 pixels) with `performance-windowed-gpu-submission.json`: five
  children each, authoritative, matching environment, workload and policy
  fingerprints and draw counts. `gpu.submission` p50 / p95 / mean:

  | Build | p50 | p95 | mean |
  |---|---|---|---|
  | Base d5eef0c3 (two runs) | 28.65 / 28.67 ms | 45.48 / 45.40 ms | 30.89 / 30.91 ms |
  | Extensions, identity values (two runs) | 28.62 / 28.60 ms | 45.69 / 45.62 ms | 30.91 / 30.89 ms |
  | Runtime-gated first version, identity values (two runs) | 32.96 / 33.01 ms | 53.2 / 53.6 ms | 35.5 / 35.7 ms |
  | Extensions, all four on every Bistro material | 38.22 ms | 62.93 ms | 41.41 ms |

  The runtime-gated row approximates the extended variant's cost when its
  terms are inactive. The runs predate b0a36418, so each tree reverted the
  KTX patches that the build applies to `vendor/ktx-software` before
  profiling.
- Per-material split, Metal, M1 Pro, Release, same case and profile, on
  44750566's scene (upstream 9459923a removed its diffuse volume, which
  lowers every time below). "Mixed" enables all four terms on every tenth
  Bistro material (26 of 254), which changes 34% of the pixels in the
  tiled snapshot. `gpu.submission` p50, alternating runs:

  | Scene | Per-frame variant (44750566) | Per-material split |
  |---|---|---|
  | Identity values | 22.88 ms | 22.74 ms |
  | Mixed (two runs each) | 26.94 / 27.02 ms | 23.42 / 23.54 ms |

  The workload fingerprint does not cover `.mt` content, so mixed and
  identity runs share it; each run's material edit is recorded here. Split
  snapshots match the per-frame build within 1/255: identity byte-identical,
  each term on every material and the mixed scene.
- Custom graphs, Metal, M1 Pro, Release, headless Bistro editor
  (`.scratch/brdf-ext/custom/check.py`): one Custom body with and without
  constant BRDF inputs lowered to one function in slot 1; its base and
  extension pipelines became READY and readiness waited for both. A floor
  and a wall bound in turn to the two materials: the extended wall's mean
  blue fell from 99–102 to 91–92 whenever it was extended, and the floor
  took 7.4 extended and 14.7–17.7 base, also when the wall was extended in
  the same frame. Editor captures vary between frames, so the check uses
  region means, not pixel equality. The tiled Bistro snapshot without
  Custom graphs stays byte-identical. `test_graph_custom` checks lowering,
  an instance override and the refusal of a varying input. With the
  two-plane compaction, the mixed Bistro timing gave 23.10 ms p50
  (authoritative, same fingerprints) against 23.42 / 23.54 ms for the
  single-plane split earlier that day; the runs were not interleaved.
- Vulkan: the SPIR-V compiles and the host row asserts pass. The runtime
  reflection checks run at pipeline creation on a Vulkan device; they, native
  execution, a desktop furnace and Vulkan timing are not recorded.

## Alternatives considered

- **Replace the base BRDF with Callisto's.** Rejected: its base lacks energy
  compensation and diffuse/specular coupling, its gains go up to 256× and
  break the furnace bound, and its per-light lobes have no environment
  counterpart.
- **Proxima rough diffuse.** Rejected for EON: Proxima did not ship and has
  no closed-form albedo for the environment split.
- **Callisto's Schlick exponent.** Rejected for F82: a per-material exponent
  needs another table dimension, while F82 adds one table channel.
- **Callisto's retroreflection and diffuse-Fresnel lobes.** Not taken: they
  pay off only for photo-matched characters, and sheen covers grazing cloth.
- **Texture maps for the new values.** Deferred by the user's choice of
  scalars; textures need G-buffer storage on the desktop pipeline.

## Revisit when

- Native Vulkan runs are available: record a desktop furnace with the dual
  lobe and F82, and a Bistro snapshot at identity values.
- Skin or characters need the retroreflection or diffuse-Fresnel lobes, or
  texture-driven values.
- glTF `KHR_materials_diffuse_roughness` is ratified: map it to
  `diffuse_roughness`.
