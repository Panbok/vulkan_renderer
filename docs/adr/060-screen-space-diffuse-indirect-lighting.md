---
status: declined
updated: 2026-10-12
authority: adr
---

# ADR-060: Independent screen-space diffuse indirect lighting

## Status

Declined (2026-10-12). Screen-space diffuse indirect lighting (SSGI) shipped
as an optional mode on the desktop pipeline, which Vulkan implements, and
was removed on 2026-10-12 by the owner's decision in
[Short-history rendering](../proposals/short-history-rendering.md), section 1.
The tiled pipeline never ran it
([ADR-087](087-gpu-class-graphics-pipelines.md)). This record keeps the
reasons, so the design is not tried again without addressing them.

## Context

Baked diffuse volumes ([ADR-054](054-baked-diffuse-volumes.md)) and
lightmaps ([ADR-088](088-baked-lightmap-sets.md)) provide static
multi-bounce light but cannot follow dynamic emission or lighting changes.
SSGI was added to supplement them with screen-visible diffuse bounce.

## Decision (removed)

Deferred lighting wrote a full-resolution source of direct diffuse light and
emission. A half-resolution pass traced one cosine-weighted ray per receiver
through a separate current-frame depth hierarchy, with at most 24 hierarchy
decisions and a 256-phase Halton sequence. A temporal pass filtered a 3×3
neighbourhood by receiver plane and normal, bounded fireflies in displayed
units and kept a colour, depth and identity history from the exact
preceding frame. The composite added the result outside valid baked-volume
coverage. The mode was off by default.

## Why it was removed

One ray per half-resolution receiver needed spatial and temporal filtering,
and the result stayed noisy and produced temporal artifacts after repeated
fixes between 2026-09 and 2026-10-12: flicker on Bistro walls and sills,
fireflies from small bright emitters, slow fades when a bounce source left
the view, and lost detail when content changed. Each fix added history
rules (all-miss and unanswered decay, placement allowances, the firefly
bound) and cost; local, non-authoritative profiles measured the SSGI passes
at about 0.6 ms per frame at 1280×720 on an RX 6700 XT, and 2.6 ms on the
removed Metal desktop implementation (M1 Pro). The owner decided that the
renderer must not hide stochastic shading behind long temporal history.

## Consequences

Diffuse indirect light comes from baked lightmaps and diffuse volumes on
both pipelines. Dynamic emission and lighting changes do not bounce until
runtime DDGI-style volume updates arrive on the desktop pipeline
([Sparse diffuse volumes](../proposals/sparse-diffuse-volumes.md),
[Short-history rendering](../proposals/short-history-rendering.md)).

Deferred lighting no longer writes the direct-light source, and its root no
longer carries `direct_source_texture` or `ssgi_enabled`. The SSGI sources,
graph passes and images, the `VKR_SSGI_DISABLED` variable and the SSGI
cases, fixtures, checks and verification files were removed with the
feature. The graphics setting `screen_space_gi` is a retired key that still
loads and has no effect. Git keeps the old record and its evidence.

## Alternatives considered

More rays per receiver or a longer history trade cost for noise or lag; a
longer history is what the owner rejected. Probe-based diffuse volumes with
ray-traced updates give low-frequency light that needs no per-pixel history
and can see off-screen sources.

## Revisit when

Baked and DDGI-style volume light cannot supply a required dynamic bounce,
and a candidate design produces a stable result from the current frame and
at most one previous frame, measured on Bistro for flicker and frame cost.
