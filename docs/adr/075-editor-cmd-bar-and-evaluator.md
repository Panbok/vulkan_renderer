---
status: implemented
updated: 2026-09-26
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
Console, when the build keeps info and warning logs; and a toast. Release
builds without `VKR_EDITOR_LOGGING` compile those log levels out, so stdout is
the dependable channel for scripts.

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
| `labels`, `labels.directional`, `labels.spot`, `labels.point` | `[on\|off\|toggle]` | Light icons |
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
the vector back through its path (`sel.position.y = 2`).

| Root | Members (read) | Writable |
| --- | --- | --- |
| `sel`, `entity("name")` | `name`, `position`, `rotation` (degrees, XYZ), `scale`, `visible`, `light`, `id` | all but `light`, `id` |
| `.light` | `kind`, `color`, `intensity` (radiance for rectangles), `range`, `enabled`, `inner`, `outer` (degrees) | all but `kind` |
| `view` | `camera`, `mode`, `grid`, `grid_spacing`, `camera_speed`, `tool` | all |
| `ui` | `zoom`, `reduce_motion` | all |
| `sim` | `running`, `time` | `running` |
| `scene` | `loaded`, `entities` | none |

Scene writes read the entity with `vkr_scene_edit_read`, change one component,
validate it and submit an `APPLY` edit, so they undo, save and reject invalid
values like Inspector edits. View writes submit a `VkrSampleViewRequest`. One
statement makes at most one scene edit and one view change; a second request
in the same frame fails instead of overwriting the first. Completion after a
dot lists members with their current values.

## Consequences

Typed and scripted editor control share one validated path with the panels.
Statements apply one per frame, so a script's reads see the previous
statement's result. The evaluator has no loops, user functions or file access,
and it cannot create entities or edit physics bodies. The field accepts at most
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
