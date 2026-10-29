---
status: implemented
updated: 2026-10-28
authority: adr
---

# ADR-029: Retain submitted image and buffer contents per subresource

## Status

Accepted.

## Context

Skipping an unchanged cascade requires valid image contents, not merely cached
allocation. A layout seeded as undefined can discard a reused allocation.

## Decision

`RETAINED` is a graph-owned, non-aliasable content contract. On an image it
can combine with `PER_IMAGE` and `RESIZABLE`; incompatible transient, external,
history and frame-slot combinations are rejected. Since 2026-10-28 a buffer can
be retained too: `RETAINED` is then its lifetime flag, it has one instance and
one subresource, and `PER_IMAGE` is also rejected. A retained buffer's write of
any kind makes its contents valid; the writer owns which bytes a reader may
consume.

Each backend owns terminal layout, stage/access and content-valid state for each
physical image instance and mip/layer, and terminal stage/access and
content-valid state for each retained buffer, through the provider's
`read_buffer` and `commit_buffer` hooks. Compilation seeds that instance from its
last successfully submitted state. A read of an invalid retained subresource is
a compile error; writing another layer does not make it valid.

Compilation stages pending terminal state. Only successful native submission
commits it with `vkr_rg_commit_retained_state()`. Cancellation commits nothing.
Description changes and target recreation invalidate affected instances before
reuse; resource destruction still requires GPU completion.
Vulkan realizes a changed image or buffer after a timeline wait covering all
instances it replaces, including history producers and readers. Unchanged
resources do not wait at this boundary. This also covers editor viewport changes
that leave the window target unchanged.

The production `shadow_map` uses `RETAINED, PER_IMAGE, RESIZABLE` on both
backends. The desktop `light_table` is a retained buffer: `Lighting.Scatter`
writes the frame's light row updates into it and every pass that shades
lights declares a read, so its first write in a frame waits for the previous
frame's last read (ADR-135). Metal installs no buffer hooks yet, so a retained
buffer there seeds with invalid contents; the tiled graph declares none. Omitted cascade passes consume previously submitted layers under
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

Aliasing or cross-queue consumers become required.

## Implementation

[`vkr_rg_compile.c`](../../renderer/src/vkr_rg_compile.c),
[`vkr_render_graph.c`](../../renderer/src/vkr_render_graph.c),
[`vkr_vulkan_graph.c`](../../renderer/src/vulkan/vkr_vulkan_graph.c), and
[`vkr_metal_packet_graph.inc`](../../renderer/src/metal/internal/vkr_metal_packet_graph.inc).
