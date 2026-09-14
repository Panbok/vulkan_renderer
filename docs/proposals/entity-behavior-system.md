---
status: proposed
updated: 2026-09-14
authority: proposal
---

# Code-first entity behavior and visual authoring

## Recommendation

Build gameplay on VKR's existing ECS: typed component state, native C systems,
explicit actions, and one ordered simulation schedule. Make attaching a behavior
an authoring operation that adds its required data and bindings to an entity.
Run repeated work in batches of compatible entities. Use entity-specific C
handlers for sparse interactions when that is clearer.

Use prefabs to describe reusable gameplay compositions and their lifecycle.
A Player prefab assembles the character root, motor, model and equipment
bindings; a Weapon prefab assembles weapon state, model and muzzle binding.
An instance may be called an actor in gameplay discussion, but the pilot needs
no separate actor registry, identity type or mandatory per-actor update.
Existing entities remain the runtime identities and component owners.

Add visual assistance in this order: a component Inspector, live state and event
inspection, connection editing, then small state charts that invoke registered C
actions. Keep arbitrary algorithms in C. A visual graph can describe when to
request an action without owning the implementation of shooting, movement,
collision, or resource lifetime.

Use statically linked C for the first playable slice and shipping builds. Add
development-only native module reload after state ownership and schemas are
stable. Replacing function bodies and migrating live component layouts are
different features; the latter should initially require restarting Play.

The initial scope is single-player with explicit command/state boundaries that
can later support server authority and prediction. It does not include a
network stack, rollback implementation, general visual programming language,
or a replacement ECS.

No implementation can guarantee zero execution cost or zero input-to-display
latency. The achievable contract is bounded work, no unnecessary frame queues,
no unbounded allocation or compilation in gameplay updates, and measured
latency and frame-time limits on specified hardware. C removes the need for a
gameplay VM; it does not remove collision cost, cache misses, bad algorithms,
thread contention, or presentation delay.

The first foundation is implemented in [ADR-073](../adr/073-native-gameplay-foundation.md):
one shared scene clock, native pre/post-physics hooks, ECS structural scopes,
query freshness, ordered input, a caller-owned weapon primitive, native character
motors, camera rigs and an opt-in playable C client in Bistro. General prefab
composition, projectile/interaction systems, advanced movement/camera policies,
native module reload and visual-authoring designs below remain proposals.
No gameplay performance measurements accompany this document.

## Current VKR foundation

The research baseline was source revision
`c25f2891fd36c7aa88c91e5bd73284c8f04f0857`; the table includes the subsequent
[foundation implementation](../adr/073-native-gameplay-foundation.md). The relevant owners are
[ADR-010](../adr/010-ecs-scene-system.md),
[ADR-004](../adr/004-stateless-render-packet.md),
[ADR-047](../adr/047-event-payload-and-resize-mailbox-lifetimes.md),
[ADR-069](../adr/069-editor-projects-and-workspaces.md),
[ADR-071](../adr/071-animation-bank-and-reference-pose.md), and
[ADR-072](../adr/072-entity-collision-and-rigid-body-physics.md).

| Area | Implemented foundation | Missing gameplay contract |
|---|---|---|
| Entities | [VkrWorld](../../runtime/src/core/vkr_entity.h) has archetypes, SoA chunks, 16 KiB default chunks, 256 component types, and generation-bearing IDs. Registration records a name, size, and alignment. | Field metadata, behavior composition, action registration, scheduling, serialization and state migration. |
| Scene | [VkrScene](../../runtime/src/renderer/systems/vkr_scene_system.h) owns the world, hierarchy, compiled queries, physics/animation bindings, and rendering synchronization. | Bounded runtime spawn/attach/detach transactions that update every affected owner. |
| Input | [InputState](../../runtime/src/core/input.c) retains held state and press/release edges. The [host](../../runtime/src/application/vkr_application_host.c) polls input before its frame callback and clears edges afterward. | Remappable bindings, gamepad action routing, possession and network transport. The C player now uses ordered owning-thread transitions and tick assignment. |
| Physics | [Scene physics](../../runtime/src/renderer/systems/vkr_scene_physics.h) supplies 60 Hz stepping, a 1,024-body bound, joints, queries, contacts/sensors and bone attachments through Jolt. | Character interaction/proxies, active projectile lifecycle and a runtime body-spawn contract. CharacterVirtual capsules now provide the initial motor. |
| Animation | [Graphs](../../runtime/src/animation/vkr_animation_graph.h) provide bounded pose composition and state transitions. [Scene APIs](../../runtime/src/renderer/systems/vkr_scene_animation.h) apply copied controllers, set parameters and read evaluated bone matrices. | Gameplay event tracks, root-motion authority and reusable managed controller assets. |
| Cameras | [Camera registry](../../runtime/src/renderer/systems/vkr_camera.h) and [free-camera controller](../../runtime/src/renderer/systems/vkr_camera_controller.h) exist. Imported camera references remain metadata. | Animated socket following, smooth transitions, shoulder aim convergence and possession. The C player now has first/third-person and shoulder modes with sphere obstruction. |
| Editor | [Animation editor](../../editor/src/editor_animation.c) has a canvas, typed nodes, connections, parameters and undo. Project publication uses managed identities and immutable revisions; persisted player/weapon bindings now run the native client through editor simulation. | Gameplay schema Inspector, reusable behavior assets, general live gameplay diagnostics and cloned Play-state isolation. |

### Integration hazards

**Structural commands still need an owner.** The foundation now protects query
and tick borrows and refreshes scene queries after archetype changes. It rejects
structural mutation during callbacks. The next slice must stage lifecycle work
between ticks and update hierarchy, render and native physics owners together;
a read barrier alone is not a spawn/despawn transaction.

**Use the shared tick hooks.** Gameplay can now enter the actual fixed loop through
`VkrSceneSimulationCallbacks`. An outer application callback still runs once per
display update. The C player now admits ordered input into those ticks; the more
detailed multi-entity phase schedule below remains future work. No-body scenes
opt into fixed animation when gameplay callbacks are configured.

**Existing time retention is limited by its input.** The physics accumulator
retains supplied debt, performs at most eight ticks per scene update, and
pauses after 60 consecutively overloaded updates. The application host first
clamps delivered delta to 0.1 seconds. Therefore the current stack does not
retain every second of wall-clock time. A gameplay clock must explicitly
distinguish wall time, admitted simulation time, pause and overload.

**Physics callbacks and editor edits are unsuitable spawn mechanisms.**
Contact payloads are borrowed, callbacks permit read-only queries, and mutation
or reentrant drains are rejected. Authored body edits require pause and staged
publication. Project entity addition is durable authoring. Shooting needs its
own bounded runtime activation path through the same scene/physics owners.
The sample runtime also drains sensors after scene update; gameplay must
replace or coordinate that consumer rather than race a second drain.

**IDs have limits.** Runtime IDs contain 16-bit world and generation fields;
both scene-loader paths currently use world ID zero, and generations wrap.
Cross-scene commands need a session epoch. High-churn slot reuse also needs a
within-session wrap policy, such as retiring a slot before generation reuse.
A scene epoch alone does not solve generation wrap inside that scene.
Persist authored references, not runtime IDs.

**The event worker is a different domain.** The existing
[EventManager](../../runtime/src/core/event.h) has a mutex, queue and worker.
It supports platform handoff but does not establish simulation ordering or
entity-safe subscription teardown. The
[event delivery boundary](#existing-event-manager-and-gameplay-delivery) below
specifies its proposed role.

## Lessons from other engines

The comparisons describe mechanisms, not a benchmark ranking. Official
documentation explains execution models but cannot establish VKR's costs.
Epic pages identify UE 5.8; Unity references identify Unity 6.0 and the listed
package branches. Godot's stable pages are rolling documentation; the C example
is explicitly version 4.4. Jolt references below match VKR's pinned 5.5.0.

### Unreal: composition and actions

Unreal distinguishes world objects, components, controllers, pawns, characters
and cameras. A controller can possess a physical pawn; a character combines
movement, skeletal representation and collision. This is a useful separation
for VKR even without C++ inheritance.
[Epic's gameplay framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/gameplay-framework-in-unreal-engine).[^1]

Unreal also provides MassEntity: data-only fragments are composed into entities,
stored in archetype chunks and processed in batches. Its command buffer defers
composition changes, while authored configuration assets supply reusable
traits. Actor composition and data-oriented execution therefore need separate
evaluation even within Unreal.
[MassEntity Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine).[^23]

Blueprint compiles to VM bytecode; native C++ compiles to machine code. Epic
identifies tight loops, many ticking instances and large datasets as cases
where the distinction matters, and recommends profiling and event/timer-driven
work. This supports native batch execution for frequent work, not a claim that
every visual graph causes visible latency.
[Blueprint versus C++](https://dev.epicgames.com/documentation/unreal-engine/coding-in-unreal-engine-blueprint-vs-cplusplus?lang=en-US).[^2]

Gameplay Abilities explicitly model activation conditions, cost, cooldown,
multi-stage execution and cancellation, with networking/prediction facilities.
Borrow those concepts for weapons and character actions; implementing the
entire Gameplay Ability System would be disproportionate to the initial need.
[Gameplay Ability](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-gameplay-abilities-in-unreal-engine).[^3]

StateTree combines hierarchical states/transitions with selector concepts.
Unreal's behavior trees provide another visual model for AI decisions and
describe event-driven evaluation. These are distinct tools with distinct
semantics, not reasons to encode every operation as a generic graph node.
[StateTree](https://dev.epicgames.com/documentation/en-us/unreal-engine/state-tree-in-unreal-engine),
[Behavior Tree Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview).[^4][^5]

### Unity: authoring convenience and data execution

Unity's GameObjects are component containers; their attached components supply
functionality. The separate Entities model uses entity IDs and component data
processed by systems. Those are distinct runtime models, and neither requires
VKR to reproduce Unity's object hierarchy to offer component-based authoring.
[GameObjects](https://docs.unity3d.com/6000.0/Documentation/Manual/GameObjects.html),
[Entities concepts](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-intro.html).[^24][^25]

Unity's MonoBehaviour model exposes lifecycle callbacks within a defined Player
loop. Its separate Entities model supports deferred structural commands;
enableable components can change query participation without repeatedly adding
and removing component types. Burst compiles a supported C# subset to native
CPU code. Language names alone therefore do not predict execution cost.
[Execution order](https://docs.unity3d.com/6000.0/Documentation/Manual/execution-order.html),
[Burst](https://docs.unity3d.com/Packages/com.unity.burst@1.8/manual/index.html),
[Enableable components](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-enableable-intro.html),
[Entity command buffers](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffers.html).[^6][^7][^8][^9]

Unity Visual Scripting represents logic with connected nodes and supports
custom programmer-authored nodes. For VKR, the transferable idea is exposing
coarse gameplay operations with typed ports. Copying every function, arithmetic
operator and variable into a graph palette would create another programming
language to maintain.
[Visual Scripting](https://docs.unity3d.com/Packages/com.unity.visualscripting@1.9/manual/index.html).[^10]

### Godot: composition and readable connections

Godot composes reusable scenes from node trees, including character, collision
and camera nodes. Signals make event connections visible without requiring
both objects to know each other. Its frame and physics processing callbacks
have distinct cadences.
[Nodes and Scenes](https://docs.godotengine.org/en/stable/getting_started/step_by_step/nodes_and_scenes.html),
[Signals](https://docs.godotengine.org/en/stable/getting_started/step_by_step/signals.html),
[Idle and Physics Processing](https://docs.godotengine.org/en/stable/tutorials/scripting/idle_and_physics_processing.html).[^11][^12][^14]

Godot also supports native extensions through a C interface. Its direct C
example demonstrates registering methods, properties and signals, while warning
that the low-level API is verbose. VKR can provide a much smaller C interface
because it controls both engine and gameplay contracts.
[GDExtension C example, Godot 4.4](https://docs.godotengine.org/en/4.4/tutorials/scripting/gdextension/gdextension_c_example.html).[^13]

### Flecs: scheduling is part of ECS correctness

Flecs separates component iteration from structural changes, stages operations,
and uses read/write information to determine when queued changes become visible.
Its documentation also permits integration into an existing application's
schedule. The relevant lesson is to define mutation visibility and query
dependencies explicitly.
[Flecs systems](https://www.flecs.dev/flecs/Systems.html).[^15]

VKR already owns an ECS. Retain it for the pilot and repair its mutation/query
boundary at the owner. Evaluate adopting Flecs only if measured workloads or
maintenance needs justify replacing the existing world and scene integration.
Introducing a second ECS implementation with identity adapters would add
ownership problems before providing evidence of a benefit.

## Runtime and authoring alternatives

| Approach | Strength | Cost or constraint | Recommendation |
|---|---|---|---|
| Per-entity native callbacks | Direct mapping from “this gun reacts to Fire” to code; useful for sparse bespoke interactions. | Scattered state, indirect dispatch, empty ticks and hidden order become expensive at scale. | Permit sparse handlers; do not require a callback per entity per frame. |
| Typed ECS state plus C systems | Fits current storage; shared rules process batches; explicit ownership and order. | Requires composition metadata, scheduling and structural publication. | Primary execution model. |
| Native C development module | Fast iteration without rebuilding the full engine; native functions and normal debugging. | ABI, schema, callback and job lifetime rules; reload can pause or fail. | Add after the statically linked slice. |
| Lua-style scripting | Compact content logic, dynamic iteration and established embedding. | Another runtime, bindings and memory policy. Lua 5.4 uses a bytecode VM and incremental or generational GC. | Reconsider for content-heavy scripting if C iteration becomes the limiting factor. |
| C compiled to WebAssembly | Potential isolation for separately supplied code. | Host-call and data-access boundaries, sandbox configuration and another toolchain/runtime. | Reconsider for untrusted mods; not needed for trusted project C. |
| Visual state/connection assets | Makes composition and transitions inspectable; can lower to bounded data calling C. | Still executes transition checks and actions; needs schemas and precise semantics. | Preferred visual authoring extension. |
| General graph VM | Broad visual expressiveness and runtime editing. | Language design, debugger, optimizer, latent execution and potentially many fine-grained calls. | Defer. |
| Graph-to-C generation | Can compile a restricted graph to native functions. | Compiler/source-map maintenance, compile iteration and possible code growth. | Consider only after graph semantics stabilize and measurements justify it. |

The Lua and WebAssembly descriptions follow the
[Lua 5.4 manual](https://www.lua.org/manual/5.4/manual.html) and
[Wasmtime security model](https://docs.wasmtime.dev/security.html).[^20][^21]
These sources establish runtime properties, not comparative gameplay timings.
Native project modules are trusted code in the engine process; choosing them
does not create a sandbox.

## Entity composition and state ownership

### Why ECS plus prefabs fits VKR

Recommend ECS for storage and execution, prefabs for reusable authoring, and
scene-owned lifecycle operations for their instances. This follows the current
VKR ownership model and supports C systems without a new object hierarchy.
It is an architectural fit assessment, not a measured claim that ECS always
outperforms actors.

The word actor does not specify storage layout, callback frequency or execution
order. A gameplay actor can be represented by ECS entities. Introducing a
distinct runtime actor owner is justified only if it supplies a responsibility
that the existing scene/world and prefab lifecycle cannot express.

| Design | Fit and tradeoff for VKR | Recommendation |
|---|---|---|
| Additional actor runtime with its own registry and object hierarchy | Offers an object-centric programming interface, but introduces identity, component ownership and scene synchronization contracts alongside those already implemented. | No separate runtime actor layer in the pilot. |
| Existing ECS with manual composition at every call site | Preserves storage and execution, but makes creating, validating and editing repeated player/weapon compositions repetitive. | Useful as a low-level API; add reusable prefab definitions above it. |
| Existing ECS plus prefabs and coordinated lifecycle | Reuses world identity, batch queries and scene ownership while giving code and editor users a complete gameplay object to instantiate. Requires explicit composition metadata and lifecycle validation. | Selected recommendation. |
| Node-only runtime hierarchy | Makes parent/child composition direct, but replacing current component storage and chunk queries would change ADR-010's selected contract. | Retain hierarchy within the existing ECS. |

No separate `VkrActor` handle is needed initially. Instance operations resolve
the prefab's root entity in a validated gameplay session. Sparse initialization,
interaction and teardown handlers may be native C callbacks, while repeated
movement, weapon and projectile work remains in shared systems. Revisit an
actor API when a concrete caller needs a distinct reusable contract, not simply
because another engine uses that name.

### Definitions and instances

Use a small vocabulary:

| Concept | Meaning | Example |
|---|---|---|
| Entity | Existing identity and component composition. | Player, weapon, projectile, trigger volume. |
| Component | Typed per-instance data. | Weapon ammo/state, motor velocity, camera target. |
| System | Native procedure over compatible component batches or queued requests. | Weapons, character motors, projectile integration. |
| Behavior definition | Immutable defaults, required components and bindings for one capability. | Rifle firing rules or character-motor settings. |
| Prefab | Reusable entity composition, behavior references, defaults and explicit ownership/reference bindings. | A complete player, weapon or door definition. |
| Prefab instance | Live root entity and its composed entities/state within a gameplay session. | One spawned player; optionally called an actor. |
| Action | Request that the owning system may accept or reject. | Fire, Reload, Jump, Equip. |
| Event | Fact produced after a change or observation. | ShotFired, ReloadCompleted, DamageApplied. |
| Presentation | Derived visual/audio/UI response. | Recoil, muzzle flash, crosshair and camera pose. |

Attaching `Weapon` resolves a definition, validates required components and
references, reserves storage, initializes state, and enables participation in
the weapon system. The Inspector can present this as one attachment even when
it owns several ECS components. A behavior is not necessarily one allocation,
one virtual object, or one source file.

For the player, compose `CharacterMotor`, `Health`, `Equipment` and an
animation binding on the character root. A controller possesses that root and
produces movement/action commands. Attach the model as a presentation child.
Keep the weapon as a referenced entity with its own state and socket binding.
A separate `CameraRig` references the controlled character.

A prefab instance coordinates spawn, initialization, enable/disable and
destruction through the existing scene/gameplay boundary. Prepare dependencies,
resolve internal references and reserve required capacity before publishing
the complete instance. Failed preparation must not leave a partially active
weapon or character.

Distinguish owned children from referenced entities. A player's model may be
owned by the player instance, while an equipped weapon may retain an independent
lifetime so it can be dropped. Transform parenting and socket attachment do
not automatically transfer destruction ownership. Destroying an instance
cancels its actions and releases its owned composition; references to external
entities are invalidated or detached according to their declared policy.

~~~mermaid
flowchart LR
    Input[Player input or AI] --> Commands[Typed commands]
    Commands --> Systems[Ordered C systems]
    Definitions[Behavior definitions and visual state charts] --> Systems
    Systems <--> State[Existing ECS component state]
    Systems --> Physics[Scene physics and character adapter]
    Physics --> Facts[Contact and sensor facts]
    Facts --> Systems
    State --> Presentation[Animation and camera presentation]
    Presentation --> Mirror[Existing scene render mirror]
    Mirror --> Packet[Renderer frame input]
    Inspector[Inspector and event trace] -. observes .-> State
~~~

Configuration and runtime state have different owners. A shared immutable
weapon definition contains magazine capacity, fire interval, damage and reload
duration. Each weapon instance owns magazine contents, reload progress,
cooldown deadline and shot sequence. The UI reads that state. It never keeps a
second authoritative ammo count.

Movement belongs to the motor; physics supplies collision/solved poses;
animation supplies pose and, only with a future explicit contract, requested
root motion. The camera writes the selected camera view. Two systems must not
independently write the character root transform. Simulated/evaluated transforms
must remain separate from authored editor transforms, preserving ADR-010/072.

Prefer one component type per concrete kind of state over a universal
`void *script_state` component. Repeated behavior instances of the same type
need explicit semantics: the pilot permits one weapon behavior per weapon
entity, with multiple weapons represented by separate entities. Add a
multi-instance behavior store only for a demonstrated use case.

### What C gameplay code looks like

These are illustrative declarations, not current APIs or a complete ABI:

~~~c
typedef struct VkrWeaponConfig {
  uint32_t magazine_capacity;
  float64_t fire_interval_seconds;
  float64_t reload_seconds;
  float32_t damage;
} VkrWeaponConfig;

typedef struct VkrWeaponState {
  uint32_t magazine_rounds;
  uint64_t shot_sequence;
  uint64_t action_generation;
  float64_t next_fire_time;
  float64_t reload_complete_time;
  bool8_t reloading;
} VkrWeaponState;

typedef enum VkrFireResult {
  VKR_FIRE_OK,
  VKR_FIRE_BLOCKED,
  VKR_FIRE_RELOADING,
  VKR_FIRE_COOLDOWN,
  VKR_FIRE_EMPTY,
  VKR_FIRE_CAPACITY,
  VKR_FIRE_INVALID_TARGET,
} VkrFireResult;

VkrFireResult vkr_weapon_try_fire(VkrGameplayContext *context,
                                  VkrEntityId weapon,
                                  const VkrFireCommand *command);

void vkr_weapon_update_batch(VkrGameplayTick *tick,
                              VkrWeaponBatch *batch);
~~~

The context supplies scoped services and a validated scene/session identity.
The weapon system resolves or receives typed storage once per operation/batch.
Most guard/math/state work remains direct C inside that system. Registration
exposes an action and its metadata once; it does not force every arithmetic
operation through reflection.

A bespoke turret can submit the same Fire action as a player. A charged weapon
can add a charge-state component and native system without changing the camera,
physics, rendering or entity identity model.

## Simulation schedule and latency

Extend the scene/runtime owner with one simulation coordinator, and make the
existing physics step a participant. Do not put a second accumulator around
`vkr_scene_physics_update()`. The first gameplay mode should retain 60 Hz,
the current catch-up cap and explicit overload reporting while integrating
actions and reactions inside each tick. The coordinator must also run in
scenes with no physics bodies.

Use an integer tick index as the canonical simulation clock and derive
seconds from the configured fixed interval. Deadlines execute on the first
eligible tick, with boundary comparisons covered by replay tests. Pause,
single-step and Reset apply to the whole gameplay session; resetting physics
alone cannot leave ammo, timers or motor state on a different timeline.

For gameplay mode, admit elapsed time from the monotonic host clock using an
explicit pause/time-scale policy, instead of losing time through an incidental
0.1-second presentation clamp. Retain admitted debt; cap work, not required
events. Long application suspension is an explicit pause and rebases the
wall-to-simulation mapping on resume. Keep the current non-gameplay/editor time
behavior until its owner is deliberately changed. A future 120 Hz mode must
change the common gameplay/physics clock together and pass new cost/behavior
gates.

Fixed stepping and presentation interpolation are established ways to decouple
simulation from display cadence, with overload and smoothing tradeoffs.
[Fiedler, Fix Your Timestep!](https://gafferongames.com/post/fix_your_timestep/).[^16]
The following phase order is a VKR recommendation, not an existing schedule:

| Phase | Work and visibility |
|---|---|
| Frame input | Poll input, apply UI/focus ownership, append ordered commands and retain held values. Read current look for presentation. |
| Tick boundary | Commit previously staged lifecycle changes; update query caches; establish the tick's simulation time and command range. |
| Actions and queries | Apply due timers and prior reactions, then resolve commands. Weapon eligibility and hit queries use the beginning-of-tick simulation snapshot. Collect hit/damage facts; commit accepted ammo/cooldown changes. |
| Motion and animation | Prepare motor intent; evaluate required CPU animation; derive desired kinematic and bone targets without writing native poses yet. A root-motion feature would have to join here. |
| Pre-physics commit | Publish reserved projectile/body activations and other allowed commands before iteration/solver entry. Invalidate affected queries. Run the active swept-projectile batch, including new activations, against the declared query snapshot before native pose writes. |
| Physics | Run the character motor adapter in stable order, apply native kinematic/bone targets, then step the native world once. Copy contact/sensor facts through one scene event owner after solver locks have been released. |
| Post-physics gameplay | After callbacks return, resolve collected damage, trigger transitions, cancellations and death. Apply ordinary owned gameplay state; stage structural/native mutations for the next safe tick boundary. |
| Tick completion | Publish completed gameplay state and presentation cues, record the tick outcome, then repeat if catching up. |
| Frame presentation | Evaluate the camera from the followed pose and latest look, derive visual feedback, synchronize the existing render mirror, prepare frame inputs and render. |

This provides explicit causality. A contact in tick N can apply damage in tick
N's post-physics stage and block actions in tick N+1, even when both ticks run
inside one display update. A post-physics reaction that spawns a new physics
object becomes active at the next tick boundary. It must not recursively step
physics or wait for an unrelated next display frame.

For the first weapon implementation, all shot collision queries in a tick read
the same beginning-of-tick physics poses. Collect damage before applying it;
two initially eligible shooters can therefore both fire in that tick. This is
an explicit simultaneous-action policy. Do not accidentally let ECS row order
choose which shooter survives long enough to act. If later gameplay requires
post-movement aim tests or historical hitboxes, change the query-time contract
explicitly and preserve its timestamp in replay.

Here, “snapshot” means a collision-query phase with unchanged target poses;
it does not require cloning Jolt's collision world every tick. Required traces
finish before motor/kinematic pose writes. Newly activated projectile bodies
are excluded from those traces according to the shot's filter policy.

Avoid a general task scheduler in the pilot. Start with an ordered C phase
table and component/service read-write declarations checked at registration.
Once justified, independent chunk jobs may run in parallel; a single writer
still owns each state partition. Merge outputs in stable order. The current
Jolt adapter uses `JobSystemSingleThreaded` and serial world operations:
parallel gameplay does not authorize concurrent physics access.

### Input must survive zero and multiple ticks

Keep press, release, held and analog semantics distinct. A press is consumed
once; held Fire drives automatic firing; mouse displacement is not a velocity
to multiply by delta again. Gamepad look rate is integrated over time. Movement
axes need dead zones and diagonal normalization according to the input policy.

Current input edge bitsets cannot reconstruct several press/release cycles
within one host update. Add a bounded ordered gameplay input record at the
input producer, with sequence and monotonic timestamp when available.
Polling-only devices use an explicitly recorded sample time. Retain edges
until the eligible tick consumes them, including frames that run zero ticks.
Catch-up assigns each edge to one tick; it must not replay the newest input
into simulated time that preceded its observation.

Map timestamps into the admitted simulation timeline across pauses and time
scale. Record the final tick assignment for replay. UI capture, focus loss,
controller disconnection and possession changes generate cancellation/release
semantics, so a lost key-up cannot leave a weapon firing or a character moving.

### A realistic latency contract

At 60 Hz, tick spacing is 16.67 ms; at 120 Hz it is 8.33 ms. Under ideal
continuous scheduling, waiting for the next tick boundary lies between zero
and one spacing. A display-driven host can add polling/scheduling delay and
overload adds debt; these figures are not end-to-end latency guarantees.
Interpolating previous/current simulation poses adds a deliberate historical
offset, commonly one simulation step relative to the render timeline.

Keep local camera rotation on the presentation path using the latest input.
Do not interpolate it merely because remote/world body positions are
interpolated. When a tick accepts Fire, expose its recoil/muzzle cue to the
current frame's presentation without another generic event-queue frame.
On a zero-tick frame, authoritative Fire remains pending. A speculative local
effect could hide that wait, but requires rejection and duplicate suppression;
it is a later prediction feature, not free latency reduction.

Track input-observed, action-accepted, simulation-completed and frame-submitted
timestamps. Input-to-photon also includes device, OS, GPU queue and display
delay, and requires an external measurement method. Faster gameplay dispatch
cannot remove those costs.

## Actions, events and bounded mutation

Use direct calls inside one owner's execution and typed queues at ownership or
phase boundaries. Separate three meanings:

| Kind | Example | Required semantics |
|---|---|---|
| Request | `TryFire`, `TryReload`, `TryInteract` | Has a target and sequence; validates current authority/state; returns or records an explicit result. |
| Fact | `ShotFired`, `ContactBegin`, `ReloadCompleted` | Describes something that happened; consumers cannot retroactively veto it. |
| Structural command | Spawn, despawn, attach, detach | Owns prepared input and executes at a declared safe boundary; cannot invalidate active iteration. |

### Existing event manager and gameplay delivery

Keep the existing `EventManager` for platform notifications and asynchronous
handoff. Its dispatch call enqueues under a mutex; the worker executes
callbacks independently of simulation ticks. Those callbacks must synchronize
their access to application state. Dispatch success reports enqueue admission,
not whether a gameplay action was accepted, and callback return values are
currently ignored.

Subscription lifetime also differs from entity lifetime.
`event_manager_unsubscribe()` selects by event type and callback function,
without a `user_data` selector, and removes the first match. An already
copied worker snapshot may still invoke that callback. Consequently, multiple
entities sharing one handler cannot use this API alone to identify and safely
retire one entity's subscription. These are current
[API](../../runtime/src/core/event.h) and
[implementation](../../runtime/src/core/event.c) constraints.

| Operation | Proposed route |
|---|---|
| Window resize or asynchronous service notification | Existing event manager delivers to a persistent host-owned bridge; the owning thread consumes copied values or validated handles at its safe boundary. |
| Player/AI requests Fire, Reload or Jump | Typed gameplay command consumed by the owning C system in a defined simulation phase. Ordinary player input comes from the input owner, without a worker-event round trip. |
| Gameplay reports ShotFired, ReloadCompleted or DamageApplied | Reserved typed fact buffer, with consumers scheduled at explicit points in the simulation/presentation sequence. |
| Gameplay requests spawn, detach or destruction | Prepared structural command applied by the scene/gameplay owner at a mutation barrier. |

The host bridge outlives its subscriptions and any in-flight callbacks; queued
messages carry session/entity generations rather than raw component pointers.
Gameplay subscriptions need an exact token or equivalent owner identity,
validated again on delivery, so despawn can invalidate that owner's pending
work.

Implement the gameplay path as small simulation-owned queues and routing
tables using suitable existing containers and allocators. Reuse lower-level
primitives where their capacity and lifetime semantics fit; keep delivery
order and ownership explicit instead of adding a mode flag that switches the
platform manager between worker and simulation execution.

The weapon flow is: Fire command, native guard/commit, accepted ShotFired fact,
then presentation response. Event delivery cannot bypass the weapon's ammo,
cooldown or permission checks.

### Gameplay buffer ownership and failure

The gameplay coordinator owns transient fact storage and the single
physics-event drain/fanout path. Use numeric type IDs and fixed-layout payloads
in reserved buffers. Resolve names and validate routes during load or
publication. Register sparse subscriptions by event type and destination;
do not broadcast every event to every entity. Handlers cannot retain borrowed
payload pointers.

Define order as tick, phase, producer order and per-producer sequence. External
references include session epoch and entity generation; action/timer references
also include an action generation. Late completion of a canceled reload then
becomes a harmless rejected stale completion. Unsubscribe/despawn invalidates
pending deliveries before slot reuse.

Queues are bounded and report occupancy, overflow and rejection. Required
gameplay facts cannot be silently dropped or delayed to conceal overload.
Reserve command, projectile and required-result capacity before accepting an
action. Refuse an uncommitted action when admission fails. If a committed
simulation unexpectedly exhausts required capacity, enter an explicit fault
state and preserve diagnostics. Cosmetic events may have an authored
coalescing/drop policy, with separate counters.

A native callback can still loop forever or corrupt memory. A transition-count
bound protects the state-chart evaluator, not arbitrary C execution. Keep
native handlers small and inspectable, profile them, and use process restart
for a crashed/hung development runtime.

On a solver failure after actions have committed, do not automatically retry
the same tick and consume ammo twice. Fault the session; resumption requires
Reset or an explicitly complete pre-tick checkpoint. Full transactional
rollback of physics and gameplay is outside the pilot.

## Weapon walkthrough

The weapon owns magazine rounds, action state and cooldown. Inventory owns
reserve ammunition. A weapon definition selects ammo type, magazine capacity,
fire mode, interval, reload duration, projectile/hitscan parameters and visual
references. Validate nonzero capacity, finite positive intervals, bounded
pellet count, compatible collision references and available assets at load.

Avoid storing redundant `can_fire`, `is_empty` and `has_ammo` flags.
Derive eligibility from the magazine, deadline, reload state and action locks.
One compact reload state plus a cooldown deadline is sufficient for the basic
weapon; an expanded enum/state chart is justified when additional states have
distinct behavior.

### Fire transaction

1. Validate the command's session/entity/action identity and controller or AI
   permission to operate the equipped weapon.
2. Reject active action locks, reload, cooldown and an empty magazine with a
   specific reason.
3. Resolve the authoritative aim and muzzle references, then reserve all
   required projectile/command/fact capacity for the shot.
4. Commit one magazine decrement and one shot sequence increment, establish
   the next eligible firing time, and publish the shot operation.
5. Produce one accepted-shot fact; damage and presentation consume its identity.

A shotgun decrements ammo once for its bounded pellet batch. If the required
batch cannot be admitted, it fires no partial batch and spends no round.
Optional visual effects may fail independently under their declared policy;
they do not refund a physically committed shot.

Semi-automatic fire uses press edges; automatic fire uses held input and a
simulation-time deadline. Advance a continuously held weapon's next deadline
from its scheduled deadline to avoid accumulating tick-rounding drift.
Newly pressed fire after an idle period starts from the current simulation
time, preventing a backlog of shots for time when the trigger was released.

Rates that exceed one shot per tick require bounded multiple-shot handling and
defined sub-tick collision/spawn semantics. The first slice should reject such
configurations rather than claim support. Hitch catch-up remains bounded by
the simulation tick policy. Later support must preserve shot count/order and
model each projectile's actual remaining integration interval.

### Reload and cancellation

At reload start, verify reserve ammo exists and the magazine is not full.
Store a completion deadline and action generation. For the basic magazine
reload, reserve ammo is transferred only at completion, in one inventory-owned
operation:

`transferred = min(magazine_capacity - magazine_rounds, available_reserve)`.

Increase the magazine and reduce inventory by exactly that amount. Revalidate
inventory at completion because another weapon or action may have spent it.
Cancellation before completion transfers nothing. Death, unequip and an
explicit cancel request invalidate the completion generation. Shell-by-shell
reload can later commit separate insertion actions; its interruption policy
must preserve already inserted rounds.

Animation does not own this timer. Playback rate, skipped rendering or a
missing animation event must not grant ammunition. If a future reload uses
animation markers, those markers need simulation-time traversal,
deduplication and cancellation rules first.

### Blocking a weapon

Use owned action-lock tokens, or independently owned reasons with reference
counts, rather than one writable `disabled` Boolean. Stun and a cinematic may
both block firing. Ending the cinematic releases only its lock; it cannot
clear stun. Cosmetic visibility, trigger permission, reload cancellation and
complete component disable are separate operations.

For the pilot, a firing lock cancels held-fire intent and requires fresh
activation after the lock clears. Death/unequip also cancel reload. Other
locks must declare whether reload continues. These policies belong to the
weapon/action definition and appear in the Inspector.

The following chart is a view of native state and guards. It need not be a
second executable authority:

~~~mermaid
stateDiagram-v2
    [*] --> Ready
    Ready --> Ready: Fire accepted / consume round and set deadline
    Ready --> Ready: Fire rejected / report reason
    Ready --> Reloading: Reload accepted / set completion deadline
    Reloading --> Ready: Deadline / transfer available ammo
    Reloading --> Ready: Cancel / invalidate completion
    Reloading --> Reloading: Fire / reject
~~~

### Bullets and collision

Start with hitscan for an instant rifle: a query result drives damage and a
separate tracer provides presentation. Also implement a small pool of swept
projectiles to prove reusable runtime activation. Keep grenades or other
physical projectiles as a separate policy using native bodies when their
interaction warrants it.

For aim, determine a target from the authoritative aim ray and test from the
muzzle toward that target, respecting walls close to the weapon. Ignore the
shooter and its declared attachments using collision filters. Camera-space
aim must not let the muzzle shoot through cover.

A swept projectile tests its traveled segment/shape, not just its endpoint.
Define range, lifetime, initial overlap, hit ordering, collision layers,
penetration and repeat-hit suppression. A sweep against a frozen target pose
does not by itself solve continuous collision with a rapidly moving target;
use a suitable native CCD path or a separately validated relative-motion
scheme for that requirement.

Pool slots retain allocated capacity and use an activation generation in
addition to entity identity, preventing a delayed hit from damaging through a
reused projectile. Prewarm supported compositions and resources. Sleeping
projectiles are absent from active work lists; thousands of bullets need not
be thousands of Jolt bodies. The existing 1,024-body scene bound remains in
force until deliberately changed.

## Character, model and camera

### Motor and control

The controller turns player or AI input into movement intent and actions.
The character motor turns intent into a collision-constrained simulation pose.
The animated model follows that pose. Separating those responsibilities
supports AI possession, spectator mode and different camera views without
rewriting locomotion.

Evaluate Jolt 5.5.0's `CharacterVirtual` through the existing C++ physics
adapter before writing a custom sweep-and-slide motor. It provides explicit
update control, ground state, stair/floor helpers and optional inner-body
representation. It is not tracked by `PhysicsSystem`; its update and saved
state need separate ownership.
[Jolt CharacterVirtual](https://jrouwe.github.io/JoltPhysicsDocs/5.5.0/class_character_virtual.html).[^17]

The recommended pilot is a motor-driven capsule with explicit acceleration,
air control, gravity, slope limit, step height, moving-platform behavior and
jump buffering. Expose a C character handle and typed commands/results;
retain native objects within `vkr_physics`. Specify character/body and
character/character interactions and overflow behavior during that adapter
work. Existing sweeps alone do not establish a production-quality motor.

Feed resulting speed, grounded state and action intent into animation.
Use motor-driven locomotion first. Root motion, motion matching and active
ragdoll control can follow their own proposals; none is a prerequisite for
attaching gameplay to an entity.

### Camera rig

A camera rig stores target entity, mode, offsets, lens settings and optional
socket binding. It references a camera registry handle; it does not own a raw
camera pointer across registry mutation. One selected presentation owner writes
the active view. Disable the default free-camera controller while the gameplay
rig is active so it cannot overwrite the result.

| Mode | Suggested behavior |
|---|---|
| First person | Follow a controlled eye anchor; apply latest look directly. Make head bob/recoil explicit optional effects. Keep authoritative aim separate from cosmetic camera displacement. |
| Third person | Follow a root-relative pivot with yaw/pitch and distance; sweep a camera volume to retract from obstacles. |
| Over the shoulder | Add side/height offsets and aim-distance/FOV policy to the third-person rig; support shoulder switching and near-cover obstruction. |

Store root-relative anchors or stable skeleton/node references and local
offsets. Resolve sockets at attachment/publication and revalidate on skeleton
replacement. Evaluate bone-following cameras after the relevant CPU pose is
available; never read a GPU skinning result back to place the camera.
Head-bone animation should not inject unwanted roll or jitter into mouse aim.

Apply obstruction correction immediately; smoothing can govern the return
distance and mode transitions. Preserve pose continuity when switching views,
and reset interpolation/history on teleport. Any local-position prediction
used to reduce follow delay is a presentation estimate, not a collision pose.

First-person mesh hiding, dedicated arms/viewmodel rendering, clipping and
shadow behavior are separate presentation requirements. The current camera
registry does not prove support for those rendering policies. Identify needed
renderer changes before claiming a complete first-person presentation feature.

## Visual assistance and authoring

### First deliverable: explain the running game

The initial Inspector should attach/detach registered behavior definitions,
edit validated configuration, choose entity/socket references, and show live
read-only state. For a weapon it should answer: who controls it, how many
rounds remain, which action is running, which locks apply, and why the latest
Fire request failed.

A bounded event timeline should show tick, entity, action sequence, accepted
or rejected result, state change and emitted facts. Link each registered
behavior/action to its C source location. Record selected entities or sampled
events instead of formatting strings for every entity each tick. Format trace
records when the panel reads them; counters and traces can be disabled in
shipping configurations.

Add a system schedule view with phase, declared read/write sets, active
instance count and measured cost. This makes hidden ordering and accidental
work visible before there is a graph editor.

### Second deliverable: connections and constrained state charts

Expose coarse operations such as Request Fire, Request Reload, Apply Impulse,
Set Camera Mode and Set Animation Parameter. A connection asset binds a typed
event to an action on an explicit entity reference, with a bounded typed
payload mapping. The weapon action still validates ammo, locks and cooldown.
Connecting an event to Fire cannot bypass weapon authority.

For stateful orchestration, add a small state-chart asset with states, typed
events, pure C guards, C actions and timers. A door could connect trigger
entry to Open, wait for a deadline and request Close. A weapon variant could
orchestrate charge/release while delegating the actual shot to native code.

SCXML provides a useful reference for hierarchy, transition priority and
run-to-completion semantics. Its complete model permits unbounded macrosteps;
VKR should adopt a deliberately bounded subset, without claiming SCXML
conformance or adopting its XML/interpreter.
[W3C SCXML](https://www.w3.org/TR/scxml/).[^22]

Proposed chart rules:

- Exactly one active leaf per chart initially; parent states may supply shared
  transitions. Separate locomotion and equipment charts avoid a combinatorial
  list of combined states.
- Select matching transitions by explicit priority and stable authored ID.
  Guards are read-only. Define exit, transition action and entry order.
- Process one external event and its bounded internal consequences before the
  next external event. Prohibit recursion through cross-entity callbacks.
- Compile references, type checks and adjacency lists at publication. Reject
  invalid references, conflicting ownership, impossible bindings, and obvious
  eventless cycles.
- Bound remaining transition chains and fanout at runtime. Exceeding the bound
  faults the affected chart/session according to an explicit error policy;
  it cannot spill silently into later frames.
- A Wait creates a deadline and continuation/action ID. It never blocks the
  thread, sleeps, or retains a C stack pointer.
- Schedule work on relevant events/deadlines. A continuous guard declares its
  polling cadence; “event-driven” must not hide a full graph scan every frame.

State-chart tables still have execution cost. Native guards/actions and compact
prevalidated tables avoid a general expression VM, but do not imply the cost
equals hand-specialized C. Benchmark equivalent behavior before adding C code
generation.

Use behavior trees later for AI task selection, where success/failure/running
and cancellation semantics help. Keep animation graphs for pose composition,
state charts for temporal orchestration, and native systems for algorithms.
Share editor canvas/selection/undo primitives only where their real uses match;
do not make the animation evaluator execute gameplay.

### One authoritative representation

For arbitrary C logic, C is authoritative and a generated chart is read-only.
It may show declared states/actions and recorded execution, not invent a full
editable representation of arbitrary control flow.

For an authored state chart, the graph asset is authoritative; generated tables
or C are derived build outputs. Custom actions remain separate handwritten C.
Do not round-trip arbitrary edited C into graph nodes or allow generated code
and the graph to diverge.

Separate graph layout from runtime semantics so moving a node does not change
execution order or invalidate a runtime artifact unnecessarily. Stable node,
port, state and connection IDs make diffs, undo and diagnostics useful.
Validate a draft completely before replacing the active immutable revision;
failed compilation leaves the last valid revision running.

### Reflection and persistence

Start with explicit C metadata tables beside each owning type: stable type and
field IDs, display names, kind, offsets, bounds/units, default values, reference
kind, editability and serialization version. Verify sizes/offsets with compiler
expressions and static assertions. The existing name/size/alignment ECS
registration is not sufficient for this.

Use a typed field-list macro only if it demonstrably removes duplicated schema
facts while keeping structs readable. A Clang-based generator becomes useful
when manual metadata causes real maintenance errors; parsing C with regex or
building a custom C compiler should not be the first feature.

Metadata lowers names and references at load. Runtime loops use component IDs,
typed views and resolved handles. Separate config fields, transient state,
editor-only layout and saved-game state. Do not persist raw C struct bytes,
padding, pointers, function addresses or registration-order component IDs.

Behavior/recipe assets should extend the managed project publication model:
stable resource identity, schema version, dependency fingerprints, immutable
revision, conflict detection and atomic Save. Imported socket references use
source identity; newly authored entity references need their own durable ID
scheme and migration, not mutable array positions alone. Resolve all
references before activating a composed instance.

Keep prefab inheritance shallow: a reusable definition plus explicit instance
overrides is sufficient initially. Editing a default must show whether it
changes the asset, one authored instance, or only the running Play instance.

Play should instantiate runtime state from an authored snapshot using the same
ECS implementation and shared immutable assets, with a distinct session
identity. Stop destroys that state and restores authored editing. Saving a
scene must not save consumed ammo, a sampled pose or a temporary projectile.
An explicit Apply operation may copy selected validated configuration changes
back to authored data. Saved games are a separate serialization boundary.

The animation proposal's future controller assets and event/root-motion work
remain in [its owner](compute-animation-and-editor.md). When implementing the
shared gameplay clock, reconcile its older optional-clock sketch with the
current ADR-072 physics loop. Do not build two clocks from two proposals.

## Native C iteration and reload

Begin with an application/game target that links `vkr_runtime` and registers
gameplay types and systems explicitly. The renderer keeps its current
packet-only responsibility. Reusable simulation services belong in runtime;
game-specific weapon rules and content registration belong in the game
consumer. Add a module boundary for independent gameplay lifetime/build
responsibility, not merely to shorten a source file.

For development reload, use one or a few native gameplay modules built by the
normal platform toolchain, with one exported versioned C entry point returning
registration/system descriptors. The host ABI should include a version,
descriptor size, capabilities and compatible schema/build fingerprints.
Keep host allocation and release paired on the host side, especially across
Windows CRT boundaries. C linkage alone is not a complete cross-compiler ABI
guarantee.

The host owns persistent component bytes, queues, registries and copied
metadata. Module code borrows typed views during execution. Do not store
module-owned static pointers or function pointers in persistent components,
timers, editor undo records or asynchronous platform subscriptions.
Resolve action IDs through a host-owned registry; dispatch once per batch
where possible.

Reload sequence:

1. Build a uniquely named candidate module off the gameplay thread. Report
   compiler errors while the old module remains active.
2. Load and validate the candidate ABI, component schemas, required services
   and registrations without publishing callbacks into the active session.
3. At a safe development boundary, stop new calls and finish all old-module
   jobs/callbacks. Use an explicit pause when quiescence cannot fit the budget.
4. For unchanged schemas, preserve host-owned state and replace registrations
   transactionally. Run only bounded initialization of the new generation.
5. Publish the new generation, resume, and unload the old module only when no
   execution or retained pointer can refer to it. Retire old copies instead of
   accumulating an unbounded library cache.

Initially, changes to component size/alignment, field layout, ownership,
system contracts or active latent-action semantics require restarting Play.
The ECS currently has no component schema replacement mechanism. Registering
the changed type under the old ID is not migration.

A later migration feature needs versioned field serialization, shadow storage
for the complete affected world/state, reference fixups, validation and atomic
publication. Migration failure preserves the old module and old state.
Same-layout reload preserves current values; changing a C default does not
silently rewrite every instance.

Epic's Live Coding documentation independently illustrates that live code
replacement, object reinstancing and stale pointer cleanup are separate
problems. Its implementation is a reference for the hazard, not a proposed
dependency.
[Unreal Live Coding](https://dev.epicgames.com/documentation/unreal-engine/using-live-coding-to-recompile-unreal-engine-applications-at-runtime).[^19]

Native compilation and linking can compete for CPU/cache and cause hitches even
on another thread/process. Measure build-to-activation time and gameplay
interference separately; offer paused rebuilding for consistent playtests.
Ship ahead-of-time code with reload machinery absent. Test native builds and
reload on both macOS/Metal and Windows/Vulkan; one host cannot establish the
other's ABI or loader behavior.

## Memory, lifecycle and failure ownership

| Storage | Owner and lifetime | Stability/release rule |
|---|---|---|
| Immutable behavior definitions | Scene/project resource owner; shared by instances. | Hold the active revision while referenced; release replaced revisions after CPU consumers finish. |
| Components and runtime handles | Existing world plus its gameplay session. | Borrows expire before structural mutation. Release independently removed allocations through their owner. |
| Projectiles/action instances | Reserved typed storage or fixed-size pools owned by gameplay. | Activation generation prevents stale completion; reset all state before reuse. |
| Per-tick commands/facts | Gameplay coordinator; reserved buffers. | Payload values or validated handles only; reclaim after all declared consumers finish. |
| Module metadata and code | Host registry and module manager. | Copy retained metadata; unload code only after old execution has quiesced. |
| Frame presentation arrays | Application frame allocator. | Borrow through renderer call return; existing GPU resource retirement remains separate. |

Use existing allocator choices: an arena for values destroyed together,
`VkrDMemory` for independently removed variable-size allocations, and
`VkrPool` for suitable fixed-size churn. Reserve and commit capacity before
hot updates. A scope allocation that commits another arena page is still a
potential hitch.

Preparation validates a composed instance and acquires required capacity/assets
before publication. Failure releases acquired resources in reverse dependency
order. Dependency removal must either remove its owning behavior transactionally
or reject the edit; it cannot leave a live motor without its collision state.

Despawn stops admission, cancels actions/timers and subscriptions, stages native
body removal, invalidates identities and updates hierarchy/query/render owners.
Cosmetic trails may outlive the entity by owning copied parameters and retained
resource handles. CPU entity destruction does not prove GPU completion; keep
the renderer's existing resource and temporal-history retirement rules.

Measure live allocations, instances, handles and subscription counts after
repeated attach/detach, respawn, Play/Stop and reload. Distinguish bounded
reserved capacity from live memory and from process resident memory.

## Preserving a multiplayer path

Make input/action commands serializable from the start, with controller
identity, tick assignment and sequence. Keep authoritative gameplay state
separate from presentation, give randomness an explicit per-system/entity
seed and stored state, and avoid wall-clock reads inside gameplay functions.
Entity references in a future protocol need network identities mapped to
local handles, not raw ECS IDs.

This supports a later server-authoritative design: the server validates Fire,
the client predicts selected local feedback, and confirmations/corrections
refer to the original action sequence. Sounds, recoil and particles require
deduplication under replay. Rollback requires all relevant state, including
ammo, locks, RNG, action continuations, spawn history and physics topology.

A fixed timestep and C code do not establish cross-platform determinism.
Jolt 5.5.0 documents ordering/build constraints, caveats for queries and
additional restoration work when bodies are added/removed. Its physics snapshot
does not replace a gameplay snapshot.
[Jolt determinism and rollback](https://jrouwe.github.io/JoltPhysicsDocs/5.5.0/).[^18]

Do not implement lockstep, replication descriptors on every field, prediction
or a full ability framework speculatively. Preserve the boundaries now; select
a networking model when an actual multiplayer feature is scoped. Same-build
recorded-input replay is a useful earlier diagnostic, with its physics and
floating-point limits stated.

## Performance and acceptance

### Execution policy

Idle behavior must not imply one empty callback per entity per frame. Keep
active lists/queries for moving characters, live projectiles, held-fire weapons
and running actions. Timers use bounded deadline storage; start with a small
active list and introduce a reserved heap or timing wheel only when measured
volume warrants it. Do not tie gameplay activity to camera visibility:
off-screen attacks and timers remain correct.

Avoid heap growth, name lookup, JSON parsing, pipeline/resource creation,
blocking waits, log formatting and arbitrary locks in per-entity work.
Ordinary state changes such as reloading/cooldown should not change archetypes
each tick. Dynamic attach/detach remains supported through the structural
boundary, with its cost reported.

Gameplay logic is CPU work. GPU skinning remains a renderer consumer;
authoritative actions, required bones and collision do not wait for GPU
readback. Job scheduling is also work: parallelize substantial independent
batches only after measuring their serial cost and join overhead.

### Candidate budgets

No target CPU, entity count or accepted gameplay budget has been established.
The following are starting targets for discussion and measurement, not
promises or amendments to an accepted frame budget:

| Quantity | Candidate acceptance target |
|---|---|
| Display envelope | Evaluate 144 Hz presentation: 6.94 ms total frame interval. Also exercise 60/120 Hz and irregular frame delivery. |
| Behavior infrastructure | At the agreed pilot load, aim for p99 at or below 0.10 ms per tick for dispatch, timers and structural bookkeeping without physics/animation/rendering. Report mutation bursts separately. |
| Gameplay logic | Aim for p99 at or below 0.50 ms per tick for weapon, movement-policy, action and projectile logic. Report native physics/query and animation cost separately and inclusively. |
| Input scheduling | Eligible input consumed in its assigned tick; no extra framework-imposed display-frame delay. Report observed latency distributions and debt. |
| Memory/failure | Zero steady-state hot-path capacity growth; zero dropped required events; failures and saturation observable. |

The 0.10 ms infrastructure scope is included in the 0.50 ms logic target,
not added twice. Query/motor native cost, physics stepping, animation and
rendering must still fit the total frame budget. Per-frame cost is the sum of
all ticks actually executed, presentation, and the rest of the engine; a
per-tick target cannot justify a catch-up hitch.

Separate framework cost from necessary gameplay work using two implementations
of the same recorded scenario: direct ordered C and the proposed
registration/routing path, with identical state and output. A gameplay-disabled
run can reveal idle overhead, but is not equivalent work and cannot support a
claim that added gameplay is free.

### Workloads and evidence

Use Bistro for every scene-based gate. The initial slice should include one
controllable character, a hitscan weapon, one swept-projectile variant, a
trigger that blocks firing, reload/cancel/unequip, a target that receives
damage, and all three camera modes. Then scale independent dimensions:
1/16/64 controllable or AI-driven characters, 0/64/256 active projectiles,
and large inactive populations within declared ECS capacity. These are
candidate workloads, not existing harness cases. Count native bodies against
the existing limit, including colliders/character proxies as applicable.

| Invariant | Independent evidence |
|---|---|
| Ammo and action authority | Known command sequences assert exact ammo conservation, accepted shot count and rejection reasons; test empty/full/zero-reserve and simultaneous requests. |
| Time and input | Replay timestamped edges with zero/multiple ticks and 30/60/120/144 Hz plus irregular delivery; compare tick assignments, deadlines, shots and state at common simulation times. |
| Cancellation/identity | Cancel reload then deliver its old completion; destroy/reuse a projectile; change scene; force generation boundary conditions. Assert no mutation reaches the new owner. |
| Prefab ownership | Fail midway through instance preparation and verify no partial publication. Destroy a player with owned model children and a referenced dropped weapon; verify only the declared owned entities/resources are released. |
| Structural correctness | Attach a component that creates a previously absent archetype, then verify gameplay and scene queries plus mirror output discover the entity. Include originally empty compiled queries. |
| Event causality | A contact in the first catch-up tick blocks an action in the next tick of the same host update; multiple consumers observe one copied fact without a second drain. |
| Capacity/failure | Exhaust shot, event and spawn capacity at admission and after forced fault conditions; verify no partial ammo transaction, silent loss or automatic duplicate retry. |
| Movement/camera | Bistro stairs/slopes/doorways, moving platform, wall-adjacent muzzle, obstruction, teleport and view switching. Inspect grounded behavior, clipping and aim/camera correspondence. |
| Visual authoring | A chart and its equivalent native scenario produce the same accepted actions/state trace. Test invalid/cyclic graphs, undo, save/reopen, missing references and failed revision replacement. |
| Reload/lifetime | Same-schema reload succeeds; incompatible schema is rejected; failed build preserves running state; repeated Play/Stop and reload drain live handles/allocations. |
| Portability | Native macOS and Windows builds/execution; renderer integration evidence on Metal and Vulkan separately. Source review or one-backend execution does not prove parity. |

Use existing deterministic CPU tests where they supply the direct oracle;
add tests only for these named failure modes. Use small synthetic fixtures for
isolated defects, and Bistro for integrated scene/visual/performance work.
Builds, CPU tests, visual captures and timing reports prove different claims.

For timing, use capture-free normal Release with validation variables unset,
matched hardware/compiler/assets/output/presentation/cache/instrumentation,
and the existing
[performance profile](../../tools/profiles/performance-windowed.json).
It requires five independent repetitions, a clean source tree, stable warmup,
actual immediate presentation and exclusive GPU use. Inspect validity,
percentiles, spread, work counts, authority flags and fingerprints. Record the
exact commands, configuration and report digests with each result. Retain
gameplay-specific inclusive/exclusive CPU scopes and queue/debt counters.

Measure code-only, trace-enabled and graph-editor-open configurations
separately. Verify rendering output independently through captures; do not
reuse capture timing as steady-state performance. A future first-person
rendering change needs its own bilateral visual evidence.

The proposed gameplay workload, metrics and external input-to-photon
measurement are not currently available as an acceptance run. No runtime,
native parity, end-to-end latency or budget pass is claimed here.

## Delivery order and decisions

| Stage | Concrete result | Exit condition |
|---|---|---|
| 1. Native gameplay slice | Single shared tick integration, mutation/query barrier, prefab lifecycle, typed actions, weapon/reload/locks, character motor adapter, camera rig and runtime projectile pool. | The Bistro example works through existing scene/render ownership; CPU invariants and both native integration gates have explicit results. |
| 2. Inspect and save composition | Registered field/action metadata, prefab authoring, attach/configure UI, durable behavior references, Play isolation and bounded state/event/schedule inspection. | Save/reopen recreates authored composition and ownership; action rejection and state ownership are explainable in the editor. |
| 3. Improve C iteration | Separately built native gameplay module, same-schema development reload and explicit restart for schema changes. | Failed builds/reloads preserve the old session; lifecycle and platform gates pass; compile interference is measured. |
| 4. Author connections and states | Typed routes and bounded charts invoking existing C actions, using the established publication and UI owners. | Authored/native trace agreement and equivalent-work cost evidence; invalid drafts cannot replace working behavior. |
| 5. Expand where needed | Additional weapon types, AI task graphs, animation events/root motion, migrations or multiplayer. | Each extension has a concrete gameplay need, owner and acceptance workload. |

Stages should be delivered as bounded changes; Stage 1 is itself a sequence
of clock/barrier integration, weapon/actions, then character/camera and
projectile work. A small read-only diagnostic Inspector can accompany it.
The recommendation is to prove controllable gameplay before building a
general authoring tool.

Before dependent implementation, accept the new clock/event/mutation boundary
and select target hardware and population limits. This research does not
require choosing a detailed weapon design now. Recommended starting decisions
and their tradeoffs are:

- Keep the existing ECS as the runtime and add prefab composition/lifecycle.
  Actor terminology remains optional; a separate actor registry or handle
  needs a concrete responsibility before it is introduced.
- Keep platform/worker events in the existing event manager and gameplay
  actions/facts under simulation-controlled delivery. This adds a small
  explicit routing boundary while preserving tick and entity-lifetime rules.
- Retain 60 Hz simulation initially; evaluate 120 Hz only if measured input
  response warrants the additional physics/gameplay work.
- Use motor-driven movement and simulation-timed reload first; animation
  markers and root motion require further authority/time contracts.
- Use C plus Inspector/state tracing first; add executable visual charts after
  the action vocabulary is useful in real gameplay.
- Permit same-schema module reload; restart Play on schema changes until a
  complete migration mechanism is justified.
- Preserve a server-authoritative multiplayer path without implementing a
  network framework in the single-player slice.

The main engineering investment is the ownership and scheduling contract.
Once that is sound, native code, property editing and visual orchestration
can all use the same gameplay systems.

## Sources

Primary references were accessed on 2026-09-13. Undated documentation is
identified by its published version/branch where available; rolling pages
may change. Local source and accepted ADR links in the baseline section own
claims about implemented VKR behavior.

[^1]:
    Epic Games. [Gameplay Framework](https://dev.epicgames.com/documentation/en-us/unreal-engine/gameplay-framework-in-unreal-engine). Unreal Engine 5.8 documentation, undated.
[^2]:
    Epic Games. [Coding in Unreal Engine: Blueprint vs. C++](https://dev.epicgames.com/documentation/unreal-engine/coding-in-unreal-engine-blueprint-vs-cplusplus?lang=en-US). Unreal Engine 5.8 documentation, undated.
[^3]:
    Epic Games. [Gameplay Ability](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-gameplay-abilities-in-unreal-engine). Unreal Engine 5.8 documentation, undated.
[^4]:
    Epic Games. [StateTree](https://dev.epicgames.com/documentation/en-us/unreal-engine/state-tree-in-unreal-engine). Unreal Engine 5.8 documentation, undated.
[^5]:
    Epic Games. [Behavior Tree Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/behavior-tree-in-unreal-engine---overview). Unreal Engine 5.8 documentation, undated.
[^6]:
    Unity Technologies. [Event function execution order](https://docs.unity3d.com/6000.0/Documentation/Manual/execution-order.html). Unity 6.0 manual, undated.
[^7]:
    Unity Technologies. [Burst compiler](https://docs.unity3d.com/Packages/com.unity.burst@1.8/manual/index.html). Burst 1.8 documentation branch, undated.
[^8]:
    Unity Technologies. [Enableable components introduction](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/components-enableable-intro.html). Entities 1.4 branch, resolved to 1.4.8.
[^9]:
    Unity Technologies. [Entity command buffer overview](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/systems-entity-command-buffers.html). Entities 1.4 branch, resolved to 1.4.8.
[^10]:
    Unity Technologies. [About Visual Scripting](https://docs.unity3d.com/Packages/com.unity.visualscripting@1.9/manual/index.html). Visual Scripting 1.9 documentation branch, undated.
[^11]:
    Godot Engine contributors. [Nodes and Scenes](https://docs.godotengine.org/en/stable/getting_started/step_by_step/nodes_and_scenes.html). Stable documentation, undated.
[^12]:
    Godot Engine contributors. [Using signals](https://docs.godotengine.org/en/stable/getting_started/step_by_step/signals.html). Stable documentation, undated.
[^13]:
    Godot Engine contributors. [GDExtension C example](https://docs.godotengine.org/en/4.4/tutorials/scripting/gdextension/gdextension_c_example.html). Godot 4.4 documentation, undated.
[^14]:
    Godot Engine contributors. [Idle and Physics Processing](https://docs.godotengine.org/en/stable/tutorials/scripting/idle_and_physics_processing.html). Stable documentation, undated.
[^15]:
    Flecs contributors. [Systems](https://www.flecs.dev/flecs/Systems.html). Rolling project documentation, especially staging, sync points and pipelines, undated.
[^16]:
    Glenn Fiedler. [Fix Your Timestep!](https://gafferongames.com/post/fix_your_timestep/). Gaffer On Games, 2004-06-10.
[^17]:
    Jorrit Rouwe and Jolt contributors. [CharacterVirtual class reference](https://jrouwe.github.io/JoltPhysicsDocs/5.5.0/class_character_virtual.html). Jolt Physics 5.5.0 documentation, undated.
[^18]:
    Jorrit Rouwe and Jolt contributors. [Jolt Physics](https://jrouwe.github.io/JoltPhysicsDocs/5.5.0/), Deterministic Simulation and Rolling Back a Simulation. Jolt Physics 5.5.0 documentation, undated.
[^19]:
    Epic Games. [Using Live Coding to recompile Unreal Engine applications at runtime](https://dev.epicgames.com/documentation/unreal-engine/using-live-coding-to-recompile-unreal-engine-applications-at-runtime). Unreal Engine 5.8 documentation, undated.
[^20]:
    Roberto Ierusalimschy, Luiz Henrique de Figueiredo and Waldemar Celes. [Lua 5.4 Reference Manual](https://www.lua.org/manual/5.4/manual.html), introduction and garbage collection. Lua.org, version 5.4.
[^21]:
    Bytecode Alliance / Wasmtime contributors. [Security](https://docs.wasmtime.dev/security.html). Rolling Wasmtime documentation, undated.
[^22]:
    Jim Barnett et al., editors. [State Chart XML (SCXML): State Machine Notation for Control Abstraction](https://www.w3.org/TR/scxml/). W3C Recommendation, 2015-09-01.
[^23]:
    Epic Games. [MassEntity Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-entity-in-unreal-engine). Unreal Engine 5.8 documentation, undated.
[^24]:
    Unity Technologies. [Introduction to GameObjects](https://docs.unity3d.com/6000.0/Documentation/Manual/GameObjects.html). Unity 6.0 manual, undated.
[^25]:
    Unity Technologies. [Entity Component System concepts](https://docs.unity3d.com/Packages/com.unity.entities@1.4/manual/concepts-intro.html). Entities 1.4 documentation branch, undated.
