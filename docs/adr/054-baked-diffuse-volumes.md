---
status: implemented
updated: 2026-10-08
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

A managed project bakes the scene the runtime loads
([`vkr_project_bake.c`](../../tools/bakery/project/vkr_project_bake.c)): the
lowered document with its authored overrides, then the project World's
entities, then the overlay's editor-created entities, whose light edit values
become the scene's light blocks and whose components carry over; a hidden
created entity and its subtree stay out, as do dynamic lights, which no bake
holds ([ADR-088](088-baked-lightmap-sets.md)). Every baked light casts
shadows, whatever its `casts_shadow`: that flag prices runtime shadow maps,
while a baked light is static and reaches the screen only through its bake.
Until atmosphere model version 3 (2026-10-05) bakes left directional lights
unshadowed, so sun light reached closed rooms, and until 2026-10-08 they left
point, spot and model lights without `casts_shadow` unshadowed, so lamps lit
the far side of walls; volumes and lightmaps baked before then need a rebake,
which `--check` does not report. The bake scene loader builds every
solid or visual brush from its `brush_face` children as the runtime does
([ADR-084](084-agent-channel-and-level-design-toolkit.md)), with the face
material or the dev grid, and keeps each entity's document id. It builds
each blockout shape's pieces as the runtime does, as geometry without a
lightmap (ADR-088). Since 2026-10-08 it leaves out every brush and blockout
shape that a `mover` on the entity or an ancestor moves, as the runtime's
`brush_mover_of` finds one: the volume neither occludes nor bounces light
off it and lights it at runtime, so a closed door no longer stops baked
light between rooms. It finds an entity block by key at the entity's root or
in its `components` object, never inside another block, so a blockout's
`shape` field is not a `shape` block; components the bake does not read are
ignored. A failed load prints the entity (index, name, id) and the block or
file at fault after `Scene preparation failed`. It reads the
`atmosphere`, `environment` and `subsurface` blocks at the top level of the
document only; when the scene has no `atmosphere` or `environment` block of
its own, the effective scene takes the component of the scene's own entities
or else the World's, as runtime resolution does. An authored override of an
entity the loader synthesizes from a top-level block, such as a block's
Sky Atmosphere, has no document entity and fails the bake; edit the block
instead. Verified 2026-10-05 on a blockout made
through `vkr_mcp` (two rooms joined by a doorway, a sealed room, three lamps
and the World sun): before this the effective scene held no entity and every
bake of an editor-built level was empty; after it, the volume bake found 39
valid probes in two regions and published.

The volume holds one SH set per probe for each baked light layer
([ADR-088](088-baked-lightmap-sets.md)), planned as for lightmaps
([`vkr_bake_layers.h`](../../tools/bake/vkr_bake_layers.h)): in an atmosphere
scene one layer per sun key, baked under that key's atmosphere with the key
light and the sky, then one per static light group with the group's lights,
where only the default group holds surface emission and, without a sun key,
the sky. Caustic photons are emitted per layer from its own analytic lights.

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
flood-filled from the bounds' faces, so explicit `--bounds` must enclose the
rooms' walls, floors and ceilings; then remaining empty components receive
region IDs.

Without `--grid`, the baker fits the grid to the bounds
(`vkr_bake_voxels_fit_grid`): the finest near-cubic spacing, at least 1 m,
whose grid of at least four probes per axis holds at most 256 probes. Probes
sit half a spacing inside the bounds and the cell proof below reaches one
voxel past the cell into the dilated boundary, so where geometry lies on a
bounds face the outer cell on that side never proves clear; four probes leave
an inner cell layer. The fixed 4 × 4 × 4 default used before 2026-10-08
ignored the bounds. `test_fitted_volume_grid_finds_every_room` requires that
a 48 × 4 × 12 m row of four rooms, which fits 16 × 4 × 4 probes at
3 × 1 × 3 m, proves cells in every room, and that 4 × 4 × 4, which spaces
its probes a room apart there, proves none; the test has not run yet.

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

`DVOL` v2 is a versioned, explicit little-endian byte format. It has a
112-byte header, the layer table of 64-byte light-layer records
([`vkr_light_layers.h`](../../runtime/src/assets/vkr_light_layers.h)), one
probe record per probe and a 4-byte cell record. The header carries layout,
dimensions, the layer count, coordinate data, payload layout, and separate
payload and header CRC32 values. Each probe stores a nonzero room region and
one canonical `VkrShL2Packed` set per layer; each cell stores its valid region
or zero. Native struct serialization is prohibited. Version 1 held one SH set
of all light per probe; the loader refuses it and asks for a new bake, which
the sun-shadow fix requires anyway. `vkr_bakery` verifies outputs with the
runtime decoder.

[`vkr_bakery bake diffuse`](../../tools/bakery/vkr_bakery_bake.c) writes an
inspect manifest before baking, records the complete source dependency closure,
recipe and tool digest, and validates the resulting `.vkdv` file. It checks the
same closure before publication and writes the sidecar `.vkdv.bake.json`;
`--check --output volume.vkdv` rejects stale inputs or corrupt output. The
command publishes only after its temporary output, manifest, and source checks
succeed. When inspection finds no valid cell, it stops before the bake pass and
exits 3 without output; a scene without geometry inspects as zero probes and
cells and takes the same path. The baker traces probes on worker threads; each path's
seed derives from its probe, pixel and sample, so the volume is byte-identical
for any `--threads` value ([ADR-077](077-asset-build-system.md)). The baker refuses to bake an
all-invalid volume, and such a volume would render like no volume. Open and
exterior scenes such as Bistro took this path at the fixed 4 × 4 × 4 grid; no
one has inspected Bistro at the fitted grid.

### Runtime and shader contract

A scene's optional `diffuse_volume.path` is prepared on a worker and uploaded on
the render thread under the existing resource finalization contract. The scene
owns one immutable `8 × probe_count` RGBA32F texture and its lattice binding;
replacement and scene reset retire the texture after its last completed GPU use.
The scene also keeps every layer's SH and composes the texture as their
weighted sum ([ADR-090](090-time-of-day.md)): the two sun keys nearest the
current sun on its daily circle share weight one by angle
(`vkr_light_layers_sun_weights`), and each lamp group weighs its light group's
factor. When a weight moves by more than one percent, at most every 0.25
seconds, the scene composes a new texture and releases the previous one
(`vkr_scene_update_diffuse_volume`). Shaders read the composed texture as
before.
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
rebake. A volume costs one SH set per probe per layer: ten layers on the
blockout baked in 14.6 s instead of 5.2 s for one. Invalid coverage preserves the old global/probe diffuse path, which can
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

On the Metal desktop implementation, removed on 2026-10-06, the native opaque
and `BLEND` numeric case reported maximum HDR error `0.000330536` and passed the
baked colored-room fixture. Its report digest is
`cceec0b4c93c8e27f51620e8958e6bbbdd814e15b463e52a28a34b0e8758cd2d`.

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

On the same implementation, an all-invalid volume and a scene without a volume
produced byte-identical HDR payloads (SHA-256
`d130dbb29986f49ef80044a67dbdad3f0679a82c64531b7f360e0dd1754018f7`).
`bake diffuse --check` detects stale source files, corrupt output, and source/output aliasing.

The ADR-088 blockout (atmosphere celestial pole set to (0, 0.8, −0.6) so its
sun sets) baked ten layers, eight sun keys and the `default` and `warm` lamp
groups, at 1.4 to 1.6 s each. Before root-scoped block lookup the bake read
the World entity's atmosphere component instead of the scene's block and
turned the keys about the default pole. In the editor with every lamp group
at zero and manual exposure, the closed room shows daylight bounce at noon and
is black at midnight. `test_light_layer_sun_weights` and
`test_diffuse_volume_layers_round_trip` cover the weights and the codec.

Neither native Vulkan nor the tiled pipeline has run the numeric case;
evidence from the removed Metal desktop implementation and compiled reflection
do not establish their output. These checks establish the implemented transport,
asset and runtime paths; they do not establish converged lighting quality or a
Bistro bake-time target.

## Revisit when

Revisit the grid and artifact only after a measured need for more than 256
probes, dynamic indirect lighting, sharper caustic reconstruction, or a
cross-backend representation that preserves the same full-cell room proof.

The first condition is met. A 25,000-entity indoor brush level of about
350 × 30 × 250 m, with rooms 4 to 20 m wide, inspected at the fixed
4 × 4 × 4 grid on 2026-10-08 as 64 probes, 0 valid, and 0 of 27 valid cells,
so its bake skipped the volume. The fitted grid gives it 9 × 4 × 7 = 252
probes at about 39 × 7.5 × 36 m, cells larger than any of its rooms, so no
cell can lie inside one room. One volume of 256 probes cannot resolve rooms
of that size across a level of that size; it needs about 1 to 2 m spacing in
the rooms only, as the
[sparse diffuse volumes proposal](../proposals/sparse-diffuse-volumes.md)
places it.

## Code evidence

- [Bake geometry](../../tools/bake/vkr_bake_geometry.h), [BVH](../../tools/bake/vkr_bake_bvh.h), [room voxelizer](../../tools/bake/vkr_bake_voxels.h), and [transport integrator](../../tools/bake/vkr_bake_integrator.h)
- [DVOL codec](../../runtime/src/assets/vkr_diffuse_volume.h) and [scene loader](../../runtime/src/renderer/resources/loaders/scene_loader.c)
- [Scene-owned binding](../../runtime/src/renderer/systems/vkr_scene_system.h), [frame input](../../renderer/src/vkr_frame_input.h), and [portable sampling kernel](../../renderer/src/shaders/shared/diffuse_volume_kernel.slangh)
