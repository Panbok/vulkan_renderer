---
status: implemented
updated: 2026-07-31
authority: design
---
# Event Payload Lifetime and Resize Mailbox

*Document Version: 1.1*
*Last Updated: 2026-07-31*
*Audience: engine maintainers and coding agents*

## Summary

This document records two implemented fixes:

1. `Event.data` is copied to event-thread scratch storage before the ring-buffer
   allocation is released, so callbacks see stable payload bytes.
2. Window resize events use an atomic, coalescing mailbox instead of taking
   `rf_mutex` on every frame.

The implementation lives in `lib/src/core/event.c` and
`lib/src/renderer/renderer_frontend.c`.

## Invariants

- When `Event.data_size > 0`, `Event.data` remains valid until every callback
  for that event returns.
- Ring-buffer storage is reclaimed before callbacks run; callback duration
  cannot hold the shared event-data buffer open.
- Platform callbacks never invoke Vulkan resize work. They publish a resize;
  the main render thread consumes it.
- A resize mailbox value of zero means no pending resize. A width or height of
  zero is ignored so minimize does not create a `0 x 0` render target.
- Repeated resize events may coalesce. The newest complete `(width, height)`
  pair wins.

## Event payload lifetime

`event_manager_dispatch()` copies non-empty payloads into
`VkrEventDataBuffer`. The event processor then:

1. dequeues the event while holding `EventManager.mutex`;
2. opens a scope in its 64 KiB thread-local arena;
3. copies the payload and rewrites a local `Event.data` pointer;
4. releases the original ring-buffer block;
5. copies the callback list;
6. unlocks the manager and invokes callbacks with the local event; and
7. ends the allocator scope after every callback returns.

Invalid event types, events without subscribers, and allocation failures still
release any pending ring-buffer payload. This ordering is part of the event
system's lifetime contract, not an optional optimization.

## Resize mailbox

`RendererFrontend.pending_resize_mailbox` is a `VkrAtomicUint64` with this
representation:

```text
0                              no pending resize
(uint64_t)width << 32 | height pending width and height
```

`vkr_renderer_on_window_resize()` is the event-thread producer. It ignores
zero-sized events and publishes the packed dimensions with release ordering.

`vkr_renderer_begin_frame()` is the main-thread consumer. It exchanges the
mailbox with zero using acquire-release ordering, unpacks a non-zero value, and
calls `vkr_renderer_resize()` before beginning backend frame work. The existing
pixel-size poll remains as a fallback for platform state changes that do not
arrive through the event path; it does not take `rf_mutex`.

The atomic transfers both dimensions and pending state in one operation, so a
consumer cannot observe width from one event and height from another.

## Ownership and synchronization

The mailbox only owns the cross-thread resize request. It does not make
renderer mutation thread-safe and does not remove `rf_mutex` from unrelated
renderer state. Vulkan resize and render-target mutation remain main-thread
operations.

Event callbacks still run on the dedicated event thread. A callback that retains
data beyond its invocation must make its own owned copy or use explicit
synchronization; the event-owned payload expires when the callback chain
returns.

## Validation

Relevant evidence for future changes:

- Run `./build_test.sh` for CPU regressions.
- Drag-resize continuously and verify that the latest size wins without
  corrupted dimensions.
- Minimize and restore; the renderer must never resize to zero.
- Run with Vulkan validation layers because resize recreates GPU resources.

Temporary logging must not remain in the per-frame resize check.

## Implementation owners

- `lib/src/core/event.c` — payload copy and ring-buffer release ordering.
- `lib/src/core/event.h` — dispatch/callback lifetime contract.
- `lib/src/renderer/renderer_frontend.h` — atomic mailbox storage.
- `lib/src/renderer/renderer_frontend.c` — mailbox producer and consumer.
