---
status: implemented
updated: 2026-09-30
authority: adr
---
# ADR-075: Editor Cmd bar and expression evaluator

## Status

Accepted.

## Context

The Commands palette only searched fixed actions. Users and agents need to
drive the editor by text: run actions, inspect values, and change scene and
editor data without pixel clicks, in the style of the Unreal Editor's Cmd field.
Scripted runs also need a way to start the editor with a command list and read
the results.

## Decision

A Cmd field in the top bar replaces the palette. Cmd/Ctrl+P focuses it; typing
lists suggestions below it, Up/Down choose, Tab completes, Enter runs, and
Escape clears then leaves the field. An empty line lists recent lines first.
`;` or a newline separates statements; `#` starts a comment line.

A line whose first word names a command runs that command. Anything else is an
expression statement, and so is a command name followed by `=` or a
required-argument command with nothing after it that reads as a value
(`ui.zoom`). Typed lines, `--exec "<script>"` and the `VKR_EDITOR_EXEC`
environment variable share one queue; the environment script runs first. The
queue runs one statement per frame because the runtime accepts one request of
each kind per UI build; `wait` and `wait.scene` pause it.

A command that starts a Bakery job or a scene load holds the queue until that
work settles, so the next statement reads its result without a timed `wait`:
`scene.load`, `scene.reload`, `scene.open`, `scene.add`, `scene.create`,
`scene.import`, `scene.instantiate`, `scene.primary`, `tab.new`, `tab.show`,
`content.import`, `content.command`, `content.place`, `build.game` and
`build.run`, and `scene.save`, so
a headless run cannot quit during a save. The hold ends when no
project job, activation or Set primary swap runs and neither the primary scene
nor an added scene is loading and no build runs, and prints `[cmd] Settled
after <s> s`; a build hold then prints the build's result line. After
600 s it reports an error and drops the queue. An unsaved-edits prompt does not
hold the queue: a script saves or discards first.

`--headless` starts the editor without a window. Frames render into an
offscreen present target (ADR-014) of the editor's 1680x1050 size in pixels at
content scale 1, and the process takes no input or focus. The editor quits once
its Cmd queue drains, discarding unsaved edits and saying so in a `[cmd]` line;
`VKR_AUTOCLOSE_SECONDS` defaults to 600 s as a backstop. Without `--scene` or
`--project <uuid>`, the project launcher shows and runs no Cmd statements, so
only that backstop ends the run.

Every result goes three ways: a `[cmd]` line on stdout, flushed per line
(`[cmd] > <statement>`, then `[cmd] <result>` or `[cmd] error: <message>`); the
Console and the session log as info records; and a toast. stdout remains the
dependable channel for scripts.

### Commands

| Command | Argument | Action |
| --- | --- | --- |
| `scene.load`, `scene.reload`, `scene.unload`, `scene.save` | | Scene file actions |
| `undo`, `redo` | | Edit journal |
| `select` | `<name>` | Select an entity: exact name, else the first containing it |
| `frame` | | Frame the selection |
| `visibility.toggle` | | Hide or show the selection |
| `panel` | `<outliner\|details\|console\|bakery\|content\|build> [on\|off\|toggle]` | Docked panels |
| `window` | `<animation\|physics\|preferences\|draws\|memory\|help\|create\|build\|script> [on\|off\|toggle]` | Floating windows |
| `build.game`, `build.run` | `[profile]` | Package the project with a build profile (quoted when it has spaces; the selected profile by default), then for `build.run` run the game ([ADR-078](078-project-build-and-packaging.md)) |
| `build.settings`, `build.open` | | Build Settings window; the last package's folder |
| `layout.reset` | | Default dock layout |
| `sim.play`, `sim.pause`, `sim.step`, `sim.stop` | | Simulation transport |
| `render.start`, `render.stop` | | Scene rendering |
| `camera.capture` | | Toggle free-camera capture |
| `camera.view` | `<perspective\|top\|left\|right\|bottom>` | Scene camera view |
| `camera.speed` | `<units/s>` | Free-camera speed |
| `view.mode` | `<lit\|unlit\|detail-lighting\|lighting-only\|wireframe>` | Render mode |
| `tool` | `<select\|move\|rotate\|scale>` | Transform tool |
| `grid` | `[on\|off\|toggle]` | World grid |
| `grid.spacing` | `<units>` | Grid cell size (shows the grid) |
| `grid.labels` | `[on\|off\|toggle]` | Grid cell numbers and letters |
| `labels`, `labels.directional`, `labels.spot`, `labels.point` | `[on\|off\|toggle]` | Light icons |
| `create` | `<object>` | Create an object kind (`empty`, `cube`, `text`, a light kind or a world component type) in the selection's container, else the primary scene, else the World; World-only settings always go to the World |
| `delete` | `[name]` | Delete the named object or the selection (undoable) |
| `script.new`, `script.open` | `<Name>`, `<file>` | Create a script module from the template in the open Scripts folder and open it, or open a listed source in the Script editor ([ADR-079](079-c-script-modules.md)) |
| `script.goto`, `script.type` | `<line>`, `<text>` | Move the Script editor's caret to a line, or type ASCII text at it as the keyboard would, completion included |
| `script.save`, `script.status` | | Save the active Script editor tab, which rebuilds and hot reloads its module; report each module's build and load state and the diagnostic count |
| `script.attach`, `script.edit` | `<type\|none>`, | Set the selection's script slot to a loaded script type or remove it (undoable, one entry); open the selection's script source |
| `ui.click`, `ui.drag`, `ui.key` | `<x> <y> [count] [right]`, `<x0> <y0> <x1> <y1>`, `<key>` | Synthetic input in window points, one step per frame before the UI reads input: a left, double or right click; a left drag that holds while a pick resolves; or an `up`, `down`, `left`, `right`, `enter`, `escape` or `tab` key. The queue holds until the steps ran |
| `component.add`, `component.remove` | `<type>` | Add or remove a live world component, or `physics_body`, on the selection (undoable); World-only types only on World objects |
| `physics.motion` | `<static\|kinematic\|dynamic>` | Set the selection's physics body motion (undoable) |
| `parent` | `<name\|none>` | Reparent the selection within its container, keeping its world pose |
| `scene.open`, `scene.create` | `<name>` | Open a project scene (the scene already loading or open stays as it is), or create an empty one and open it |
| `scene.add`, `scene.remove` | `<name\|path>`, `<slot\|name> [discard]` | Load a project scene or scene file beside the primary one, or unload it |
| `scene.primary` | `<slot\|name>` | Make an added project scene the primary scene, adding the previous primary back beside it |
| `scene.instantiate` | `<name>` | Copy another project scene into the open one under a new root at the origin, as an unlinked prefab instance |
| `scene.inherit` | `[on\|off\|toggle]` | Whether the open scene uses the World's objects |
| `tab.new`, `tab.show` | `<n>` for `tab.show` | New World document, or switch viewport documents |
| `content.search` | `[text]` | Search below the current Content folder |
| `content.open`, `content.mkdir` | `<folder>` | Show a Content folder by path, shown name or shown path (`Level One`, `Level One/Textures`, `System/Objects`), or create a project folder with its parents |
| `content.command` | `<load\|open\|place\|rename\|delete> <item>` | Run an item's or folder's context menu command, as a right-click would |
| `content.move` | `<item\|folder> <folder>` | Move an item (id or name, quoted when it has spaces) or folder |
| `content.view` | `<list\|tiles>` | Content view |
| `content.reveal` | `<path>` | Select the asset that owns a workspace file (its artifact or build revision) in its folder, as a Build diagnostic's Reveal does |
| `content.import` | `<path> [world\|new <name>\|scene <name>\|content]` | Import a file into the project, filed in the current folder; with a placement, run the import step's choice for a model: the World's root, a new scene, or a project scene |
| `content.drop` | `<path>` | Act as an OS file drop on the current folder: opens the import step |
| `content.place` | `<item>` | Act as a drop of an item at the viewport centre: places a mesh or adds an object kind |
| `preset.save`, `preset.apply` | `<component>`, `<name>` | Save the selection's component as a preset, or apply one |
| `ui.zoom` | `<scale\|in\|out\|reset>` | Interface zoom |
| `ui.reduce_motion` | `[on\|off\|toggle]` | Eased motion |
| `help` | `[command]` | List commands or describe one |
| `echo` | `<text>` | Print text |
| `wait` | `<seconds>` | Pause the queue (at most 600 s) |
| `wait.scene` | | Pause the queue until a scene is loaded (120 s limit; a timeout drops the queue) |
| `quit` | `[discard]` | Close the editor; refuses while scene edits are unsaved unless `discard` |

Shared commands reuse the menu table's enabled checks, so an unavailable action
reports an error instead of acting.

### Expressions

Values are numbers, booleans, strings, vec3, entities, lights and the data
roots below. Operators: `+ - * / %`, comparisons, `== !=`, `&& ||`, unary `-`
and `!`. Vectors combine component-wise and scale by numbers; `+` with a string
concatenates. `(x, y, z)` is a vector literal. Functions: `vec3`, `len`,
`normalize`, `dot`, `cross`, `lerp`, `min`, `max`, `clamp`, `pow`, `sqrt`,
`sin`, `cos`, `tan`, `abs`, `round`, `floor`, `ceil`, `exp`, `log`, `deg`, `rad`,
`str`, and `entity("name")`. Constants: `pi`, `true`, `false`.

`name = expr` stores a session variable (32 slots). Assigning a member writes
editor or scene data, and assigning `x`, `y` or `z` of a vector member writes
the vector back through its path (`sel.position.y = 2`). Descriptor vector
and color properties read and assign as vec3: a two-component property ignores
`z`, and a four-component property such as a shape or text color keeps its
fourth component (`sel.shape.color = (1, 0.2, 0.2)`).

| Root | Members (read) | Writable |
| --- | --- | --- |
| `sel`, `entity("name")` | `name`, `position`, `rotation` (degrees, XYZ), `scale`, `visible`, `light`, `id`, `world_position` (the evaluated pose, which simulation moves) | all but `light`, `id`, `world_position` |
| `.light` | `kind`, `color`, `intensity` (radiance for rectangles), `range`, `enabled`, `inner`, `outer` (degrees) | all but `kind` |
| `.<component>` | Descriptor properties of a component the entity carries, by type name (`sel.post_process.exposure_compensation_ev`, `sel.point_light.intensity`) | visible, non-read-only properties |
| `view` | `camera`, `mode`, `grid`, `grid_spacing`, `grid_labels`, `grid_through`, `collision` (0 off, 1 selected, 2 all), `camera_speed`, `tool`, `snap` (`free`, `surface`, `grid`), `snap_offset`, `snap_yaw`, `snap_align`, `snap_centers` | all |
| `ui` | `zoom`, `reduce_motion` | all |
| `sim` | `running`, `time` | `running` |
| `scene` | `loaded`, `entities` | none |
| `stats` | `frame_ms`, `frame_ms_p95` (median and 95th percentile of the last 120 frame intervals), `finalizing`, `replaced_materials` (finished materials the current or last background finalize applied), `pending_replacements`, `pending_textures`, `render_width`, `render_height` (the Scene's current internal extent) | none |

Scene writes read the entity with `vkr_scene_edit_read`, change one component,
validate it and submit an `APPLY` edit, so they undo, save and reject invalid
values like Details edits. View writes submit a `VkrSampleViewRequest`. One
statement makes at most one scene edit and one view change; a second request
in the same frame fails instead of overwriting the first. Completion after a
dot lists members with their current values.

## Consequences

Typed and scripted editor control share one validated path with the panels.
Statements apply one per frame, so a script's reads see the previous
statement's result. The evaluator has no loops, user functions or file access,
and it cannot create entities; `create` and `component.add` do. The field accepts at most
255 bytes; the queue holds 4 KiB. A headless run proves scripted editor state,
not window resize, DPI, input or presentation.

## Alternatives considered

An embedded scripting language would add a runtime, a binding layer and a
second source of truth for editor state. Extending the palette with free text
would still lack values and assignment.

## Revisit when

Scripts need control flow or entity creation, or tools need a structured result
channel richer than `[cmd]` lines.

## Code evidence

- [Cmd bar, commands, queue](../../editor/src/editor_cmd.c)
- [Expression evaluator](../../editor/src/editor_cmd_eval.c)
- [Shared editor commands](../../editor/src/editor_windows.c)
- [Startup scripts and `--headless`](../../editor/src/editor_application.c)
- [Headless offscreen target](../../runtime/src/vkr_sample_runtime_config.c)
- [Quit request consumer](../../runtime/src/vkr_sample_runtime.c)
- [Agent procedure](../../.codex/skills/vkr-editor-cmd/SKILL.md)

Verified on macOS Release with Bistro through `--exec` scripts and System Events
typing: commands, completion, variables, entity/light/view/ui reads and writes,
undo of evaluator edits, and error paths. Queue holds and `--headless` were
verified on macOS Release (Metal) with Bistro through `scene.reload`; holds
after project Bakery jobs and added-scene loads, Windows and native Vulkan are
unverified.
