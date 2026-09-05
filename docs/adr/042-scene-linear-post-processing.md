---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-042: Scene-linear exposure, bloom and ambient visibility

## Status

Accepted.

## Context

Post effects must preserve temporal radiance, expose independent bypasses and
modify the lighting terms they physically approximate.

## Decision

Keep temporal history scene-linear. Meter the post-temporal HDR source with a
256-bin log-luminance histogram and percentile exposure resolve. Renderer-owned
frame delta/discontinuities control completion-safe EV history: valid adaptation
is rate-bounded and invalid history snaps to target. Tonemap consumes GPU state
without synchronous CPU readback; delayed completed samples expose diagnostics.
Manual exposure remains an explicit alternative.
Histogram dispatches use complete 16x16 threadgroups on both backends: all 256
lanes initialize and merge bins, while edge lanes omit out-of-extent source reads.

Bloom prefilters scene-linear HDR with threshold/soft knee into a bounded
half-resolution chain, downsamples, accumulates deepest-first, and combines into
full-resolution HDR before exposure multiplication/tonemap. Separate graph
resources preserve read/write dependencies. Shared arithmetic handles non-finite
and extreme input and pins the knee/Karis behavior. Bloom can bypass independently.

GTAO uses current-frame depth and normals before deferred lighting. A dedicated
positive-view-depth R16 pyramid feeds full-resolution three-slice/three-step
horizon evaluation, separate raw R8 visibility and edge data, and an edge-aware
3x3 denoise. A white fallback disables the effect without per-light branching.
Depth-pyramid horizon samples use nearest texel and nearest mip filtering.
Slice directions use the signed view-space pixel scale, including projection Y,
and the positive/negative horizon bounds share the integration sign convention.
It multiplies indirect diffuse after material AO; direct and specular terms keep
their separate policies. It is not reused HZB history or general wall visibility.

Automatic exposure, bloom and GTAO are enabled by production initialization.
Authored graph conditions and packet globals retain isolated bypasses and direct
intermediate capture channels. Final output follows ADR-043.

## Consequences

Effects have observable inputs and independent controls. Pixel equivalence,
quality acceptance and GPU cost still require matched cases on each backend;
enabling effects is a workload change.

## Alternatives considered

Pre-exposing temporal history couples adaptation to reconstruction. AO on all
lighting darkens direct/specular terms incorrectly. Reusing stale HZB as current
GTAO depth changes the input contract.

## Revisit when

Accepted image-quality fixtures require a different metering, bloom or AO model,
or matched Release cost justifies changing resolution/quality.

## Implementation

[`vkr_exposure.c`](../../lib/src/renderer/vkr_exposure.c),
[`vkr_bloom.c`](../../lib/src/renderer/vkr_bloom.c),
[`vkr_gtao.c`](../../lib/src/renderer/vkr_gtao.c),
[`shared/`](../../lib/src/renderer/shaders/shared), and
[`main.rendergraph.json`](../../assets/render_graphs/main.rendergraph.json).
