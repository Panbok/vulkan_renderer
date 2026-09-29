---
status: partial
updated: 2026-09-29
authority: adr
---

# ADR-079: C script modules, Script objects and the Player Start

## Status

Accepted (partial). The script ABI, the runtime script host, Script objects,
the Player Start and the FPS sample as a statically linked module are
implemented. The app, editor and `vkr_player` link the FPS module. No
executable loads a project's shared script library yet, and packaged games
cannot link a project's static archive. Those steps and a TypeScript layer
remain in the [script modules proposal](../proposals/script-modules.md).

## Context

`vkr_runtime` held one specific game: `runtime/src/gameplay/` (a 12-round
rifle, 5 m/s movement, rifle clip names), a `--gameplay` training platform
inside the app shell, and fixed `player`/`player_weapon` entity fields that the
scene loader parsed into `VkrScene`. A packaged game could only run that FPS.
Bakery already compiled `*.script.json` C modules into a shared library and a
static archive ([ADR-077](077-asset-build-system.md)), but nothing defined how
the engine calls them.

## Decision

### One ABI: a host-owned function table

[`vkr_script.h`](../../runtime/src/script/vkr_script.h) defines the ABI. A
module exports `vkr_script_module_<name>(const VkrScriptApi *api)` and returns
a static `VkrScriptModuleDesc`. The module reaches the engine only through
`VkrScriptApi`: entity, component and transform access, the Player Start, the
shared simulation clock, physics bodies, queries and characters, animation
playback, synchronous input observation and logging. Runtime headers supply
value types and inline math; scene, input and asset pointers are opaque to the
module. The same sources therefore link statically into an executable, or
build as a shared library that imports no engine symbols. The table and the
descriptor carry `version` and `size`; a module refuses a shorter table.

A module describes:

- **Component types.** Plain-data `VkrTypeDesc` tables for the behaviors users
  attach to entities. The host registers them as world types before any scene
  initializes ([ADR-076](076-project-object-model.md)), so documents, overlays,
  Details, Add component, presets and Cmd paths accept them.
- **State.** `state_size` bytes the host allocates once per module and zeroes
  before every session. Modules keep no session state elsewhere.
- **Hooks.** `start` and `stop` at a paused boundary; `before_physics`,
  `after_physics` and `reset` on the [ADR-073](073-native-gameplay-foundation.md)
  clock; `frame` before the scene advances and `present` after. `frame` may
  replace the elapsed time the scene advances. `present` publishes a camera
  pose and HUD text.

### The script host

[`VkrScriptHost`](../../runtime/src/script/vkr_script_host.h) owns the API
table, up to eight modules and one session on the active scene. It is the
scene's only simulation callback client and runs active modules in
registration order.

- `start` pauses the scene and resets a simulation that already advanced.
  It then starts every module. `START_IDLE` means nothing in the scene uses a
  module, and that module receives no further calls. The host installs
  callbacks only when a module is active.
- A module that fails `start` must leave nothing behind. The host then stops
  the modules it already started and reports `<module>: <message>`.
- A hook failure faults the simulation with the same prefix; the coordinator
  copies the message.
- `stop` pauses the scene, detaches the host and stops the modules in reverse
  order.

Session instance IDs increase for the process.

The shell ([`vkr_sample_runtime.c`](../../runtime/src/vkr_sample_runtime.c))
registers the modules its caller passes in `VkrSampleRuntimeConfig`:

- **Start.** The session starts the first time simulation runs or steps from
  a reset boundary. The app and player start it once the scene is ready,
  because they run from launch.
- **Reset.** The transport Reset stops the session and resets physics, so the
  next run starts again from the authored scene. This is Play/Stop semantics.
  Backspace during play still resets inside the session.
- **Failure.** A start failure keeps the scene paused and shows the reason.
- **Camera and HUD.** While a module publishes a camera, the free-camera
  controller rests and the editing camera is restored afterwards.

### Player Start

`player_start` ([`vkr_scene_types.c`](../../runtime/src/renderer/systems/vkr_scene_types.c))
is an engine component whose entity's world transform is a spawn pose. It has
one `enabled` property. `vkr_scene_player_start` resolves the scene's first
enabled, visible start in entity order, then the root World's. The editor offers
a Player Start object with a walking-person icon.

`vkr_scene_character_create` takes an optional explicit foot position. The
character spawns there and every reset returns it there, while authored TRS
keeps the designer's placement.

### Script objects

Each registered module component type is also an object kind. The Add menu,
Content's System/Objects and `create <type>` make a new entity carrying that
script. Add component attaches the same script to an existing entity. The
Outliner shows scripted entities with a code icon.

### The FPS module

[`scripts/fps`](../../scripts/fps/src/fps_module.c) is the former
`runtime/src/gameplay` client, now called through the API table. It registers
two component types:

- `fps_player`: move, crouch and jump speed, magazine and reserve.
- `fps_weapon`: the animation bone that holds the weapon.

At start:

- **Authored player.** An `fps_player` entity becomes the player. With a
  Player Start, its motor spawns at the start's position and yaw.
- **Spawned player.** Without an `fps_player` entity, a Player Start spawns a
  capsule player, which the module destroys at stop.
- **Idle.** With neither, the module is idle.
- **Sample content.** `--gameplay` adds the Bistro training platform with a
  Player Start on it.
- **Rejected scenes.** More than one player or weapon, or a weapon without a
  player, fails start with a specific message.

`fps.script.json` builds the same sources with Bakery.

The scene loader no longer parses `player` or `player_weapon`. A document that
still carries one loads with a warning and no player. The packager warns when
the startup scene has neither a Player Start nor an `fps_player`.

## Consequences

- Engine code no longer contains weapon, player or rifle-animation rules.
  Game code gets the same composition path as engine components.
- The API table becomes an ABI to maintain: new engine capabilities need
  entries, and removing or reordering an entry needs a version change.
  Header value types remain shared, so a changed struct layout still requires
  rebuilding modules.
- Starting Play in the editor now spawns and removes gameplay objects. Stepping
  starts the session as well, so a stepped scene never runs without its scripts.
- Module state is process-lifetime storage from the application arena. That
  bounds memory across Play/Stop cycles; a module's state size is fixed.

## Evidence

macOS 26.6.2, Apple M1 Pro, Release, Metal, 2026-09-29:

- `./build_release.sh`, `./build_editor.sh Release` and
  `VKR_BUILD_TARGET=vkr_player|vulkan_renderer_tester|vkr_bakery ./build.sh Release`
  build with no compiler warnings.
- `build_release/tests/vulkan_renderer_tester` passes. It covers the moved
  weapon, camera rig, input, player and player-animation suites, and
  `script_host_test.c`:
  - a failed, idle and active start; tick hooks and the frame delta;
  - a hook fault carrying the module name;
  - reset, stop and fresh state per session;
  - Player Start resolution with a disabled start and the World fallback.

  `character_test.c` checks that an explicit spawn survives reset while
  authored TRS stays put.
- `vkr_bakery cook scripts/fps/fps.script.json` builds `libfps.dylib`.
  - `nm -u` lists only libc and compiler-runtime symbols.
  - `dlopen` without the engine returns the `fps` description with both types,
    and a too-short table returns NULL.
  - The cook reports 72 `-Wmissing-braces` warnings from `lib/src/math/vec.h`
    under the script's `-Wall`.
- `VKR_AUTOCLOSE_SECONDS=45 build_release/app/vulkan_renderer --gameplay --scene
  assets/scenes/bistro.scene.json` ran without an error line.
- Headless editor `--exec` runs on Bistro:
  - Creating a Player Start at (2, 1, 3) and playing spawned `Player` at
    (2, 1, 3) (5,990 to 5,992 entities). Its body was hidden while playing and
    shown when paused, and stop removed both spawned entities.
  - An authored `fps_player` kept its authored position and spawned nothing.
  - A second `fps_player` refused to start with "fps: A scene holds at most one
    fps_player and one fps_weapon", and the simulation stayed paused.

Unavailable: Windows and Vulkan builds of the module and host, a
Windows/Vulkan packaged game, a timing comparison (no frame-time claim is made)
and interactive input.

## Alternatives considered

- **Modules link engine symbols directly.** Rejected. Windows DLLs cannot
  import from the host executable without export libraries. A binding
  generator for later languages also needs one explicit surface.
- **One `script` component listing behavior names.** Rejected. Per-behavior
  properties would need a second storage and editing path. One component type
  per behavior reuses descriptors, Details, presets and undo.
- **Keep the session attached from scene load.** Rejected. A Player Start
  would spawn while editing, and Reset could not return to the authored scene.
- **Allocate module state per session.** Rejected. The application allocator
  is an arena, so repeated Play/Stop would grow memory.

## Revisit when

A shared library loads at runtime, a packaged game links a project's archive,
a second language binds the table, or a module needs more than one session
(additive scenes or a cloned Play world).
