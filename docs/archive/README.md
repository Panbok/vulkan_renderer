# Archive

Historical documentation. **Nothing here is current.** Every Markdown document
carries `status: superseded` or `status: investigation` and a pointer to
whatever replaced it. Non-Markdown artifacts are indexed through their
containing legacy directory.

Kept because the reasoning is expensive to reconstruct and cheap to store. Read
it for context on why something is shaped the way it is — never as a description
of how the code works today. For that, start at
[`../architecture/renderer-architecture-spec.md`](../architecture/renderer-architecture-spec.md).

## Superseded designs

The view/layer system was removed entirely; orchestration is now the JSON render
graph plus pass executors under `lib/src/renderer/passes/`.

| Document | Superseded by |
|---|---|
| [view-layer-system-refactor.md](view-layer-system-refactor.md) | [Render graph design](../rendering/render-graph-design.md) |
| [layer_event_communication_refactor.md](layer_event_communication_refactor.md) | [Render graph design](../rendering/render-graph-design.md) |
| [renderer_frontend_refactoring.md](renderer_frontend_refactoring.md) | [Architecture spec](../architecture/renderer-architecture-spec.md) |
| [multithreaded-vulkan-backend-spec.md](multithreaded-vulkan-backend-spec.md) | [Architecture spec](../architecture/renderer-architecture-spec.md) — retired from active scope |
| [frustum-culling-design.md](frustum-culling-design.md) | [ADR-013 draw-submission strategy](../architecture/adr/013-draw-submission-strategy.md) |
| [adr-001-frontend-backend-separation.md](adr-001-frontend-backend-separation.md) | [ADR-025 selected renderer implementation](../architecture/adr/025-selected-renderer-implementation-strategy.md) |
| [adr-011-vulkan-1-2-baseline.md](adr-011-vulkan-1-2-baseline.md) | [ADR-023 bindless capability profile](../architecture/adr/023-vulkan-1-4-bindless-capability-profile.md) and [ADR-026 retirement](../architecture/adr/026-vulkan-1-2-retirement.md) |

## Completed progress logs

Migrations that finished. The design documents they tracked are still active;
these logs recorded the phase-by-phase work.

| Document | Tracked work now described in |
|---|---|
| [render-graph-progress.md](render-graph-progress.md) | [Render graph design](../rendering/render-graph-design.md) |
| [stateless_renderer_progress.md](stateless_renderer_progress.md) | [Stateless renderer spec](../rendering/stateless_renderer/stateless_renderer_spec.md) |
| [pipeline-layout-reflection-and-cache-progress.md](pipeline-layout-reflection-and-cache-progress.md) | [Pipeline reflection spec](../rendering/pipeline-layout-reflection-and-cache-spec.md) |
| [multithreaded-vulkan-backend-progress.md](multithreaded-vulkan-backend-progress.md) | [Architecture spec](../architecture/renderer-architecture-spec.md) |
| [multithreaded-vulkan-backend-validation-matrix.md](multithreaded-vulkan-backend-validation-matrix.md) | [`vkr-validation` skill](../../.codex/skills/vkr-validation/SKILL.md) |
| [multithreaded-vulkan-backend-performance-matrix.md](multithreaded-vulkan-backend-performance-matrix.md) | [`vkr-performance` skill](../../.codex/skills/vkr-performance/SKILL.md) |

## Closed investigations and postmortems

Diagnoses, not plans. The conclusions that mattered were folded into the active
documents and skills listed beside them.

| Document | Conclusion lives in |
|---|---|
| [memory_leak_resolution.md](memory_leak_resolution.md) | [`vkr-memory` skill](../../.codex/skills/vkr-memory/SKILL.md) — allocator stats vs. real memory, hash-key ownership |
| [skybox-implementation-postmortem.md](skybox-implementation-postmortem.md) | [Architecture spec](../architecture/renderer-architecture-spec.md) |
| [csm-implementation-analysis.md](csm-implementation-analysis.md) | [CSM design](../rendering/cascading-shadow-mapping-design.md) |
| [csm-debugging-postmortem-and-next-steps.md](csm-debugging-postmortem-and-next-steps.md) | [CSM design](../rendering/cascading-shadow-mapping-design.md) |
| [csm-shadow-cutoff-investigation.md](csm-shadow-cutoff-investigation.md) | [CSM design](../rendering/cascading-shadow-mapping-design.md) |
| [world-text-picking-investigation.md](world-text-picking-investigation.md) | [Editor viewport and picking](../editor/editor-viewport-and-picking-design.md) |

## `spec-legacy/`

The former top-level `.spec/` directory: 45 markdown files (plus one `.txt`)
of design work that predates the render graph, the packet API, and the ECS
scene model. Much of it describes the view/layer system, the old shader-state
model, and pre-reflection pipeline registration — all removed.

It is kept whole rather than curated because its value is archaeological: it
explains how the current design was arrived at. Do not cite it as a
specification. [`spec-legacy/spec_index.md`](spec-legacy/spec_index.md) is its
original index.
