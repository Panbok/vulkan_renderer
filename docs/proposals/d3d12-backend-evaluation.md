---
status: proposed
updated: 2026-09-05
authority: proposal
---

# Conditional D3D12 backend evaluation

D3D12 is not scheduled. The current renderer explicitly selects only Metal or
Vulkan in [vkr_renderer_impl.c](../../lib/src/renderer/vkr_renderer_impl.c),
and [vkr_renderer_impl.h](../../lib/src/renderer/vkr_renderer_impl.h) exposes
only their implementation properties. Platform-selected typed functions define
the native operation boundary a third implementation would need to satisfy in
[vkr_renderer.c](../../lib/src/renderer/vkr_renderer.c).

## Current implementation baseline

The reusable contracts are the authored graph, versioned frame input, capture state,
GPU-memory and slot-table cores, and scene-facing systems. The backend-specific
work remains substantial: D3D12 would own DXGI targets, command submission,
completion fences, descriptor lifetime, graph lowering, pipeline and DXIL ABI
validation, capture, and diagnostics. Adding an enum value is not backend
implementation.

## Proposed gap

Reopen D3D12 only when a written requirement identifies a blocked Windows
deployment, a diagnostic that existing Vulkan tooling cannot provide, WARP as a
valuable software correctness target, or an explicit backend-breadth goal. The
first design then needs a parity-only vertical slice; image-quality features and
optional API capabilities remain separate proposals.

## Unsettled decisions

- The concrete requirement that justifies a permanent third maintenance and
  validation matrix.
- Whether D3D12 supplements Vulkan on Windows or replaces it for a defined
  platform profile.
- The required hardware, WARP, resize, capture, cache, and validation evidence
  for its first parity slice.
- The DXIL resource and root-signature contract that preserves completion-gated
  publication without introducing a generic command interface.
