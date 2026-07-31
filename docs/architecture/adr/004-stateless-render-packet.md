---
status: partial
updated: 2026-07-31
authority: adr
---
# ADR-004: Versioned Render-Packet Submission

**Status:** Accepted (partial)

## Context

The former public draw path exposed a call-ordered sequence of binds, globals,
and draws. That made a frame difficult to validate as a unit and tightly coupled
the application to render-pass order.

The renderer still owns retained resource registries, scene-facing systems,
descriptor state, and graph caches, so “stateless renderer” is too broad a
requirement. The useful boundary is a value-like description of one frame's
requested work.

## Decision

Submit optional typed pass payloads through one versioned `VkrRenderPacket`.
Packet version 2 includes frame data, globals, world/shadow/skybox/UI/editor/
picking payloads, text updates, and debug controls.

Implemented contract:

- a `NULL` pass payload disables that payload's work;
- packet version and payload ranges/pointers/handles are validated with a
  structured `VkrValidationError` field path;
- packet memory is caller-owned and must remain valid until submit returns;
- draw items use renderer handles and instance ranges rather than Vulkan state;
- pass executors obtain typed payloads through the graph context.

The application normally calls `vkr_renderer_prepare_frame()` first, builds the
packet in scratch storage, and calls `vkr_renderer_submit_packet()`.

### Current limits

The implementation does not yet satisfy the stronger “immutable frame validated
before any recording” model:

- `prepare_frame` waits/acquires and begins a command buffer before the packet
  exists;
- validation failures call `vkr_renderer_end_frame()` rather than canceling the
  acquired frame;
- submission mutates retained text, UI, globals, descriptors, graph caches, and
  resource-finalization state;
- packet handles depend on retained registries, so the packet is not
  independently serializable/replayable;
- graph execution failures are not part of the submit error contract because
  `vkr_rg_execute()` returns `void`.

The packet is therefore a good extraction and validation boundary, but not a
fully stateless renderer or transactional submission API.

## Consequences

**Positive**

- Per-frame inputs are grouped and mostly validated in one place.
- Pass activation and data are explicit rather than spread across draw calls.
- Caller/backend ownership is clearer than in an immediate public binding API.
- The payload arrays are a natural future partitioning boundary for parallel
  recording.

**Negative / risks**

- The complete frame must be materialized before submission.
- Adding/changing public payload layout requires versioning discipline.
- Retained handles and text/resource side effects prevent standalone replay.
- The prepare/submit split still carries ordering and failure-state complexity.
- Invalid-packet finalization currently assumes a swapchain color-attachment old
  layout even when no graph pass ran; this is a correctness bug, not merely an
  API naming issue.

## Alternatives Considered

- **Immediate public recording API.** More streamable, but restores application
  call-order coupling. Rejected.
- **Renderer-owned retained scene only.** Avoids packet construction, but binds
  renderer architecture to one scene representation. Rejected.
- **Transactional packet submission that acquires only after validation.** This
  is the preferred refinement of the accepted design and does not require
  abandoning packets.

## Revisit When

- Move validation before swapchain acquisition or implement a correct cancel
  path that preserves image layout and frame state.
- Define whether replay/capture is an actual requirement; if so, snapshot or
  remap retained resource identities explicitly.
- Make graph/pass execution return errors and include them in submit results.
- Introduce parallel command recording or packet construction.
