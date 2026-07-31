---
status: partial
updated: 2026-07-31
authority: adr
---
# ADR-002: Compiled Render Graph with Declared Resource Access

**Status:** Accepted (partial)
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

This decision is only partially implemented:

- the compiler records image access masks, but `vkr_rg_execute.c` drops them and
  calls a layout-pair transition API;
- image barriers with equal old/new layouts are skipped, so same-layout memory
  hazards are not synchronized;
- buffer barriers with equal access flags are also skipped;
- image state is whole-resource even when a pass names a mip/layer slice;
- the backend supports a finite table of layout transitions;
- compute/transfer pass types do not imply separate queues or cross-queue
  scheduling;
- picking and IBL executors record graphics work against resources not declared
  in the graph.

`VKR_RG_RESOURCE_FLAG_TRANSIENT` resources are cached/reused across frames and
recreated when their resolved descriptions change. They are not currently freed
after each frame, despite the stale header comment, and they are not aliased.

## Consequences

**Positive**

- Topology, declared dependencies, pass culling, and instrumentation are
  centralized.
- Conditional/repeated pass structures are data rather than application call
  sequences.
- Known lifetimes provide the basis for future subresource tracking and
  aliasing.
- Generation-bearing handles catch some stale graph references.

**Negative / risks**

- An undeclared access is invisible to the compiler.
- The current executor does not yet honor enough of the compiled access state
  for synchronization correctness to be considered a graph guarantee.
- Hidden nested render passes make graph inspection incomplete.
- Per-frame graph realization and occasional cache rebuilds add CPU cost and
  can invoke device-idle waits when graph-owned objects change.
- Execution currently returns `void`; compile, barrier, and begin/end-pass
  failures are logged rather than propagated to packet submission.

## Alternatives Considered

- **Ordered layers with manual barriers.** Simpler, but preserves implicit
  dependencies and call-order coupling. Rejected.
- **Infer access from shader reflection.** Reflection cannot express attachment
  load/store operations or all non-shader transfers. Rejected.
- **Third-party graph.** No selected library matched the C11 allocator and
  backend model. Rejected at the time of implementation.

## Revisit When

- Complete stage/access/layout barrier emission, including same-layout hazards
  and mip/layer state.
- Move picking and IBL work into declared graph passes.
- Make compile/execution fallible and propagate errors to the caller.
- Measure per-frame realization and introduce topology caching if warranted.
- Add queue-aware compute/transfer scheduling or transient aliasing.
