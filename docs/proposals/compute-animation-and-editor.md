---
status: proposed
updated: 2026-09-13
authority: proposal
---

# Animation graphs, baking and GPU pose evaluation

## Remaining scope

[ADR-071](../adr/071-animation-bank-and-reference-pose.md) owns the implemented
exact-key animation bank, CPU playback, managed publication, compute skinning,
submitted deformation history, and movable in-editor animation window. The window
already provides an independent live preview, weighted blend graphs, 1D/2D blend
spaces, conditional state transitions and sequential replay with crossfades. This proposal retains the research and extensions beyond
that implementation.

The next work is reusable managed graph/sequence assets,
events and root-motion policy, an optional fixed 60 Hz simulation clock, and
error-controlled baking. GPU pose evaluation is optional and needs a measured
comparison with CPU evaluation feeding the existing compute skinning path.
Rendering remains independent of animation simulation cadence.

Retargeting, IK authoring, procedural physics, cloth, motion matching, detached
native windows, and a general cinematic editor remain outside this scope. Morph
animation and additional influence sets require explicit import, cooking and
rendering contracts before assets relying on them can be accepted.

## Current VKR baseline

| Area | Implemented boundary |
|---|---|
| Assets | Static v17 and skinned v18 meshes retain source identity; `.vka` banks preserve exact TRS keys, node closure and skins. Managed import/rebuild/reimport publishes compatible mesh/bank revisions. Morph animation and more than four influences remain unsupported. |
| Playback and scene | Independent CPU players and inline graph controllers use seconds, local-TRS crossfades, blend spaces and conditional states while preserving authored ECS transforms. Frame extraction supplies current palettes, per-instance bindings and conservative bounds. |
| Rendering | Both native implementations contain compute skinning and deformation consumers for visibility, shadows, picking, material reconstruction, blend and motion. Previous deformation comes from compatible submitted history. Native Metal has an animated Bistro snapshot; native Vulkan comparison is unavailable, so the shader contract remains **UNALIGNED**. |
| Editor | A movable window has typed blend nodes, parameter/state editing, clip transport and a 16-block sequence with crossfades and undo/redo. Workspace settings preserve authoring; Apply installs a copied controller in the live scene. Managed controller assets and scene-override serialization remain future work. |
| Preview | An independent player and orbit camera render actual deformed geometry into a 512-square graph target consumed by UI. The initial neutral directional material omits authored materials and main-scene postprocessing. |
| Scheduling | The existing graph orders skinning, consumers and UI on the graphics submission path. There is no asynchronous compute queue. |

See [ADR-030](../adr/030-offline-mesh-optimization-and-cooking.md),
[ADR-031](../adr/031-versioned-packed-static-geometry-abi.md),
[ADR-071](../adr/071-animation-bank-and-reference-pose.md), and the
[shader contract](../adr/044-shader-cross-backend-contract.md) for current formats,
ownership and evidence. Broader panel/accessibility work remains in the
[UI extensions proposal](editor-ui-extensions.md).

### Inspected player

Read-only source metadata inspection found project **FPS Test**, scene **Testbed**,
and model **humanoid_player_grey**, with skin `SK_Vector_Humanoid`.
Its managed mesh asset ID is `ecaf71f4-d858-4e98-b6ce-007a3046a92f`, and its
source import ID is `cad9886c-6407-4e68-8512-44b742fba39f`.
The source JSON SHA-256 is
`ff0c0eb93ed427c79d8da634c642852ed9f0542d1d62a4d5faff581d5983a17d`.
This digest identifies the inspected JSON, not the complete asset dependency set.

| Metadata | Observed value |
|---|---|
| Hierarchy | 72 nodes; one skin with 68 joints; inverse-bind accessor with 68 matrices; optional `skin.skeleton` omitted |
| Geometry | Three skinned primitives/mesh nodes, three materials; 71,972 source vertices before VKR cooking |
| Influences | `JOINTS_0`: unsigned-byte VEC4; `WEIGHTS_0`: float32 VEC4; no additional influence set in this source |
| Animation | 25 clips, each with 204 TRS channels addressing 68 nodes; STEP and LINEAR samplers |
| Timing | Declared `sample_rate: 60`; durations about 0.2333–2.8 seconds; dense tracks and two-key tracks coexist |
| Deformation | No morph targets; names include `Walk_Forward_RootMotion` and `Run_Forward_RootMotion` |

The rig root and the three mesh nodes are separate scene roots; do not infer
skeleton ownership from the mesh's parent. The importer must retain intervening
non-joint nodes. Clip names do not establish the actual root-motion curve.
The subsequent binary audit verified finite four-weight influences (maximum sum
error `1.37e-7`), rest skin displacement below `1.61e-6` metres, scales down to
about `0.0001`, and 30 opposite-sign adjacent quaternion pairs. The two root-motion
clips move node 67 along negative Z by about 1.30 and 2.437 metres respectively.
ADR-071 records source/cooked pose checks and local Metal rendering evidence.
Bilateral numeric GPU comparisons remain unavailable.

The source inspection used the user's Testbed assets. Scene-based renderer
acceptance uses a Bistro case containing this character; no such animation case
was executed or created for this documentation task.

## What the other engines do

Sources were checked on 2026-09-13. Godot references are pinned to 4.5 and
`4.5-stable`; Unity references describe 6000.0, with Timeline 1.8 documentation;
Epic's pages currently identify Unreal Engine 5.8. Epic's unversioned URLs may
later serve a different release. These are architectural examples, not VKR
performance measurements or evidence of identical glTF import behavior.

| Engine | Time and pose evaluation | GPU deformation | Import and authoring |
|---|---|---|---|
| Godot | Physics, process-frame, or manual animation advancement. CPU animation mixing samples and blends tracks. | The RenderingDevice renderer has a skeleton compute path. This does not put `AnimationTree` on the GPU or describe every Godot renderer. | Import can bake at a chosen rate. `AnimationPlayer` stores clips; `AnimationTree` supplies blending/state machines. |
| Unreal | Animation update receives elapsed time. Animation Blueprint evaluation uses game/worker-thread work, with update-rate and budget controls. | Skin Cache computes positions, normals, and tangents into vertex buffers; vertex-shader skinning also exists. | Sampled sequences and compression settings are separate from runtime update cadence. Animation Blueprint, Sequence, and Montage/Sequencer editors serve different tasks. |
| Unity | `Animator` offers Normal, Fixed, and UnscaledTime. CPU animation jobs evaluate states, clips, blends, events, root motion, and transforms. | Mesh deformation can use CPU, GPU compute, or batched GPU compute. | Animator state machines/blend trees control behavior; Timeline arranges clips. Model import has resampling and compression controls. |

Godot's [AnimationMixer](https://docs.godotengine.org/en/4.5/classes/class_animationmixer.html)
defines the three advancement modes. Its
[CPU mixer](https://github.com/godotengine/godot/blob/4.5-stable/scene/animation/animation_mixer.cpp)
and [skeleton shader](https://github.com/godotengine/godot/blob/4.5-stable/servers/rendering/renderer_rd/shaders/skeleton.glsl)
show the division between pose processing and GPU mesh deformation.
The [scene importer](https://docs.godotengine.org/en/4.5/classes/class_resourceimporterscene.html)
defaults animation baking to 30 samples/second, with interpolation between samples;
higher bake rates trade storage for fidelity during rapid changes.
[AnimationTree](https://docs.godotengine.org/en/4.5/tutorials/animation/animation_tree.html)
and the [animation editor](https://docs.godotengine.org/en/4.5/tutorials/animation/introduction.html)
separate graph control from clip tracks and playback.

Unreal's [update API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UAnimInstance/NativeUpdateAnimation)
and [optimization guidance](https://dev.epicgames.com/documentation/unreal-engine/animation-optimization-in-unreal-engine)
describe elapsed-time updates, parallel work, and reduced update rates. Root motion
needed by character movement can constrain parallel updates. Its
[Skin Cache](https://dev.epicgames.com/documentation/unreal-engine/skeletal-mesh-rendering-paths-in-unreal-engine)
has a memory budget and can fall back to vertex skinning; that fallback is an
Unreal mechanism, not an existing VKR path. Optional tangent recomputation adds
passes. [ACL compression](https://dev.epicgames.com/documentation/unreal-engine/animation-compression-library-in-unreal-engine)
provides error and mesh-aware controls, distinct from presentation FPS.

Unity's [update modes](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/AnimatorUpdateMode.html)
make Fixed mean `FixedUpdate`, not an intrinsic 60 Hz cap. Its
[animation profiler markers](https://docs.unity3d.com/6000.0/Documentation/Manual/profiler-markers.html)
document CPU evaluation stages, while
[mesh deformation settings](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/PlayerSettings-meshDeformation.html)
document the separate compute choices. The
[compression API](https://docs.unity3d.com/6000.0/Documentation/ScriptReference/ModelImporterAnimationCompression.html)
and [resampling API](https://docs.unity3d.com/cn/6000.0/ScriptReference/ModelImporter-resampleCurves.html)
describe model-import controls, primarily for its model/FBX pipeline; a glTF
importer must be checked separately. The
[Animator window](https://docs.unity3d.com/6000.0/Documentation/Manual/AnimatorWindow.html)
edits behavior graphs. Timeline supports
[inserting sequential clips](https://docs.unity3d.com/Packages/com.unity.timeline@1.8/manual/clip-insert.html)
and [overlap blends](https://docs.unity3d.com/Packages/com.unity.timeline@1.8/manual/clip-blend.html).

The inference for VKR is to separate control, pose evaluation, deformation, and
authoring. Compute skinning is well supported by these examples. Putting all
gameplay decisions on the GPU is not a necessary consequence.

## Playback time and the 60 Hz option

Seconds-based playback is implemented. The fixed simulation mode, event traversal
and root-motion policies below remain proposed; the editor’s 1/60-second step
buttons do not implement them.

| Control | Proposed default | Meaning |
|---|---|---|
| Playback clock | Seconds, float64 on CPU | Clip position, speed, pause, loop count, and sequence placement |
| Simulation cadence | Variable by default; explicit fixed 60 Hz mode | When parameters, transitions, gameplay events, and root-motion deltas advance |
| Pose presented | Evaluated/interpolated for the requested presentation time | Visible motion at 30, 60, 120, 144 Hz or irregular rendering |
| Cooked sample rate | Exact authored keys today; configurable only with a future resampling recipe | Storage fidelity, independent of rendering |
| Timeline display | 60 fps ruler initially; seconds available | Snapping and frame-step UI, with no change to clip duration |

For variable playback, accumulate the selected clock's delta multiplied by play
speed. Keep an unwrapped time for event traversal and derive a bounded clip-local
time for GPU sampling. A two-second clip lasts two seconds at every render rate.
Represent a loop as `[0, duration)` during playback, with an explicit final-sample
inspection position when paused. Preserve the final interval for clips whose
duration is not an integer multiple of the cook rate; do not extend their duration
to the next sample tick. Single-key/static channels need no division by duration.

For fixed simulation, use an accumulator with `h = 1 / 60`. Advance zero or more
simulation ticks per rendered frame, then present between the previous and current
simulation snapshots with `alpha = remaining_time / h`. This deliberately adds up
to one simulation tick of visual latency. Interpolate local TRS and actor movement
consistently; do not interpolate joint matrices or mix poses from unrelated graph
states. Smooth clip segments can instead be sampled directly at that delayed
presentation time. A transition snapshot must retain the clip times and blend
weights needed to reproduce its pose.

The [application host](../../runtime/src/application/vkr_application_host.c)
currently offers a fixed delta override per host callback and clamps delivered
delta to 0.1 seconds. That is not an accumulator-driven 60 Hz simulation loop.
Implement the accumulator in the runtime clock owner. Preview playback should
use an explicit monotonic, unscaled clock with pause/resume anchoring; gameplay
uses the simulation clock. After suspension, resume from the paused time rather
than accumulating an accidental jump.

Bound simulation catch-up work per callback. Retain remaining simulation debt and
report overload; never silently skip gameplay events to meet a pose budget. A
real-time preview can seek to its current wall-clock position without executing
gameplay side effects. Manual export/replay advances a supplied clock. Fixed
ticks improve repeatability of control decisions, but GPU floating-point pose
evaluation is not a cross-platform bitwise lockstep guarantee.

Events traverse the unwrapped interval `(old_time, new_time]`, including crossed
loops and clips; the reverse interval has a separately defined ordering. Do not
test only whether the current frame equals an event frame. Scrubbing suppresses
gameplay callbacks by default and displays crossed markers in the editor. Capture
event IDs and loop indices to prevent duplicates after pause, transition, or seek.
During blends, the graph designates one event source per event category, with
explicit handoff at a transition boundary. The default locomotion policy follows
the dominant clip, resolving equal weights by stable node ID. Report suppressed
markers in preview; two blended clips must not both fire the same footstep action.

Hard-locking both animation and rendering to 60 would simplify one playback mode,
but would impose display pacing and still leave drops, scrubbing, and interpolation
to solve. Recommend offering a **60 Hz simulation mode**, not making a 60 FPS cap
part of the animation asset contract.

## Runtime graph and sequence assets

The runtime graph already has versioned inline JSON, bounded evaluation,
parameters and transitions. Extend workspace authoring and the playlist into
managed assets with publication, stable IDs, rig fingerprints and conflict
handling. Masks and additional parameter types need separate contracts. Sequence assets
need clip/graph references, placements, trims, rates, loops, crossfades and markers.
Scene animators then select those assets rather than an editor workspace document.

Keep hierarchy/rest-transform and skin-binding compatibility checks. A changed
rig must report an authored-document conflict rather than silently remapping by
bone name. Graph and sequence compilation should reject missing pins, incompatible
rigs, recursive references and excessive capacity before publication. Pose-data
cycles are invalid; state-machine transitions may form cycles.

Managed mesh/bank cooking and atomic publication already exist. Extend Bakery
with explicit resampling/compression recipes, error limits, selected clips,
influence-reduction policy and root extraction only as those features ship.
Recipe settings participate in dependency identity, and failure preserves the
last published revision. Runtime compilation must stay outside render recording.

The current four-influence path remains the reference. An eight-influence
extension needs matching cooker remapping, shader layout and native validation;
excess influences must never be silently truncated. glTF channel semantics remain
those of the [glTF specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#animations).

## Error-controlled baking

Keep the exact-key bank as the reference. A future bake recipe may produce a
GPU-friendly local-pose representation; it should not bake all animated vertices. Default
to constant tracks plus uniformly sampled TRS for smooth tracks. Retain STEP
change times exactly and avoid interpolation across their discontinuities. Sampling
locally preserves hierarchical blending and permits arbitrary instance times.
An indexed track table distinguishes constant, sampled, and step data without
runtime glTF parsing or a per-frame CPU decompression stage.

First implement float32 samples and establish an oracle. Then evaluate quaternion
packing, translation/scale quantization, constant-track removal, and lower sample
rates against that oracle. A 60 Hz input is not automatically faithful after a
30 Hz recook; resampling above the source rate cannot invent lost motion detail.
Compression settings belong to the clip recipe and dependency hash.

Proposed initial error targets are 1 mm maximum skinned-position error on this
meter-scale player and 0.1 degree joint-angle error, with separate checks for feet,
hands, and extracted root displacement. They are proposed acceptance targets, not
existing VKR budgets. Measure source-vs-cooked poses at original keys, between keys,
near discontinuities, and adaptive probes around extrema. Sampling tests establish
reported error over tested times; do not call them a proof of conservative bounds.
Store worst clip/time/joint/vertex, max/RMS error, raw/cooked bytes, and recipe.
Raise bake rate or retain an exact track when error fails; never accept silently.

The inspected clips total 1,743 inclusive-endpoint 60 Hz sample frames. A naïve
68-joint, 40-byte TRS representation would occupy 4,740,960 bytes; 48-byte alignment
would occupy 5,689,152 bytes. These are calculated upper-shape examples before
constant removal, metadata, or compression, not generated artifact sizes. Vertex
animation textures would scale with roughly 72,000 vertices rather than 68 joints
and would constrain graph blending; defer them to an independently justified
fixed-motion crowd use case.

## Root motion and gameplay control

CPU control remains authoritative for state transitions, sequence placement,
events and application-required joints. Do not require GPU readback to move a
character or deliver an event. Root extraction needs an explicit source node,
axis/yaw convention and modes: in-place, extract only, and apply through an
application movement callback. Define how accepted controller movement and the
rendered pose avoid applying displacement twice.

Bake or evaluate root translation/orientation curves with loop displacement.
Blend root deltas through transitions and integrate across boundaries. Without a
movement callback, report the delta while leaving actor movement with the
application. Attachment/collision queries may evaluate a CPU subset with ancestor
closure; densely queried rigs may remain fully CPU evaluated.

## Optional GPU pose evaluation

CPU pose evaluation and 64-thread compute skinning already ship. The operations
below are proposed additions; they must justify their cost against that baseline.
Current conservative CPU influence bounds remain valid until a replacement passes
the same culling and shadow invariants.

1. `Animation.SampleBlend`: sample active clips and evaluate local-space pose
   operations. Begin with Clip, Blend2, a bounded state-machine transition,
   Sequence, and Output. Translations/scales blend linearly; rotations use the
   documented quaternion rule. Missing channels use rest values. Additive layers
   later require an explicit reference pose and multiplication order.
2. `Animation.Hierarchy`: form asset-space globals and skin palettes after local
   blending. The imported closure includes non-joint parents. For this small rig,
   prototype one workgroup per pose, 128 lanes, with strided work for larger
   supported closures and uniform barriers between hierarchy levels. All lanes
   reach every barrier, including lanes without a joint. Validate a bounded
   closure capacity, initially 256 nodes; unsupported rigs fail cooking clearly.
   A child cannot read another workgroup's unfinished parent through an ordinary
   workgroup barrier. Larger-rig scheduling is a later measured extension.
3. `Animation.Bounds`: reduce current positions per submesh/instance before GPU
   culling. A reduction needs ordered partial/final work or an equivalent proven
   construction. Do not let independent workgroups race a shared AABB reset.

Compile pose graphs into a typed, topologically ordered, bounded program and
precompute scratch liveness at publication. State-machine cycles are legal;
cycles in pose-data dependencies are not. Reject incompatible skeletons, missing
pins, or excessive operation/scratch counts before publication. The first GPU
sampler can specialize common one/two-clip programs; a generic per-joint program
must have uniform bounded traversal and measured register/scratch cost. Graph
nodes do not compile arbitrary user-authored shader code.

With two active full TRS clips and 68 joints, sampling conceptually reads up to
four TRS samples per joint, then writes one local result. At 40 bytes per sample
that is about 10.6 KiB of nominal sample data per pose before caching/constants.
Hierarchy costs scale with the retained node closure and depth, not vertices.
Skinning reads one bind vertex, an influence row, and four/eight palette entries
per vertex, writes one current vertex, and supplies history position storage.
Bounds add position reads and partial reductions. Record actual strides, launched
lanes, active clip counts, bytes, and temporary capacity before optimization.
These counts are not GPU timings or cache-miss estimates.

## Editor extensions

The existing movable window remains the owner of preview and transport. Its
Blend2, blend spaces and timed state transitions use scalar parameters. Add
Boolean/trigger inputs, masks and additional transition policies as extensions.
Preserve keyboard alternatives, undo/redo and visible validation errors. Runtime
asset editing needs explicit save/dirty state, versioned documents and conflict
handling; automatic workspace persistence alone does not provide that contract.

The ordered sequence already crossfades consecutive clips with bounded overlap.
Extend it with move/trim, source offsets and per-block rate/loops while retaining
at most two simultaneous base clips. Gaps output rest pose by
default, with an explicit hold policy. Add markers and parameter tracks with
stable equal-time ordering. Cross-rig placement fails visibly.

Exactly one Clip, Graph or Sequence controller drives preview at a time. Stateful
graph scrubbing resets and replays recorded parameters, optionally from bounded
checkpoints. It cannot infer past transitions from current parameter values.
Current graph seeking explicitly replays current parameters; recorded-input
seeking needs a new history owner. Direct clip sequences use time queries.
Recursive graph/sequence references
remain invalid.

Preview extensions include authored materials, a floor/grid, pan/frame controls,
bones/bounds/root trails, source-versus-cooked comparison, and import readiness
inspection before a scene binding exists. Preserve independent camera, clock and
GPU lifetime. Full-material or postprocessed preview needs its own declared
rendering cost; the present fixed target is not a general second scene viewport.

## Delivery order

| Extension | Result and acceptance gate |
|---|---|
| Runtime graphs and sequences | Versioned executable assets with blends, transitions and compatible rig references. A saved Idle → Walk → Jump → Land composition loads into a scene animator and agrees with preview. |
| Events, root motion and fixed ticks | Explicit traversal/controller rules and optional accumulator-driven 60 Hz mode. Common-time poses, event order and displacement agree across display rates and overload cases. |
| Authoring and preview tools | Trims/crossfades, parameters, runtime-asset save/reload, authored materials and diagnostic views. Test undo, conflicts, focus, resizing and independent main/preview state. |
| GPU pose evaluation | Sample/blend/hierarchy kernels preserve CPU-required joints and match an independent reference on both native backends. Adopt only with an acceptable measured cost/memory tradeoff. |
| Baking and scale | Error-controlled compression/resampling, immutable GPU bind-data caching and batching. Preserve discontinuities, timing, deformation quality and retirement under matched Release measurements. |

## Acceptance for the remaining work

| Invariant | Cheapest useful evidence |
|---|---|
| Import/math correctness | Independent CPU fixtures for non-joint ancestors, missing inverse binds, transformed mesh nodes, remap with distinct weights, uneven keys, antipodal linear quaternions, cubic tangents, STEP boundaries, and partial-channel defaults. Analytically known transforms provide the oracle. |
| Playback independence | Replay the same timestamped inputs at 30/60/120/144 Hz and irregular deltas; compare poses at common times, sequence duration, root displacement, and event IDs/order. Fixed mode also checks zero/multiple ticks and overload debt. |
| Skinning/parity | Small synthetic isolated arithmetic cases plus the player in Bistro. Compare GPU positions, normals, palettes, and bounds to the CPU oracle; inspect both native outputs under the same declared tolerances. |
| Render integration | Bistro character visible, outside camera but casting shadows, moving against static geometry, masked/blended materials, picking, and stationary actor with moving limbs. Capture motion/validity/depth and inspect temporal consumers, including SSR rigid-transport rejection. |
| History/lifetime | Pause/seek/reverse/loop, spawn/despawn, differing instance times, skipped render, graph reload, asset replacement, resize, and closing preview while work is in flight. Validate producer identity, generations, live bytes and handles after retirement drains. |
| Editor behavior | Mouse and keyboard graph edits, two-clip overlaps, invalid third overlap, frame stepping, scrub event suppression, root-motion display, undo/redo, save/reopen, DPI resize, focus loss, main-camera isolation, and preview/main color agreement under matched SDR/HDR settings. |
| Cost | Capture-free normal Release with validation variables unset; matched Bistro camera/output/settings and 1, 16, and 64 player instances as capacity permits. Compare CPU pose + compute skinning with GPU pose + compute skinning at equal output; include static-scene and preview-open/closed cost. |

Record CPU control/pose/extraction time, GPU sample/hierarchy/skin/bounds time,
vertices and joints processed, active clips, dispatch counts, upload bytes,
live/retired/high-water allocation, and frame sample distribution. Use valid
samples, spread, exact configuration, report digests, and the repository's
authority policy. A 60 Hz frame is 16.67 ms for the whole application, not an
animation allocation; propose the animation budget only after the baseline is
measured. A 68-joint player may be cheaper with CPU pose evaluation even when
compute skinning is beneficial.

Use repository build wrappers and the
[harness workflow](../../.codex/skills/vkr-harness/SKILL.md) for animated Bistro
cases. Native diagnostics follow
[validation policy](../../.codex/skills/vkr-validation/SKILL.md), with one minimal
Metal process at a time and no broad shader-validation capture suite. Compile,
reflection, CPU tests, native execution, and timing establish different claims.
Do not label Metal/Vulkan parity complete until the
[shader contract](../adr/044-shader-cross-backend-contract.md)'s applicable gates
pass; unavailable native evidence remains explicit. ADR-071 owns the implementation
and its verification record. A native Metal animated Bistro snapshot now exists;
native Vulkan comparison, GPU-versus-reference numerical checks, and matched
performance measurements remain outstanding. No performance baseline is published.
