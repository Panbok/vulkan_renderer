---
status: implemented
updated: 2026-09-05
authority: adr
---

# ADR-025: One coarse selected renderer implementation

## Status

Accepted.

## Context

The frontend must own portable scene-facing systems without selecting backend
behavior in each pass or exposing a generic native-command abstraction.

## Decision

Select exactly one `VkrRendererImpl` during initialization: Metal 4 on macOS
or capability-gated Vulkan 1.4 on Windows. Immutable capabilities and a coarse
operations table cover frame preparation/submission/cancellation, target changes,
completion, capture, metrics and destruction. The table has no per-pass or
per-draw operations.

The selected implementation owns native pipelines, resources, graph realization,
barrier lowering, command recording, submission and timestamps. Shared C owns
packet validation, authored graph semantics, portable arithmetic and the
lifetime cores in ADR-024. Assets publish through `VkrAssetPublisher`.

No legacy Vulkan 1.2 implementation, frontend shader/pipeline registry,
render-pass object system, descriptor-instance API, compatibility adaptor or
generic command RHI remains. The application and editor share `renderer_lib`
and a neutral sample runtime; neither executable supplies the other's sources.
Backend-specific quality modes require an explicit capability boundary, as in
ADR-039/040.

## Consequences

Ownership is concentrated at frame and resource boundaries. API-specific code
remains substantial, while portable semantics still require both native paths
to be verified. This is not an implementation promise for Linux or D3D12.

## Alternatives considered

Per-operation backend switches duplicate selection policy. A generic draw RHI
would restore an abstraction the GPU-pointer packet model does not require.
Keeping the retired renderer as a fallback would preserve incompatible owners.

## Revisit when

A concrete platform requirement authorizes another implementation or an existing
shared policy gains multiple native callers that justify extraction.

## Implementation

[`vkr_renderer_impl.h`](../../lib/src/renderer/vkr_renderer_impl.h),
[`vkr_renderer_impl.c`](../../lib/src/renderer/vkr_renderer_impl.c), and
[`renderer_frontend.c`](../../lib/src/renderer/renderer_frontend.c).
This record incorporates former ADR-020/021/022/026.
