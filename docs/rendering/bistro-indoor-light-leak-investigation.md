---
status: investigation
updated: 2026-08-31
authority: investigation
---

# Bistro indoor light-leak investigation

## Outcome

The reported Bistro leak is corrected without exposing authored volume planes:

- one coherent diffuse probe covers the café; a rejected intermediate split
  into three tight boxes was reverted because its transitions crossed visible
  floors and ceilings;
- 54 imported exterior point lights have exact-node, world-space influence
  AABBs authored in `bistro.scene.json`;
- the CPU light grid clips conservative membership to those AABBs;
- the 96-byte point-light GPU row carries the exact bounds; and
- every Metal and Vulkan forward, deferred, and transmission punctual loop
  rejects fragments outside the bound before distance, cone, square root, or
  BRDF work.

All four owner cameras pass the final five-channel Release snapshot cases. The
large magenta/blue punctual patches no longer cross the authored partitions,
and the fourth camera no longer shows rectangular diffuse-probe transitions.
All 72 lights remain selected, none are dropped, one probe is packed, and
G-buffer resolve reports no invalid values.

The punctual bounds provide authored room containment, not arbitrary
local-light shadowing. Furniture and walls inside one influence volume still do
not cast punctual shadows. A budgeted retained point/spot shadow tier remains
separate follow-up work.

The architecture status is authoritative in
[renderer-architecture-spec.md](../architecture/renderer-architecture-spec.md),
and the rationale is recorded in
[ADR-019](../architecture/adr/019-bounded-forward-spatial-lighting.md).

## 1. Root cause

The artifact had two independent sources.

### 1.1 Diffuse probes do not encode wall visibility

The indoor local probe has center `[-7.5, 2.2, 9.0]`, extents
`[5.5, 2.5, 8.0]`, and diffuse intensity `3.5`. Its resulting box,
`[-13,-2] x [-0.3,4.7] x [1,17]`, contains the café interior. Probe selection
works as authored, but the representation has no room topology or wall
visibility. The isolated `indirect_diffuse` captures therefore carry the broad
warm ambient field.

The first Stage A authoring attempt divided this field into three touching
AABBs with `0.2`-metre blends. The fourth owner camera exposed the flaw: those
boxes ended on visible floor and ceiling surfaces, so their transitions to the
global environment appeared as large world-axis-aligned rectangles. A
five-channel replay placed the rectangles entirely in `indirect_diffuse`;
`unlit`, `direct_diffuse`, and `direct_specular` did not contain them. Tight
probe boxes are therefore rejected as a substitute for visibility.

### 1.2 Punctual membership did not encode opaque separation

The stable 128-light table and 384-cell grid retained every valid Bistro light
and performed exact range and spot-cone rejection. Neither structure contained
triangles, depth, portals, room IDs, or another visibility representation.
Point and spot lights inside range therefore illuminated receivers through
opaque partitions. Directional CSM could not affect them because only the sun
branch samples the cascade map.

The isolated `direct_specular` captures exposed the saturated exterior string
lights. For the second camera, the final residual's brightest receiver was
reconstructed from captured depth at world position
`[-8.33, 2.46, 3.80]`. Its dominant source was the exterior pink node
`LMBR_000019a_Paris_StringLights_01_Pink_Color_41` at
`[-12.17, 4.16, 4.52]`. The first authored plane extended through the wall to
`x = -7.25`; moving that five-light cluster's indoor-facing plane to the
façade boundary at `x = -10.5` removed the response.

Exposure, bloom, TAA, GTAO, material lowering, normal reconstruction, and CSM
bias were not first causes. The diagnostic cases disable TAA, bloom, and GTAO,
fix exposure, and capture the lighting contributions separately.

## 2. Implemented contract

### 2.1 Scene schema and import validation

A mesh with `gltf_light_source` may override imported point or spot lights by
their exact glTF node names:

```json
{
  "mesh": {
    "path": "assets/models/example.vkb",
    "gltf_light_source": "assets/models/example.gltf",
    "gltf_light_overrides": [
      {
        "node": "ExactLightNode",
        "influence_bounds": {
          "min": [-10.5, -64.0, -64.0],
          "max": [64.0, 64.0, 5.5]
        }
      }
    ]
  }
}
```

The AABB is already in world space and is not transformed by the scene entity.
Resolution uses the actual glTF node name, not the referenced light object's
display name. Scene preparation fails for:

- missing or malformed fields;
- duplicate override entries;
- ambiguous duplicate node names;
- unknown nodes;
- directional-light targets;
- non-finite values; and
- any axis where `min > max`.

An imported light without an override remains compatible with existing scenes.
It lowers to finite canonical bounds
`[-VKR_FLOAT_MAX, +VKR_FLOAT_MAX]` rather than carrying an optional flag into
the renderer or shader.

### 2.2 CPU grid and packet lowering

`ScenePointLight` retains the optional authoring state at the load boundary;
`VkrPointLight` always holds canonical minimum and maximum bounds. Grid
construction uses these cases:

- a finite ranged light marks the range sphere intersected with its AABB;
- a bounded range-less light marks the AABB;
- a bounded polynomial light marks the AABB; and
- an unbounded range-less or polynomial legacy light uses the global mask.

Empty intersections mark no cells. Conservative cell coverage retains a small
epsilon so CPU broad-phase clipping cannot reject a fragment accepted by the
exact shader test.

`VkrGpuPointLightRow` is one shared, 16-byte-aligned 96-byte record:

| Vector | Meaning |
| --- | --- |
| `p0..p3` | Existing position, cone, color, attenuation, range, kind, and direction fields |
| `p4.xyz` | Inclusive world-space influence minimum |
| `p5.xyz` | Inclusive world-space influence maximum |

One shared packer writes the row for both backends. Packet version 24 carries
the enlarged row. Host assertions and backend reflection validate all six
vectors and the total size before use.

### 2.3 Shader behavior

`shaders/shared/point_light.slangh` owns the common row and exact containment
predicate. Metal's native MSL record mirrors it because that library cannot
include the Slang declaration directly. All production punctual loops use the
same ordering:

1. test inclusive influence bounds;
2. reject exact range;
3. evaluate attenuation;
4. reject the spot cone when applicable; and
5. evaluate the requested diffuse/specular BRDF lobes.

The check is present in Metal and Vulkan forward shading, deferred lighting,
and transmission shading. Bounds therefore have the same meaning for opaque
and transmissive receivers.

### 2.4 Bistro authoring

The scene retains one coherent indoor diffuse probe:

| Center | Extents | Blend | Diffuse / specular intensity |
| --- | --- | ---: | --- |
| `[-7.5, 2.2, 9.0]` | `[5.5, 2.5, 8.0]` | `0.75` | `3.5 / 0.0` |

Its support covers the visible café receivers so no probe boundary cuts the
floor or ceiling. It supplies an authored ambient field, not geometry-aware
indirect visibility.

Fifty-four unique exterior-light nodes carry punctual influence bounds. Their
planes sit inside or on façade/partition boundaries, so the exact cutoff is
hidden by opaque construction. The remaining 18 lights preserve their existing
unbounded range behavior.

## 3. Reproduction and visual evidence

The permanent cases pin the four owner cameras at 1600x1200 and capture
`final_color`, `unlit`, `direct_diffuse`, `direct_specular`, and
`indirect_diffuse`:

| Case | Position | Yaw / pitch |
| --- | --- | --- |
| `bistro_light_leak_camera_1.case.json` | `[-5.19214678, 2.89391732, 11.0450354]` | `364.375854 / -12.6190548` |
| `bistro_light_leak_camera_2.case.json` | `[-7.37701988, 2.03192472, 8.66078377]` | `614.673218 / 8.15400887` |
| `bistro_light_leak_camera_3.case.json` | `[-7.37701988, 2.03192472, 8.66078377]` | `508.154724 / 11.4639463` |
| `bistro_light_leak_camera_4.case.json` | `[-12.2170467, 2.40752053, 11.3275013]` | `-14.3014297 / -8.57547569` |

The cases use manual exposure `0.3`, disable TAA, bloom, and GTAO, request
three target images, and use isolated-warm caches. The deliberately dark
`final_color` output is not an attempt to reproduce interactive automatic
exposure. Source attribution comes from the separated channels.

Before implementation, the three five-channel reports were:

| Camera | Report digest | Observation |
| --- | --- | --- |
| 1 | `sha256:056cbdcee8b61471bf51ce2d9075a1f100cf2ac3fc961a2339fffa127db589d1` | Broad warm response in `indirect_diffuse` |
| 2 | `sha256:7220f354f94a663bb147349e0e0c5d5ce0651f66ab3cb516dac43f7177d0de08` | Broad indirect response plus localized purple/magenta direct specular |
| 3 | `sha256:27ee247db8f399f94846827c9fb80834b4863f9391b8fdf54685fe809f079d86` | Strong magenta/blue direct-specular ceiling and wall patches plus broad indirect response |

The rejected three-probe camera-4 report has digest
`sha256:53e33d89e5f9093b609858698fedc50947d4423689bf31fc4fb38546d32f365b`.
Its `indirect_diffuse` image contains the same rectangular floor and ceiling
bands as the owner screenshot, while its other isolated channels do not.

Corrected implementation reports:

| Camera | Case digest | Report digest | Result |
| --- | --- | --- | --- |
| 1 | `sha256:f59335a25dda95ad903d0ac6bae1cc904d3316f9ae7ff027ac1ceecaf41cb047` | `sha256:e14218d71ee8647653c0c60c62b3121cd535f661aeb50df56a7da576efd9f7f6` | pass, 5/5 captures |
| 2 | `sha256:952712cd6044235a52d1876a9f7d1a8076859d14eef022a38bc674af7e19e6bd` | `sha256:e9f2b616f88dccad2e25963008398c3c20869797c316f0985380adbd0565c653` | pass, 5/5 captures |
| 3 | `sha256:9d6e99e723858c17d41131f4dd569626261deed98c069df644aeed8af6e76b83` | `sha256:1d2e225c925a0e24c42e33eda1bfe417eb4843666a4e3444eb686e34171b1a9c` | pass, 5/5 captures |
| 4 | `sha256:262ca531c05832e10220c5107c58438c02daa071a3a1cdb01c689593a2eb3dd8` | `sha256:e329cfb5c785364f6af9dbf85d0ea8f31032ce43dbf32652ea02f64cfd557b03` | pass, 5/5 captures |

Every final replay reports:

- `lighting.point.selected = 72`;
- `lighting.point.dropped = 0`;
- `lighting.ibl.probes_packed = 1`;
- `visibility.gbuffer.resolve_invalid = 0`;
- `lighting.point.grid.cells = 363`;
- `lighting.point.grid.references = 1,458`; and
- `lighting.point.grid.max_lights_per_cell = 33`.

Full-resolution review shows no rectangular probe transitions and no large
colored punctual lobes crossing the authored partitions. The coherent warm
indirect field, visible openings, and small fixture/edge highlights remain.
During the final camera-2 authoring iteration,
the magenta diagnostic fell from 21,077 matching pixels with mean RGB
`[0.442, 0.090, 0.359]` to zero matching pixels with mean RGB
`[0.039, 0.050, 0.022]` after correcting the five-light west-cluster plane.

These reports are local, dirty-tree correctness evidence. They do not publish
or replace a baseline, and their warmups are not performance evidence. A
separate post-effect-enabled offscreen `final_color` replay was rejected as a
visual witness because its color output did not match the interactive path;
the contribution-channel evidence above is the acceptance authority.

## 4. Build, test, validation, and performance evidence

The final source passes:

```sh
./build_release.sh
./build_test.sh
./build.sh Debug
```

Release and Debug compilation cover the application, Metal and Vulkan shader
libraries, and shader reflection. The root build wrappers did not invoke mesh
cooking; they consumed the existing `.vkb` assets. Texture preparation
discovers 2,095 outputs, skips all 2,095 content-identical files, and reports
zero failures. Manual mesh preparation remains available through
`tools/cook_vkr_meshes.sh` and is intentionally outside the build wrappers.
The CPU suite runs with Debug ASan/UBSan and covers valid and invalid override
loading, grid clipping, canonical packing, and GPU ABI offsets. The final
standalone Debug rebuild also passes before the validation replay below.

One isolated-cold Debug profile ran with both `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1`. The log confirms Metal API and GPU validation,
selects the Metal 4 packet renderer, passes all four case assertions, and emits
no validation, ASan, or UBSan error. Report digest:
`sha256:a7d062b9e1dc29696feed236f909398d6d40cc0c68187ea0ac5d81519d009486`.
This is correctness evidence only.

The local Release orbit observations were not authoritative: warmup was
unstable, repetition work volume differed, provenance was dirty, and GPU timing
was disabled. They are retained only as structural work-volume evidence. The
authored bounds reduced grid references from 1,635 to 1,458 and the maximum
lights in one cell from 47 to 33. No timing comparison or speed claim is made.
The final report digests were
`sha256:4af635381c81b60a07766511569548cddf5f9298ce4090bc2e9139d79dfaebca`
and
`sha256:cc0c265518f380ae88476261006403feb02b52e164030693db47dce8c89acbc7`.

The production Vulkan shaders compile and their SPIR-V reflection validates
the row. Native Vulkan runtime validation is not available on this macOS host
because the selected Vulkan renderer requires the descriptor-buffer capability
profile. The cross-backend ledger therefore remains unaligned until a native
Vulkan validation run and crossed exact-camera pixels are recorded.

## 5. Remaining limitation

An influence AABB answers whether a receiver belongs to a punctual light's
authored zone. A local-probe AABB answers how an ambient field blends into the
scene. Neither answers whether geometry blocks a light path. Probe volumes must
therefore not be tiled with transitions across visible surfaces to approximate
room topology. Visibility-aware diffuse GI (for example baked lightmaps or a
probe system with occlusion data) is the long-term solution for indirect wall
occlusion.

Stage B remains appropriate for punctual lights that need arbitrary same-room
occlusion:

- opt-in `casts_shadow` metadata and stable authored priority;
- a hard point/spot light and face budget;
- six retained faces per selected point light and one projection per spot;
- completion-safe reuse and invalidation for light, caster, scene, and resource
  generations;
- one shared visibility meaning in forward, deferred, and transmission; and
- separate update, reuse, raster, overflow, and sample metrics.

Directional bias tuning, GTAO multiplication, or screen-space contact shadows
cannot replace that contract. No Stage B implementation or budget is claimed
by this change.

## 6. Reproduce

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION \
  build_release/tools/vkr_harness snapshot \
  --case tools/cases/local/bistro_light_leak_camera_1.case.json \
  --profile tools/profiles/local-offscreen.json

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build_debug/tools/vkr_harness profile \
  --case tools/cases/local/bistro_light_leak_metal_validation.case.json \
  --profile tools/profiles/local-metal-dual-validation-serial.json

env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION \
  build_release/tools/vkr_harness profile \
  --case tools/cases/performance/bistro_shadow_orbit.case.json \
  --profile tools/profiles/local-windowed.json
```

Run cameras 2, 3, and 4 by changing the first case suffix. Run validation cases
serially; never overlap Metal shader-validation renderer processes.
