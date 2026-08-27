---
status: partial
updated: 2026-08-27
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
Packet version 22 includes frame data, exposure, bloom, and GTAO globals,
world/shadow/skybox/UI/editor/picking payloads, text updates, debug controls,
and candidate publication generations.

Implemented contract:

- a `NULL` pass payload disables that payload's work;
- packet version and payload ranges/pointers/handles are validated with a
  structured `VkrValidationError` field path;
- packet memory is caller-owned and must remain valid until submit returns;
- draw items use renderer handles and instance ranges rather than Vulkan state;
- pass executors obtain typed payloads through the graph context;
- world candidates are one borrowed static-first array with nonzero
  static/dynamic/publication generations; selected implementations lower it into
  completion-protected per-slot candidate/instance buffers;
- static residency state is staged while recording and committed only after
  successful queue submission, so packet rejection cannot publish rows that
  were never copied;

The application normally calls `vkr_renderer_prepare_frame()` first, builds the
packet in scratch storage, and calls `vkr_renderer_submit_packet()`.

### Current limits

The implementation does not yet satisfy the stronger “immutable frame validated
before any recording” model:

- `prepare_frame` waits/acquires and begins a command buffer before the packet
  exists;
- packet validation is side-effect-free, and failures cancel the acquired frame
  by re-recording a discard-only submission;
- submission mutates retained text, UI, globals, descriptors, graph caches, and
  resource-finalization state;
- packet handles depend on retained registries, so the packet is not
  independently serializable/replayable;
- graph execution and backend cancellation failures are part of the submit
  error contract, but retained mutations before a graph failure are not
  transactional.

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
- Validation still occurs after swapchain acquisition, so invalid packets pay a
  discard submission and present instead of avoiding WSI work entirely.

## Alternatives Considered

- **Immediate public recording API.** More streamable, but restores application
  call-order coupling. Rejected.
- **Renderer-owned retained scene only.** Avoids packet construction, but binds
  renderer architecture to one scene representation. Rejected.
- **Transactional packet submission that acquires only after validation.** This
  is the preferred refinement of the accepted design and does not require
  abandoning packets.

## Revisit When

- Move validation before swapchain acquisition if the extra packet-build/API
  sequencing is justified by measured invalid-packet frequency.
- Define whether replay/capture is an actual requirement; if so, snapshot or
  remap retained resource identities explicitly.
- Define transactional semantics for retained-state mutation if callers need a
  rejected packet to leave no retained changes.
- Introduce parallel command recording or packet construction.
