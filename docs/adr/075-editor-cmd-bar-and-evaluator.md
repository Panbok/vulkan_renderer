---
status: implemented
updated: 2026-09-27
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
| `panel` | `<hierarchy\|inspector\|console\|bakery\|content> [on\|off\|toggle]` | Docked panels |
| `window` | `<animation\|physics\|graphics\|draws\|memory\|help> [on\|off\|toggle]` | Floating windows |
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
| `component.add`, `component.remove` | `<type>` | Add or remove a live world component, or `physics_body`, on the selection (undoable); World-only types only on World objects |
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
| `content.import` | `<path>` | Import a file into the project, filed in the current folder |
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
| `sel`, `entity("name")` | `name`, `position`, `rotation` (degrees, XYZ), `scale`, `visible`, `light`, `id` | all but `light`, `id` |
| `.light` | `kind`, `color`, `intensity` (radiance for rectangles), `range`, `enabled`, `inner`, `outer` (degrees) | all but `kind` |
| `.<component>` | Descriptor properties of a component the entity carries, by type name (`sel.post_process.exposure_compensation_ev`, `sel.point_light.intensity`) | visible, non-read-only properties |
| `view` | `camera`, `mode`, `grid`, `grid_spacing`, `grid_labels`, `camera_speed`, `tool` | all |
| `ui` | `zoom`, `reduce_motion` | all |
| `sim` | `running`, `time` | `running` |
| `scene` | `loaded`, `entities` | none |

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
255 bytes; the queue holds 4 KiB.

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
- [Startup scripts](../../editor/src/editor_application.c)
- [Quit request consumer](../../runtime/src/vkr_sample_runtime.c)
- [Agent procedure](../../.codex/skills/vkr-editor-cmd/SKILL.md)

Verified on macOS Release with Bistro through `--exec` scripts and System Events
typing: commands, completion, variables, entity/light/view/ui reads and writes,
undo of evaluator edits, and error paths. Windows and native Vulkan are
unverified.
