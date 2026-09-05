---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-036: Window content scale before UI layout

## Status

Accepted.

## Context

Window extent is not display scale. UI text, layout tracks, spacing, and input
geometry need one explicit logical-unit-to-device-pixel conversion.

## Decision

`VkrWindow` publishes an atomic content-scale snapshot with a revision. Windows
publishes `GetDpiForWindow() / 96`; macOS publishes its backing scale. Offscreen
UI uses an explicit positive scale. The UI system invalidates layout whenever
the snapshot changes and resolves authored style, tracks, text sizes, spacing,
and bounds into device pixels before layout. Retained text transforms do not
apply content scale again.

## Consequences

A scale-only display transition invalidates layout even when the output extent
does not change. UI layout, mouse coordinates, and scissor rectangles remain in
backing-pixel space after resolution. Content scale is an output/UI property;
internal Scene rendering scale does not redefine it.

## Alternatives considered

Deriving text scale from a reference resolution changes nominal UI size on an
ordinary resize. Physical panel DPI does not represent the operating system's
accessibility policy.

## Revisit when

A new production window backend defines its compositor-scale and event
semantics, or a HUD needs a separately accepted resolution-relative policy.

## Code evidence

- [window snapshot](../../lib/src/core/vkr_window.c)
- [Windows publication](../../lib/src/platform/vkr_window_windows.c)
- [macOS publication](../../lib/src/platform/vkr_window_macos.m)
- [UI scale use](../../lib/src/renderer/systems/vkr_ui_system.c)
- [style resolution](../../lib/src/core/ui/vkr_ui_style.c)
