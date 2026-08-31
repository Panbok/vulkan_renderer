---
status: investigation
updated: 2026-08-31
authority: investigation
---

# Bistro indoor light-leak investigation

## Outcome

The punctual-light AABB correction from 2026-08-30 was invalid and is removed.
It did not model visibility, left the indoor indirect leak in place, and made
its hard world-space planes visible as colored rectangular slabs outside the
café.

The replacement is a cold scene calibration:

- the six colored Bistro string-light definitions import with a 5 m range
  instead of their source-authored 7.5 m range;
- the one coherent indoor probe remains in place, with diffuse intensity
  reduced from 3.5 to 1.0;
- the original `bistro-lights.gltf` is unchanged;
- no mesh is recooked and no build script invokes mesh cooking; and
- the renderer returns to the four-vector, 64-byte punctual-light row with no
  AABB test in the grid or production shaders.

In the isolated lighting captures, this removes the hard exterior boundary and
reduces the diagnosed indoor direct and indirect contributions. Owner review of
the normal interactive path remains the final visual acceptance gate. This is a
scene correction, not a general geometry-visibility implementation: point and
spot lights still do not cast shadows from walls or furniture, and the local
diffuse probe still has no occlusion data.

The architecture status is authoritative in
[renderer-architecture-spec.md](../architecture/renderer-architecture-spec.md),
and the stable spatial-lighting decision is recorded in
[ADR-019](../architecture/adr/019-bounded-forward-spatial-lighting.md).

## 1. Diagnosis

### 1.1 Why the AABB correction broke the exterior

The failed implementation assigned 54 imported lights exact world-space
influence boxes. Grid construction intersected range spheres with those boxes,
and every Metal and Vulkan punctual loop rejected fragments outside them.

The owner exterior camera exposed the flaw directly. Its isolated
`direct_diffuse` and `direct_specular` captures contain the same hard vertical
and horizontal color transitions as the final image. `unlit` does not contain
them. Those transitions coincide with the authored box planes, so they are not
mesh, tonemap, exposure, TAA, bloom, GTAO, or CSM artifacts. They are the
containment algorithm working as written.

An influence box answers whether a point lies inside a box. It does not answer
whether opaque geometry blocks the segment from a receiver to a light. Hiding
the planes behind one camera's walls only moved the artifact to another camera.

### 1.2 Why the original indoor leak remained

The indoor view contains two independent contributions:

1. The six colored string-light definitions have a 7.5 m source range. Their
   instances are close enough to contribute saturated direct light through the
   café shell because punctual lights have no opaque visibility test.
2. The indoor diffuse probe covered the café at intensity 3.5. Probe AABBs are
   blend supports, not room topology, so this produced a broad warm
   `indirect_diffuse` field on every receiver in the volume.

Removing the failed boxes restored smooth exterior pools but did not change
either source. A diagnostic source-only edit from 7.5 m to 5 m removed the
remaining colored direct patch at the second owner camera while preserving
smooth exterior fixture pools. Reducing the probe from 3.5 to 1.0 reduced the
broad indirect wash without exposing a probe boundary.

The two adjustments are deliberately separate: imported punctual range affects
`direct_diffuse` and `direct_specular`; probe intensity affects only the
environment-diffuse contribution.

## 2. Implemented correction

### 2.1 Cold glTF light-range override

A cooked mesh entity with an explicit `gltf_light_source` may override the
range of an imported point- or spot-light definition:

```json
{
  "mesh": {
    "path": "assets/models/example.vkb",
    "gltf_light_source": "assets/models/example.gltf",
    "gltf_light_range_overrides": [
      {
        "light": "ExactGltfLightDefinitionName",
        "range": 5.0
      }
    ]
  }
}
```

The name resolves against `KHR_lights_punctual` light definitions, not node
names. One override applies to every node that instances that definition. Scene
preparation fails when an entry is malformed, duplicated, unknown,
directional, non-finite, non-positive, or resolves to no light node. Overrides
also require an explicit `.gltf` or `.glb` source. Validation and normalization
happen during scene preparation; frame and shader paths receive ordinary
finite ranges.

Bistro applies 5 m to its Yellow, Pink, Red, Green, Orange, and Blue string
definitions. Other definitions retain their source values. The source glTF
digest after the correction is
`sha256:2263b26382cb7955b19fb9f7218c15c49eabaa1d012b8c607a2c5a24f5c9df5b`;
its six colored ranges remain 7.5 m on disk.

### 2.2 Probe calibration

The café retains one continuous local probe so no blend edge crosses a visible
floor, wall, or ceiling:

| Center | Extents | Blend | Diffuse / specular intensity |
| --- | --- | ---: | --- |
| `[-7.5, 2.2, 9.0]` | `[5.5, 2.5, 8.0]` | `0.75` | `1.0 / 0.0` |

The previously rejected three-box subdivision remains rejected. Its visible
axis-aligned transitions demonstrated that probe tiling cannot substitute for
diffuse visibility.

### 2.3 Restored renderer contract

The failed containment feature is removed from scene components, the light
grid, packet lowering, Metal and Vulkan resource declarations, forward and
deferred lighting, transmission lighting, and tests. `VkrGpuPointLightRow`
remains the one shared host row and `point_light.slangh` remains the shared
Slang declaration. The row is again four `Vec4` values (64 bytes):
position/cone, color/cone, intensity/attenuation/range/kind, and direction. One
packer serves both uploaders, and host assertions plus Metal/Vulkan compiled
reflection validate its fields and size.

`VKR_RENDER_PACKET_VERSION` advances from 24 to 25 because binaries built
against the rejected 96-byte row must not accept the restored 64-byte layout.
No optional check, string lookup, allocation, or new branch enters a frame or
fragment hot path.

## 3. Evidence

The two latest owner cameras are retained as local five-channel cases:

| View | Position | Yaw / pitch |
| --- | --- | --- |
| Exterior regression | `[-26.88, 5.04, 8.86]` | `1.50 / -9.23` |
| Interior leak | `[-8.86, 2.72, 9.84]` | `7.99 / -10.32` |

The earlier second indoor camera at
`[-7.37701988, 2.03192472, 8.66078377]`, yaw `614.673218`, pitch
`8.15400887`, remains the direct-light calibration witness.

| Run | Report digest | Observation |
| --- | --- | --- |
| Rejected AABB exterior | `sha256:53651fabae88c3ea2ed829e4d2db56da1ffb0b6cf950eb0b052b77b2b31ee333` | Hard colored slabs are present in direct lighting |
| Rejected AABB interior | `sha256:3bae0aa842a71a988d4fd13c2d659dec36413f35c097c3da22e26c6d2cee4c30` | Direct light is mostly suppressed, but the broad indirect wash remains |
| Boxes removed, probe at 1.0 | `sha256:17ece33f879783fc7043cf3790f61c2202883d4a84ec0f901bb1dd5c911cf5ce` / `sha256:c8605840743cd32abbd9e6074d065d9afaf56d190ef44220dff2d1dfa1b05b35` | Exterior transitions disappear and indoor indirect energy drops |
| Final exterior / interior | `sha256:e5b664284c6fe7e297ec049a7725f4dad8100cb4b791afd2c32d853e1392b8e2` / `sha256:290debd829f0c00e96dde4294d39de1ee482f759a9fda0bfafdf54bcb36ecd0b` | Five isolated channels retain smooth exterior pools and the calibrated indoor field |
| Final earlier indoor camera | `sha256:1983907f352cd128a7e00edd061c36cb9dab730a19348ec76339f257ae1c8ceb` | Range override removes the colored direct patch |

At the earlier indoor camera, the final scene override produces byte-identical
`unlit`, `direct_diffuse`, and `direct_specular` captures to the temporary
source-edited 5 m diagnostic (`sha256:8bb544d139c2bbb85c96dc1e3026b4a3b2b64ece53124814da36e49eff47a8b3`):

- `unlit`: `sha256:b7656bf399d5905f4e830aa9e1ff7776c453e5de3cb66b97683bf82e2f2e3ef4`;
- `direct_diffuse`: `sha256:0f6ea3ff990ecf91841f3fcb519cac3f37bdcca9a569e851b3dea818dab5a8ae`;
- `direct_specular`: `sha256:6981222a774da382933be6ba262826c79d703b6669b533866e8e67520b932858`.

Separate Release profile replays pass all assertions. Both select 72 lights,
drop zero, build 363 cells with 1,247 references and a maximum of 43 lights per
cell, pack one probe, and report zero invalid G-buffer resolves. Their report
digests are
`sha256:41f1b5ad6fd218878ed05a7b64e78e4be81d0b0d2c4f5b0d77bf0b1278c85a57`
and
`sha256:c99421e36adbbe0bfcf76696e19e4ce7bb27436c929b0dc34b3d5cbcf325ee39`.

These are local dirty-tree correctness observations, not accepted baselines or
performance evidence. The current harness final-color path also produces a
known dark/green output that predates this rollback, so final-color captures
from that path are not used as an interactive visual witness. The separated
lighting channels are the attribution evidence; owner review of the normal
interactive path remains the visual acceptance gate.

## 4. Build and validation

The corrected source passes:

```sh
./build_test.sh
./build_release.sh
./build.sh Debug

MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  build_debug/tools/vkr_harness profile \
  --case tools/cases/local/bistro_light_leak_metal_validation.case.json \
  --profile tools/profiles/local-metal-dual-validation-serial.json
```

The focused Debug replay passes under Metal API and GPU validation with all
four assertions satisfied and no renderer, sanitizer, or validation error.
Report digest:
`sha256:c4b43bbd921a4eaf419a92dd3e52ea99fd3ede5e2fd0cbd0638f6a6bc70a662e`.

Release and Debug builds compile both production shader libraries. Native
Vulkan runtime validation is unavailable on this macOS host, so the shader
parity ledger remains unaligned until a native Vulkan run and crossed pixels
are recorded. No speed claim is made.

The root build wrappers ran only their normal texture-preparation pass. They
did not cook meshes. Manual mesh cooking remains a separate explicit command:

```sh
tools/cook_vkr_meshes.sh
```

## 5. Remaining visibility limit

Range is the right cold control for decorative lights whose authored reach is
too large. It cannot make arbitrary opaque blockers visible to a light. A
general punctual solution still requires a separately budgeted point/spot
shadow or another visibility representation with completion-safe retained
resources. Geometry-aware indirect containment likewise needs occlusion-bearing
GI data such as baked lightmaps or visibility-aware probes.

Until those systems exist, scene ranges and probe energy must stay conservative
and must be verified from both sides of every partition. Hard influence AABBs
must not be reintroduced as a shadow substitute.
