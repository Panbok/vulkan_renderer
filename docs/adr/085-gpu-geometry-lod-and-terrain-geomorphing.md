---
status: partial
updated: 2026-10-06
authority: adr
---

# ADR-085: GPU geometry LOD table and terrain geomorphing

## Status

Accepted (partial). The LOD table, per-view selection, level encoding and
terrain geomorphing are implemented in both backends and used by both
pipeline classes, and cooked static meshes carry levels (2026-10-06). Native
Vulkan runs terrain levels (2026-10-04); cooked levels have no native Vulkan
run yet.

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
have seven levels, 64 cells a side down to one, each with its skirt. A tile
with a hole sample (ADR-084) keeps level 0 alone, so its row has one level
and never morphs. A level's error is its surface's largest height difference from the samples,
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

### Cooked mesh levels

The mesh cooker gives each static range of the world domain up to six levels
after itself (`vkr_mesh_cooked_build_lods` in
[vkr_mesh_encode.c](../../tools/assets/vkr_mesh_encode.c)); skinned ranges and
other domains, blended surfaces among them, keep one. Each level targets half
the previous level's triangles and is simplified from the range with
meshoptimizer's `meshopt_simplifyWithAttributes`, so errors do not compound:
open borders stay locked, so ranges that meet do not crack; small disconnected
parts may be pruned; normals and texture coordinates weigh 0.1 of a mesh
extent per unit. Levels stop when one keeps more than 85% of the previous
level or would fall below 64 triangles. A level's error is meshoptimizer's
absolute result error in model space, attribute and pruning terms included,
raised to the previous level's where it is smaller. Level indices reference
the range's own vertices and are ordered for the vertex cache; vertices are
not reordered by level.

Static artifacts stay at version 17 and set header flag
`VKR_MESH_COOKED_FLAG_LOD` (bit 1,
[vkr_mesh_cooked.h](../../runtime/src/assets/vkr_mesh_cooked.h)). Its block
follows the source metadata and any lightmap block: one 72-byte entry per range
with the level count, the CRC, offset and size of one meshoptimizer index
stream that holds levels 1 and up, and each level's index count and error.
Those streams follow every range's own streams, in range order. The reader
rejects more than seven levels, an error that shrinks, a level no smaller than
the one before it, and streams out of order; artifacts without the flag decode
with one level per range. The decoder places all levels after every range's
indices in the merged index buffer and appends the LOD rows after the decode
records; the mesh manager ends the merged geometry's default submesh before
the level indices.

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

Cooked levels change a range's silhouette by up to a pixel where they switch,
without morphing. Lightmapped Bistro keeps 65% more index data (3.44 million
indices over 5.28 million; the artifact grows from 43.0 to 45.8 MB) and cooks
in 28 s instead of 26 s. Locked open borders and the lightmap's chart seams
stop most of its ranges after one to three levels: without a lightmap the same
model reaches level 2 with 1.08 million indices instead of 0.83 million.

Simplifying a source mesh's primitives together, with one selection sphere
per group so its rows switch level at once, unlocks the borders between
them without cracks. It was built and dropped (2026-10-06) because Bistro
gains nothing: each of its meshes has one primitive, and grouping the
per-material meshes that always share a transform would unlock 2.5% of its
open-border vertices. The rest are true open edges of cards, petals and open
shells. Unlocking every open border, the ceiling of any grouping, saved
0.3 ms of `Tiled.Opaque` p95 and up to 0.2 ms per cascade but visibly thinned
alpha-tested petals ([evidence](#evidence)).

## Revisit when

A camera threshold other than one pixel is wanted per quality preset, shadow
views need morphing, or meshes whose primitives, or per-material meshes
sharing one transform, meet along their borders carry a large share of a
scene's triangles.

## Evidence

- `./build_test.sh` suite `mesh_cooked` covers metadata validation: a valid
  row, an unreferenced row, a row before its decode record, a shrinking error,
  too many levels, and level ranges outside the buffer or not matching the
  range (2026-10-04, macOS Debug). Suite `heightfield` covers terrain tile
  levels: each level covers the tile's cells once counter-clockwise, a spike
  at an odd sample is level 1's error, and flat ground costs the cell floor;
  and terrain holes: Jolt's height field from the same samples hits exactly
  where a drawn triangle covers four probes per cell (2026-10-06, Windows
  Debug).
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
- Headless Windows Debug editor on Bistro, RX 6700 XT (2026-10-04): the same
  1 km terrain under Vulkan validation with synchronization checks reports no
  VUID; wireframe captures show coarser tiles far away and finer ones as the
  camera nears a hill, with no cracks; the layer blend paints the chosen
  layer. An 8 km streamed terrain showed thin dark strips at some tile edges
  (level toolkit audit A20): the driver offset the LOD row's pointer cast by
  the row's stride, so holed overview tiles read a zero row. The row's address
  is now byte arithmetic, and the strips are gone (2026-10-05).
- `mesh_cooked` suite, `test_mesh_cooked_lod_round_trip`: a curved 40 × 40
  grid gains halving levels with growing errors, a blended copy keeps one,
  cooking is deterministic, the decoded row passes the publication checks and
  its levels draw only the range's vertices; entries with too many levels or
  a negative error, and a cleared header flag, are rejected (2026-10-06,
  macOS Release).
- Cooked levels, Release, M1 Pro (2026-10-06): lightmapped Bistro cooked
  without and with levels, alternated on one build, two runs each. Captures of
  `tiled_bistro_baked_capture` and its desktop counterpart on the Metal
  desktop implementation, `tiled_bistro_baked_capture_desktop`, a Metal
  desktop case since removed, differ in 3,560 and 3,794 pixels by more than 2
  of 255, as sub-pixel edge shifts. Pass timings
  (`local-windowed-gpu-single`, median / p95):

  | Case and pass | Without levels | With levels |
  |---|---|---|
  | `tiled_bistro_baked_native`, `Tiled.Opaque` | 8.74 / 12.52, 8.90 / 12.63 ms | 7.57 / 11.63, 7.56 / 11.60 ms |
  | same, `Shadow.Cascade.0` / `.2` / `.3` per refresh | 1.32, 2.15, 1.63 ms | 1.12, 1.47, 0.87 ms |
  | `tiled_bistro_baked_dynamic_native`, `Tiled.Opaque` | 10.60 / 17.95, 10.75 / 17.83 ms | 9.36 / 16.78, 9.50 / 16.75 ms |
  | `tiled_bistro_baked_native_desktop` (Metal desktop case since removed), `VBuffer.Opaque` | 1.87 / 2.37, 1.89 / 2.95 ms | 1.44 / 1.77, 1.46 / 1.88 ms |

  `gpu.submission` (`local-windowed-gpu-submission-single`): the tiled native
  orbit 12.56 / 16.66 and 12.61 / 16.84 ms without levels, 11.33 / 15.52 and
  11.29 / 15.49 ms with them; with its dynamic lights 14.44 / 21.87 and
  14.38 / 21.85 ms, then 13.23 / 20.81 and 13.27 / 20.70 ms; the Metal
  desktop orbit 54.61 / 69.20 and 54.63 / 68.96 ms, then 54.27 / 68.45 and
  54.07 / 68.38 ms. Reports, without then with levels: tiled native
  `sha256:1981b30e239480dda4f3f0e3e7750e607c25cbd56783426f8640ef446f725f2d`,
  `sha256:81e7f27e97d19c3ab79b37b9049b3a51d4ef5323e9ac5ac3124cf621a4cbcdfa`,
  `sha256:c23fafdcce47d94d8ea84d794ddf731d45466fb6a99c403a604f2dc90c26059c`
  (`incomplete` on warm-up stability),
  `sha256:91879b5a6bbe26c8bc3132146424396763b69f39dd996f086a88fd1a50349bf3`;
  dynamic
  `sha256:a7322dfd97e2201729783ae688d9766d59ce7470dd2e02f64f36c30f2cb60fa8`,
  `sha256:fb0c91b69756aa8fdabe82e21861952ab14861685fa84e339ab81771c46a41fa`,
  `sha256:d97a98b03ae7f0162072e07979b869e95c534bbe2f94afdf64660466be646d00`,
  `sha256:1df477985cfd45a84a87e1b1ecf688f308ec6240c125b5100880679eac47e889`;
  desktop
  `sha256:5fcdebb6280c6ad6a47af95b1b7a32412fa6edb9ee8bb9ffd31b939b54590b39`,
  `sha256:3b0263df6bf5c397f0bf644a9d11fb1915b04be948ddb874f5dcec873a1b7fab`,
  `sha256:b139f02a8e9b85a026abeb7f576896875124ac859b7835805958bd447038e999`,
  `sha256:165e2080959722edf54f79a39a2d010c3b7d8dc89acd1281ed8f0e75261ffe1f`.
  Pass reports: tiled native
  `sha256:0996864075badd6be034f997e219dbd0275cb7b611165d710f3ce860619d1e4e`,
  `sha256:7b1deab79c9a42531fed9c99b0fdc22233f9ef586ba69921e83a465b1dd5a43d`,
  `sha256:06d19daeecd224a1f8c18e69e2a228f867ca22915d01637ae2743b4dff76ae34`,
  `sha256:10c04e45a0218c428a480556ea60d8c7aa053c404e1ee8de7b4bf457c6241e5a`;
  dynamic
  `sha256:c52d46ea307ba0446cb6115eccb84d422f8e846819c96394b141b4ace127f5ad`,
  `sha256:c42aeadb14acfb01834c5ef4f127bb0e1582ca9b49c9d4274a14f690865430f8`,
  `sha256:0a08f3479552f7a778f39b0a0e140011c591873154846367021d8b15d677a3c1`,
  `sha256:9ff147381353169cfdfa33602df78ea4a6b277f8bf03c2a68f1d09844f70d3b5`;
  desktop
  `sha256:6140ee393e6c118d3d2fb880c2bdef258f2ff19d3c671c8d5b794fe59b888c38`,
  `sha256:8581b1a9bf52eaff1ee1e9b82351ab5bc7ddd898599374e2abd3d7fc39c3e7d2`,
  `sha256:c1f7330443dca5fcbec5a679c8aaddb48e656300193f5a62469989befef9c342`,
  `sha256:63be06f798329ada6844b22522f088b99b2489bc4db672a70f25f3468d22d0f2`.
  Weight 0.5 for the attributes, without pruning, removed only 0.77 ms
  median from `Tiled.Opaque`; pruning alone moved cascade 3 from 1.20 to
  0.91 ms. The tiled capture case passes under Metal API validation with the
  levels published.
- Border unlocking, Release, M1 Pro (2026-10-06): `bistro-lights.gltf` places
  551 single-primitive meshes on 5,982 nodes. Of its 404,551 open-border
  vertices, counted per mesh in position space, 10,104 lie on the border of
  another mesh that every node places with the same transform (35 groups).
  A diagnostic cook of lightmapped Bistro without meshoptimizer's border
  lock alternated with the locked levels on one build, two runs each
  (`local-windowed-gpu-single`, median / p95): `Tiled.Opaque` in
  `tiled_bistro_baked_native` went from 7.44 / 11.55 ms (both runs) to
  7.25 / 11.23 and 7.32 / 11.23 ms, and in `tiled_bistro_baked_dynamic_native`
  from 9.29 / 16.55 and 9.34 / 16.58 ms to 9.09 / 16.26 and 9.08 / 16.16 ms.
  In the native case cascade 0 went from 1.13 to 1.02 to 1.04 ms and
  cascade 3 from 0.85 to 0.64 ms. Its `tiled_bistro_baked_capture` differs in
  3,891 pixels by more than 2 of 255: flower baskets lose petals, and thin
  frames shift by a pixel. Reports, locked then unlocked: native
  `sha256:9eaffc00325b59c9c51d1f78cb72c1bdb35238aa8662f4fb4ae558f819319d77`,
  `sha256:5a08c00e519459de551ec8b2acd9ce54ed29a4cb367c83c66cf9ea3e2c2442d0`,
  `sha256:7649960cfc46b58eb8c6def165dab825693f9e86f1e363985287a9fe51ff5472`,
  `sha256:155a9ca783bd9dc30862a22f4588a8a89c3295363e4985d03dc0492043fe2b91`;
  dynamic
  `sha256:8ba5dd4c39c21826598325c6138a3fb4e6dc2dd7b847b4d41f01135239d3c08e`,
  `sha256:fcf79f6e8841ea2a9030225613b42c922fab9ea9164f1f515211675d90b5a08e`,
  `sha256:7ffa586e183ea765a446a691edf60b6933d35a6d6055cb9446a3d022b37ecb2d`,
  `sha256:7429d8809e20a66ef8c68f999daaad518f8600860b12dfa27cd68a3ca2ced1eb`.
- Native Vulkan execution of cooked levels is unverified.
