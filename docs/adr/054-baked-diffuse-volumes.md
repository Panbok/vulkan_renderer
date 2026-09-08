---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-054: Baked diffuse volumes

## Status

Accepted.

## Context

Global environment SH and local reflection probes describe diffuse irradiance but
cannot prove room membership or prevent interpolation through static walls.
The renderer needs static, scene-local, multi-bounce diffuse transport without
adding a per-frame ray-tracing workload. The runtime already evaluates packed L2
SH as scene-linear `E/π`.

The bake must import the same opaque, masked, blended, transmissive, and
specular material semantics as the scene. A non-transmissive `BLEND` surface is
not a room boundary. Opaque surfaces, `MASK` surfaces without sampled-opacity
proof, and refractive or transmissive material block room membership. Thick
transmissive glass participates in the offline physical path phase; it is not a
runtime approximation for the volume lookup. The material conventions follow
[KHR_materials_specular](https://registry.khronos.org/glTF/extensions/2.0/Khronos/KHR_materials_specular/)
and [KHR_materials_volume](https://registry.khronos.org/glTF/extensions/2.0/Khronos/KHR_materials_volume/).

## Decision

### Offline bake

The CPU baker flattens the scene into caller-owned triangles and builds a
deterministic, binned-SAH BVH. Scene creation discards exact zero-area triangles
from cooked geometry, retaining every positive finite area. The BVH rejects
non-finite or degenerate input at its boundary, accepts at most 8,000,000
triangles, partitions in place
into leaves of at most four triangles, and bounds depth to 128. The bake arena
owns the pre-sized BVH nodes; it does not own triangle storage.

Automatic room detection voxelizes blocking triangles conservatively. Voxel
occupancy is limited to 8,000,000 cells, the grid has at most 256 probe nodes in
total, and the voxel size is at most half the smallest derived probe spacing.
One-cell dilation supplies clearance from boundaries. Exterior empty voxels are
flood-filled, then remaining empty components receive region IDs.

A region is retained only when a representative's nearest blocking boundary on
all six axial rays is front-facing toward room air. Missing, grazing, mixed, or
outward-facing boundaries reject the region. An interpolation cell is valid only
when every overlapping dilated occupancy voxel is clear and has the cell's one
region ID. This full-cell proof is deliberately conservative: an invalid,
outside, or unclassified cell falls back to existing global/probe diffuse
lighting instead of leaking through geometry.

The integrator traces multi-bounce diffuse transport and uses a bounded
fixed-radius photon density estimate for analytic-light caustics after qualifying
specular or thick eta-changing chains. Photon normalization and visibility stay
in the offline bake. The estimator smooths caustics by its radius; packed diffuse
L2 cannot reproduce sharp caustic detail at runtime. The transport design is
based on [PBRT's stochastic progressive photon mapping treatment](https://pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/Stochastic_Progressive_Photon_Mapping).

### Portable artifact and provenance

`DVOL` v1 is a versioned, explicit little-endian byte format. It has a 112-byte
header, 116-byte probe record, and 4-byte cell record. The header carries
layout, dimensions, coordinate data, payload layout, and separate payload and
header CRC32 values. Each probe stores a nonzero room region and the canonical
`VkrShL2Packed` coefficients; each cell stores its valid region or zero. Native
struct serialization is prohibited.

[`tools/bake_diffuse_volume.py`](../../tools/bake_diffuse_volume.py) writes an
inspect manifest before baking, records the complete source dependency closure,
recipe, tool and wrapper digests, and validates the resulting `.vkdv` file. It
checks the same closure before publication and writes the sidecar
`.vkdv.bake.json`; `--check --output volume.vkdv` rejects stale inputs or corrupt
output. The wrapper publishes only after its temporary output, manifest, and
source checks succeed.

### Runtime and shader contract

A scene's optional `diffuse_volume.path` is prepared on a worker and uploaded on
the render thread under the existing resource finalization contract. The scene
owns one immutable `8 × probe_count` RGBA32F texture and its lattice binding;
replacement and scene reset retire the texture after its last completed GPU use.
Texels 0 through 6 contain canonical SH vectors. Texel 7 stores probe region in
`x` and lower-corner cell region in `y`, with zero marking an invalid cell.

The shader first validates the single lower-corner cell proof, then trilinearly
combines all eight matching probe records. It evaluates the stored `E/π` response
and replaces only global/probe **diffuse** indirect lighting. Existing probe and
global specular remain active, and existing ambient occlusion still applies.

Diffuse volumes entered `VkrFrameInput` before version 37 added rectangle lights.
The Metal frame root is now 512 bytes: the diffuse-volume texture remains at 480
and its 48-byte parameter pointer at 488 (origin at 0, inverse spacing at 16,
dimensions at 32); the later LTC pointer is at 496. The Vulkan root is now 576
bytes: the volume texture remains at 496 and values at 512/528/544; its later LTC
block pointer is at 560. [ADR-044](044-shader-cross-backend-contract.md) owns
cross-backend ABI validation.

## Consequences

Static geometry, material boundary policy, and baked lighting changes require a
rebake. Invalid coverage preserves the old global/probe diffuse path, which can
be less local but cannot claim a room proof. The volume does not supply dynamic
indirect lighting, sharp caustic maps, or runtime glass transport.

The fixed-radius photon estimate has smoothing bias. Packed L2 preserves broad
diffuse color and directional variation but loses high-frequency caustics. These
limits are accepted for the bounded runtime texture and eight-probe lookup.

## Alternatives considered

Using broad reflection-probe bounds for diffuse lighting cannot establish wall
occlusion or room membership. Per-frame ray tracing moves unbounded static work
into the frame budget. A ray-free raster capture bake remains possible, but it
does not by itself establish converged multi-bounce transport or the accepted
room-containment proof.

## Evidence and remaining checks

The native Metal opaque and `BLEND` numeric case reports maximum HDR error
`0.000330536` and passes the baked colored-room fixture. Its report digest is
`cceec0b4c93c8e27f51620e8958e6bbbdd814e15b463e52a28a34b0e8758cd2d`; the API
validation report digest is
`bfa61103bf2deee59dc6338a3210047db4f216e1cbac075e9a1d821311d38733`.

The CPU Lambert furnace evaluates 27 valid probes on six axes. With emission
1 and albedo 0.5, depth 1 gives 1 and depth 3 gives 1.75; maximum errors are
2.91e-9 and 5.10e-9. Actual compiled Vulkan reflection confirms the 576-byte
root, the unchanged volume offsets, and the appended LTC block.

Bistro inspection succeeds with 4,208,488 triangles after creation compacts 518
exact zero-area triangles. A two-triangle fixture discards its collapsed triangle
and retains its 0.001-edge triangle. The rebuilt CPU baker completes a nested,
shared-material glass fixture containing blended receivers: 20,000 emitted
photons produce 2,178 caustic deposits, and all 100 valid probes yield finite SH.
The `.vkdv` SHA-256 is
`583fa2113db932af81095936d492ae7289b1ae1588786a3f7fad78a90da4615f`.

On Metal, an all-invalid volume and a scene without a volume produce byte-identical
HDR payloads (SHA-256
`d130dbb29986f49ef80044a67dbdad3f0679a82c64531b7f360e0dd1754018f7`).
The wrapper detects stale source files, corrupt output, and source/output aliasing.

Native Vulkan execution is unavailable; Metal evidence and compiled reflection
do not establish Vulkan parity. These checks establish the implemented transport,
asset and runtime paths; they do not establish converged lighting quality or a
Bistro bake-time target.

## Revisit when

Revisit the grid and artifact only after a measured need for more than 256
probes, dynamic indirect lighting, sharper caustic reconstruction, or a
cross-backend representation that preserves the same full-cell room proof.

## Code evidence

- [Bake geometry](../../tools/bake/vkr_bake_geometry.h), [BVH](../../tools/bake/vkr_bake_bvh.h), [room voxelizer](../../tools/bake/vkr_bake_voxels.h), and [transport integrator](../../tools/bake/vkr_bake_integrator.h)
- [DVOL codec](../../runtime/src/assets/vkr_diffuse_volume.h) and [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c)
- [Scene-owned binding](../../runtime/src/renderer/systems/vkr_scene_system.h), [frame input](../../renderer/src/vkr_frame_input.h), and [portable sampling kernel](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh)
