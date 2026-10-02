---
status: partial
updated: 2026-10-02
authority: adr
---

# ADR-079: C script modules, the script SDK, hot reload, the Script editor and the Player Start

## Status

Accepted (partial). Implemented:

- the script SDK (`sdk.h`, version 5) and the runtime script host, with
  temp, scoped and persistent lifetimes released through ledgers, owner and
  timed lifetimes, behaviors per entity with destroy hooks, script instances
  per attached container, structural edits in fixed updates queued until
  the tick ends, and tasks on worker threads;
- shared-library loading with hot reload that keeps instance data, with
  reloads copied and opened on a worker;
- models spawned by scripts loading on the resource system's workers;
- Script assets attached to objects, the authoring macros and the Player
  Start;
- project `Scripts/` packages, modules and libraries with dependencies,
  built by Bakery into one project library loaded before the project's
  documents, with a project's first build on a worker;
- the floating Script editor with highlighting, completion and diagnostics;
- the FPS sample as a statically linked module;
- the script SDK headers staged as one include root and shipped in the
  editor distribution;
- packaged games that ship the project's script library and load it in
  place before their World.

Remaining in the [script modules proposal](../proposals/script-modules.md):

- Exports between modules.
- A TypeScript layer.

## Context

`vkr_runtime` held one specific game: `runtime/src/gameplay/` (a 12-round
rifle, 5 m/s movement, rifle clip names), a `--gameplay` training platform
inside the app shell, and fixed `player`/`player_weapon` entity fields that the
scene loader parsed into `VkrScene`. A packaged game could only run that FPS.
Bakery already compiled `*.script.json` C modules into a shared library and a
static archive ([ADR-077](077-asset-build-system.md)), but nothing defined how
the engine calls them, and the editor offered no way to write them.

The first table (ABI version 2) left scripts to release every entity,
character and runtime component by hand, handed them raw animation players,
engine structs and `const char **` errors, made them include runtime and
renderer headers, and ran one session on the played scene only, so nothing
could live with the World while zones came and went.

## Decision

### One SDK: `sdk.h` over a host-owned table

[`sdk.h`](../../sdk/sdk.h) is everything a module may use. It includes only
the foundation's `defines.h` and inline math.
[`vkr_script_sdk.cmake`](../../cmake/vkr_script_sdk.cmake) lists `sdk.h` and
the seven foundation headers it reaches (`defines.h`, `vkr_pch.h` and
five math headers) and stages them flat into
one include root, `<build>/script_sdk`. That root is the only engine include
modules compile against:

- **Bakery** uses `sdk` beside its executable when it holds `sdk.h`, else
  the staged root of the build tree that produced it
  (`VKR_BAKERY_SCRIPT_SDK_DIR`). The root joins each object's recipe.
- **The editor distribution** installs the staged root as `sdk/` beside the
  programs (ADR-078), so projects compile outside the repository.
- **Script completion** reads the same headers through
  `vkr_editor_script_sdk_dir`, resolved like Bakery's.
- **The test probes** compile from the staged root alone, so a header the
  list misses fails the build. A module exports
`vkr_module_<Name>(sdk_version)` and returns a static `VkrModuleDesc`, or
NULL when the version differs; there is no compatibility with older modules.

- **Context and calls.** Every hook receives an opaque `VkrCtx`. The SDK's
  functions (`vkr_spawn`, `vkr_set_render_pose`, `vkr_raycast`,
  `vkr_character_move`, `vkr_anim_blend`, `vkr_hud` and the rest) are static
  inline calls through a private `VkrSdkTable` the host fills, so a module
  imports no engine symbols and links statically or as a shared library.
- **Handles and SDK types.** Entities are generational `VkrEntity` handles
  whose world id names their container, so one handle works across
  containers and a stale one fails the call. Physics, characters, input,
  animation samples and transforms use SDK value types
  ([`vkr_script_sdk.c`](../../runtime/src/script/vkr_script_sdk.c) converts or
  statically asserts identical layouts). Scene, input, asset and animation
  player pointers never reach a module.
- **Hooks.** Hooks return void and take the context and the script's own
  data: module hooks `start`, `stop`, `update`, `late_update`, `fixed_update`,
  `late_fixed_update` and `input`; behavior hooks `(ctx, self, component)`,
  which add `destroy`.
  - A behavior's `destroy` runs when its entity is destroyed during a
    session (by a script, the editor or a destroyed parent), before anything
    is torn down; `stop` follows and the scope is released. The scene tells
    the host through its one destroy observer
    (`vkr_scene_observe_destroy`). Ending Play or unloading a container runs
    `stop` only.
  - `vkr_fail` fails the running hook. A failed start ends the session; a
    failed tick faults the simulation; another failed hook pauses it.
  - `vkr_disable` makes an instance idle; only `stop` runs afterwards.
  - The camera and HUD are calls (`vkr_set_camera`, `vkr_hud`) from
    `late_update`; a later module's camera wins.
  - `vkr_set_time_step` from `update` replaces the frame's elapsed time, as
    a module that owns the input clock does.
  - The host refreshes changed transforms and child lists before reads and
    after frame hooks; there is no `update_transforms` call.
- **Structural edits in ticks.** In `fixed_update` and `late_fixed_update`,
  structural calls (spawn, destroy, models, characters, bodies, shapes,
  names, parents, visibility, render poses, components and runtime state)
  are queued with copies of their arguments and replay in order through the
  same SDK calls right after the tick, from the simulation's `after_tick`
  callback, where structural edits are allowed again.
  - A spawn reserves its entity ID at once (`vkr_scene_reserve_entity`), so
    the handle is valid inside the tick: it reads as alive, calls on it are
    queued, and its transform and components appear after the tick. The host
    keeps 256 IDs free in every attached world before each frame, so a tick
    never grows an entity directory.
  - `vkr_state_add` in a tick returns the queued value, which the tick's end
    stores.
  - Edits queued by a scope that ends before the replay are dropped, except
    destroys, and their reservations return.

### Tasks

`vkr_task_run(ctx, fn, data, size)` runs `fn` on a `VkrJobSystem` worker
over the task's own copy of up to `VKR_TASK_DATA_MAX` (64 KB) bytes of
`data`. A task:

- **Has no SDK.** Its function receives only the copy, so it cannot call the
  engine. It may also touch memory nothing else changes until the task is
  taken, such as a global result buffer.
- **Belongs to its scope.** It is recorded in the calling scope's ledger.
  When the scope ends, the host waits for it, running it on the frame thread
  if no worker has started it, and drops its result.
- **Returns through a take.** `vkr_task_take` copies the data back and
  forgets the task once it has finished; `vkr_task_wait` waits first, so a
  tick can fan work out and join it.
- **Outlives no code.** A kept-state reload leaves the old library mapped
  until Stop, and Stop waits for every task and for every worker job to let
  go of it before superseded libraries close.

Without a job system in the session, as in tools and tests, or when no job
can be queued, a task runs inside `vkr_task_run`. Tasks run at high
priority among the engine's general jobs.

### Lifetimes and the ledger

| Lifetime | Use | Released |
| --- | --- | --- |
| Temp | `vkr_temp_alloc`, `vkr_temp`, `vkr_format` | When the hook returns: one host arena rewinds after every hook |
| Scoped | What a behavior hook acquires | When the entity stops carrying the component, the entity dies or the instance ends |
| Persistent | Module data and what module hooks acquire | When the instance ends |

Every acquiring call (`vkr_spawn`, `vkr_spawn_model`, `vkr_character_create`,
`vkr_state_add`, a first `vkr_set_render_pose`, `vkr_task_run`) records the
resource in the calling scope's ledger; an explicit release forgets it. Ending a scope
releases what remains newest first, skipping resources already gone, so a
failed start leaves nothing behind. `vkr_destroy` destroys descendants too.
A ledger that fills up first drops records of entities already gone, so
spawns that expire keep a long session's ledgers small.

A spawn can end earlier than its scope:

- **Owner.** `VkrSpawnDesc.owner` names an entity in any attached container;
  the spawn is destroyed with it, through the destroy observer.
- **Lifetime.** `VkrSpawnDesc.lifetime` is simulated seconds; the clock stops
  while the simulation pauses. Expired spawns are destroyed after each tick
  and at each frame start, running their destroy hooks.
- **Transient.** Everything scripts spawn carries the scene's runtime-only
  `SceneTransient` tag (`vkr_scene_set_transient`), and saving skips edits to
  tagged entities, so an edit made to a spawned object during Play neither
  saves it nor fails the save.
Ledgers, bindings and instance data live in the host's own `VkrDMemory`;
registered types live in the allocator the shell passes, for the process.

### The script host

[`VkrScriptHost`](../../runtime/src/script/vkr_script_host.h) owns the SDK
table, the registered modules and their libraries in growable tables, and
one session. A session attaches the active
container (the played scene, or the World when it plays alone) and the root
World ([ADR-076](076-project-object-model.md)).

- **Instances.** A container-scoped module runs one instance per attached
  container; a World-scoped module (`.scope = VKR_SCOPE_WORLD`) runs one on
  the World for the whole game. Each instance owns zeroed data and a ledger.
  `vkr_container_self`, `_active` and `_world` name containers; `vkr_spawn`
  and `vkr_find_in` take one.
- **Behaviors.** Each frame, an instance whose containers' world revisions
  changed matches its behaviors with the entities carrying their components:
  new entities start, gone ones stop and release their ledgers. A World
  instance's behaviors cover every attached container.
- **Order.** Instances start in module order, World first; module hooks run
  before their behaviors' hooks; instances end in reverse.
- **Start.** Pauses the active scene and resets a simulation that already
  advanced, runs every `start`, then starts behaviors, then installs the
  simulation callbacks when an enabled instance has tick hooks and the input
  observer when one has `input`. A failure ends everything started and
  reports `<module>: <message>`.
- **Reset.** A native reset (Backspace) restarts the instances at the next
  frame boundary, where structural edits are allowed, and restores the run
  state.
- **Detach.** `vkr_script_host_detach` ends a container's instances and the
  World behaviors on its entities before the container unloads; the shell
  calls it before the World unloads. Detaching the active container stops
  the session.
- **Stop.** Pauses the scene, detaches the host, ends instances in reverse
  order and closes superseded libraries.
- **Tools.** `vkr_script_host_open_context` gives tests and tools a context
  bound to one scene outside any module, and
  `vkr_script_host_bind_animation` lets it drive a caller-owned animation
  player.

**Libraries.** `vkr_script_host_load_library(name, path)` loads one module's
library through `vkr_module_<name>`; `vkr_script_host_load_project(name,
path)` loads a project library and every module its `vkr_project_modules`
lists. Either loads a byte copy at `<path>.<pid>-<serial>.loaded`, so the
build can be replaced and a reload never reuses a cached image. Up to eight
libraries are loaded at once. A new module registers its types. A known
module reloads between frames, never from a hook:

- **Component types keep their names and count.** Adding, removing or
  renaming a module's component types is refused and the previous code keeps
  running. Display metadata and defaults follow the new code; the new
  descriptors bind to the registered copies.
- **Changed fields migrate.** A type whose size, alignment or field names,
  kinds or offsets changed keeps its registered copy, which takes the new
  layout, and its values move with it before any hook runs. The application
  installs the mover (`vkr_script_host_set_migrator`), because it owns the
  scenes and journals; without one, as in tools, the change is refused as
  before. The shell's mover:
  - converts each value with `vkr_type_migrate`: the new defaults, then each
    field the old layout had by name. The same kind copies; scalar numbers
    convert among BOOL, I32, U32, F32, ANGLE and ENUM; float vectors keep
    their shared components; strings truncate; numbers clamp to new bounds.
  - moves every loaded container's components with
    `vkr_scene_migrate_world_type`: the new layout registers as a new ECS
    component, `<name>#<n>`, that replaces the old one on each entity, and
    the old id stays registered and unused, so every migration costs a scene
    one of its 256 component ids;
  - converts the bytes undo entries hold, in every container's journal, and
    the frame's pending scene edit (`vkr_scene_edit_migrate_type`), so undo
    and an edit in flight write the new layout.

  The editor's Content panel reads `presets.json` again after every applied
  load (`vkr_editor_scripts_load_serial`), so presets of a migrated type
  hold the new layout.

  A prepared reload waits while a scene, the World or an added scene is
  still loading, because the loader parsed its components with the layout
  registered then. Script types carry `VKR_TYPE_FLAG_TOLERANT`: documents
  saved with an earlier build load with the members the type dropped
  skipped and a member whose kind changed at its default, so a project
  saved before a field changed still opens after a restart.
- **Same data shape** (`data_size`, `data_align`, `data_version`, scope and
  behavior components): the new code runs with the running instances' data,
  and the old library stays mapped until the session stops, since data may
  still point into its code or constants. At most 32 libraries can be
  superseded in one session.
- **Changed data shape**: the host stops the session with the old code, swaps
  and starts it again with the new code.

**Preparing off the frame thread.** A load is three steps, which
`vkr_script_host_load_library` and `_load_project` run back to back:

- `vkr_script_host_prepare` picks the copy's unique path on the frame
  thread.
- `vkr_script_prepare_run` copies, opens and lists the library on any
  thread. It reads nothing of the host, so frames and hooks continue
  meanwhile; opening runs the library's C runtime startup.
- `vkr_script_host_commit` checks and applies it between frames, or
  `vkr_script_host_discard` closes it and removes the copy.

**In place.** `vkr_script_host_prepare(..., in_place)` opens the library
where it lies instead of a byte copy, and closing it removes nothing. A
packaged game loads this way: its folder may be read-only, a signed macOS
application must not change, and it never reloads.

**Packaged games.** `vkr_bakery bundle` builds the project's `Scripts/`
into the package and names the library in `game.script_library` (ADR-078).
On its first frame the [player](../../player/src/player_ui.c) asks the shell
to load it in place, flagged `project` and `in_place` in
`VkrSampleScriptLoad`, before its World and startup scene. The FPS sample
stays linked into `vkr_player` for packages that use it.

A project library reloads atomically. The host checks every listed module
first; one refused module, a changed component type list or a type name
another module owns, refuses the load and every module keeps its previous
code. Otherwise
new modules register, known ones swap, and modules the library no longer
lists retire. The session keeps its data only when no module was added or
removed and every one kept its data shape; otherwise it restarts.

`vkr_script_host_retire_libraries` retires every library module, as when a
project closes. Its types stay registered without defaults, and a later load
of the same name adopts them. Registered types have no removal, so the
host's allocator must live for the process. `vkr_scene_sync_world_types`
gives already loaded scenes the types of a module that loaded after they
initialized.

**The shell** ([`vkr_sample_runtime.c`](../../runtime/src/vkr_sample_runtime.c))
registers linked modules from `VkrSampleRuntimeConfig`. It applies each
frame's `VkrSampleScriptRequest` (retire, then loads) after the UI build and
before scene and World requests, and publishes per-load results.

- **Requests.** A load flagged `project` goes through
  `vkr_script_host_load_project`; any load that did not fail gives already
  loaded scenes the new types.
  - A library's first load runs at once, because the project's documents
    load after it in the same frame and need its types.
  - A reload of an open library prepares on a `VkrJobSystem` worker (on the
    frame thread when no job can be queued) and commits at the first frame
    start after the worker finishes. Its result is published then.
  - A newer request for a library still preparing replaces any queued one
    and starts once the current one commits.
  - Retiring and shutdown wait for preparations still running and discard
    them.
- **Simulated scene.** The open primary scene once it is ready; with no
  scene open or loading, the root World, so a World-only project plays. The
  transport, Step, Reset and the physics toggle act on the same scene.
- **Start.** The session starts the first time simulation runs or steps from
  a reset boundary, with the World attached beside the played scene. The app
  and player start it once the scene is ready.
- **Reset.** The transport Reset stops the session and resets physics
  (Play/Stop).
- **Failure.** A start failure keeps the scene paused and shows the reason; a
  later hook failure stops the simulation with the host's message.
- **Camera and HUD.** While a module publishes a camera, the free-camera
  controller rests and the editing camera is restored afterwards.

### Project scripts in the editor

[`editor_scripts.c`](../../editor/src/editor_scripts.c) manages a Scripts
folder: a project's `<project>/Scripts`, or `--scripts <dir>` beside
`--scene`. A package is `Scripts/<Name>/<Name>.script.json` with its sources
([`vkr_bakery_script.c`](../../tools/bakery/vkr_bakery_script.c)):

- **Kinds.** `"kind": "module"` (the default) defines `vkr_module_<Name>`, so
  its name is a C identifier. `"kind": "library"` has no entry point and may
  be headers only; it holds code other packages share.
- **Dependencies.** `"dependencies": ["Common"]` names packages whose headers
  a package includes: their include roots, or their folders, and those of
  everything they depend on. A cycle or an unknown name fails the build with
  a diagnostic on the description.
- **One library per project.** Every package of the folder compiles to cached
  objects that link into one project library. Library code and state exist
  once, and the project pays one load and one C runtime startup however many
  packages it has. A compile error in any package keeps the previous project
  library running.

The manager drives it:

- **Open.** Opening a project scans its packages. A built project library
  under `<workspace>/scripts/<project>` joins the frame's script request at
  once, which comes before the World request, so project documents see the
  types. Without one, the first build starts on the worker while frames
  continue; `vkr_editor_scripts_settling` stays true until the build fails
  or its load reports, and the project holds its World request, showing it
  as loading, until then.
- **Build.** Runs `vkr_bakery scripts <Scripts> --name project --root
  <Scripts> --out <output> --json` on a worker, one at a time. Bakery
  generates `project_modules.c`, whose `vkr_project_modules` lists each
  module's entry, and rewrites it only when the list changes, so an
  unchanged project is fully cached. It adds the script SDK root as a system
  include, so the engine's own warnings are not script diagnostics. On Windows the library
  links with its own static C runtime and the default DLL entry, which
  initializes it. Modules therefore free what they allocate themselves and
  pass no allocation or `FILE` across the SDK. The runtime's DLL startup adds
  about 105 KB per library (the empty template); the printf family adds
  about 35 KB more. `vkr_bakery cook <Name>.script.json` still builds one
  package without dependencies into its own library.
- **Diagnostics.** `diag` events become file, line, column and message, and
  belong to the package owning the file. Every package shows the project
  library's build and load state.
- **Reload.** A project library whose bytes differ from the loaded one is
  requested for reload.
- **Rebuild triggers.** Saving in the Script editor and the Bakery daemon's
  watch of each package folder and of `Scripts/` both rebuild.
- **New Script.** Writes a module from a template: one component with a
  `speed` field and a behavior whose `start`, `update`, `fixed_update` and
  `stop` hooks have empty bodies.
- **Close.** Closing or switching the project retires its library.

**Content** lists each package's `.c`, `.h` and `.script.json` as Script items
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
  or on Ctrl+Space. Candidates come from `sdk.h` and the foundation's math
  headers parsed at first use (functions, including inline definitions, with
  their declarations, types, struct members, macros and enum constants), the
  headers of the open file's package and of the packages it depends on,
  read again when the package list changes or a file is saved, the open
  file's declarations and identifiers, and C keywords.
  - After `->` or `.` it lists the members of the struct the variable points
    to. It knows `event`, `hit`, `body`, `motor`, `move` and `type`, and
    reads the file's own `Type *name` declarations, such as a hook's
    component parameter.
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

### Scripts are assets attached to objects

A script is an asset, not an object of its own. Content's Scripts folder
shows one Script asset per project module; double-clicking it opens the
module's `<name>.c`. Dragging it onto an object, in the Scene or on an
Outliner row, attaches the module's first component type to that object as
one more of its components. Dropped on empty space in the Scene, it adds an
object named after the script that runs it, placed where the pointer meets
the ground. A Scene drop picks under the pointer as a right click does
(`VkrSamplePickRequest`, answered through the frame's context fields).

An entity's script slot is the first script component it carries; there is
no separate slot component, so nothing can disagree with the component that
holds the script's fields. Every placed entity shows a Script row in Details
above its component sections, and right-click offers the same choices in a
Script submenu:

- the component types of the loaded, unretired modules, the current one
  checked;
- New script, Edit script (the module's `<Name>.c`) and Remove script.

Choosing a script adds its component with defaults, or replaces the current
one in a single undo entry that restores the replaced values
(`vkr_scene_edit_replace_component`). Remove script, or the section's trash
button, detaches it. New script from an object's slot or menus makes the
object wait for the module: once the new module loads, its first type
becomes the object's script. Add component also lists the loaded script
types under Scripts, with New script, and adds one beside any others.
Scripts are tags on an object: its icon and kind in the Outliner, Details,
Content and the Scene stay what the object is, and the Outliner names each
script in a chip on its row. `create <type>` still makes an entity carrying
a named script type.

Double-clicking a scripted object in Content or the Outliner opens its
source: a project module's `<Name>.c`, or for a module linked into the
editor, such as `fps`, `scripts/<name>/src/<name>_module.c` in this
repository with a notice that saved changes apply after a rebuild. An object
whose script has no source shows a notice instead.

### Authoring macros

`sdk.h` declares a component, its behavior and the module once, instead of a
struct, a field table, a defaults function, a descriptor, typed hook
adapters, a module description and an entry point written by hand:

```c
VKR_COMPONENT(Door, door, "Door",
              VKR_FIELD(F32, speed, "Speed", 0.25f, .unit = "turns/s")
              VKR_FIELD(BOOL, locked, "Locked", false_v))

static void door_update(VkrCtx *ctx, VkrEntity self, Door *door,
                        float32_t dt) {
}

VKR_BEHAVIOR(door, .update = door_update)
VKR_MODULE(Door, VkrNoData, VKR_EXPORT_BEHAVIOR(door))
```

- `VKR_FIELD(kind, name, label, default, options...)`: one saved field. The
  kinds are `BOOL`, `I32`, `U32`, `F32`, `ANGLE` (radians), `VEC2`, `VEC3`,
  `VEC4`, `QUAT`, `COLOR`, `DIRECTION` and `ENUM` (with `.names`). Options
  are `VkrFieldDesc` designators; the host converts the fields to property
  descriptors.
- `VKR_COMPONENT(Type, name, label, FIELDS)`: the `Type` struct,
  `name_type()`, `name_get(ctx, entity)` and `name_find(ctx, out, capacity)`.
  A module split across files uses `VKR_COMPONENT_DECLARE` in a header and
  `VKR_COMPONENT_DEFINE` in one file.
- `VKR_BEHAVIOR(name, hooks...)`: typed per-entity hooks for the component.
- `VKR_MODULE(Name, Data, EXPORTS, options...)`: the exported
  `vkr_module_Name`, with `VKR_EXPORT_COMPONENT` and `VKR_EXPORT_BEHAVIOR`
  exports, `.scope`, `.data_version` and typed module hooks.

The macros walk sequences with two alternating macros, so lists have no
length limit. Typed hooks reach the erased description through generated
trampolines, so no function is called through a pointer of another type.
Each descriptor is built once, on first use, in writable static storage the
host binds to its registered copy.

A render pose (`vkr_set_render_pose`) replaces the rigid-body pose until the
scope that set it ends. `vkr_spawn_model`, `vkr_anim_blend` and
`vkr_has_visual` serve the default mannequin
([ADR-080](080-default-mannequin-character.md)). `vkr_spawn_model` only
requests the model: its files load on the resource system's workers and its
nodes appear at a later frame start, when `vkr_model_state` turns
`VKR_MODEL_READY`. Releasing it while it loads cancels the loads.

### The FPS module

[`scripts/fps`](../../scripts/fps/src/fps_module.c) is the former
`runtime/src/gameplay` client, written against `sdk.h` as a World-scoped
module. It receives input through its `input` hook, owns the input clock
through `vkr_set_time_step`, and registers two component types:

- `fps_player`: move, walk, crouch and jump speed, magazine and reserve,
  camera mode, and the third-person orient-to-movement, acceleration and turn
  rate.
- `fps_weapon`: the animation bone that holds the weapon.

At start:

- **Authored player.** An `fps_player` entity of the played container
  becomes the player. With a Player Start, its motor spawns at the start's
  position and yaw. Its own animation, mesh or shape is its body; without one
  it gets the default mannequin.
- **Spawned player.** Without an `fps_player` entity, a Player Start spawns a
  capsule player with the default mannequin as its body
  ([ADR-080](080-default-mannequin-character.md)) in the played container.
  The instance's ledger releases both, its character, runtime state and
  render poses at stop or when a start fails part way.
- **Idle.** With neither, the module disables itself.
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
- Modules release nothing by hand unless they want to end a resource early:
  the ledger covers failed starts, removed components, Stop and container
  unloads. A module must still undo edits to authored entities it does not
  own, such as the FPS module's visibility change, in `stop`.
- The SDK is the only surface: new engine capabilities need table entries
  and inline calls, and `VKR_SDK_VERSION` changes refuse older modules
  outright. Foundation math types stay shared, so a changed `Vec3` layout
  still requires rebuilding modules.
- Hot reload trusts `data_version`: a module that changes its data struct
  without bumping it runs new code over old bytes. A component's fields
  migrate by name: renaming a field drops its value to the default, and a
  value the new field's kind cannot take keeps the default.
- Structural edits in fixed updates take effect after the tick, not inside
  it: a spawned projectile has no transform or body until the next tick, and
  a destroyed entity stays readable until the tick ends.
- Superseded libraries stay mapped until Stop, bounded at 32 per session.
- A project builds and reloads as one unit: a compile error or a refused
  module in one package holds back every package's changes until it is
  fixed. Splitting a project into a few separately loaded groups is the way
  to isolate failing work.
- Registered types and their host copies live for the process. A later
  project whose module reuses a retired module's type name must match that
  layout.
- Starting Play in the editor spawns and removes gameplay objects. Stepping
  starts the session as well, so a stepped scene never runs without its
  scripts. Backspace restarts every instance, so spawned objects are
  recreated.

## Evidence

Windows 10, Ryzen 5 2600, Radeon RX 6700 XT, Vulkan, clang 20, 2026-10-02
(the SDK, version 3; tasks with version 4; model requests with version
5):

- `build.bat Debug` (tester, `vkr_script_fps`, `vkr_bakery`),
  `build_editor.bat Debug` and `Release`, `build_release.bat` and
  `VKR_BUILD_TARGET=vkr_player build.bat Release` build with no warnings
  beyond Bakery's existing `strdup` and `getenv` deprecations.
- The Debug and Release testers pass all 88 suites.
  - `script_host_test.c`:
    - A failed start reports `life: refused 7` and releases the entity it
      spawned.
    - A disabled instance is inactive and installs no callbacks.
    - `update` sets the elapsed time (two ticks); temp memory is zeroed and
      rewinds after the hook; a spawn in a fixed update is refused.
    - A late fixed-update failure faults with `life: tick three`.
    - A native reset restarts on fresh data and releases the old spawn; Stop
      releases the new one.
    - Behaviors start with the session and when an entity gains the
      component, write the component every frame, and stop and release only
      that entity's spawn when it loses the component.
    - In a fixed update, a spawn with a 0.1 s lifetime and an owned trail
      reads as alive without a transform; after the tick it has the queued
      transform, the queued runtime state (42) and the transient tag. A
      destroy in the next tick leaves the entity alive until the tick ends.
      The spawn survives six ticks and is gone, with its trail, after ten.
    - Destroying an entity through the scene, as the editor's delete does,
      runs `destroy` with the component (`speed` 1.5) and the behavior's
      spawn still alive, then `stop`, then releases the spawn; Stop runs no
      `destroy` and removes the scene's destroy observer.
    - A container-scoped module runs on the World and the scene, a
      World-scoped one once; detaching the World releases what its instances
      spawned.
    - Through a tool context, `vkr_has_visual` reads child meshes and shapes,
      and `vkr_destroy` takes the children.
    - The macros' descriptors carry offsets, kinds, options, defaults and
      behaviors, and a different SDK version gets no description.
    - `scene_animation_tests.c`: a requested model reserves its wrapper,
      reads `LOADING` with no player, and is `READY` with its animation after
      the next scene update; despawning it first creates nothing; a bank of
      another source fails at the update with a reason and leaves no nodes
      or pooled memory.
    - With a two-worker job system, a task started in `start` sums 1 to
      100,000 (5,000,050,000) on another thread while ticks continue, and a
      tick takes it. A task waited for inside a tick is taken there and not
      again. A task over 64 KB does not start. Stop waits for a 50 ms task
      nothing took and frees every task. Without workers a task runs on the
      calling thread before `vkr_task_run` returns.
  - `script_reload_test.c` loads
    [`reload_probe.c`](../../tests/scripts/reload_probe.c), built as four
    `MODULE` libraries from the staged script SDK only:
    - a code-only reload keeps data (3 ticks, then 23);
    - a new `data_version` restarts on zeroed data;
    - a changed field is refused while the old code keeps counting;
    - retiring keeps the type registered, and a later load adopts it.
  - It also loads [`project_probe.c`](../../tests/scripts/project_probe.c),
    a project library built four ways:
    - two listed modules share one library entry and register both types;
    - a code-only reload prepared on another thread while the session ticks
      once on the old code (3, then 4) commits with data kept (then 24);
    - a discarded preparation changes no module and removes its copy, and a
      run that cannot copy its library fails at commit;
    - dropping `ProbeB` and adding `ProbeC` retires B, keeping its type,
      registers C and restarts on zeroed data;
    - a changed `ProbeA` field refuses the load with
      `probe_a changed its fields`, and `ProbeC`, whose code also changed,
      keeps its generation and old code (one tick adds 1, not 100).
  - `gameplay_player_test.c` and `player_animation_test.c` run the FPS
    player, input admission, action animation and locomotion through a tool
    context with unchanged expected values.
- `vkr_bakery cook scripts/fps/fps.script.json` builds `fps.dll`. Its only
  import is `KERNEL32.dll` and its only export is `vkr_module_fps`. The New
  Script template, expanded for `Door`, builds `Door.dll` (107,008 bytes)
  without warnings.
- The first project build was checked on the order of loads, not on a
  scene: a headless Release editor with the same reserve workaround opened a
  copy of an empty managed project (`--workspace`, `--project`) whose
  `Scripts/Spinner` had never built. Two `script.status` statements in
  consecutive frames reported "building", "Scripts: Loaded" followed, and
  the project World (5 entities) loaded after it. `create spinner` then
  made an entity with `speed` 1.
- Script SDK headers: the Debug build staged nine headers into
  `build_debug/script_sdk`, the reload and project probes compiled with
  `-I<build>/script_sdk` as their only include, and `vkr_bakery cook
  scripts/fps/fps.script.json` rebuilt all 8 actions against it.
  `cmake --install build_release --prefix %TEMP%\vkr-sdk-dist --component
  editor` installed the same nine under `sdk/`. With the build tree's
  `script_sdk` renamed away, the installed `vkr_bakery scripts` built a
  copy of the sample folder under `%TEMP%` into `project.dll` (107,520
  bytes); with the installed `sdk/` renamed away too, it failed with
  "'sdk.h' file not found".
- `platform/vkr_platform.h` left the SDK: `math/vkr_math.h` no longer seeds
  `vkr_rand_i32` from the platform clock (nothing called it), so the math
  headers need no platform header. After deleting the staged copies, the
  Debug build of every target and the editor restaged eight headers, the
  probes built from them, and the full tester passed.
- Component migration:
  - `type_desc_test.c`: a value moves between two hand-written layouts with
    fields reordered, an I32 read as F32 (-3), a speed clamped to its new
    maximum (9 to 5), a label truncated to its new capacity ("abcdef" to
    "abc"), a dropped field and an added one at its default (2.5). A
    tolerant type reads a document naming the dropped field and giving the
    changed field an array, keeping that field's value; without the flag
    the same document fails.
  - `script_reload_test.c`: project probe version 4 inserts `extra` (default
    0.5) before `value`. With a migrator installed, the reload keeps state;
    an entity's `value` 9 and a pending edit's 9 read back at their new
    offset with `extra` 0.5, the scene's ECS component is 8 bytes, and undo
    writes the journal's 7 in the new layout. Reloading version 3 moves them
    back. With the journal conversion disabled, the test fails on the
    pending edit.
  - Headless Release editor on Bistro with the reserve workaround: during
    Play, typing `VKR_FIELD(F32, boost, "Boost", 2.0f)` before the Spinner's
    `speed` (set to 3) and saving logged "Component spinner moved its values
    to its new fields" and "Reloaded; state kept"; `speed` stayed 3 and
    `boost` read 2, during Play and after Stop.
- Packaged game, Windows, Debug: `vkr_bakery bundle` on a copy of an empty
  managed project with a never-built `Scripts/Spinner` ran the Scripts stage
  (1.5 s) and wrote `scripts/project.dll` beside `f.exe`, exporting
  `vkr_module_Spinner` and `vkr_project_modules` and importing only
  `KERNEL32.dll`, with `game.script_library` `../scripts/project.dll`. The
  packaged player, rebuilt with the reserve workaround, logged "Script
  library project loaded" before its World (5 entities) loaded, left no
  copy beside the library, and exited 0 when its autoclose timer ended.
  `script_reload_test.c` reloads the project probe in place: no copy path,
  and the library file remains after shutdown.
- `vkr_bakery scripts` on a sample folder (a `Common` library, a `Door`
  module depending on it and a `Spinner` module) ran 5 actions (4 objects,
  1 link) in 0.64 s and wrote `project.dll` (107,520 bytes). It exports
  `vkr_module_Door`, `vkr_module_Spinner` and `vkr_project_modules` and
  imports only `KERNEL32.dll`, so two modules cost about what one module's
  own library did. A second run was 5 of 5 cached. A dependency cycle and an
  unknown dependency each failed with a diagnostic naming the package.
- Headless Release editor on Bistro
  (`--scene assets/scenes/bistro.scene.json`), with a temporary, reverted
  render-graph reserve increase because the Vulkan renderer at this revision
  cannot create its graph image table (see Unavailable):
  - Without a Player Start the FPS module disabled itself: 5,989 entities
    before Play and after Stop.
  - `create player_start` at (2, 1, 3): Play spawned `Player` with a hidden
    first-person body, 5,990 to 6,073 entities with the mannequin's nodes;
    Stop returned to 5,990. Bistro's city has no collision, so the player
    fell.
  - With SDK version 5, the same Play read 5,992 entities right after Play
    (`Player` and `PlayerBody`, the mannequin still loading) and 6,073 four
    seconds later. The simulation kept running once the late animation
    bound, and Stop returned to 5,990.
  - With `--scripts`, `script.new Spinner` built and loaded the template with
    0 diagnostics, and `create spinner` made an entity with `speed` 1.
  - Typing `spinner->speed += dt;` into the update hook and `script.save`
    before Play reported "Reloaded; state kept", and during Play `speed`
    rose to 17.1.
  - In a later run, the same edit saved during Play reloaded through a
    worker preparation: "Reloaded; state kept", `speed` 1 before the save,
    13.85 after it and 15.88 two seconds later, and Stop returned to 5,990
    entities.
    An edit outside a function reported "Build failed with 2 errors; the
    previous code keeps running". Stop returned to 5,990 entities.

Unavailable:

- macOS and Metal builds and runs of the SDK.
- The Vulkan editor at this revision without the reserve workaround:
  `VKR_LOCAL_SHADOW_FACE_COUNT_MAX` 768 (`c87bce97`) makes the graph image
  table about 289 MiB, beyond the renderer's 98 MiB render-graph allocator.
- The windowed Script editor: highlighting, diagnostics and completion over
  `sdk.h` and package headers were built but not exercised interactively,
  including completion from an installed editor's `sdk/`.
- A macOS package with a script library: staging in `Contents/Frameworks`
  and its signature were built but not run.
- Timing: no frame-time claim.

Earlier macOS evidence (Apple M1 Pro, Metal, 2026-09-29 and 2026-09-30)
covered ABI version 2: the FPS sample on Bistro, the Script editor's
highlighting, diagnostics and completion, script attach and undo, and
project scripts in a managed project. The SDK replaced the calls those runs
exercised; items not listed above were not repeated on the SDK.

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
  state speeds iteration. The restart path remains for changed data shapes.
- **Keep the ABI v2 table beside the SDK.** Rejected at the user's choice:
  the engine is early, so the table, its session struct and its descriptor
  form were replaced outright.
- **One session on the played scene only.** Rejected: World-attached scripts
  must outlive a zone's unload, so instances belong to containers.
- **Module-owned input observers.** Rejected: an observer pointer into an
  unloaded library would dangle; the host observes and forwards.
- **Label widgets per token for highlighting.** Rejected: one text node has
  one colour, and a file's tokens exceed the frame's node budget.

## Revisit when

A platform needs script modules linked statically, a second language binds the table,
additive scenes simulate and need their own instances, a project needs
separately loaded groups, or field renames should carry values.
