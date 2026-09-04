---
status: implemented
updated: 2026-09-05
authority: adr
---
# ADR-047: Event callback payload lifetime and coalesced resize handoff

## Status

Accepted.

## Context

Events may carry variable payloads through a bounded ring buffer, while window
events arrive off the render thread. Callbacks need stable bytes without holding
the shared ring allocation for their full execution, and resize must not mutate
renderer state from the event worker.

## Decision

`EventManager` copies a dispatched payload into its ring buffer. The event
worker copies that payload into its 64 KiB local arena, releases the ring block,
copies the callback list, unlocks the manager, and calls subscribers. The local
payload remains valid only until all callbacks for that event return. A callback
that retains data makes its own copy.

`EventManager` owns the queued ring block; the event worker owns the local-arena
copy; each callback owns any retained copy it creates. `RendererFrontend` owns
the mailbox, and its render thread owns consumption and every resulting
renderer mutation.

The renderer resize subscriber writes a nonzero `width << 32 | height` value to
`RendererFrontend.pending_resize_mailbox` with release ordering. It ignores
zero dimensions. `vkr_renderer_prepare_frame()` atomically exchanges that value
with zero using acquire-release ordering and performs the resize before backend
frame preparation. Newer resize events may replace older pending dimensions.

## Consequences

The event mutex protects only event manager structures; callbacks synchronize
their own shared state. The mailbox transfers one complete latest dimension pair
but does not make renderer mutation thread-safe. The render thread is the sole
owner of resize and render-target mutation.

## Alternatives considered

Keeping the ring payload through callbacks couples callback duration to bounded
queue capacity. A renderer mutex in the event callback serializes platform
events with frame work and does not establish a renderer lifecycle boundary.

## Revisit when

Resize must preserve every intermediate size rather than only the newest value,
or renderer work moves to a separately owned render thread.

## Code evidence

- [event ownership and callback API](../../lib/src/core/event.h)
- [event worker lifetime ordering](../../lib/src/core/event.c)
- [payload ring](../../lib/src/core/vkr_event_data_buffer.c)
- [resize producer](../../lib/src/renderer/renderer_frontend.c)
- [mailbox consumer](../../lib/src/renderer/renderer_frontend.c)
