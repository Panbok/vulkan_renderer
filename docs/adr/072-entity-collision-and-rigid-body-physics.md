---
status: implemented
updated: 2026-09-13
authority: adr
---

# Entity collision and rigid-body physics

## Status

Accepted. Scene bodies, collider children, simulation, editor authoring and overlay
persistence are integrated. This status records the implementation; native
platform checks, interactive acceptance and matched frame-cost measurements are
separate evidence. No performance or bilateral native success is claimed here.

## Context

The requested first collision milestone includes gravity, impulses and stacking.
The existing ECS already supplies entity hierarchy, source identities and an edit
journal. Rendering bounds and visibility cannot supply collision response or own
simulation state. A later AVBD/destruction implementation should retain authored
entities and editor data without depending on Jolt's internal contact structures.

The [engine research and remaining extensions](../proposals/entity-collision-and-physics.md)
compare Unreal, Godot and Unity authoring and describe the future solver boundary.

## Decision

### Solver and ownership

Use Jolt 5.5.0, pinned at `23dadd0e603f1b321142d4c74df07fce85064989`, through
[the explicit C++17 adapter target](../../cmake/vkr_physics.cmake).
[The C interface](../../runtime/src/physics/vkr_physics.h) exposes VKR value types,
generation-checked handles and fallible operations. Scene, editor and gameplay
callers remain C11. C++ types, exceptions and native Jolt body IDs stay inside
[the adapter](../../runtime/src/physics/vkr_physics.cpp).

A scene lazily creates its physics world when physics is authored. All world
operations are synchronous and serialized, including operations across different
worlds; the adapter uses Jolt's single-threaded job system. Process-wide Jolt
registration survives until the final world is destroyed. The renderer does not
own a solver, bodies or physics allocations.

[The scene owner](../../runtime/src/renderer/systems/vkr_scene_physics.c) retains
up to 1,024 bodies, each with zero to 32 colliders and up to 16 authored joints. Its native world has
2,048 slots so a full batch can stage replacements before commit. Scene records
and prepared transactions use individually freed DMemory storage with a 1 MiB
initial commitment and 64 MiB reserve. Jolt, adapter vectors and fixed native tick
scratch own separate allocations outside VKR allocator-tag accounting. The
runtime reuses a bounded application-owned sensor event drain buffer. Unloading
a scene releases its physics world and records; render resources retain their
existing GPU-completion retirement rules.

### Authored bodies and shapes

The owning entity has Static, Kinematic or Dynamic motion, total mass, gravity
factor, damping, friction, restitution, sleep and CCD settings. Solid/Sensor role,
16-bit collision membership and a 16-bit collision mask belong to the whole body.
A pair participates only when each body's membership intersects the other's mask.
Sensors use Static or Kinematic motion and produce no physical response.

Each collider is an ordinary direct child entity with an owner reference and a
nonzero authored ID unique within that owner. Its snapshot stores enabled state,
Box/Sphere/Capsule/Convex hull/Triangle mesh kind, offset, rotation, positive scale
and dimensions or a cooked asset path. Boxes use half-extents;
capsules use a radius and Y-axis cylinder half-height. Enabled shapes form one
compound. Zero enabled shapes leave an authored body suspended without a native
body. Render visibility does not change collision participation.

Body owners may be nested beneath other entities. Their composed world transform
must decompose into finite translation, a proper rotation and positive scale;
reflections and shear are rejected. Static and Kinematic bodies follow evaluated
parent transforms. Dynamic bodies keep their simulated world pose when a parent
moves. Authored local TRS remains available for Reset. Box, convex and triangle
shapes support nonuniform scale; sphere and capsule shapes require uniform scale.
Composing rotated colliders with nonuniform owner scale must also remain free of
shear. Changes that require collision rebuilding are authored while paused.
Physics uses meters, kilograms, seconds and negative-Y gravity. Existing scenes
acquire no implicit collision bodies.

[Collision cooking](../../tools/assets/vkr_collision_import.c) accepts static glTF
or GLB triangle geometry, either the default scene or a selected node subtree.
It bakes instance transforms and corrects reflected triangle winding. Skinned,
morphed and compressed source geometry is rejected; use an explicit static proxy.
Convex cooking builds one enclosing hull with at most 256 vertices and rejects
input requiring more vertices instead of silently simplifying it. Concave dynamic
objects need multiple convex colliders. Triangle meshes are Static/Kinematic,
with counterclockwise front faces and one-sided simulation contact.

[The VKC1 asset format](../../runtime/src/assets/vkr_collision_cooked.h) stores
explicit little-endian positions, outward triangles, source fingerprint and
checksum. It is solver-neutral and capped at 64 MiB, 1,048,576 vertices and
1,048,576 triangles. Hulls also require a closed outward convex surface with
positive volume. The reader validates sizes, finite coordinates, indices,
degenerate triangles and hull topology. Malformed input does not publish an asset.
[Immutable CPU assets](../../runtime/src/physics/vkr_collision_asset.c) own a
private arena and reference count; scene bodies retain them until replacement,
discard or shutdown. Reapplying the same path retains the loaded bytes. A new
scene load reads newly cooked bytes. Paths are workspace-relative for managed
projects and repository-relative for legacy scenes; explicit absolute paths work
locally. Managed scene import copies collision dependencies into the destination
bundle and rewrites paths, including attachment and joint source fingerprints.

Jolt computes compound center of mass and inertia from the shapes and scales
inertia to the authored total mass. Adapter pose and impulse operations preserve
the body's authoring origin across center-of-mass conversion. Friction combines
with the geometric mean, restitution with the maximum, and restitution has a
1 m/s bounce threshold. CCD selects Jolt's linear-cast motion quality for
supported solid bodies; it does not make discrete sensors continuous.

### Simulation and publication

Simulation runs at 60 Hz, with at most eight ticks per scene update. Excess elapsed
time remains as debt; overflow fails and pauses the simulation. After 60
consecutive updates that still owe at least one tick after the eight-tick cap,
the scene pauses with an overload reason and retains all debt. Resume resets the
consecutive-overload counter and clears the error unless the native world is
faulted. Completed ticks advance the public physics clock. Rendering interpolates previous/current poses
with `min(debt / fixed_dt, 1)` while playing and uses the exact current pose when
paused, stepping or globally disabled. Authored position, rotation and scale remain
unchanged; only evaluated world transforms flow into the existing scene render
mirror. Save therefore preserves authored transforms after bodies have moved.

With physics bodies, animation samples before every completed fixed tick so
Kinematic bone attachments follow the corresponding pose. Scene animation uses
asset-space global bone transforms, before inverse-bind multiplication. Dynamic
drive-bone attachments publish solved global transforms through the existing CPU
pose and skin-palette path. The renderer retains its existing deformation buffers,
shader layouts and GPU-completion rules. Scenes without bodies use the existing
elapsed animation path. Root-motion extraction remains separate.

Bone attachments resolve an animation wrapper by source identity and a node by
source index. Their explicit local offset places the body relative to the bone.
Fixed, hinge, distance and swing/twist joints connect body-local frames, with
limits and stable authored IDs. Connected bodies do not collide by default.
Missing target bodies leave authored links suspended and visibly labeled in the
Inspector; restoring the target allows the next staged graph to recreate them.
Self-joints are invalid.
The ragdoll action fits capsules to an imported skin's existing node entities,
adds parent swing/twist links and publishes the batch as one edit. Leaf bones use
small spheres. Disable returns the bodies to Kinematic animation without blending;
Enable starts Dynamic drive from the current evaluated pose. Seeking changes the
sampled animation, while active Dynamic bones retain their solved pose until
Reset. Generated
sizes and limits are starting values to inspect and tune. It does not create a
new anonymous skeleton or implement an active animation/physics blend controller.

Pause stops stepping; Step requires pause. Manual pause and session mutes retain
elapsed-time debt. Reset rebuilds the native world from authored state, clears
physics clocks/debt and seeks every scene animation player/controller to time zero
through the existing scene animation API. Reset stages native bodies/joints and
checkpoints animation playback, crossfade and graph state; failure restores the
previous pose slots and clocks. A successful Reset clears body/global session
mutes while retaining authored enable flags. These mutes are excluded from overlays. A kinematic-target
API supplies simulation targets separately from authored poses. Impulses can target
the center of mass or an explicit world point. Physics failure preserves the last
completed published pose; a faulted native world rejects further steps and queries
until Reset.

Rays, sphere overlaps and translational shape sweeps return body/entity and
collider identities. Sweeps accept box, sphere, capsule or convex hull query
shapes, with a fixed orientation during the cast. Filters select membership,
sensor inclusion and up to 32 ignored owner IDs for rays and sweeps; sphere
overlaps expose membership-mask filtering. These queries do not implement a
character controller or a rotational weapon sweep.

Sensor tracking emits buffered begin/end membership, including sleeping occupants.
Solid contact tracking emits BEGIN/PERSIST/END with body/collider IDs and contact
position/normal; PERSIST continues for sleeping contacts. Scene callbacks run after
each completed tick outside Jolt locks. Events are borrowed for the callback only;
read-only queries are allowed and mutation/reentrant drains are rejected. Queue
gameplay changes for the next update boundary. With no callback, scene contact
events are drained automatically. Event/pair exhaustion faults the world rather
than dropping events. Structural preparation reserves terminal-event capacity.

[Scene collision settings](../../runtime/src/renderer/systems/vkr_scene_collision_layers.h)
name 16 membership bits and up to 16 presets, and store a symmetric 16-by-16 matrix.
A body's effective mask is its requested mask intersected with the union of matrix
rows for its membership bits; both bodies must permit the pair. Presets copy
membership, mask and Sensor role into the Inspector draft. Renaming/editing a
preset does not retroactively rewrite bodies that previously used it.

### Editor transactions and persistence

[Inspector authoring](../../editor/src/editor_scene_panels.c) uses local drafts.
Apply submits a complete owning-body snapshot to the runtime; UI construction
borrows scene data and does not mutate ECS. Add, duplicate, remove, enable,
placement, scale, cooked asset, attachment and joint settings share this transaction. Fit uses loaded descendant render
bounds conservatively, changes only the chosen collider draft and resets its local
rotation. It is an approximation requiring Apply, not an assertion of mesh fit.
Selecting a collider child opens an owner-selection action; viewport move/rotate/
resize edits the same owning snapshot, records one journal entry on release and
restores the initial snapshot on Escape. Fallible rebuild and restore errors are
reported. Body transforms and collider authoring require paused physics.

[The edit journal](../../runtime/src/renderer/systems/vkr_scene_edit.c) stages
physics allocations with its existing name/value preparation. A batch validates
schema, source identities, fingerprints and all records before committing any
record. Commit cannot fail; discard releases provisional bodies and children.
Existing live collider handles remain stable when their authored IDs match;
undo/recreation preserves authored IDs without promising identical ECS generations.

Overlay version 3 adds scene collision settings and a version 2 physics object
with scale, cooked paths, attachments and joints within each source-node record.
Owner source IDs and source fingerprints continue to detect reimport conflicts.
Collider IDs are hexadecimal strings to preserve all 64 bits. Legacy version 1/2 overlays and physics version 1 remain readable; old colliders
receive unit scale. Runtime handles, velocities, evaluated
poses, session mutes and contacts are never serialized. Managed scene publication
continues through the existing immutable overlay-revision owner. The journal
allocates exact payload storage for each live entry and frees redo/evicted entries.
Ragdoll and matrix changes stage the complete affected body/joint graph; failed
preparation retains the old world and saved revision. The movable Physics window
contains layer names, the matrix and presets, with Apply/Revert/Undo/Redo/Save.

[The editor collision overlay](../../editor/src/editor_physics.c) projects line
segments on the CPU into the existing UI stream, capped at 512 lines and remaining
UI node capacity. Display supports off, selected-body and all-body modes. It adds
no shader or native render-packet contract. Collider ray selection respects gizmo
priority; muted/disabled bodies remain selectable in the hierarchy. Debug drawing
is an authoring aid and is separate from native collision queries.

## Consequences

The implementation supplies rigid-body response while keeping authoring and
saved identities in VKR. The dependency adds C++ build and adapter maintenance.
Positive scale, decomposable transforms and bounded compound sizes constrain
authoring. Conservative bounds fitting can substantially overestimate geometry.
Projected debug lines are capped and do not establish depth-tested native debug
rendering. Allocator tags do not measure the complete physics memory footprint.

Replacing Jolt may retain these authored contracts, but its broad/narrow phase,
contacts, CCD and solver are not separate VKR-owned plugins. An AVBD implementation
must provide equivalent queries, event meaning and pose publication or explicitly
revise them. Fracture topology, mass transfer and two-way solver coupling
require their own design and evidence.

## Validation boundary

On 2026-09-13, `./build_editor.sh Release` and `./build_test.sh` passed on
Apple M1 Pro/macOS. The Debug CPU suite uses ASan/UBSan for VKR code; vendor Jolt
code follows the repository's uninstrumented vendor policy. Tests cover cooked
format corruption and lifetime, reflected source import and hull enclosure,
scaled mesh/hull response, sweeps/filters, contact lifecycle/capacity, all four
joint types, hierarchy/shear rules, callback mutation rejection, ragdoll skin
palettes and failed-Reset playback restoration. Scene editing tests exercise
matrix/batch Undo, legacy overlays, suspended links and failed asset acquisition.
Native-cooker project checks cover dependency relocation, detached scene cloning,
nested source fingerprints and corrupt-asset rollback.

With graphics validation variables unset, `vkr_harness autotest --case
tools/cases/local/physics_bistro.case.json --profile
tools/profiles/local-offscreen.json` produced passing primary and snapshot reports.
Both repetitions verified three cooked convex bodies stacked on a cooked
triangle-mesh platform, with scaled parents, matching evaluated render poses and
solid BEGIN/PERSIST delivery. The combined command returned `missing_baseline` (4); image comparison
was not run and no baseline was published. Native paneled editor checks exercised
collider selection/movement, impulses/Step/Reset, and the new layer-name,
symmetric-matrix, Undo/Redo, Save and reload paths. Native Vulkan/Windows,
skinned-ragdoll renderer captures, cross-backend image comparison and matched
performance evidence remain unavailable from this local check.

## Alternatives considered

- Custom collision plus a conventional solver would also require contact
  persistence, friction, stacking, sleep and CCD before delivering the requested
  milestone.
- AVBD immediately would make solver/destruction research part of this milestone;
  it remains a future prototype rather than a second interacting simulation world.
- Colliders embedded only in the body would avoid child entities but lose direct
  hierarchy selection and existing transform tooling.
- A generic solver registry would add an abstraction without a second implemented
  solver; the current C boundary carries only operations used by VKR.

## Revisit when

Revisit dynamic concave geometry, deforming collision, rotational sweeps, active
ragdoll blending, event/scene capacities and memory accounting against concrete
workloads. Revisit solver ownership before AVBD, fracture or two-way interaction
between different solvers. Native editor acceptance and matched Release
measurements must establish their own claims.
