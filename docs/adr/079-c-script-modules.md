---
status: partial
updated: 2026-09-30
authority: adr
---

# ADR-079: C script modules, hot reload, the Script editor and the Player Start

## Status

Accepted (partial). Implemented:

- the script ABI (version 2) and the runtime script host;
- shared-library loading with hot reload that keeps state;
- Script objects and the Player Start;
- project `Scripts/` modules built by Bakery and loaded before the project's
  documents;
- the floating Script editor with highlighting, completion and diagnostics;
- the FPS sample as a statically linked module.

Remaining in the [script modules proposal](../proposals/script-modules.md):

- Packaged games do not load a project's script library yet; `vkr_player`
  still links only the FPS module.
- Headers for projects outside this source tree.
- Windows verification.
- A TypeScript layer.

## Context

`vkr_runtime` held one specific game: `runtime/src/gameplay/` (a 12-round
rifle, 5 m/s movement, rifle clip names), a `--gameplay` training platform
inside the app shell, and fixed `player`/`player_weapon` entity fields that the
scene loader parsed into `VkrScene`. A packaged game could only run that FPS.
Bakery already compiled `*.script.json` C modules into a shared library and a
static archive ([ADR-077](077-asset-build-system.md)), but nothing defined how
the engine calls them, and the editor offered no way to write them.

## Decision

### One ABI: a host-owned function table

[`vkr_script.h`](../../runtime/src/script/vkr_script.h) defines the ABI. A
module exports `vkr_script_module_<name>(const VkrScriptApi *api)` and returns
a static `VkrScriptModuleDesc`.

- **Engine access.** The module reaches the engine only through
  `VkrScriptApi`: entity, component and transform access, the Player Start, the
  shared simulation clock, physics bodies, queries and characters, animation
  playback, key state and logging.
- **Opaque handles.** Runtime headers supply value types and inline math.
  Scene, input and asset pointers are opaque to the module.
- **Two build forms.** The same sources link statically into an executable,
  or build as a shared library that imports no engine symbols.
- **Versioning.** The table and the descriptor carry `version` and `size`; a
  module refuses a shorter table.

A module describes:

- **Component types.** Plain-data `VkrTypeDesc` tables for the behaviors users
  attach to entities. The host copies each into host storage and registers the
  copy as a world type ([ADR-076](076-project-object-model.md)). Documents,
  overlays, Details, Add component, presets and Cmd paths then accept it. API
  calls that take the module's own descriptor resolve to the copy by name.
- **State.** `state_size` bytes at `state_align`, owned by the host and zeroed
  before every session start. Modules keep no session state elsewhere.
  `state_version` names the state's shape.
- **Hooks.** The engine keeps no module function pointer past one call except
  those in the description, which the host reads again after every reload.
  - `start` and `stop` run at a paused boundary.
  - `before_physics`, `after_physics` and `reset` run on the
    [ADR-073](073-native-gameplay-foundation.md) clock.
  - `input` receives every ordered transition; the host is the input's only
    observer.
  - `frame` runs before the scene advances and may replace the elapsed time
    it advances.
  - `present` runs after the scene advances and publishes a camera pose and
    HUD text.
  - `reload` runs after a hot reload that kept the running state.

### The script host

[`VkrScriptHost`](../../runtime/src/script/vkr_script_host.h) owns the API
table, up to sixteen modules and one session on the active scene. It is the
scene's only simulation callback client and runs active modules in
registration order.

- **Start.** Pauses the scene and resets a simulation that already advanced,
  then starts every module. `START_IDLE` means nothing in the scene uses a
  module, and that module receives no further calls.
- **Start failure.** A module that fails `start` must leave nothing behind.
  The host stops the modules it already started and reports
  `<module>: <message>`.
- **Hook failure.** Faults the simulation with the same prefix.
- **Stop.** Pauses the scene, detaches the host, stops modules in reverse order
  and closes superseded libraries.

**Libraries.** `vkr_script_host_load_library(name, path)` loads a byte copy of
the library at `<path>.<pid>-<serial>.loaded`. The build can then be replaced,
and a reload never reuses a cached image. A new module registers its types. A
known module reloads between frames, never from a hook:

- **Component types must keep their layout**: same names, size, alignment,
  version, flags and property names, kinds, offsets and capacities. A changed
  layout is refused and the previous code keeps running, because live
  components and documents hold the old bytes. Display metadata and hooks
  follow the new code.
- **Same state shape** (`state_size`, `state_align`, `state_version`): the new
  code takes over the running state, `reload` runs, and the old library stays
  mapped until the session stops, since state may still point into its code or
  constants. At most 32 libraries can be superseded in one session.
- **Changed state shape**: the host stops the session with the old code, swaps
  and starts it again with the new code.

`vkr_script_host_retire_libraries` retires every library module, as when a
project closes. Its types stay registered without hooks, and a later load of
the same name adopts them. Registered types have no removal, so the host's
allocator must live for the process. `vkr_scene_sync_world_types` gives
already loaded scenes the types of a module that loaded after they
initialized.

**The shell** ([`vkr_sample_runtime.c`](../../runtime/src/vkr_sample_runtime.c))
registers linked modules from `VkrSampleRuntimeConfig`. It applies each
frame's `VkrSampleScriptRequest` (retire, then loads) after the UI build and
before scene and World requests, and publishes per-load results.

- **Simulated scene.** The open primary scene once it is ready; with no
  scene open or loading, the root World, so a World-only project plays. The
  transport, Step, Reset and the physics toggle act on the same scene.
- **Start.** The session starts the first time simulation runs or steps from
  a reset boundary. The app and player start it once the scene is ready.
- **Reset.** The transport Reset stops the session and resets physics
  (Play/Stop). Backspace during play resets inside the session.
- **Failure.** A start failure keeps the scene paused and shows the reason.
- **Camera and HUD.** While a module publishes a camera, the free-camera
  controller rests and the editing camera is restored afterwards.

### Project scripts in the editor

[`editor_scripts.c`](../../editor/src/editor_scripts.c) manages a Scripts
folder. A module is `Scripts/<Name>/<Name>.script.json` with its sources: a
project's `<project>/Scripts`, or `--scripts <dir>` beside `--scene`.

- **Open.** Opening a project scans its modules. A module with a built library
  under `<workspace>/scripts/<project>/<Name>` loads it at once. A module
  without one builds synchronously first. Both loads join the frame's script
  request, which comes before the World request, so project documents see the
  types.
- **Build.** Runs `vkr_bakery cook <description> --root <module> --out
  <output> --json` on a worker, one at a time. Bakery adds the engine SDK
  directories of this source tree as system includes
  (`VKR_BAKERY_SCRIPT_SDK_DIRS`), so the engine's own warnings are not script
  diagnostics.
- **Diagnostics.** `diag` events become file, line, column and message.
- **Reload.** A library whose bytes differ from the loaded one is requested
  for reload.
- **Rebuild triggers.** Saving in the Script editor and the Bakery daemon's
  watch of each module folder and of `Scripts/` both rebuild.
- **New Script.** Writes a module from a template: one component that spins
  its entity through an evaluated transform acquired at `start`.
- **Close.** Closing or switching the project retires its libraries.

**Content** lists each module's `.c`, `.h` and `.script.json` as Script items
in Scripts. Double-click or Edit script opens a source. The toolbar's code
button asks for a new module. Script sources cannot be deleted from Content.

### The Script editor

[`editor_code.c`](../../editor/src/editor_code.c) builds a floating, resizable
Script editor window (View > Script editor).

- **Tabs.** Up to eight files, each with an unsaved marker.
- **Editing.**
  - Caret and selection by keyboard (words, lines, pages) and mouse (drag,
    double-click word, triple-click line); wheel scrolling.
  - Clipboard, and undo/redo in coalesced snapshots.
  - Two-space indentation that follows `{` and `}`, and Tab/Shift+Tab on
    selections.
  - Cmd/Ctrl+S saves, which rebuilds and reloads.
- **Highlighting.** A C tokenizer colours keywords, types, functions,
  strings, numbers, comments (including block comments across lines),
  preprocessor lines and macros.
- **Completion.** Opens after two identifier characters, after `->` or `.`,
  or on Ctrl+Space. Candidates come from the script SDK headers parsed at
  first use (API table members with their declarations, types, functions,
  macros and enum constants), the open file's declarations and identifiers,
  and C keywords.
  - After `->` or `.` it lists the members of the struct the variable points
    to. It knows `api`, `session`, `frame`, `view`, `desc` and `type`, and
    reads the file's own `Type *name` declarations.
  - The list opens below the caret, or above it where it would leave the
    window.
- **Diagnostics.** Compiler messages mark gutter lines and underline the
  token. A problems list jumps to them, and the status bar shows module state
  and the message under the caret.
- **Files changed on disk.** An unmodified tab reloads; a modified one is
  marked.

The text is drawn by a new UI primitive,
[`vkr_ui_code_view`](../../runtime/src/renderer/systems/vkr_ui_system.h).

- **Input.** The caller supplies visible lines as coloured byte spans, with
  selection, caret, underline, gutter numbers and markers per line.
- **Drawing.** It emits monospace MTSDF glyph quads per codepoint, clipped to
  the text area. It copies the lines into frame memory and hashes them for
  tile damage.
- **Focus.** A press focuses it as text, so shortcuts and Tab navigation
  leave keys to the caller.

**Cmd** gains `script.new`, `script.open`, `script.goto`, `script.type`,
`script.save`, `script.status`, `script.attach <type|none>` and `script.edit`,
and a `script` window
([ADR-075](075-editor-cmd-bar-and-evaluator.md)). Object words refresh when a
module registers more types.

### Player Start

`player_start` ([`vkr_scene_types.c`](../../runtime/src/renderer/systems/vkr_scene_types.c))
is an engine component whose entity's world transform is a spawn pose.

- **Property.** One `enabled` property.
- **Resolution.** `vkr_scene_player_start` resolves the scene's first
  enabled, visible start in entity order, then the root World's.
- **Editor.** A Player Start object with a walking-person icon at its
  position. Outside Play, every visible start in the scene and the World
  draws the capsule the character spawns as, from
  `vkr_physics_character_default`, with its -Z facing.

`vkr_scene_character_create` takes an optional explicit foot position. The
character spawns there and every reset returns it there, while authored TRS
keeps the designer's placement.

### Script slot and Script objects

An entity's script is the first script module component it carries; there is
no separate slot component, so nothing can disagree with the component that
holds the script's fields. Every placed entity shows a Script row in Details
above its component sections, and right-click offers the same choices in a
Script submenu:

- the component types of the loaded, unretired modules, the current one
  checked;
- New script, Edit script (the module's `<Name>.c`) and Remove script.

Choosing a script adds its component with defaults, or replaces the current
one in a single undo entry that restores the replaced values
(`vkr_scene_edit_replace_component`). Add component also lists the loaded
script types under Scripts, with New script, and adds one beside any others.

The Add menu and Content's System/Objects list one Script object: an empty
entity named Script whose Script picker opens beside it in the Scene once it
exists. It shows the empty-object icon until a script is chosen, then the code
icon. `create <type>` still
makes an entity carrying a named script type. The Outliner shows scripted
entities with a code icon. Double-clicking a scripted object in Content or
the Outliner opens its source: a project module's `<Name>.c`, or for a module linked into
the editor, such as `fps`, `scripts/<name>/src/<name>_module.c` in this
repository with a notice that saved changes apply after a rebuild. An object
whose script has no source shows a notice instead.

### The FPS module

[`scripts/fps`](../../scripts/fps/src/fps_module.c) is the former
`runtime/src/gameplay` client, now called through the API table. It receives
input through its `input` hook. It registers two component types:

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

The scene loader no longer parses `player` or `player_weapon`. A document that
still carries one loads with a warning and no player. The packager warns when
the startup scene has neither a Player Start nor an `fps_player`.

## Consequences

- Engine code no longer contains weapon, player or rifle-animation rules.
  Game code gets the same composition path as engine components, and a
  project's code changes without restarting the editor.
- The API table becomes an ABI to maintain: new engine capabilities need
  entries, and removing or reordering an entry needs a version change.
  Header value types remain shared, so a changed struct layout still requires
  rebuilding modules.
- Hot reload trusts `state_version`: a module that changes its state struct
  without bumping it runs new code over old bytes. Component layout changes
  need the project reopened.
- Superseded libraries stay mapped until Stop, bounded at 32 per session.
- Registered types and their host copies live for the process. A later
  project whose module reuses a retired module's type name must match that
  layout.
- Starting Play in the editor spawns and removes gameplay objects. Stepping
  starts the session as well, so a stepped scene never runs without its
  scripts.

## Evidence

macOS 26.6.2, Apple M1 Pro, Release, Metal, 2026-09-29 and 2026-09-30:

- `./build_release.sh`, `./build_editor.sh Release` and
  `VKR_BUILD_TARGET=vkr_player|vulkan_renderer_tester|vkr_bakery ./build.sh Release`
  build with no compiler warnings.
- `build_release/tests/vulkan_renderer_tester` passes.
  - It covers the moved weapon, camera rig, input, player and player-animation
    suites.
  - `script_host_test.c` covers start (failed, idle and active), tick hooks and
    the frame delta, a hook fault with the module name, reset and stop, fresh
    state per session, and Player Start resolution with the World fallback.
  - `script_reload_test.c` loads [`reload_probe.c`](../../tests/scripts/reload_probe.c),
    built as four `MODULE` libraries that import no symbols.
    - A code-only reload keeps state (3 ticks, then 20) and runs `reload`.
    - A new `state_version` restarts on zeroed state.
    - A changed field is refused while the old code keeps counting.
    - Retiring keeps the type registered, and a later load adopts it.
  - `character_test.c` checks that an explicit spawn survives reset.
- `vkr_bakery cook scripts/fps/fps.script.json` builds `libfps.dylib` without
  warnings.
  - `nm -u` lists only libc and compiler-runtime symbols.
  - `dlopen` returns the `fps` description, and refuses a too-short table.
- Headless editor on Bistro (`--scene assets/scenes/bistro.scene.json`):
  - **Player Start:** playing spawned `Player` at (2, 1, 3), 5,990 to 5,992
    entities. Its body was hidden in first person, and stop removed both
    spawned entities. This passed again on ABI version 2.
  - **Authored player:** an `fps_player` kept its authored position.
  - **Two players:** a second `fps_player` refused to start with its message.
  - **Script lifecycle** (`--scripts`):
    1. `script.new Spinner` built and loaded the module, and `create spinner`
       made an entity with `speed` 0.25.
    2. During Play, an external code edit reported "Reloaded; state kept".
    3. A state struct change with a `state_version` bump reported
       "Reloaded; its state changed, so the simulation restarted".
    4. A syntax error reported "Build failed with 1 error; the previous code
       keeps running", with 1 diagnostic, and Play kept running.
- Windowed editor on Bistro, captured with `screencapture`:
  - highlighting;
  - the gutter marker and underline at `spinner.c:101:10`;
  - the problems row and status;
  - `session->api->ra` completing `raycast` with its declaration;
  - `session->api->s` listing ten `VkrScriptApi` members above the caret.
- Minimal managed project (empty World, headless):
  - `script.new Door` wrote `projects/<id>/Scripts/Door` and built into
    `<workspace>/scripts/<id>/Door`.
  - `create door` worked.
  - A `door` component saved with `speed` 1.5 read 1.5 after reopening, so
    the type registered before the World loaded.

Editor feedback round, macOS 26.6.2, Apple M1 Pro, 2026-09-30:

- `./build_editor.sh Release`, and `vulkan_renderer_tester` and `vkr_player`
  in Debug, build with no compiler warnings.
- `build_release/tests/vulkan_renderer_tester` passes all 88 suites. The Debug
  (AddressSanitizer) tester passes every suite except `run_script_reload_tests`,
  where ASan faults describing a global of the newly loaded probe after the
  restart closed earlier copies. The suite passes under
  `ASAN_OPTIONS=report_globals=0`; neither the host nor the probe changed.
- Managed project with a World-only Door script, headless and windowed:
  - `create script; script.attach door`, undo, redo, `script.attach none` and
    undo each left exactly the expected component. With a second module
    `Spin`, `script.attach spin` on Door replaced `door`; undo restored `door`
    and redo `spin`.
  - `script.edit` opened the module source. The Details picker and the
    Outliner's Script submenu listed FPS player, FPS weapon, Spin and Door
    with Door checked; `ui.key` Down five times, Right and Down moved into
    the submenu.
  - A World Player Start at (4, 1, 2) spawned `Player` there on Play with no
    scene open; its capsule drew at the start.
  - Placing Point Light from System/Objects switched Content to the World's
    folder with the light selected.
- Bistro Play: `Player` spawned at (2, 1, 3), the body was hidden and stop
  returned the scene to 5,990 entities.

Unavailable:

- Windows and Vulkan builds of the host, scripts and editor.
- A packaged game running a project's library.
- Interactive keyboard and mouse editing; only the Cmd hooks drove the editor.
- Timing: no frame-time claim.

## Alternatives considered

- **Modules link engine symbols directly.** Rejected. Windows DLLs cannot
  import from the host executable without export libraries. A binding
  generator for later languages also needs one explicit surface.
- **One `script` component listing behavior names.** Rejected. Per-behavior
  properties would need a second storage and editing path. One component type
  per behavior reuses descriptors, Details, presets and undo.
- **Keep the session attached from scene load.** Rejected. A Player Start
  would spawn while editing, and Reset could not return to the authored scene.
- **Hot reload by restarting Play.** Rejected at the user's choice: keeping
  state speeds iteration. The restart path remains for changed state shapes.
- **Module-owned input observers.** Rejected: an observer pointer into an
  unloaded library would dangle; the host observes and forwards.
- **Label widgets per token for highlighting.** Rejected: one text node has
  one colour, and a file's tokens exceed the frame's node budget.

## Revisit when

A packaged game loads a project's library, a second language binds the table,
a module needs more than one session (additive scenes or a cloned Play world),
or component layout changes should migrate live data.
