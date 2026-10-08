---
status: partial
updated: 2026-10-08
authority: adr
---

# ADR-089: Editor workbenches

## Status

Accepted (partial). The workbench row with General, Level Design, Terrain,
Lighting, Scripting, Art ([ADR-093](093-material-graphs-and-art-workbench.md))
and custom copies, their palettes, the docked Level checks, Script editor,
Terrain and Material panels, switching, the keys, the tab menu
and inline rename, Cmd `workbench*` statements and `ui.workbench`, the
`workbench.*` agent operations, and persistence in project settings and in
the layout file are implemented and verified headless on Windows/Vulkan.
Each tab keeps its own project scene, the scene a switch closes keeps its
meshes, materials and textures loaded for a switch back, and Delete,
Duplicate and Merge act on the whole selection. No macOS capture of the row
and its title-bar drag region exists, and the runtime's asset hold has not
run on Metal.

## Context

Level design tools were spread over the Create menu's Level group, the View
menu, Alt+click face selection and agent operations
([ADR-084](084-agent-channel-and-level-design-toolkit.md)); hollow, carve and
extrude had no button. The Script editor, Level checks and Terrain were
floating windows over the Scene. The editor had one dock layout, so arranging
panels for one task lost the arrangement for another. On 2026-10-05 the owner
asked for task tabs in the style of Blender's workspaces, with Level Design as
the second tab, and took the proposal's recommendations: the term
"workbench" and a first workbench named General. The owner then asked for the
tabs to show the open scene's name and keep the icon to identify the
workbench, and chose the name on the active tab only. The same day the owner
asked to remove the Scene panel's document tabs, so the Scene always shows
the scene the active workbench tab names, and then for each tab to keep its
own scene, as in General for one scene and Level Design for another. The
owner then asked for Delete and Duplicate to act on the whole selection and
for tab switches to stop reloading, and chose to keep the closed scene's
assets loaded over keeping two scenes live.

## Decision

### Term

A **workbench** is a dock layout, the floating windows open in it and the
Scene's editing mode for one task. "Workspace" keeps its meaning, the
directory that holds `.vkreditor`
([ADR-069](069-editor-projects-and-workspaces.md)). Code uses the
`vkr_editor_workbench_*` prefix and Cmd uses `workbench`.

### Row and switching

A 28-point row under the 38-point top bar shows one tab per workbench, in the
order General, Level Design, Terrain, Lighting, Scripting, Art. The dock tree's
toolbar leaf takes both heights through `VkrUiDockTree.toolbar_pt`, which the
editor sets every build; the app and `--scene-only` keep 38 points, and the
top bar alone stays the window's drag region.

The active tab shows the workbench's icon and the open scene's name
(`vkr_editor_scene_label`): the project scene's name, else the scene file's
name. The Scene panel has no document tabs; it shows that scene
([ADR-076](076-project-object-model.md)). Each tab keeps a project scene:
the active tab adopts whatever the project shows once no job, load or prompt
stands between scenes (the top bar's Scenes list, Content, or a switch a
prompt cancelled), and switching to a tab whose scene differs opens it
through `vkr_editor_projects_show_scene`, which asks about unsaved edits
first. Only one primary scene is loaded, so such a switch rebuilds the
scene's objects; its assets are usually still loaded (Warm assets, below).
Copies of one kind keep their own scenes too: + or Duplicate makes "Level
Design (1)" beside Level Design, and each shows its scene. A tab
whose scene differs from the shown one names it beside its icon; the other
tabs show their icons only. Every tooltip names the workbench, its scene when
it differs, and its shortcut, for example "Level Design: Second (Ctrl+2)".
Outside a project, as with `--scene`, every tab shows the one scene.

The active tab has the panel fill, the heading font and an accent
underline; dock tabs mark the active tab on top. Below 520 points of window
width the active tab shows its icon only.

A tab click, Ctrl+1 to Ctrl+9 by position, Ctrl+PageUp and Ctrl+PageDown
(Cmd on macOS),
the View menu's Previous and Next workbench, the tab menu, Cmd `workbench` and
`workbench.switch` request a switch. It applies at the start of the next UI
build, before the dock, the keymap and the Cmd queue read input
([editor_workbench.c](../../editor/src/editor_workbench.c)):

1. A switch waits while the left button is down, a dock tab drag or splitter
   resize runs, or a face, box-draw or raise gesture is in progress. Cmd and
   the agent report the refusal.
2. The leaving workbench stores the live dock tree, the open floating windows
   and its mode: the gizmo tool, the Scene tool (none, box drawing, clip or
   terrain), the Snapping target and the grid toggle.
3. The target's tree replaces the live tree with cleared interaction and a new
   revision, its windows open and the others close, and its mode applies. The
   Scene tool setting ends the leaving tool and its pending clicks.

The selection, camera, loaded scenes, edit journal and simulation stay. A
switch makes no undo entry. Floating window geometry is shared.

A right click on a tab opens Switch to this workbench, Reset panel layout,
Duplicate, Rename, Move left, Move right and Delete. Reset panel layout,
there, in the View menu, in a dock tab's menu and as `layout.reset`,
restores one workbench's built-in dock tree only: the tab's, or the active
one's.

### Warm assets

Before a scene switch or `scene.reload` closes the primary scene, the
runtime takes a reference on every loaded mesh asset an instance draws
(`vkr_mesh_manager_hold_drawn_assets`). A held asset keeps its geometry and
its submeshes' materials, and the materials keep their textures, so when a
scene with those meshes opens again its instances reuse them: the loader
still reads each mesh file, but uploads no geometry and loads no texture.
Each hold names its scene's runtime path. One closed scene stays warm; the
hold before it becomes the opening hold. A project switch unloads the scene
before its Bakery job and names the next scene only when the job finishes,
so the opening hold waits until a request names a scene: it is released
unless that is its scene, and otherwise lasts until the scene opens, fails
or is cancelled. A switch that names its next scene at once, such as
`scene.reload`, decides when it closes. Unloading the scene with no next
scene (`scene.unload`, U) and shutdown release every hold. Each closing
switch logs `WARM_ASSETS kept=<n> opening=<n> released=<n>`. Project scenes'
runtime paths hash their content, so a scene saved with changes after it
closed opens cold. The hold also covers the World's and added scenes' drawn
meshes while they stay open, which changes nothing for them. Shape materials
and animation clips are not held
([vkr_sample_runtime.c](../../runtime/src/vkr_sample_runtime.c),
[vkr_mesh_manager.c](../../runtime/src/renderer/systems/vkr_mesh_manager.c)).

A material request that finds the material already loaded borrows its
handle, as a request that creates one does; holders take their own
references. It used to acquire one that no unload returned, so a material
two loads requested stayed loaded with its textures after both closed
([material_loader.c](../../runtime/src/renderer/resources/loaders/material_loader.c)).

### Custom workbenches

A row holds up to nine workbenches, one per Ctrl+digit. Each has a stable
id, a name and a kind: the built-in whose palette, icon and default layout it
uses. Duplicate (the tab menu, the + button after the tabs, which copies the
active tab, and `workbench.duplicate`) inserts a copy after its source with
the id `custom_<n>`, the name `<source> (1)` numbered like duplicated
objects, and the source's layout, windows and mode, then switches to it.
Delete removes a copy, switching to the tab before it first when it is
active; built-in workbenches stay, and Reset panel layout restores one.
Move left and right swap a tab with its neighbour, so positions, Ctrl+digit
keys and the Ctrl+digit commands (named "Workbench 1" to "Workbench 9")
follow the order. Rename takes 1 to 31 bytes without quotes or backslashes,
unique in the row; a second click on a tab within 0.4 s, or Rename in its
menu, opens an inline field that Enter or a click elsewhere keeps and Escape
drops.

### Built-in workbenches

| Workbench | Layout | Mode |
|---|---|---|
| General | `vkr_ui_dock_default_editor_layout` | As the user left it; it is active at first start |
| Level Design | Tools left of the Scene; Level checks, Content and Console under it; Outliner over Details on the right | Select tool, Grid snapping, grid shown, no Scene tool |
| Terrain | Terrain left of the Scene, a fifth of the width so the sculpt modes keep their labels; Content and Console under it; Outliner over Details | Select tool, terrain tool on, Surface snapping, grid hidden |
| Lighting | Tools left of the Scene; Console under it; the Outliner over a taller Details | Move tool, Surface snapping, grid hidden |
| Scripting | The Script editor over the Console on 60% of the width; the Scene over the Outliner and Details on the right | Move tool, Surface snapping, grid shown |

Level Design never starts box drawing on entry, so entering it does not turn a
click into a brush. Terrain turns its tool on because sculpting is its task;
leaving Terrain ends the tool.

### Docked tool panels

Dock panel kinds gain `tools`, `level_checks`, `script` and `terrain`
([vkr_ui_dock.h](../../runtime/src/core/ui/vkr_ui_dock.h)). The last three
build the bodies of the Level checks, Script editor and Terrain windows. One
rule keeps one host per body: while any tab of the live tree holds the panel,
the window stays closed, and whatever opens the window (its View menu command,
Cmd `window`, a script double-click in Content) shows the tab instead.
Level checks' issue markers draw while either host shows.

The Tools panel shows the active workbench's palette; General and Scripting
have none and say so. Palettes share one layout
(`vkr_editor_palette_*` in [editor_workbench.c](../../editor/src/editor_workbench.c)):
headed rows of equal buttons, two labelled columns when the panel is at least
200 points wide, one column below that, and icons only below 110 points.
Palettes add no geometry or scene code. Each button runs an existing command,
object kind or agent operation, and each operation is one undo step without
review:

| Palette | Section | Buttons | Runs |
|---|---|---|---|
| Level Design | Draw and create | Draw box, Box, Wedge, Cylinder, Room, Trigger, Stairs, Corridor | `CMD_BRUSH_DRAW`; the Create menu's Level kinds at the Scene's placement point; the drag-to-place stairs tool and the multi-point corridor tool, both building editable shapes (ADR-084) |
| Level Design | Stairs and corridors | Straight, L turn, U turn, Curved, Spiral; Turn left, Curved | The kind and turn of the next stairs; whether the next corridor rounds its corners |
| Level Design | New brush role | Solid, Visual, Clip, Trigger | The `role` of the next box, wedge or cylinder from the palette or the Create menu, a drawn box and stairs |
| Level Design | Edit | Select, Move, Clip, Extrude, Hollow, Carve, Doorway, Merge, Snap, Free, Bake, Duplicate, Delete | Tool and brush commands; `brush.extrude` on the selected face by one grid step; `brush.hollow`; `brush.carve` with the selection as cutter; the grid patch cut through the brush, else `blockout.doorway` through the middle of the selected wall brush; `brush.merge` of 2 to 8 selected brushes; `brush.snap` of 2 to 16 selected objects against the first; `free_placement` added to the selection or cleared; the selected shape's `blockout` component removed |
| Level Design | Surface, Mark | None, Concrete, Metal, Wood, Tile, Plaster, Brick, Rock, Dirt, Grass, Glass, Fabric, Water, Emissive; None, Hazard, Orange, Blue, Red, Green, Dark | The surface tag or mark of the next brush, and of the selected face as one `brush_face` edit, else `brush.set_surface` on every face of the selected brush (ADR-084). The lit button is the selected face's value, the selected brush's when all its faces share one, else the next brush's |
| Level Design | Snapping | Free, Surface, Grid, Magnet; Show grid, Finer, Coarser | The Snapping target; the brush magnet (ADR-084); the grid toggle; the grid step halved or doubled between 1/16 m and 64 m |
| Level Design | Check | Level checks | Shows the Level checks tab |
| Terrain body | Create | Terrain, Spline, Scatter, Road | The Level kinds; `terrain.road` along the selected spline over the first terrain of its scene |
| Lighting | Lights, Environment | Point, Spot, Rect, Directional; Sky, Clouds, Fog, Volumetric fog, Post process | The Create menu's kinds |
| Lighting | View | Lit, Lighting only, Detail lighting, Light icons | The render mode, as `view.mode` sets it; `CMD_LABELS` |
| Lighting | Bake | Bake lighting, Preferences | `CMD_SCENE_BAKE`, `CMD_GRAPHICS` |
| Art | Materials | Edit, Assign, Reset | Opens the selected face's or brush's material in the Material panel; `face.set_material` of the open material on the selected face, else every face of the selected brush; the same with an empty material |
| Art | View | Greybox | Toggles the greybox view (ADR-084) |

The Terrain body's Create section shows in the Terrain window too, so the
floating window opens 440 points tall. Brushes the Create path makes are
selected, so palette operations apply to them next.

The stairs and corridor tools are Scene tools like box drawing, the clip tool
and terrain sculpting, and one helper (`vkr_editor_scene_tool_set`) switches
between them so they exclude each other. The first click on the grid plane
sets the start, a preview line follows the pointer, and the second click runs
`brush.stairs` (rising 3 m) or `blockout.corridor` as one undo step; Escape
drops the start, then turns the tool off. A click the grid plane cannot take,
as when the view looks above the horizon, says so. While any Scene tool runs,
a one-line hint under the Scene's centre says what the next click does.

### Multiple selection

The runtime's selected entity stays the primary selection; the editor keeps
up to 15 more. Ctrl+click (Cmd on macOS) on an Outliner row, or in the Scene,
adds an object or takes it out; taking the primary out hands it to the first
extra object. In the Scene the runtime runs the click's pick without
selecting and answers with `VKR_SAMPLE_PICK_SELECT_TOGGLE`. A plain click
clears the extra objects, as does any change of the primary selection that
no toggle made, and dead entities leave at the next build. Outliner rows of
the selection are highlighted, and extra brushes outline their faces in the
Scene overlay. Merge in the Level Design palette runs `brush.merge` on the
selected brushes as one undo step. With extra objects selected, Duplicate and
Delete (Ctrl+D, Delete, the Edit and context menus, and Cmd `duplicate` and
`delete` without a name) act on the selected objects of the primary's
container as one edit batch and one undo step
(`vkr_editor_selection_apply`): Duplicate skips an object whose ancestor is
selected, because the ancestor's copy includes it, and selects the copies;
Delete removes the deepest objects first. A move (the Move gizmo or a
Select-tool drag) carries the other selected objects of the primary's
container by the same world offset (`VkrSampleUiClient.move_companions`) and
undoes as one step; rotate and scale act on the primary selection alone.
Cmd `select.toggle <name>` toggles as a Ctrl+click does,
and `ui.selected` reads the count.

### Agent operations

`workbench.list` reports each workbench's id, name, position, shortcut, panel
tabs, open windows and mode, and the active id. `workbench.switch` takes an
id, a name or a position and answers two builds later, once the switch has
applied and the Scene has its new size, with the Scene rectangle in window
points; a drag that holds the mouse fails it with `VKR-AGENT-0006`.
`editor.status` adds `workbench` and `scene_tool`
([editor_ops.c](../../editor/src/editor_ops.c)).

### Persistence

Project settings store a `workbenches` member in place of `layout`, written by
`vkr_editor_workbench_write_json`:

```json
"workbenches": {
  "version": 1,
  "active": "level_design",
  "general": {
    "layout": { "version": 1, "root": 0, "nodes": [] },
    "windows": { "animation": true },
    "mode": { "tool": "move", "scene_tool": "none", "snap": "surface",
              "grid": true }
  },
  "level_design": { "layout": {}, "windows": {}, "mode": {} }
}
```

Each workbench has a member named by its id that holds its `kind` and
`name`, and `order` lists the ids in tab order. On load, built-ins the order
leaves out follow it, and a copy whose kind is unknown is dropped. The active
workbench's layout is the live tree and its windows are the open ones. On load, a missing member or
a workbench without a valid layout keeps its built-in state, so a new built-in
workbench needs no format change. The active workbench's mode is not applied,
because the runtime preferences and the Snapping settings restore it.
Settings without `workbenches` give their `layout` to General. The settings
merge keeps an old `layout` member, which is then ignored. The legacy
`--scene` layout file (`.vkr-editor-layout.json` or
`VKR_EDITOR_LAYOUT_PATH`) holds the same object and opens the active
workbench's windows; a file with a bare dock tree becomes General's layout.

## Consequences

The dock area starts 28 points lower. Scripted `ui.click` coordinates that
target docked panels in the 1680 x 1050 headless layout move down by 28
points; the dock area now starts at y 66.

The active tab's label tracks the open scene, so the row's tabs shift when a
scene with a longer name opens. Inactive workbenches are told apart by icon
and tooltip only, so copies of one kind share an icon. The Scene panel no
longer opens a second scene beside the first; switching scenes replaces the
shown one.

Nine full 31-node trees would exceed the 64 KiB project settings buffer; the
save then fails with its message. Typical 11-node trees write about 2 KiB
each.

Each workbench holds a full dock tree in editor state, about 5 KiB each, and a
switch copies trees without allocating. Different Scene sizes between
workbenches resize the Scene image once per switch, which restarts temporal
histories as a splitter drag does; in the headless layout General renders 850
and Level Design 758 pixels wide at 67% scale.

Warm assets keep the closed scene's geometry, materials and textures in
memory until the next switch: Bistro's textures take about 2.2 GB once
streamed. A switch back still rebuilds every object; Bistro's warm reload
takes about 1.3 s where a cold one takes 7 s.

Ctrl+1 to Ctrl+9 select workbenches by position rather than Unity's main
windows. The keymap rule still applies: modified keys act unless a text field
holds the keyboard.

## Alternatives considered

"Workspace", Blender's word, would collide with the on-disk workspace, its
code and `--workspace`. "Layout" and "mode" collide with dock layout,
`layout.reset`, `view.mode` and the gizmo mode. The owner weighed three tab
labels: the scene's name on every tab repeats one name five times, and tabs
bound to scenes would duplicate the Scene panel's document tabs; the name on
the active tab keeps the row short. Once the document tabs were gone, the
owner chose per-tab scenes, which name a scene only where it differs. A second fixed-height dock leaf for the
row would have to stay in every tree and out of tab drags; the taller toolbar
leaf needs one field. Storing workbenches as an array of objects would need
array iteration in the reader; members keyed by id read with the existing
root-field lookups. Per-workbench window geometry would move windows between
tabs; shared geometry keeps each window in one place. A Tools palette for
Terrain would build the terrain body a second time; the Terrain panel hosts
the body itself.

## Revisit when

Teams sharing a project want per-user layouts, a task needs a palette the
five kinds do not cover, warm assets cost more memory than a project can
spare, switches back need to skip rebuilding objects too (two live scenes),
or rotate and scale need to act on the whole selection.

## Implementation

- [Workbench state, switching, row, palettes, Lighting palette and persistence](../../editor/src/editor_workbench.c) and [header](../../editor/src/editor_workbench.h)
- [Workbench types in editor state](../../editor/src/editor_ui.h) and [palette and window-host declarations](../../editor/src/editor_internal.h)
- [Level Design palette](../../editor/src/editor_level.c) and [Terrain body](../../editor/src/editor_terrain.c)
- [Dock panel kinds and toolbar height](../../runtime/src/core/ui/vkr_ui_dock.c) and [panel bodies](../../editor/src/editor_dock.c)
- [Commands, keymap, View menu and tab menu](../../editor/src/editor_windows.c), [Cmd `workbench`](../../editor/src/editor_cmd.c), [`ui.workbench`](../../editor/src/editor_cmd_eval.c) and [agent operations](../../editor/src/editor_ops.c)
- [Scene label](../../editor/src/editor_viewport.c), [selection batches](../../editor/src/editor_scene_panels.c)
- [Warm assets](../../runtime/src/vkr_sample_runtime.c) and [mesh asset holds](../../runtime/src/renderer/systems/vkr_mesh_manager.c)
- [Project settings](../../editor/src/editor_projects.c) and [layout file](../../editor/src/editor_application.c)

## Verification

Release editor, Windows/Vulkan, headless 1680 x 1050 with isolated
`HOME`, `VKR_EDITOR_LAYOUT_PATH` and `VKR_GRAPHICS_SETTINGS_PATH`, Bistro:

- `workbench`, `ui.key ctrl+1` to `ctrl+5`, `ui.key ctrl+pagedown`,
  `workbench 1` and `workbench.switch` switch, and `ui.workbench` reads each
  result. Level Design reads `view.snap` grid and `view.tool` select; General
  returns to surface. `editor.status` in Terrain reports `scene_tool`
  terrain; `workbench.switch` to Level Design answered with the Scene
  rectangle [238, 97, 1137, 696].
- Window captures of each workbench show the active tab named "bistro" with
  the workbench's icon and the others as icons; a project scene shows its
  name, "Level Design Test".
- Level Design palette clicks create a selected box brush (+7 entities),
  paint it Orange, duplicate it, hollow it (+35 entities), and three `undo`
  statements return to one brush. Every palette operation answered
  `ok: true`.
- Terrain: the panel's Terrain button creates a selected terrain; a
  `ui.drag` stroke raised 25 sampled heights from 0 to a 2.05 m sum (1.05 m
  peak); `workbench general` reports `scene_tool` none, and `undo` returns
  the heights to 0.
- Scripting, in a scratch copy of a project: `script.new`, `script.type`,
  `script.save` and `script.status` run in the docked Script editor, and
  `window script` reports the docked tab; in General it opens the window.
- The tab menu's Reset panel layout on the inactive Level Design removes a
  Bakery tab added there, and General stays active.
- Custom workbenches: `workbench.duplicate` copies Level Design as
  `custom_1`; rename, move left, a second copy of Terrain and its delete
  apply, and deleting General is refused. A restart restores the order, the
  names and the active copy, and the next copy takes `custom_2`. A double
  click on the active tab, `ui.type Rooms` and Enter rename it.
- Per-tab scenes, in a scratch copy of a project: Level Design creates and
  shows a second scene, General then reopens the first, Level Design the
  second, and the row shows "Level Design Test" on General's tab and
  "Second" beside Level Design's icon.
- Multiple selection on two touching boxes in Bistro's street: a click
  selects one, Ctrl+click in the Scene (and on the Outliner row) adds the
  other (`ui.selected` 2), Merge joins them (6003 to 5996 entities) and
  `undo` restores both.
- Selection-wide edits in Bistro: Duplicate of two selected boxes adds 14
  entities with both copies selected (`ui.selected` 2) and `undo` removes
  them; Delete of the two boxes and the Sun goes from 6003 to 5988 entities
  and `undo` restores them.
- Warm assets, Bistro: `scene.reload` settled in 7.02 s and 7.05 s before
  the hold and in 1.24 s to 1.37 s with it, the hold keeping 646 mesh
  assets; after two warm reloads `scene.unload` dropped texture memory from
  2281 MB to 22 MB, and the next `scene.load` settled in 7.06 s. Before the
  material loader fix that unload kept 1846 MB and all 632 texture streams.
- Warm assets, in a scratch copy of a project with FPS Arena (416 mesh
  assets) and two empty scenes: FPS Arena to Second kept 416; back to FPS
  Arena opened with them (0.48 s); Second to Third released 416 once Third
  was named, and FPS Arena then opened cold (0.67 s).
- Level Design additions, looking down at Bistro's street: the stairs tool's
  two clicks add 113 entities and `undo` removes them; the corridor adds 29;
  Visual in New brush role makes the next box's role `visual`; Doorway splits
  a 4 x 3 x 0.25 m wall (+14 entities) and `undo` restores it. The hint
  changes after the first click.
- Layout file: a Bakery tab added in Level Design and an Animation window
  open there survive a restart with Level Design active; a bare dock tree
  file loads as General's layout.
- Project settings (a scratch copy of a project with a version 1 `layout`):
  the first run migrates it, and the next run starts in Level Design with its
  window, Grid snapping, and Surface again after `workbench general`.
