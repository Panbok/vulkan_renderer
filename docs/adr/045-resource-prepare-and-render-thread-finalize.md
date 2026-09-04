---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-045: Worker preparation and render-thread resource finalization

## Status

Accepted.

## Context

Meshes, scenes, and textures require CPU parsing and decoding plus renderer
state mutation and GPU publication. Those stages have different thread-safety
and lifetime rules.

## Decision

`VkrResourceSystem` tracks requests through pending CPU, dependency, GPU,
ready, failed, and canceled states. A loader may prepare a CPU-only payload on
a worker. Only `vkr_resource_system_pump()` invokes its finalizer, after the
renderer has activated a frame, so GPU and renderer state mutation stays on the
render thread. The resource system owns a prepared payload until exactly one
release callback runs, including cancellation and failure paths.

The pump applies request, upload-operation, and upload-byte budgets. Loaders
declare dependencies before reaching ready; consumers inspect request state and
resolved handles rather than treating a queued request as a usable resource.

## Consequences

Worker callbacks must not create GPU objects or mutate renderer/resource state.
Finalizers must be bounded by their declared cost and safely handle cancellation.
Shutdown first quiesces outstanding work, then lets subsystems release through
the still-registered loaders.

## Alternatives considered

GPU finalization on arbitrary workers would require broad renderer locking and
would break frame-lifecycle ownership. Synchronous loading remains available for
callers that require a terminal result.

## Revisit when

A backend provides an independently owned upload queue with a completion and
publication contract that preserves this ownership boundary.

## Code evidence

- [resource states and loader callbacks](../../lib/src/renderer/systems/vkr_resource_system.h)
- [request scheduling and pump](../../lib/src/renderer/systems/vkr_resource_system.c)
- [scene asynchronous loader](../../lib/src/renderer/resources/loaders/scene_loader.c)
- [texture asynchronous loader](../../lib/src/renderer/resources/loaders/texture_loader.c)
