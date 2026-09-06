---
status: implemented
updated: 2026-09-06
authority: adr
---

# ADR-002: Declared frame dependencies and JSON topology

## Status

Accepted.

## Context

Frame order, conditional work, resource state and completion must agree across
two native command implementations. Implicit layer order cannot describe these
contracts or explain why a pass is retained.

## Decision

Author production topology in `assets/render_graphs/main.rendergraph.json`.
The selected backend parses it once, resolves frame conditions, extent/format
aliases, named executors and repeated passes, then builds and compiles the frame
graph. Each backend registry resolves executor names and pass types to operation
IDs; native switches record those operations. Repeated passes carry a typed
`repeat_index` for their native recorder.

`vkr_render_graph_prepare_frame()` owns portable per-frame conditions and counts:
viewport, exposure mode, transmission, picking, timing, shadows and
HZB/transmission/bloom/GTAO mip chains, plus GTAO constants. Both native paths
consume it. Native formats, realized resources, retained-content validity and
completion-based history selection remain backend-owned. This extraction does
not change the authored topology or introduce cached schedules. Both native
implementations prepare every enabled pass's resources, roots and command
parameters before recording the compiled schedule. Prepared command recorders
consume these typed records; native encoder and command-buffer boundaries remain
fallible.

Name/type binding includes conditional declarations before frame conditions are
evaluated. Vulkan recognizes the shared MetalFX executor names, but rejects them
if they enter the compiled execution order, before resource realization or command
recording. Disabled Metal-only passes do not prevent portable graph startup.

The shared compiler validates declarations, orders dependencies, culls work
outside exported/present/`NO_CULL` roots and emits subresource image barriers and
whole-buffer barriers. Same-layout writes remain hazards. Compatible uses within
one pass are combined; incompatible layouts fail compilation. Compute dispatch
and indirect-read dependencies have production callers.

Allocation-bearing builder mutators and frame reset return must-use success
values; resource and pass constructors preserve invalid-handle error reporting.
JSON realization aborts on a failed declaration. The builder retains ownership
of partial declarations until frame reset or destruction. Compilation rejects
scratch or capacity failure and exposes no execution order after failure.
Retained resource state commits only after native submission succeeds.

Image instance domains and buffer lifetimes are explicit. `TRANSIENT` resources
have frame-local contents in backend-owned overlap-safe reusable allocations,
recreated when descriptions change; they are not aliased. The shared graph owns
declarations and scheduling, while native caches own physical resources and their
allocation statistics. `RETAINED` contents follow ADR-029. History selection
requires completion and metadata checks in the owning backend.

Draw-table byte sizes use typed frame-capacity sources. Frame preparation derives
source capacity from the next power of two covering the scene candidate count,
with a minimum of one and the existing 262,144-candidate ceiling. Visible-row,
classification and indirect-command strides preserve four buckets, each allowing
at least the smaller of source capacity and the existing 65,536-draw limit.
The five main-view storage regions remain fixed in number.

Owned, non-history buffers can declare `GROW_ONLY`. The graph still records the
current requested size and generation. Each native cache owns its physical
high-water capacity, reuses sufficient backing and adopts the current generation.
Growth waits for all old instances' submitted uses, releases completed backing,
then allocates replacements. This avoids simultaneous old/new reservations;
first use of a larger scene can wait and allocate. Images, imported storage,
history and retained contents cannot use this policy.

All scheduled work uses the backend's graphics submission path. A compute or
transfer pass type does not imply another queue. The uncullable `IBL.Bake` pass
is scheduled by the graph, but its nested resource accesses and barriers remain
backend-owned. Upload and native capture/presentation operations outside complete
graph declarations also retain explicit barriers and completion ownership.

## Consequences

Topology and dependency rules are shared without a generic command RHI.
Undeclared accesses remain invisible to the compiler. Per-frame realization
cost, whole-buffer barriers and lack of aliasing are current limits.

## Alternatives considered

Imperative ordered layers hide dependencies. Reflection alone cannot infer
attachment, copy or presentation access. JSON code generation would trade
runtime authoring for build-time checks and is not implemented.

## Revisit when

Measured realization cost, asynchronous queue work or allocation pressure
justifies topology caching, queue ownership or aliasing. Extend declarations
before moving additional work under the graph.

## Implementation

[`vkr_render_graph_frame.c`](../../lib/src/renderer/vkr_render_graph_frame.c),
[`vkr_rg_json.c`](../../lib/src/renderer/vkr_rg_json.c),
[`vkr_rg_compile.c`](../../lib/src/renderer/vkr_rg_compile.c),
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json),
[`vkr_vulkan_graph.c`](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c), and
[`vkr_metal_packet_graph.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_graph.inc).
This record incorporates former ADR-003.
