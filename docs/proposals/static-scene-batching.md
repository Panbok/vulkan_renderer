---
status: proposed
updated: 2026-09-05
authority: proposal
---
# Static-scene batching

## Current baseline

Geometry is published into backend megabuffers and described by stable geometry
and material rows. The frame path classifies GPU candidates and emits indirect
work by draw-state bucket. `VkrMeshManager` still owns per-instance state and
the scene mirrors renderable entities into it. There is no separate static
scene batch asset or CPU-built MDI submission path.

## Proposed change

Investigate a static-instance representation that reduces only work proven
redundant after the existing geometry, material, candidate, and indirect paths.
It must identify which instance fields become immutable, how they are removed
from or represented in candidate streams, and how edits promote an instance
back to the normal mutable path.

## Decision boundaries

- Define static eligibility, mutation invalidation, and scene reload ownership.
- Preserve picking IDs, visibility, shadow mobility, material replacement,
  completion-gated publication, and resource retirement.
- Do not reintroduce retired CPU draw arrays or treat old CPU-MDI assumptions
  as the current architecture.

## Evidence needed

Use a representative static-heavy scene and matched Release measurements.
Report CPU collection cost, GPU candidate/indirect work, memory residency,
publication and unload behavior, and output/picking equivalence. Retain the
normal path unless results establish a workload-specific advantage.

## Code baseline

- [mesh instances](../../lib/src/renderer/systems/vkr_mesh_manager.c)
- [GPU geometry ABI](../../lib/src/renderer/vkr_gpu_abi.c)
- [renderer draw diagnostics](../../lib/src/renderer/vkr_renderer.c)
- [Vulkan frame graph](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c)
