---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-010: ECS-owned scene state with a retained render mirror

## Status

Accepted.

## Context

Scenes need stable entity identity, composable components, transform hierarchy,
editor selection, and asset-owned mesh instances. The renderer cannot use
ECS storage directly as a GPU lifetime owner.

## Decision

`VkrWorld` owns entity identity, generations, archetypes, and components.
`VkrScene` owns scene-specific component registration, scene lifetime, compiled
queries, transform hierarchy state, and scene-level environment data.

`VkrMeshManager` is the retained rendering mirror. A scene synchronizes dirty
mesh/shape entities into mesh-manager instances; packet construction consumes
that mirror. Scene state remains authoritative for transforms, names, picking
IDs, text, shapes, and lights.

Transform updates use two passes: compiled-query chunk iteration rebuilds dirty
local matrices, then a parent-before-child order updates world matrices. A
child index accelerates hierarchy traversal; a query scan remains the fallback
when the index cannot be built. The same update marks renderable entities for
mirror synchronization.

## Consequences

Structural changes can move components between archetypes, so borrowed component
pointers do not survive those changes. A scene must release its mesh instances,
resource handles, and freeable keys before destroying its ECS arena. The render
mirror duplicates selected scene state by design; changing a render-relevant
component requires synchronization.

The current hierarchy order is rebuilt after structural hierarchy changes. There
is no spatial acceleration structure for scene draw classification.

## Alternatives considered

A node-only scene graph does not provide the required component composition or
chunk queries. Direct ECS packet extraction has no implemented GPU/resource
lifetime owner and is not selected.

## Revisit when

Measured scene churn, hierarchy updates, or visibility collection make the
current ECS-to-mesh-manager synchronization a frame cost.

## Code evidence

- [ECS](../../lib/src/core/vkr_entity.h)
- [scene ownership and update](../../lib/src/renderer/systems/vkr_scene_system.c)
- [retained mesh instances](../../lib/src/renderer/systems/vkr_mesh_manager.c)
