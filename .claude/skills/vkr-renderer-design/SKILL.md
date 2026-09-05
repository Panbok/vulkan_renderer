---
name: vkr-renderer-design
description: Specify and review VKR renderer architecture, backend or graph changes, packet APIs, and hot paths. Apply to renderer refactors and resource or command ownership changes.
---

# VKR renderer design

Performance is correctness. Preserve pixels, ownership, GPU completion, and the
frame budget together. Use `vkr-performance` for timing claims.

## Before editing

Follow the repository's architecture discovery order. Read the affected status
and known-issue sections, then the owning ADR and concrete callers. State the
changed contract and the invariants the patch must preserve.

If an unresolved choice changes ownership, backend compatibility, resource
lifetime, public behavior, or an architectural boundary, ask the user immediately.
Give the concrete tradeoff and recommendation in a short question. Continue
independent work; do not implement the disputed choice or park it in a document.
Routine choices within an accepted contract do not need another approval.

Load references only for their purpose:

- [PRINCIPLES.md](PRINCIPLES.md) when changing a module interface or consolidating code.
- [VULKAN_PATTERNS.md](VULKAN_PATTERNS.md) when changing backend, graph, or submission ownership.
- [WORKFLOW.md](WORKFLOW.md) for an audit or migration spanning multiple slices.
- `vkr-memory` for allocator selection and CPU/GPU lifetime changes.
- `vkr-shaders` for shader source or a shader-visible host contract.

## C and hot paths

Keep each operation near the data and lifetime it owns. Prefer direct C
procedures, contiguous arrays, explicit bounds, and local `static` helpers.
Choose SoA, indirection, specialization, or batching from actual access patterns;
measure throughput claims. Keep one authoritative value and lower it once.

Validate and normalize at creation, publication, packet validation, or graph
compilation. Recording loops trust those producer guarantees. Per draw,
instance, and dispatch, permit no validation, recovery, null-guard, or assertion
branches. Partition optional work or provide valid sentinel records before the
loop. Algorithmic choices still need a data and cost justification.

Those loops also permit no heap allocation, arena growth or page commitment,
blocking wait, mutex acquisition, string formatting, name lookup, handle
acquire/release churn, or pipeline creation. Prepare capacity and pipeline state
before recording. A bump into already committed capacity is permitted.

GPU completion checks and required target/slot waits belong at frame preparation
or lifecycle boundaries. Removing a wait requires another completion proof;
removing the check alone is a lifetime bug.

## Graph and packet contracts

The shared graph compiler owns declared dependencies, pass order, culling, and
subresource state. Each selected backend realizes resources and records native
commands. Declare frame resource accesses in the authored graph. Backend-owned
uploads, IBL preparation, capture, and presentation outside that schedule retain
explicit access and completion ownership.

`VkrFrameInput` borrows arrays until `vkr_renderer_render_frame()` returns.
`vkr_renderer_begin_frame()` acquires a target and returns a `VkrFrame` context.
Input rejection and recording failure must cancel through the native backend,
resolving acquired resources and recorded-but-unsubmitted uses. Reject stale
frame contexts before touching a newer acquisition.

Graph `TRANSIENT` resources are reusable across realizations. Recreate them when
the resolved description changes; do not infer per-frame destruction or aliasing.

## Verification

Choose the smallest evidence loop that exercises the changed invariant through
`vkr-validation` and `vkr-harness`. Add a unit test only when an independent CPU
oracle detects a named failure more directly than the renderer case. A CPU
suite, shader compile, or source review alone cannot prove GPU correctness.
Use matched Release measurements for hot-path changes. Report unavailable
native evidence as unavailable; do not claim bilateral compatibility from one
backend's run.
