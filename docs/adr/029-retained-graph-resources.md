---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-029: Retain submitted image contents per subresource

## Status

Accepted.

## Context

Skipping an unchanged cascade requires valid image contents, not merely cached
allocation. A layout seeded as undefined can discard a reused allocation.

## Decision

`RETAINED` is an image-only, graph-owned, non-aliasable content contract. It
can combine with `PER_IMAGE` and `RESIZABLE`; incompatible transient, external,
history and frame-slot combinations are rejected. Retained buffers are unsupported.

Each backend owns terminal layout, stage/access and content-valid state for each
physical image instance and mip/layer. Compilation seeds that instance from its
last successfully submitted state. A read of an invalid retained subresource is
a compile error; writing another layer does not make it valid.

Compilation stages pending terminal state. Only successful native submission
commits it with `vkr_rg_commit_retained_state()`. Cancellation commits nothing.
Description changes and target recreation invalidate affected instances before
reuse; resource destruction still requires GPU completion.

The production `shadow_map` uses `RETAINED, PER_IMAGE, RESIZABLE` on both
backends. Omitted cascade passes consume previously submitted layers under
ADR-041. `PERSISTENT` only relaxes a diagnostic; it is not this content proof.
`HISTORY` selects another completed temporal instance and remains a distinct
lifetime policy.

## Consequences

A skipped producer is safe only with a valid selected physical instance and
matching shadow reuse proof. More per-subresource state is retained by native
resource owners. Source integration exists on both backends; that alone is not
a fresh native correctness result.

## Alternatives considered

Cached allocations or `PERSISTENT` cannot prove retained contents. A temporal
history ring changes which image is read rather than retaining one layer in
place. Cross-queue retention would need explicit queue dependencies.

## Revisit when

Retained buffers, aliasing or cross-queue consumers become required.

## Implementation

[`vkr_rg_compile.c`](../../lib/src/renderer/vkr_rg_compile.c),
[`vkr_render_graph.c`](../../lib/src/renderer/vkr_render_graph.c),
[`vkr_vulkan_graph.c`](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c), and
[`vkr_metal_packet_graph.inc`](../../lib/src/renderer/metal/internal/vkr_metal_packet_graph.inc).
