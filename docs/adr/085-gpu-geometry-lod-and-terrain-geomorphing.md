---
status: partial
updated: 2026-10-04
authority: adr
---

# ADR-085: GPU geometry LOD table and terrain geomorphing

## Status

Accepted (partial). The LOD table, per-view selection, level encoding and
terrain geomorphing are implemented in both backends; terrain tiles are the
only ranges with levels. Cooked mesh levels remain in the automatic mesh LOD
proposal. Native Vulkan execution is unverified.

## Context

Every visible candidate rasterized its full index range in every view
([ADR-028](028-gpu-driven-deferred-visibility-buffer.md)). A 1 km terrain at
1 m spacing ([ADR-084](084-agent-channel-and-level-design-toolkit.md#terrain))
is two million triangles per view, most of them far from the camera. The
owner chose a shared GPU LOD table, which cooked mesh LOD will use too, with
geomorphing for terrain so that level changes do not pop.

## Decision

### Table

A range with levels has a `VkrGpuGeometryLodRow`
([vkr_gpu_abi.h](../../renderer/src/vkr_gpu_abi.h)): a level count of up to
seven, flags, and per level a first index relative to the range, an index
count and a model-space error in metres. Level 0 is the range itself; later
levels draw fewer indices over the same vertices and decode record, and their
errors never shrink. Rows live in the geometry's decode metadata after its
decode records, four records each, in the order of the decode records that
reference them; a decode record's `lod_record` gives its row's record offset,
zero for one level. Their lifetime is the geometry publication's.
`vkr_packed_geometry_metadata_is_valid` and
`vkr_packed_geometry_lod_ranges_are_valid`
([vkr_packed_geometry.c](../../renderer/src/vkr_packed_geometry.c)) validate
rows, level ranges and their vertex indices at publication in both backends.
Runtime geometry declares one range's levels through `VkrGeometryConfig.lod`
and `range_index_count`; cooked files carry none.

### Selection

Each culling view has a `VkrGpuLodView`: its position, the image size of one
metre of error over the 1-pixel threshold (`vkr_gpu_lod_view`), and flags. A
perspective view divides by the distance to the candidate's bounding sphere;
an orthographic view (a top camera or a cascade) does not. The camera morphs;
shadow views do not, so cached shadow content stays valid. Classification
([lod_kernel.slangh](../../renderer/src/shaders/shared/lod_kernel.slangh),
`vkr_gpu_lod_select`) picks the coarsest level whose error, grown by the
instance's sphere stretch, projects to at most one, for candidates with
valid bounds, and packs it above the bucket in the classification word.
Encoding draws that level's indices and writes the visible row's state bits:
bit 7 marks a morphing terrain grid, bits 8 to 10 hold the level, and bits 11
to 18 and 19 to 26 the morph factor now and one frame earlier. The draw count
and visible row per candidate are unchanged.

### Terrain geomorphing

Terrain tiles ([vkr_scene_terrain.c](../../runtime/src/renderer/systems/vkr_scene_terrain.c))
have seven levels, 64 cells a side down to one, each with its skirt. A
level's error is its surface's largest height difference from the samples,
at least a tenth of its cell size so that painted weights thin out with
distance. A morphing view starts moving the vertices level L + 1 drops at 75%
of the distance where level L + 1 takes over, and has finished there, so the
switch changes no geometry. A dropped vertex moves to the midpoint of the two
level L + 1 vertices on its edge or diagonal, which
[terrain_kernel.slangh](../../renderer/src/shaders/shared/terrain_kernel.slangh)
finds from its grid position; its normal and layer weights follow.
`vkr_gpu_terrain_morph` applies the same morph in the visibility raster, both
G-buffer resolves, transmission surfaces and the forward vertex path, and
moves the previous position by the previous frame's morph for motion
vectors. Picking reads the morphed visibility buffer and depth. Skirts hide
cracks between tiles at different levels or morphs.

## Consequences

Distant terrain costs a fraction of its full-resolution triangles, and level
changes do not pop at the camera. Coarser levels thin terrain layer weights
with their vertices. The vertex shader of a morphing tile decodes up to three
more vertices per vertex, and the resolve per corner. Shadow views can draw a
terrain coarser than the camera within one texel of error. The classify
kernel reads geometry rows, which Metal's classify root now carries.

## Alternatives considered

- CPU level selection before candidate publication would leave cascades and
  local faces at one level per frame and the GPU path unaware of levels.
- Per-view separate index buffers per neighbour combination (stitching)
  instead of skirts would multiply index data by the neighbour patterns.
- A separate LOD table indexed per candidate would need per-frame uploads;
  decode metadata already shares the geometry's lifetime.

## Revisit when

Cooked meshes gain levels (the automatic mesh LOD proposal), a camera
threshold other than one pixel is wanted per quality preset, or shadow views
need morphing.

## Evidence

- `./build_test.sh` suite `mesh_cooked` covers metadata validation: a valid
  row, an unreferenced row, a row before its decode record, a shrinking error,
  too many levels, and level ranges outside the buffer or not matching the
  range (2026-10-04, macOS Debug). Suite `heightfield` covers terrain tile
  levels: each level covers the tile's cells once counter-clockwise, a spike
  at an odd sample is level 1's error, and flat ground costs the cell floor.
- Headless macOS Release on Bistro, M1 Pro (2026-10-04): a 1 km terrain at
  1 m spacing north of the town. Wireframe captures from 300 m show finer
  tiles below the camera and coarser ones around them; an orthographic top
  capture shows coarse flat tiles and full-resolution hills; lit captures show
  no cracks. The run under Metal API validation reports no diagnostics. All
  105 Vulkan modules pass `spirv-val --target-env vulkan1.4
  --scalar-block-layout`.
- Geomorphing, from a 32-step, 5 m dolly toward hills, measured as the mean
  absolute RGB change between consecutive captures: at the 1-pixel threshold
  neither morphing nor a no-morph control shows a spike, as pops stay below a
  pixel. With the threshold raised to 16 pixels for the experiment, the
  largest local spike was 0.092 with morphing and 0.238 without, and the sum
  of positive spikes 0.58 against 1.03.
- Indicative, not a harness claim: the headless editor's `stats.frame_ms`
  with the hilly 1 km terrain in view was 8.78 ms while selection was inert
  and 8.55 ms with it.
- Native Vulkan execution and a matched Release harness measurement are
  unverified.
