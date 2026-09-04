---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-046: One editor viewport mapping for scene presentation and interaction

## Status

Accepted.

## Context

A docked editor Scene panel has an output rectangle, an optional internal Scene
extent, and pointer coordinates. Camera projection, composition, picking, and
gizmos must agree on the same mapping.

## Decision

`VkrViewportMapping` is the source for conversion between the dock-owned Scene
panel, the scene target, and normalized coordinates. The editor derives this
mapping from the current dock panel rectangle and uses it to build the packet
viewport payload. The renderer realizes Scene resources at the mapped extent,
composites Scene output into the panel rectangle, and draws editor UI afterward
at the native drawable extent.

Picking requests use mapped scene coordinates. Picking IDs distinguish scene
entities and gizmo handles; the runtime resolves those results into selection
and gizmo interaction. Scene-only mode uses the complete drawable mapping while
preserving the dock tree.

## Consequences

No caller may independently infer camera aspect, picking coordinates, gizmo
coordinates, or compositor placement from window size. A panel resize changes
the mapping before the next packet. Picking remains asynchronous and may be
pending when its completion has not arrived.

## Alternatives considered

Separate mappings for camera, picking, and composition drift when a dock split,
fit mode, or internal Scene scale changes. A retained editor mesh duplicates
the panel rectangle without owning the dock decision.

## Revisit when

The editor supports multiple simultaneous Scene panels or a presentation mode
that cannot be expressed by the existing panel-to-target mapping.

## Code evidence

- [viewport mapping](../../lib/src/renderer/systems/vkr_editor_viewport.c)
- [picking lifecycle](../../lib/src/renderer/systems/vkr_picking_system.c)
- [gizmo IDs](../../lib/src/renderer/systems/vkr_gizmo_system.h)
- [editor interaction caller](../../runtime/src/vkr_sample_runtime.c)
