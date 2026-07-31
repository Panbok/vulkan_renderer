---
status: implemented
updated: 2026-07-31
authority: adr
---
# ADR-011: Vulkan 1.2 Baseline with Classic Render Passes

**Status:** Accepted

## Context

The API baseline controls minimum device support but does not by itself decide
which optional/core features the renderer enables. Vulkan 1.2 includes useful
capabilities such as timeline semaphores and descriptor indexing feature
structures, while dynamic rendering and synchronization2 can also be exposed by
extensions on implementations below Vulkan 1.3.

The project targets Windows and macOS through MoltenVK. Capability claims about
those platforms should come from tested device matrices, not from assuming a
fixed “practical ceiling” for a moving implementation.

## Decision

Request Vulkan 1.2 at instance creation and reject physical devices reporting
an API version below 1.2. Use the classic `VkRenderPass` / `VkFramebuffer`
recording model in the only backend.

Current feature choices are:

- binary WSI semaphores and fences (ADR-009);
- classic pipeline barriers rather than synchronization2;
- traditional descriptor sets rather than descriptor indexing/bindless;
- no `vkCmdBeginRendering` dynamic-rendering path;
- a persisted `VkPipelineCache` with load-failure fallback and the repository
  `validate_pipeline_cache.sh` check.

These are implementation choices, not all requirements of Vulkan 1.2. Timeline
semaphores and optional descriptor-indexing features could be adopted without
necessarily changing the API version floor, subject to capability checks.

## Consequences

**Positive**

- One tested render-pass model serves the current Windows/macOS target intent.
- Attachment compatibility and load/store behavior are explicit objects.
- The current graph/pipeline registry already understands named render passes
  and targets.
- Pipeline cache persistence can reduce repeat pipeline creation work.

**Negative**

- Render-pass/framebuffer compatibility and recreation add backend complexity.
- The renderer does not demonstrate modern dynamic-rendering or
  synchronization2 APIs.
- Per-material descriptor sets contribute to draw submission overhead.
- Binary-only internal synchronization is cumbersome for future async
  upload/compute dependencies.
- The repository does not contain a maintained cross-device capability matrix
  that proves which optional paths are safe on each target.

## Alternatives Considered

- **Require Vulkan 1.3.** Simplifies access to modern core APIs but raises the
  device/translation-layer floor. Defer until target measurements justify it.
- **Keep 1.2 and add optional modern extensions/features.** Preferred upgrade
  direction: capability-gated dynamic rendering, synchronization2, timeline
  semaphores, or descriptor indexing can be adopted independently.
- **Lower baseline with extensions.** Broadens compatibility but multiplies
  feature negotiation and testing. Rejected for current scope.

## Revisit When

- Establish a validation matrix for native Windows Vulkan and the supported
  MoltenVK/macOS versions.
- A modern feature materially simplifies graph barriers, pipeline creation, or
  async work enough to justify dual paths or a higher floor.
- Bindless/material tables or timeline-based upload dependencies are designed.
