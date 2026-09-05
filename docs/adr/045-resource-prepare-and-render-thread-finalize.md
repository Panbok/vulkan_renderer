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
release callback runs, including cancellation and failure paths. An executing
callback pins its request slot, path and payload against cancellation release;
the pump reacquires the request view after callbacks because worker dependency
requests can relocate the table. Cancellation preserves its terminal state and
unloads any finalized resource when its last request reference is released.

The pump budgets completion processing and terminal request transitions, and
schedules callbacks against estimated upload operations and bytes. A positive
upload budget permits one oversized request
when that budget dimension is unused, so a large indivisible upload can progress;
zero disables work with a nonzero cost in that dimension. These are scheduling
limits, not strict per-frame upload ceilings. Loaders declare dependencies before
reaching ready; consumers inspect request state and resolved handles.

## Consequences

Workers may enqueue CPU-side dependency requests through the resource system;
mesh preparation uses this path for materials. They do not publish GPU objects
or mutate render-thread subsystem state.
Finalizers must estimate their work and safely handle cancellation.
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
