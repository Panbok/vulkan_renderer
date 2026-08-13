---
status: partial
updated: 2026-08-13
authority: adr
---
# ADR-002: Compiled Render Graph with Declared Resource Access

**Status:** Accepted (partial). Binding-addressed uses, executable compute
descriptors, graph buffers, indirect-read synchronization, explicit overlap-safe
lifetimes, mip chains, and per-subresource image views shipped with deferred
visibility-buffer phases P0-P2 on 2026-08-13.
**Supersedes:** the removed `VkrViewSystem` / `VkrLayer` composition path.

## Context

The former ordered-layer model made topology, resource lifetime, and
synchronization implicit in call order. Conditional editor rendering and
multi-pass features required imperative branching, while per-pass timing and
unused-pass elimination had no common home.

A graph can make dependencies and lifetime visible, but it is only correct when
all accesses are declared and the executor faithfully implements the compiled
barriers.

## Decision

Use `vkr_render_graph.*`, `vkr_rg_compile.c`, and `vkr_rg_execute.c` to declare,
compile, and execute frame work.

Passes declare image/buffer reads and writes, color/depth attachments, a
pipeline domain, flags, and an executor. Compilation:

- validates handles and declarations;
- builds dependencies and a topological execution order;
- culls passes not reachable from present/export/`NO_CULL` roots;
- resolves graph-owned resources and their lifetime intervals;
- records pre-pass image and buffer barrier descriptions.

Images may identify attachment mip/layer slices, and graph handles include a
generation. Imported swapchain, depth, and external textures can participate in
the graph. CPU timings, optional GPU timestamps, and graph-resource statistics
are part of the implementation.

The graph is rebuilt from the parsed JSON model for every packet submission and
compiled during execution. Resource/render-pass/target caches avoid recreating
stable objects, but topology realization itself is currently per frame.

### Current synchronization boundary

Synchronization for declared resources is implemented, with these boundaries:

- declared image accesses reach the backend with access masks and mip/layer
  ranges; same-layout write hazards are preserved;
- declared buffers are realized per persistent, external, target-image,
  frame-slot, or history lifetime and are replaced/reused only after the
  recorded submit completes;
- storage-write to indirect-read dependencies lower through the authored
  `DRAW_INDIRECT` stage on Metal and Vulkan;
- compute passes carry a typed executor plus validated direct or indirect
  dispatch data; no production GPU kernel exercises the generic path yet;
- barrier state is tracked per image subresource, and compatible declarations
  within one pass are combined before its pre-barriers are emitted;
- incompatible same-pass image layouts fail graph compilation;
- buffer barriers are write-aware but still cover whole buffers;
- compute/transfer pass types do not imply separate queues or cross-queue
  scheduling;
- picking attachments and pixel-copy access are declared, but the IBL executor
  still records nested graphics work against resources not declared in the
  graph.

Image resources are cached/reused across frames and recreated when their
resolved descriptions change. They are not currently aliased. Buffers require
one explicit overlap-safe lifetime class; no default single transient buffer is
accepted while frames overlap.

## Consequences

**Positive**

- Topology, declared dependencies, pass culling, and instrumentation are
  centralized.
- Conditional/repeated pass structures are data rather than application call
  sequences.
- Known lifetimes and subresource state provide the basis for future aliasing.
- Generation-bearing handles catch some stale graph references.

**Negative / risks**

- An undeclared access is invisible to the compiler.
- Hidden nested render passes make graph inspection incomplete.
- Per-frame graph realization and occasional cache rebuilds add CPU cost and
  can invoke device-idle waits when graph-owned objects change.
- Queue ownership and buffer byte ranges are not modeled.

## Alternatives Considered

- **Ordered layers with manual barriers.** Simpler, but preserves implicit
  dependencies and call-order coupling. Rejected.
- **Infer access from shader reflection.** Reflection cannot express attachment
  load/store operations or all non-shader transfers. Rejected.
- **Third-party graph.** No selected library matched the C11 allocator and
  backend model. Rejected at the time of implementation.

## Revisit When

- Move the remaining IBL bake work into declared graph passes.
- Measure per-frame realization and introduce topology caching if warranted.
- Add queue-aware compute/transfer scheduling or transient aliasing.
