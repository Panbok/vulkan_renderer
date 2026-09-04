---
status: proposed
updated: 2026-09-05
authority: proposal
---
# Editor UI extensions

## Current baseline

The immediate UI supports panels, labels, buttons, checkboxes, sliders, scroll
areas, and text fields with stable IDs, mouse layers, retained focus, and text
input. The dock tree supports bounded in-window splits and tabs with persisted
layout. The editor also has three application-specific draggable floating
metric/help windows; they are not dock-tree panels and are not persisted.

## Decision boundary: advanced components and accessibility

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

- [immediate widget API](../../lib/src/renderer/systems/vkr_ui_system.h)
- [UI state and focus](../../lib/src/renderer/systems/vkr_ui_system.c)
- [dock tree](../../lib/src/core/ui/vkr_ui_dock.c)
- [current floating overlays](../../editor/src/editor_windows.c)
