---
status: proposed
updated: 2026-09-13
authority: proposal
---
# Editor UI extensions

## Current baseline

The immediate UI supports panels, labels, buttons, checkboxes, sliders, scroll
areas, and text fields with stable IDs, mouse layers, retained focus, and text
input. The dock tree supports bounded in-window splits and tabs with persisted
layout. The editor also has four application-specific draggable floating windows
for Graphics, Draws, Memory, and Help. Their geometry, visibility, and z order are
persisted by [workspace settings](../../editor/src/editor_projects.c); they are
not dock-tree panels.

## Decision boundary: advanced components and accessibility

[Editor Projects](editor-projects.md) specifies the project chooser, scene wizard,
creation progress and content grid, including their keyboard and focus behavior.
This proposal retains the broader accessibility and floating-panel decisions;
Projects does not depend on native detachable windows or a general widget layer.

[Compute animation and editor](compute-animation-and-editor.md) proposes a
concrete node canvas, sequence timeline, and live model preview in a movable
in-window editor. It owns that feature's interactions and second-view rendering;
this document retains general panel detachment and accessibility scope.

Choose a specific missing interaction before adding a generic component layer.
The first accepted component must use the immediate API and retained-ID state,
not a create/destroy handle tree. Accessibility work needs an explicit target:
keyboard traversal alone, semantic metadata for testing, or a platform assistive
technology bridge. Define focus order, disabled semantics, names, and input
capture for that target before implementation.

## Decision boundary: first-class floating panels

Decide whether dock panels may detach into in-window floating containers, native
windows, or neither. A first-class in-window design must own z order, geometry,
input layering, focus, close/reattach behavior, persistence, and Scene-panel
mapping. It may reuse the existing metric/help overlay mechanics only after
their lifetime and serialization model match dock tabs.

## Evidence needed

For each accepted slice, add focused interaction coverage for mouse, keyboard,
layout restore, and docking/reattachment. A native-window option also needs
platform lifecycle and content-scale behavior on both production platforms.
No UI performance benefit is assumed.

## Code baseline

- [immediate widget API](../../runtime/src/renderer/systems/vkr_ui_system.h)
- [UI state and focus](../../runtime/src/renderer/systems/vkr_ui_system.c)
- [dock tree](../../runtime/src/core/ui/vkr_ui_dock.c)
- [current floating overlays](../../editor/src/editor_windows.c)
