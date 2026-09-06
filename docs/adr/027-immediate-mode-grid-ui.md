---
status: implemented
updated: 2026-09-06
authority: adr
---
# ADR-027: Immediate-mode grid UI with retained CPU state

## Status

Accepted.

## Context

The app and editor need stable widget interaction, docked panels, text editing,
and native-resolution overlay drawing without a second renderer-owned UI scene.

## Decision

Callers build UI each frame through an immediate-mode API. The UI system retains
state by stable widget ID, including interaction, text editing, scroll offsets,
last layout, and reusable draw storage. Layout uses grid tracks only. Input
hit-testing uses the previous frame's retained rectangles.

The system lowers the complete visual tree to ordered, scissored batches of
vertices and indices. When the tree, target, and scale are unchanged, it reuses
retained CPU geometry; each frame still uploads and draws that stream. Tile
hashes expose changed regions but do not imply a cached GPU target.

The dock tree is bounded, validated, and persisted as JSON. Its scene panel
rectangle drives editor viewport mapping. UI is composited after Scene output
at the drawable's native extent. In-window leaves support compact tab stacks,
reordering, edge splits, and resizing. Drawing, tab hit-testing, and drop insertion
share dock geometry; stable tab IDs survive moves. Recursive subtree minima
preserve 96-point panel width and 64-point content height when the window fits;
smaller windows divide the shortage proportionally. The editor saves its layout to
`.vkr-editor-layout.json` unless `VKR_EDITOR_LAYOUT_PATH` overrides or disables it.

Editor tabs use category-colored vector icons and an amber focused border. Real
Ubuntu Mono Bold supplies headings; vector controls keep the existing UI vertex
ABI. Icon strokes and fills use inner/outer convex polygon rings with a
one-physical-pixel alpha transition centered on each edge. Shared CPU lowering
emits opaque inner and transparent outer vertices; both backends interpolate
coverage through their existing linear-alpha blending. Capacity checks admit
complete polygons, including their fringe, and tile bounds include the outer ring.
Scene load/unload, simulation, rendering and camera controls are icon-only
with tooltips in a floating Scene toolbar. Its grip supports dragging; releasing
near a viewport edge anchors that edge, and resize clamps the toolbar inside the
Scene. Narrow panes wrap the controls. Floating windows and popups have input
priority above the toolbar.
Inspector, Console and Bakery use bordered field surfaces and bold action
buttons; read-only fields, disabled actions, severity and follow-tail states
retain distinct styling.

Enabled visible controls support Tab/Shift-Tab focus and Enter/Space activation.
An unobstructed Scene click clears widget focus and gives Tab to free-camera
capture. Clicking a panel or control restores Tab/Shift-Tab widget navigation.
F3 and the toolbar camera button also enter camera mode; Escape releases capture.
Stopped or hidden Scene views do not accept camera entry.
Window input retains press/release edges, button press positions and press-time
shortcut modifiers until frame completion. Docking consumes final movement before
releasing its drag, including a complete gesture received in one event drain.
The input module selects the platform shortcut modifier for all callers.
Scroll containers capture hover and wheel input while child controls own clicks.
Tab also focuses scroll containers: Page Up/Down moves one viewport and Home/End
moves to the declared bounds. Tab then reaches the visible child controls. A
focused container keeps its border visible without moving its children's layout.
Keyboard scope follows floating input layers, including popups. Text fields
support UTF-8 clipboard input, mouse and keyboard selection, Cmd/Ctrl A/C/X/V,
read-only selection, and caret scrolling. Text drags retain their press anchor
through the final movement; keyboard editing ends the prior mouse selection.
The controls do not provide a native
VoiceOver or Windows UI Automation tree.

Hierarchy caches scene structure and expanded/search-matching rows when their
inputs change, then emits only its visible window. Display slots are bounded;
selection uses generation-bearing entity IDs rather than row positions. Inspector
borrows selected component values and sends a typed edit request after validation.
It edits name, visibility, local TRS and light values. Apply commits a transaction;
Revert/Escape discards the field draft. Authored shear matrices remain read-only.
Runtime selection is shared with viewport picking, and frame selection fits the
selected subtree's mesh bounds.

Commands (Cmd/Ctrl+P) searches scene, layout, panel and transport actions.
Pointer hover selects a visibly highlighted result; click or Enter runs it.
Held Up/Down repeats after 350 ms at 55 ms intervals, keeping the selected
result visible; the wheel scrolls the list independently. Hover takes precedence
over focus when choosing a tooltip, and the search field has no obscuring hint.
Navbar buttons toggle their open dropdown, window or active Bakery tab closed;
an inactive Bakery tab is selected. The navbar remains reachable while Commands
owns input below it.

The Scene resolution badge also shows the runtime's averaged frame cadence
(FPS and wall-clock frametime, including the editor loop), replacing timing with
stopped/unloaded state when Scene rendering is inactive. These are not isolated
GPU pass timings.

The
editor accepts a project-relative `--scene <scene.json>` or `VKR_SCENE_PATH`;
the default remains
Bistro. Cmd/Ctrl+S saves committed edits, and Cmd/Ctrl+Z / Shift+Z undo/redo
when a text field does not own keyboard editing.

The runtime owns 128 undo entries and releases them on scene replacement. A whole
gizmo drag becomes one entry before selection or transport actions can end it. Explicit Save writes `<scene>.editor.json` atomically,
using source entity/node identities and fingerprints. Loading validates complete
JSON, resolves every source identity and stages names before mutation. A conflict
leaves the source scene unchanged and prevents overwriting the conflicting file.
Original glTF and scene JSON files are not rewritten. Reload/unload refuses unsaved
committed edits. These sidecars are applied by the shared interactive app/editor
runtime; harness scene loads do not implicitly consume editor overrides.

Console keeps 2,048 structured records with stable sequences, severity, source,
UTC time and bounded message text. Producers append under the existing logger
mutex; the UI copies at most 256 new records per build. Each history ring uses
4,653,056 bytes, with one logger ring and one editor snapshot ring. Eviction,
truncation and lag remain visible. Text/source search, severity filters, follow-tail,
record/range copying and read-only detail selection operate on copied data.
Rows pair distinct CPU vector severity icons with red/coral, amber, blue, violet
and green text; text labels preserve severity without relying on color.
Severity filters live in one dropdown with independent colored checkboxes and an
enabled-count label. Its keyboard navigation and pointer handling do not activate
the covered log rows.
Editor builds compile all log levels; capture defaults to INFO, and Verbose capture
enables DEBUG/TRACE before formatting. App builds retain their existing compile
policy and do not allocate the editor logger ring.

Bakery queues mesh, font and texture cooker jobs on one worker. The worker launches a
cancellable child with explicit arguments, writes no renderer state, and publishes
completion before the UI reads results. Shutdown cancels and joins the worker.
New bake separates mesh, font, single texture and texture-directory sources,
retaining each source draft when the type changes. Jobs shows textual status,
selection, cancellation and retry; Output shows a wrapped 4 KiB display tail and
copies up to 16 KiB of captured status/output. Controls stack in narrow docks.
Mesh jobs accept `.obj`, `.gltf` and `.glb`, always rebuild, and replace the
source extension with `.vkb`. Font and texture cookers own incremental checks;
their Rebuild option bypasses unchanged-output skipping. All cookers own atomic
artifact publication. Reload the scene after mesh or texture baking; restart
the editor after baking a font it already loaded. Normal wrappers compile the
cookers without running asset baking. Pinned bootstrap fonts keep the first editor
launch independent of Bakery; see [ADR-034](034-offline-cooked-font-artifacts.md).

## Consequences

Widget IDs must be stable across frames. New geometry may have one-frame input
latency because hit tests use prior rectangles. Retained storage survives frame
scratch resets; changed-frame nodes and commands use frame scratch. The UI
direct path owns no persistent render target.

## Alternatives considered

A retained application widget tree duplicates caller state. A flex layout engine
adds a second layout policy. A persistent cached target adds storage, compositing,
and invalidation work without an implemented benefit.

## Revisit when

Measured UI cost justifies a target cache or a product requirement needs a
layout behavior grids cannot express.

## Code evidence

![Editor UI](../../assets/editor/editor-ui.png)

![Windows UI at 100% scale with icon edge coverage](../../assets/editor/ui-antialiasing-windows.png)

- [UI state and lowering](../../lib/src/renderer/systems/vkr_ui_system.c)
- [grid solver](../../lib/src/core/ui/vkr_ui_grid.c)
- [dock tree](../../lib/src/core/ui/vkr_ui_dock.c)
- [editor UI caller](../../editor/src/editor_ui.c)

- [editor scene panels](../../editor/src/editor_scene_panels.c)
- [edit journal and sidecars](../../lib/src/renderer/systems/vkr_scene_edit.c)
- [Console](../../editor/src/editor_console.c)
- [Bakery](../../editor/src/editor_bakery.c)
