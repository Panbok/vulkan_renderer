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
worker copies that payload and the callback list into its local arena, initialized
at 64 KiB, then releases the ring block, unlocks the manager, and calls subscribers.
The local payload remains valid only until all callbacks for that event return. A callback
that retains data makes its own copy.

`EventManager` owns the queued ring block; the event worker owns the local-arena
copy; each callback owns any retained copy it creates. `Application` owns the
resize subscription and mailbox. Its render thread consumes the mailbox and
performs the resulting renderer mutation.

Subscription returns false when its callback list cannot grow, preserving
existing registrations. Duplicate callback/user-data pairs succeed without
adding a row. Application initialization propagates registration failure through
partial initialization cleanup. The renderer has no event-manager dependency.

The application resize subscriber writes a nonzero `width << 32 | height` value
to `Application.pending_resize_mailbox` with release ordering. It ignores zero
dimensions. `application_draw_frame()` exchanges that value with zero using
acquire-release ordering and calls `vkr_renderer_resize()` before frame acquisition.
Newer resize events may replace older pending dimensions.

The renderer increments target generation on resize and successful target
recreation, including unchanged dimensions. The next acquired `VkrFrame` exposes
that generation. The application uses its change to resize UI target state and
invalidate retained shadow fitting before preparing scene data.

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
- [resize producer and mailbox consumer](../../lib/src/application.h)
- [target generation](../../lib/src/renderer/vkr_renderer.c)
