---
status: implemented
updated: 2026-09-05
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

[`vkr_rg_json.c`](../../lib/src/renderer/vkr_rg_json.c),
[`vkr_rg_compile.c`](../../lib/src/renderer/vkr_rg_compile.c),
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json),
[`vkr_vulkan_graph.c`](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c), and
[`vkr_metal_packet_graph.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_graph.inc).
This record incorporates former ADR-003.
