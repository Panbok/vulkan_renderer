---
status: partial
updated: 2026-09-13
authority: adr
---

# Animation assets, compute skinning and editor playback

## Status

Accepted (partial). The standalone glTF cooker, `.vka` reader and CPU pose library
are implemented and tested. Mesh cooking also preserves vertex influences and
node skin bindings under [ADR-030](030-offline-mesh-optimization-and-cooking.md).
Typed animation resource loading, per-wrapper playback and optional scene JSON
bindings are implemented. Both renderer backends now consume CPU palettes through
compute skinning, with per-instance output, conservative current bounds and
submitted deformation history. A movable editor window has independent playback,
a model preview, parameter-driven blend graph, conditional state transitions and
sequence timeline. Managed import/rebuild publishes the mesh and bank together. Native checks are recorded below; shader parity
remains UNALIGNED under [ADR-044](044-shader-cross-backend-contract.md).

## Context

The player asset contains 25 clips, 68 joints and 72 source nodes. Its animations
include STEP channels, very small positive scales used to hide props, and adjacent
quaternions with opposite signs. Importing only joint nodes, forcing unit scales,
or resampling before establishing a reference would lose authored behavior.

## Decision

Keep animation data in a standalone `.vka` bank. Preserve float32 source key times
and values with their STEP, LINEAR or CUBICSPLINE interpolation. Uniform baking,
compression and error budgets remain future work. The existing static `.vkb`
version 17 remains supported; version 18 adds CPU skin data for skinned sources.
Immutable static geometry remains shared; deformation is a per-instance stream.

Retain all source nodes with stable indices, parent links, rest TRS or authored
matrices, and a validated parent-first evaluation order. Each skin retains its
joint mapping and inverse bind matrices. This bank does not yet encode the
mesh-to-skin assignment or vertex influences; those belong to the cooked mesh
and are now preserved in its version 18 extension. Each clip
retains its name, duration and node-targeted TRS channels. The importer rejects
morph channels, required extensions and unresolved compressed accessors rather
than silently discarding them. Sparse and strided core glTF accessors are read
through cgltf; source files are never rewritten.

The [schema and sampling API](../../runtime/src/assets/vkr_animation.h) separate
immutable asset data from caller-owned pose buffers. Sampling is stateless,
uses seconds, and clamps to clip endpoints; it does not implement playback
clocks, looping, events or root-motion extraction. LINEAR rotation uses the short
quaternion arc. Cubic tangents retain their source signs and time scaling, with
normalization of the interpolated quaternion. Static matrix ancestors participate
in global transforms. Skin palettes contain `global_joint * inverse_bind` in
asset space; an eventual instance transform must be applied once.

The [reader](../../runtime/src/assets/vkr_animation_cooked.c) and
[encoder](../../tools/assets/vkr_animation_encode.c) define little-endian version 1.
A 48-byte header contains magic/version, file size, source fingerprint, checksum,
three counts and a reserved field; sequential records contain names, nodes,
evaluation order, skins and channels. No native pointers or struct padding are
serialized. The FNV-1a source fingerprint and checksum detect ordinary changes
and corruption; they are not security hashes or mesh compatibility proofs.

Decode validates lengths, counts, hierarchy, channels, finite values and checksum
before publishing an asset. Encoded size and decoded storage are capped at 1 GiB;
node, clip and key limits are explicit in the shared header. Result storage belongs
to a caller arena, independent of input bytes and scratch storage. Decode failure
leaves the output empty. Import can leave partial allocations in its result arena
on failure, which the caller must release. Sampling allocates nothing and borrows
validated assets and caller-provided buffers. No GPU ownership is introduced.

The [standalone cooker](../../tools/vkr_animation_cooker.c) imports and atomically
writes an explicitly named `.vka`, or inspects and samples an existing bank. The
shell and Windows wrappers build it without selecting or modifying project assets.

### Runtime ownership and playback

The [animation loader](../../runtime/src/renderer/resources/loaders/animation_loader.c)
registers `VKR_RESOURCE_TYPE_ANIMATION`. It decodes `.vka` through the existing
CPU worker path when asynchronous loading is enabled. Each ready result owns an
arena and source path; keeping a result requires keeping its resource request.
Pending cancellation retains the request key until worker completion drains.

Each [player](../../runtime/src/animation/vkr_animation_player.h) borrows a bank
and owns an independently destroyable arena with preallocated local poses, two
global-pose buffers and two sets of skin palettes. Playback uses compensated
float64 seconds. Advance accepts nonnegative elapsed time; finite negative rates
play in reverse. Looping wraps into `[0, duration)`, nonlooping clamps, and seek
allows the exact endpoint even in loop mode. Pause preserves playback intent;
selecting a clip resets its time. Initial time is zero, including reverse playback.
Crossfades and weighted local-pose sampling are implemented. Fixed-step simulation,
events and root-motion extraction remain outside this implementation.

Evaluation publishes the new clock and pose only after all globals and palettes
succeed. Failure retains the last good snapshot. Views expire on the next successful
pose mutation. Pose generation and discontinuity counters distinguish changes and
seek/selection/wrap; they are not previous-rendered-pose history. That history still
requires a renderer submission and GPU completion contract.

The [scene binding](../../runtime/src/renderer/systems/vkr_scene_animation.h) takes
ownership of ready mesh and bank requests on successful attachment. It checks the
original-source fingerprint, node parents/rest matrices, clip and skin counts,
palette sizes and source-index-to-entity mapping. This initial binding requires a
version 18 mesh with skin definitions. Rigid-only version 17 assets do not yet carry
the matching animation identity. Binding against modified source rest matrices is
rejected; the authored wrapper remains the placement transform.

Scene-owned mappings retain full entity generations, not ECS component pointers.
The scene advances each player after authored transform propagation. Deleting the
wrapper or a mapped node detaches its animator; source reparenting or rest edits
invalidate it on update. Shutdown destroys players before releasing their retained
bank and mesh requests. Pose failure pauses that player. Evaluated matrices remain
separate from `SceneTransform`, so editor saves cannot persist a sampled pose as an
authored rest transform. Root motion remains inside the asset-space pose.

### Scene configuration

Both synchronous and asynchronous scene loaders accept an optional entity-level
`animation` object alongside `mesh`:

```json
{
  "name": "Player",
  "mesh": { "path": "./player.vkb" },
  "animation": {
    "path": "./player.vka",
    "clip": 0,
    "rate": 1.0,
    "loop": true,
    "playing": true
  }
}
```

Only `path` is required; the other values shown are defaults. Explicit `./` and `../` paths resolve relative to the scene file; bare paths
retain the existing startup-relative convention. Unknown/duplicate fields, invalid types and out-of-range clip
selection fail loading. The asynchronous loader requests both dependencies in its
worker stage and waits for ready results before attaching. Failure and cancellation
release untransferred requests; scene teardown releases transferred requests. This
configuration enables runtime pose evaluation and compute deformation for skinned
mesh nodes. The movable editor uses a separate player, so scrubbing it does not
advance or seek the scene character.

## Managed publication

Import, pending asset preparation, rebuild and reimport cook a `.vka` beside the
mesh when the source contains skins and clips. Both artifacts belong to one
immutable revision, and lowering adds the runtime `animation.path` reference while
preserving authored clip, rate, loop and playing controls. A failed animation cook
leaves the previous published pair intact. Reimporting a source without clips
removes its generated animation reference. Existing cooked assets require Rebuild;
opening an old scene does not silently recook it.

## Compute deformation and bounds

Frame input version 47 carries skinning jobs and per-instance binding indices.
Each job borrows decoded bind vertices, four normalized influences and the current
asset-space joint palette through `render_frame`. Native frame uploads own the GPU
copies. Mesh vertices, UVs, colors and indices remain immutable. A 64-thread kernel
writes one 32-byte deformation record per geometry-local vertex: float32 position,
octahedral packed normal/tangent, tangent sign and reserved fields.

Positions use the weighted joint matrix, normals its inverse transpose, and
tangents its linear part followed by orthogonalization. Singular blends retain a
finite bind normal and orthogonal tangent fallback; non-finite positions fall back
to the bind position. Skinned candidates use two-sided rasterization because a
blended skin can change local orientation independently of the actor transform.
This first path uploads bind vertices and influences with each submitted job;
immutable GPU caching and deduplication across equal actor/skin bindings remain
future work. No speedup is claimed.

Scene attachment computes per-joint influence boxes from referenced bind vertices.
Each pose transforms their corners and unions the boxes, covering convex weighted
positions with outward rounding and a margin for weight normalization and affine
rounding. These asset-space bounds drive camera culling, blend sorting and shadow
fitting. Pose changes invalidate retained caster content; HZB reuse is conservatively
disabled while skinning jobs are active. Authored ECS transforms remain untouched.

The graph owns a completion-protected history buffer sized to rounded vertex
demand. A packet supports 64 jobs, at most 2,097,152 submitted vertices in total,
and 65,536 joints per job; capacity overflow rejects the frame. At 32 bytes per
vertex, the maximum output is 64 MiB per history instance, allocated from actual
demand rather than committed unconditionally. Preview jobs consume this same
capacity. Source instance records are 96 bytes; native prepared instances are 144
bytes with current and previous deformation addresses at offsets 128 and 136.

Previous positions come from the actual selected transform/color history producer,
with matching geometry, instance generation, vertex count and discontinuity. A
seek, replacement or unavailable producer invalidates motion. Submission commits
the metadata; acquired slot age is never the completion proof. Material resolve
uses current triangle barycentrics to reconstruct previous deformed positions.
Main visibility, shadows, picking, deferred material reconstruction, transmission
and ordinary blend all consume the stream. Rigid-only SSR/SSGI history reuse
rejects deforming surfaces; native Metal additionally avoids choosing incompatible
independent transform producers for its motion reconstruction.

## Movable animation editor

Settings → Animation (or the command palette's Animation command) opens a movable
window inside the editor. It uses the selected animated wrapper or its descendant;
without a selection it chooses the first live animated wrapper. Its own player
supports seconds-based playback, pause, seek, reverse, speed, looping and 1/60-second
step buttons. Those buttons do not lock runtime animation to 60 Hz.

The graph supports Clip, Blend2, 1D blend-space and triangulated 2D blend-space
nodes. Parameters drive mixture weights and sample coordinates. States reference
pose roots; ordered threshold conditions and optional normalized exit gates start
elapsed-time crossfades. Graph elapsed time follows the supplied simulation delta
and scene playback rate; direct player fade duration uses unscaled elapsed seconds
while playing. State transitions finish before another starts. Explicit
player crossfades can be interrupted by capturing the displayed local pose, so a
new destination starts without a pose jump.

The runtime [graph evaluator](../../runtime/src/animation/vkr_animation_graph.h)
validates node references, pose cycles, sample coordinates and capacities before
publication. Each root samples its clips at a shared normalized phase, using an
explicit cycle duration. One-dimensional spaces interpolate neighboring samples;
two-dimensional spaces use triangle barycentric weights and nearest-edge weights
outside their authored triangle domain. Leaf contributions feed normalized local
TRS blending before hierarchy and palette evaluation. Translations and scales
use weighted sums; rotations use hemisphere-corrected normalized quaternion sums.
Joint matrices are never blended.

Scene animation JSON accepts an inline versioned `controller` object. A live scene
can install a copied controller with `vkr_scene_animation_apply_graph()` and change
parameters with `vkr_scene_animation_set_parameter()`. Controller storage belongs
to the scene binding and is reused on replacement, then released at detach. Failed
configuration or pose evaluation preserves the previous published pose. The
underlying player supplies playing/rate controls; authored ECS transforms remain
unchanged. Reverse graph seeking resets and replays using current parameters,
not a recording of past input changes. A single advance/seek may cross at most
128 state boundaries; excessive catch-up and instantaneous transition cycles fail
without publishing partial state. Compensated clocks preserve cycle-boundary
agreement across update rates.

The editor exposes typed nodes, connections, parameters, state/transition settings
and sequence overlaps. Its sequence remains bounded to 16 clip blocks. Managed
workspace settings retain the authoring graph, sequence and layout keyed to the
bank fingerprint, with undo/redo. Applying a graph to the live scene is a session
operation; the scene override Save action does not serialize that controller.
Reusable managed controller assets, event tracks and root-motion authoring remain
future extensions.

The preview is a separate 512-square color/depth target with an orbit camera and
neutral directional material. It renders actual model geometry using the editor
player's GPU deformation; it does not reproduce the model's authored materials or
the main scene's postprocessing. Graph-owned images remain alive through UI
sampling. A reserved UI image reference resolves the current target, and its paint
generation invalidates the preview rectangle while preserving other retained UI.
The preview has no main-view temporal correspondence. Camera framing uses cached
rest-pose asset-space bounds, so it does not change as the clip plays.

## Consequences

Exact source data provides a reference for future GPU pose evaluation and lossy
cooking.
A separate bank preserves static mesh loading. Version 18 meshes carry the same
original-source fingerprint as the bank, and influences survive deduplication and
fetch remapping. Scene integration now retains compatible mesh/bank requests and
independent playback state. Compute output is frame/history-owned; the bank alone
does not provide compatible geometry or a live binding.

## Alternatives considered

- Uniform 60 Hz samples now: simpler indexing, but changes STEP boundaries and
  requires an error policy. Retain exact keys first.
- Store clips inside `.vkb`: couples reusable clip data to each mesh revision.
  Keep clips in a companion bank while v18 owns influences and skin bindings.
- GPU-only evaluation first: lacks a small independent reference for debugging.

## Verification

`./build_test.sh` passes the reference, codec and importer tests with the wrapper's
Debug AddressSanitizer/UndefinedBehaviorSanitizer configuration on macOS. Tests
include analytical interpolation/hierarchy results, independently constructed
binary records, truncation/corruption and sparse/strided/GLB source fixtures.

`VKR_BUILD_TARGET=vkr_animation_cooker ./build.sh Release` builds the cooker. A
private player-source cook produced 1,518,286 bytes with 72 nodes, one skin and all
25 clips. Artifact SHA-256:
`9456ab8dcbdd5a1b808e53e53d4f16a7d04271a1c7f9db087235ad99d190e4dd`.
An independent double-precision source evaluator compared 129 poses across all
clips, including endpoints and opposite-sign quaternion intervals. Maximum global
matrix component difference was `1.19934e-6`, below the `3e-5` check tolerance.
These are CPU and asset checks, not rendering or performance evidence. The local
task note retains exact private-source commands and reports. The Windows wrapper has not been exercised.

Mesh checks pass `./build_test.sh` with analytical deformed-triangle
checks through deduplication and fetch remapping, normalized integer glTF weights,
shared-mesh binding rejection, CRC-repaired malformed extensions and source-variant
preservation. `VKR_BUILD_TARGET=vkr_mesh_cooker ./build.sh Release` builds the mesh
cooker. Cooking an isolated unchanged copy of the player source produced a version
18 mesh of 3,652,128 bytes, with 71,972 vertices, 340,962 indices and three ranges.
Independent parsing verified the normalized influence multiset, all 72 node
bindings, 68-joint skin and animation fingerprint `2f2a93735f2760a1`. This multiset
check establishes retained values; the analytical tests check their geometry
association. These asset checks do not establish GPU output.

Runtime checks pass `./build_test.sh` under Debug ASan/UBSan: elapsed-time
agreement at 30/60/144 updates per second, independent players, reverse/loop/seek,
failed-pose preservation, bank load/unload accounting, asynchronous cancellation,
and synchronous/asynchronous scene attachment and dependency retention. Scene
fixtures use metadata-only synthetic glTF assets and the real resource loaders;
they require no GPU. They verify that sampled poses leave authored transforms
unchanged and that stale bindings release their resources. JSON number tests
also reject malformed playback rates through the shared number reader.
`VKR_BUILD_TARGET=vkr_runtime ./build.sh Release` builds the runtime. These checks
do not establish visible deformation, native Metal/Vulkan animation parity, or
performance.

Compute integration passes the Debug ASan/UBSan CPU suite, including selected-submit
history identity, stale geometry rejection and non-finite preview input rejection.
Both shader inventories compile through the Release wrappers. A local Bistro case
with two independent player instances passes Metal Release snapshots of final
color, normals and motion. The frame-30 motion payload is finite, with 66,456
nonzero pixels and maximum component 0.00205803. This is exercised motion evidence,
not a CPU-reference numeric deformation comparison.

A separate eight-frame, single-process Bistro run with `MTL_DEBUG_LAYER=1`
passes; stderr confirms Metal API Validation Enabled and contains no errors.
Report SHA-256: `3f45c7afa84bda4945c1c81d6915f1367608ba81a4b2985b696cd03a49d5a075`.
The floating editor was exercised with the same Bistro/player fixture: clip
playback, two-clip sequence playback, graph-node dragging and preview orientation.
A separate single-process editor run with Metal API validation exercised preview
playback, seeking and closing; stderr contains only validation initialization.

Native Vulkan execution is unavailable on this macOS host, so the animation
shader contract remains **UNALIGNED**. No performance claim or baseline publication
is made. The local task note retains fixture paths, exact commands and reports.

Blending checks pass `./build_test.sh` under Debug ASan/UBSan. Independent tests
cover weighted TRS and quaternion hemispheres, interrupted crossfades, 1D/2D
weights, phase synchronization, 30/60/144 Hz cycle boundaries, transition exit
and completion times, invalid-graph rollback, JSON parsing, and synchronous and
asynchronous scene-controller ownership. The Bakery publication test also verifies
that inline controllers survive mesh/bank rebuilds unchanged.

The Release Bistro blending snapshot passes with an Idle-to-Blend2 state
transition and an independent 2D blend space. All captured motion components are
finite; frame 30 has 66,541 nonzero motion pixels. Report SHA-256:
`eb9009cb10e3f5ab46690c04f3066cf670141012c91e53e448c8b4bf592d4487`.
Both capture children have empty stderr. Live editor checks exercise Blend2
wiring, a changed weight parameter, Apply to scene, state/transition controls,
cycle-phase scrubbing, and a two-clip sequence with a visible 0.2-second overlap.
These checks establish local integration; native Vulkan parity remains unavailable.

## Revisit when

Reusable managed controller and executable sequence assets need publication and
conflict handling beyond inline controllers and workspace authoring.
Fixed simulation, events and root-motion extraction need explicit time and
application ownership rules. GPU pose evaluation, immutable GPU bind-data caching,
resampling and compression need independent error checks and matched cost evidence.
Authored-material preview and additional rig features need their own rendering
and compatibility validation. These remaining extensions are described in the
[animation proposal](../proposals/compute-animation-and-editor.md).
