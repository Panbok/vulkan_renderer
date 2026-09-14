---
status: partial
updated: 2026-09-14
authority: adr
---

# ADR-073: Native gameplay on the shared scene clock

## Status

Accepted (partial). Shared ticks, ordered input admission, a C player/weapon
client, native character movement and camera rigs are implemented. The sample
application offers an opt-in playable Bistro training platform. Managed scenes
can persist a player and an attached weapon and run them through editor simulation.
Prefab assets,
a general action registry, projectile pools, network transport and visual
behavior authoring remain in the [behavior proposal](../proposals/entity-behavior-system.md).

## Context

Gameplay needs to run on every simulation tick, including catch-up ticks and
scenes with no rigid bodies. Attaching C component data can introduce archetypes
that previously compiled scene queries did not contain. The existing asynchronous
platform event manager does not provide simulation-thread ordering.

## Decision

Retain `VkrEntityId` and ECS component storage. There is no additional actor
registry or mandatory callback per entity. A client registers its component types
and attaches data outside active queries/ticks, then installs scene callbacks
that invoke its C systems. The shared coordinator owns no component pointers.

[VkrSceneSimulation](../../runtime/src/renderer/systems/vkr_scene_simulation.h)
is embedded in `VkrScene`; native physics no longer owns a separate accumulator.
The existing physics pause, single-step, time, debt and reset APIs operate on this
shared clock. Retain 60 Hz, at most eight ticks per update, supplied elapsed-time
debt and the 60-update overload pause. The application host's existing display-delta clamp remains. The opt-in player
client admits monotonic elapsed time measured after the input pump directly to
scene update, retaining hitches rather than passing the clamped display delta.

Configure callbacks at a paused, reset boundary with zero ticks and debt.
Configuration is copied, while context is borrowed until replacement or scene
shutdown. The caller releases context after detaching or shutting down the scene.
Shutdown does not invoke reset. A paused owner can detach its exact callback
context without resetting other scene state. No coordinator allocation occurs during ticks.
The scene-owning thread executes:

1. `before_physics(scene, tick, context)` with tick IDs starting at one.
2. Animation sampling, authored/kinematic synchronization, native physics,
   pose publication and native contact dispatch.
3. `after_physics(scene, tick, context)` after contact callbacks return.
4. Completed-tick publication and elapsed-debt subtraction.

Scenes with configured callbacks advance fixed animation even without bodies.
Scenes with neither bodies nor callbacks retain elapsed-time animation preview.
These two hooks do not yet implement the proposal's complete action, projectile,
contact-fanout and lifecycle schedule. Clients must not infer that target poses
remain immutable across arbitrary native intents in a hook.

A callback or native tick failure faults and pauses the coordinator. Resume does
not retry potentially committed gameplay state. Successful native reset clears
the clock and invokes an infallible caller reset callback to restore gameplay
state, cancel external actions and replace reused instance identities. Native
failure diagnostics remain available through the physics error API.

[VkrWorld structural read scopes](../../runtime/src/core/vkr_entity.h) reject
entity/component creation, destruction, registration and archetype migration
while queries or scene ticks borrow rows. Nested queries are allowed; existing
component value writes remain available. The scope is an owning-thread counter,
not a lock or permission for concurrent access. Query objects and their owning
world must outlive iteration; callbacks cannot destroy/recompile an active query.
Scene hierarchy changes and scene teardown are rejected inside gameplay hooks.
Apply structural edits between updates; a deferred structural-command queue is
still future work.

Compiled queries retain their world and archetype count even when initially
empty. Freshness is checked in every build; stale iteration asserts in Debug and
returns without visiting rows in Release. Clients explicitly rebuild stale
queries at a boundary. Scene update refreshes its retained queries before
callbacks and after authored physics synchronization. Invalidation frees old
query arrays rather than overwriting their allocation. Scene setters remain
responsible for hierarchy and render dirtiness; raw component writes cannot
replace those owner contracts.

[VkrWeaponState](../../runtime/src/gameplay/vkr_weapon.h) is caller-owned C value
state suitable for an ECS component. Initialization validates magazine capacity,
nonzero integer tick durations and a nonzero instance identity. Fire validates
ammo, reload, cooldown and independent firing locks, then asks a caller sink to
reserve every required shot output before consuming ammo or advancing sequence.
Refusal leaves weapon state unchanged. The sink cannot invoke gameplay consumers
or reenter the weapon. A successful reservation must make later publication
infallible; this primitive does not allocate projectiles or apply damage.

Reload transfers at most the missing magazine rounds from exclusively borrowed
inventory at its integer deadline. Cancellation and completion validate instance
and action tokens. Eight independent block tokens prevent one owner from clearing
another's firing lock. Tokens and shot sequences reject overflow; a reused weapon
must receive an identity distinct from any still-deliverable old action. Input
latching, automatic-fire scheduling, death/unequip cancellation and allocation of
those identities belong to the client. The weapon owns no clock or event bus.

### Input and the C player client

[input_observe](../../runtime/src/core/input.h) installs one borrowed synchronous
observer on an input context. Producers report each accepted key/button transition
and mouse delta with monotonic observation time before queueing the existing
asynchronous platform event. Frame latches still serve editor/UI clients. The
observer and producer share one thread; this adds no worker-to-gameplay dispatch.
These timestamps record engine observation, not hardware/OS event arrival time.

[VkrGameplayInput](../../runtime/src/gameplay/vkr_gameplay_input.h) holds 256
ordered typed commands by tick and sequence. Each interval `[n/60,(n+1)/60)` maps
to tick `n+1`; rapid press/release sequences remain separate records. Future
commands survive empty or catch-up frames. Adjacent absolute LOOK samples within
one tick coalesce to the latest aim, while every discrete event and the aim on
each side of it retain their order. This bounds normal high-rate mouse bursts.
The synchronous producer admits a newly observed boundary event into the next
unconsumed tick if floating-point wall-clock subtraction placed it behind that
boundary; serialized/replay command admission remains strict. Late/reordered
commands, discrete overflow and invalid values fault admission with a specific
reason instead of losing releases. The coordinator copies callback diagnostics
into scene-owned storage before publishing a fault, and the editor reports it. Pause, focus loss and
reset cancel pending/held actions; resume establishes a new wall-to-simulation
mapping and requires fresh presses. Commands have serializable fields but no
network/wire encoding or cross-platform deterministic replay guarantee.

[VkrGameplayPlayer](../../runtime/src/gameplay/vkr_gameplay_player.h) demonstrates
composition on an existing root entity: `VkrPlayerState` stores weapon, inventory,
held intent and simulation aim; scene physics owns its motor. Before physics it
completes due reloads, consumes ordered commands, performs hitscan fire, and steps
the motor. After native physics it consumes a reserved shot fact and applies a
hit impulse. Motor and animation failures retain their specific diagnostic.
One pending shot slot is sufficient for the example's six-tick fire
interval. This is a single-player client, not the proposed multi-shooter damage
fanout or projectile system. Its context stays at a stable address until shutdown
detaches the input observer and scene callbacks and releases the motor/component.

### Character and camera ownership

The pinned Jolt adapter exposes world-owned `CharacterVirtual` capsules through
bounded generation-bearing C handles. Scene records associate a character with
an existing root entity at unit scale. Cold creation/destruction require pause;
a before-physics hook supplies velocity/gravity and steps each motor at most once
per tick. The native controller handles solid-body contacts, slopes, stairs and
ground following; the client currently supplies direct five-metre/second movement
and grounded jumping. Acceleration, jump buffering, coyote time and root motion
remain policy work. Characters have no native inner-body proxy: rays, sensors and
ordinary body-contact callbacks do not treat them as targets; character/character
collision and damageable character proxies remain unimplemented.

Reset stages character replacements with the native world and publishes them only
after success. Scene entity identity remains stable while old native handles
expire. Solved translation reaches the evaluated transform/render path; authored
TRS remains unchanged. A character and rigid body cannot own the same entity.
Standing and crouched capsules are created once per native character. Ctrl
requests crouch; returned stance is authoritative. Shape replacement checks
penetration before standing, so releasing Ctrl below a ceiling remains crouched.
Both stances keep the foot anchor. The default capsule shrinks from 1.8 m to
1.08 m, camera eye height changes from 1.6 m to 0.9 m and crouched speed is
0.6 m/s. Stance changes allocate no new shapes; Jolt contact storage may grow.

[VkrCameraRig](../../runtime/src/gameplay/vkr_camera_rig.h) computes first-person,
third-person and left/right shoulder poses from a supplied target foot position
and radians-based look. It clamps pitch and optionally retracts a sphere sweep
against obstruction. The player supplies interpolated motor position and latest
render look separately from authoritative tick aim. Switching modes is immediate;
transition smoothing, view-specific arms/shadows and shoulder aim convergence
remain future work. The fallback sample uses a cube child; authored scenes hide
the player model in first person and show a camera-mounted weapon. Other views
attach that weapon to the player's evaluated animation bone.

### Character action animation

[VkrPlayerAnimation](../../runtime/src/gameplay/vkr_player_animation.h) is a
caller-owned C playback controller borrowing the scene's animation player. Cold
attachment/reset resolves named clips, preferring rifle actions with available
fallbacks; indices are not persisted. Existing scene animation remains the sole
clock. Before each animation/physics step, accepted shot sequence, reload state,
actual motor stance/ground state and horizontal speed choose interruptible
crossfades. Walk/run playback rate follows solved movement speed. Jump takeoff
enters the airborne loop immediately because the asset's start clip contains
anticipation; landing follows actual contact, without delaying physical input.

Reload completion uses the clip duration rounded up to fixed ticks (168 ticks,
2.8 seconds, for Testbed's rifle). Cancel/reset changes both gameplay and playback
state. The weapon's first-person transform includes animation-bone motion relative
to its initial rifle-idle bone, so recoil/reload move the rendered weapon too.
Other views retain the animated bone attachment. These are whole-body poses;
upper-body masks, additive recoil, individual magazine/bolt motion and separate
first-person arms remain unimplemented. An editor graph cannot concurrently own
this player's playback. A removed/replaced animation binding stops with a
specific error; reset resolves the current binding before borrowing it again.

### Playable sample

Build with `./build_release.sh`, then launch the sample with `--gameplay --scene
assets/scenes/bistro.scene.json`. This creates a training platform, a step and
three dynamic targets above the loaded Bistro scene. Only those explicitly
created bodies participate in gameplay collision; the decorative city does not
implicitly become collision geometry. Existing scene/asset owners retain all
created entities, geometry and native resources; normal scene unload releases
them after GPU idle.

Controls: WASD moves, mouse looks, left button fires, R reloads, Space jumps,
Ctrl holds crouch, V cycles camera modes, Tab/Escape captures/releases the mouse,
and Backspace
resets the player and native scene. The HUD shows ammo, reload state and hits.
An entity can declare `"player": {"yaw": -1.57079632679}` to bind the C client
without creating the sample platform. One separate entity can declare
`"player_weapon": {"bone": 45}`; its bone index addresses the sole player's
animation source. Both entities must be roots with unit scale. Parsing rejects
duplicate bindings and a weapon without a player; attachment checks the loaded
animation and post-overlay motor constraints. Managed projection preserves these
fields and asset ownership. This is a fixed native composition, not a general
script or prefab registry.

The editor attaches this client while paused. Start Simulation captures the mouse
and activates the player camera; Pause returns to the editing camera. Saved viewport
recall retains that editing camera during gameplay. Reset restores authored motor
spawn, spawn aim and inventory. Simulation still uses the loaded scene; a cloned
Play world and a visual behavior inspector remain future work.

`SceneEvaluatedTransform` stores a transient world matrix, with precedence over
native pose and hierarchy evaluation. The client acquires its player/weapon
components at attachment, then updates their existing storage. Authored TRS and
edit journals remain unchanged. `vkr_scene_update_transforms()` propagates these
presentation poses without advancing animation, simulation ticks or retained debt.
Removing an override restores the underlying authored/native pose. The runtime
updates camera basis through `vkr_camera_set_pose()` and changes visibility only
when its value changes.

## Consequences

C clients can compose and test native behavior against the scene clock without a
VM or actor hierarchy. Structural operations are explicit between ticks; callbacks
can safely borrow component rows for their duration. Arbitrary C can still violate
ownership or run unbounded work. Allocation-free coordinator/weapon code does not
establish a frame-time or input-latency result; animation and physics retain their
own costs and allocation behavior.

## Alternatives considered

A second gameplay accumulator would drift from physics and complicate failure
recovery. Platform event callbacks would add worker-thread ordering and lifetime
requirements to simulation state. A universal actor callback registry would
introduce duplicate identity and per-instance dispatch before a use case needs it.

## Revisit when

Extend the playable client with scene-owned prefab lifecycle, reserved typed
action/projectile output, character interaction/proxies and animation bindings. Native module reload and visual authoring require
schema metadata and session lifetimes before callback replacement can be relaxed.
