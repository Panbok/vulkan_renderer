---
status: implemented
updated: 2026-08-05
authority: adr
---
# ADR-001: Frontend/Backend Separation via Function-Pointer Interface

**Status:** Accepted

## Context

A renderer that exposes Vulkan types in its public API cannot be ported, cannot
be tested without a device, and forces every consumer to depend on the Vulkan
headers. At the same time, a fully virtualized RHI adds indirection to hot paths
and tends to lowest-common-denominator the feature set.

The project needed a seam that is real enough to keep Vulkan out of the public
API, but cheap enough to sit in the frame loop.

## Decision

Split the renderer into a **frontend** (`lib/src/renderer/renderer_frontend.c`)
and a **backend** (`lib/src/renderer/vulkan/`), connected by a
`VkrRendererBackendInterface` struct of function pointers, selected at
initialization by `VkrRendererBackendType`.

- The public header `vkr_renderer.h` contains **no Vulkan types**. GPU resources
  are opaque handles:
  `VkrTextureHandle`, `VkrBufferHandle`, `VkrPipelineHandle`,
  `VkrRenderPassHandle`, `VkrRenderTargetHandle`.
- Internal backing structures use the `struct s_*` convention
  (`struct s_BufferHandle`, `struct s_TextureHandle`).
- Renderer-level concepts that are backend-independent — the render graph,
  material system, mesh manager, scene system, font systems, pipeline registry —
  live in the frontend and call through the interface.

The frontend owns most subsystem state as direct members of
`struct s_RendererFrontend`, tying their lifetime to the renderer. The render
graph is separately allocated through a frontend-owned allocator, and the
active scene is an external reference; those exceptions have distinct lifetime
rules.

## Consequences

**Positive**

- The public API compiles without the Vulkan SDK.
- Backend-independent subsystems can be reused against a future backend, subject
  to auditing render-pass, descriptor-set, and synchronization assumptions that
  currently reflect the only implementation.
- Capability differences are surfaced explicitly as frontend flags
  (`supports_multi_draw_indirect`, `supports_draw_indirect_first_instance`)
  rather than hidden behind backend branching.

**Negative**

- One indirect call per backend operation. Negligible at current granularity
  (per-pass, per-bind, per-draw), but it is a real cost that would matter if the
  interface were ever pushed to per-primitive granularity.
- The interface must be extended in three places (interface struct, frontend
  wrapper, backend implementation) whenever a new backend capability is added.
- The abstraction is **unvalidated**: only one backend exists. Seams that have
  never been exercised by a second implementation usually leak.

## Alternatives Considered

- **Direct Vulkan calls throughout.** Simplest and fastest, but makes the
  renderer permanently single-API and forces Vulkan headers on all consumers.
  Rejected.
- **Compile-time backend selection via macros.** Zero indirection cost, but
  makes multi-backend builds impossible and pollutes the source with
  preprocessor branching. Rejected.
- **Full RHI with command-list recording abstraction.** More portable, but a
  large amount of machinery for a project with one backend, and it tends to
  obscure exactly the Vulkan synchronization details this project wants to make
  explicit. Rejected as premature.

## Revisit When

- A second backend is actually started — at that point the interface should be
  audited for Vulkan-shaped assumptions (render pass/framebuffer objects,
  descriptor set model) that leaked through.
  A design-stage audit was performed for the
  [bindless GPU-pointer renderer design](../bindless-gpu-pointer-renderer-spec.md)
  and found the predicted leak: `renderpass_create_desc`,
  `render_target_create`, `begin_render_pass`, `instance_state_acquire`, and
  `get_and_reset_descriptor_writes_avoided` encode Vulkan 1.2 concepts rather
  than renderer concepts. [ADR-020](020-bindless-backend-seam.md) proposes a
  parallel renderer implementation and defers a shared low-level seam until
  working Metal and modern-Vulkan slices expose real commonality. The actual
  implementation trigger has not fired: this ADR remains Accepted and the
  interface it defines continues to serve the shipping Vulkan 1.2 backend.
- Profiling shows interface dispatch in the frame loop's hot path.
