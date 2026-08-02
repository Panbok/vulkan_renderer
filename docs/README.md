# Documentation Index

Index of the active documentation tree. Every active document and non-Markdown
artifact is listed here. `archive/README.md` owns the recursive archive index;
the legacy pre-render-graph collection is deliberately indexed as one preserved
unit.

## Authority

1. **Code** is the implementation authority. If a document disagrees with the
   code, the code is what runs.
2. [Renderer architecture and status](architecture/renderer-architecture-spec.md)
   is the **status** authority — what is implemented, partial, or absent.
   §4 is the feature table; §8 is the prioritized known-issues list.
3. [Architecture Decision Records](architecture/adr/README.md) are the
   **rationale** authority — why a decision was made and what would revisit it.
4. Everything else is a design document, a specification, or an investigation.

**A design document's existence is not evidence that its feature ships.** Read
the `status` front-matter first.

## Status vocabulary

Every Markdown document except `README.md` files carries `status`, `updated`,
and `authority` front-matter. Non-Markdown artifacts, such as JSON schemas,
carry their status in this index.

| `status` | Meaning |
|---|---|
| `implemented` | Described behaviour matches current code |
| `partial` | Implemented with a stated gap — the gap is named in the document |
| `proposed` | Design only; no production code, or no production call site |
| `superseded` | Historical; names what replaced it. Lives under `archive/` |
| `investigation` | A diagnosis or postmortem, not a plan |

Conventions for writing and updating docs are in
[`.codex/skills/vkr-docs/SKILL.md`](../.codex/skills/vkr-docs/SKILL.md).

`docs/` is committed agent and contributor context. Keep this index and the
owning document status in the same change.

## Architecture

| Document | Status | Purpose |
|---|---|---|
| [renderer-architecture-spec.md](architecture/renderer-architecture-spec.md) | implemented | Current architecture, feature status, memory, sync, prioritized issues |
| [adr/README.md](architecture/adr/README.md) | — | ADR index and format |
| [event-payload-lifetime-and-resize-mailbox.md](architecture/event-payload-lifetime-and-resize-mailbox.md) | implemented | Stable event payload copies and lock-free resize mailbox |

### Architecture Decision Records

| ADR | Status | Decision |
|---|---|---|
| [001](architecture/adr/001-frontend-backend-separation.md) | implemented | Frontend/backend separation via function-pointer interface |
| [002](architecture/adr/002-render-graph.md) | partial | Compiled render graph with declared resource access |
| [003](architecture/adr/003-json-authored-render-graph.md) | implemented | JSON-authored render graph with named executors |
| [004](architecture/adr/004-stateless-render-packet.md) | partial | Versioned render-packet submission |
| [005](architecture/adr/005-reflection-driven-pipelines.md) | implemented | SPIR-V-reflected resource layouts with declarative manifests |
| [006](architecture/adr/006-cpu-memory-allocators.md) | implemented | Lifetime-specific CPU allocators behind a common interface |
| [007](architecture/adr/007-gpu-memory-allocation.md) | partial | Per-resource device-memory allocation |
| [008](architecture/adr/008-cpu-gpu-communication.md) | partial | Lifetime-tiered CPU↔GPU data paths |
| [009](architecture/adr/009-frame-synchronization.md) | partial | Per-image present semaphores, bounded frames in flight |
| [010](architecture/adr/010-ecs-scene-system.md) | implemented | Archetype ECS as authoritative scene state |
| [011](architecture/adr/011-vulkan-1-2-baseline.md) | implemented | Vulkan 1.2 baseline with classic render passes |
| [012](architecture/adr/012-texture-compression-pipeline.md) | implemented | Offline KTX2/UASTC packing with runtime transcode |
| [013](architecture/adr/013-draw-submission-strategy.md) | partial | Measured draw submission — culling, instancing, MDI; native Vulkan validation pending |
| [014](architecture/adr/014-offscreen-present-target.md) | proposed | Present target seam decoupling the frame path from the swapchain |
| [015](architecture/adr/015-metrics-module.md) | implemented | Centralized metrics registry with pre-registered slots |

## Rendering

| Document | Status | Purpose |
|---|---|---|
| [render-graph-design.md](rendering/render-graph-design.md) | partial | Access/subresource synchronization implemented; IBL bake coverage remains incomplete |
| [render-graph-schema.json](rendering/render-graph-schema.json) | — | JSON schema for `assets/render_graphs/*.rendergraph.json` |
| [stateless_renderer/stateless_renderer_spec.md](rendering/stateless_renderer/stateless_renderer_spec.md) | partial | Packet API design; see ADR-004 for the real boundary |
| [pipeline-layout-reflection-and-cache-spec.md](rendering/pipeline-layout-reflection-and-cache-spec.md) | partial | Reflection-driven pipeline layout and cache |
| [pbr-material-system-design.md](rendering/pbr-material-system-design.md) | implemented | Metallic-roughness PBR materials and texture slots |
| [texture-format-and-colorspace-design.md](rendering/texture-format-and-colorspace-design.md) | implemented | sRGB vs linear UNORM format selection |
| [cascading-shadow-mapping-design.md](rendering/cascading-shadow-mapping-design.md) | partial | CSM ships; historical view/layer integration examples remain |
| [csm/cascade-coverage-spec.md](rendering/csm/cascade-coverage-spec.md) | partial | Cascade policy ships; debug-mode validation remains unchecked |
| [csm/confirmed-assumptions.md](rendering/csm/confirmed-assumptions.md) | investigation | Verified CSM assumptions |
| [stable-csm-spec.md](rendering/stable-csm-spec.md) | proposed | Texel-snapped stable directional CSM |
| [lighting-system-design-plan.md](rendering/lighting-system-design-plan.md) | partial | Lighting across scene, ECS, and picking |
| [uniform-buffer-std430-migration.md](rendering/uniform-buffer-std430-migration.md) | partial | DX cbuffer → std140/std430 migration; phase 0 done |
| [render-pass-and-target-improvements.md](rendering/render-pass-and-target-improvements.md) | proposed | Render pass and target system improvements |
| [terrain-rendering-design.md](rendering/terrain-rendering-design.md) | proposed | Terrain rendering; no code exists |

## Assets and GPU data

| Document | Status | Purpose |
|---|---|---|
| [gltf-loader-design.md](assets/gltf-loader-design.md) | partial | glTF import ships; material conversion has named gaps |
| [texture-compression-policy/SPEC.md](assets/texture-compression-policy/SPEC.md) | implemented | Capability-driven `.vkt` policy with transcodable `NORMAL_RG` fallbacks |
| [texture-compression-vkt-ktx2-uastc-spec.md](assets/texture-compression-vkt-ktx2-uastc-spec.md) | partial | `.vkt` via KTX2 + Basis Universal UASTC |
| [texture-compression-vkt-ktx2-uastc-implementation-tracker.md](assets/texture-compression-vkt-ktx2-uastc-implementation-tracker.md) | implemented | Completed implementation tracker, including normal-map fallback hardening |
| [parallel-asset-loading.md](assets/parallel-asset-loading.md) | partial | Async CPU prep; parallel upload needs an unsafe opt-in |
| [async-gpu-transfer-queue.md](assets/async-gpu-transfer-queue.md) | proposed | Independent-submit end state; in-frame uploads are deferred, out-of-frame uploads still wait |
| [resource_loading_analysis.md](assets/resource_loading_analysis.md) | proposed | Resource loading analysis |
| [static-scene-batching-spec.md](assets/static-scene-batching-spec.md) | proposed | Static batching; `VkrDrawBatcher` is unwired |
| [san-miguel-obj-import-megabuffer-and-mdi-plan.md](assets/san-miguel-obj-import-megabuffer-and-mdi-plan.md) | proposed | Import dedup, mega-buffers, multi-draw indirect |

## Mesh, instancing, and performance

| Document | Status | Purpose |
|---|---|---|
| [mesh-system/mesh-assets-and-instances/SPEC.md](mesh-system/mesh-assets-and-instances/SPEC.md) | partial | Mesh asset dedup/instances ship; historical renderer names remain |
| [instanced-rendering/SPEC.md](instanced-rendering/SPEC.md) | partial | Direct instancing and shadow MDI ship; bindless/material-table phase absent |
| [instanced-rendering/opaque-compaction/SPEC.md](instanced-rendering/opaque-compaction/SPEC.md) | proposed | Opaque index compaction for an MDI megabuffer |
| [performance/ecs-hot-path-optimization-spec.md](performance/ecs-hot-path-optimization-spec.md) | partial | ECS hot-path changes ship; before/after performance is unmeasured |
| [performance/gpu-memory-baseline.md](performance/gpu-memory-baseline.md) | investigation | Captured device-memory baseline; concludes pooling is not justified by the numbers |
| [performance/p2-throughput-findings.md](performance/p2-throughput-findings.md) | investigation | Measured P2 plus review corrections for visibility, instancing, MDI, and camera handedness |

Performance workflow and evidence rules:
[`.codex/skills/vkr-performance/SKILL.md`](../.codex/skills/vkr-performance/SKILL.md).

## Tooling and automation

| Document | Status | Purpose |
|---|---|---|
| [renderer-harness-and-metrics-spec.md](tooling/renderer-harness-and-metrics-spec.md) | partial | Metrics, windowed profiling/automation boot, and direct snapshot phases 1-4 ship; baseline/debug replay and offscreen phases remain proposed |
| [renderer-metrics-phase1-verification.md](tooling/renderer-metrics-phase1-verification.md) | investigation | Phase-1 functional, Vulkan, pipeline-cache, and paired Release-overhead evidence |
| [renderer-metrics-phase1b-verification.md](tooling/renderer-metrics-phase1b-verification.md) | investigation | Phase-1b GPU-owner propagation, exactness, Vulkan, and paired Release-overhead evidence |
| [renderer-harness-phase2-verification.md](tooling/renderer-harness-phase2-verification.md) | investigation | Phase-2 parser, determinism, artifact, build, and isolated-profile evidence |
| [renderer-harness-phase2b-verification.md](tooling/renderer-harness-phase2b-verification.md) | investigation | Phase-2b benchmark-parity, authoritative-policy, retirement, and Release profile evidence |
| [renderer-harness-phase3-verification.md](tooling/renderer-harness-phase3-verification.md) | investigation | Phase-3 dependency-plan, full/automation work-equivalence, boot/residency, and Vulkan-validation evidence |
| [renderer-harness-phase4-verification.md](tooling/renderer-harness-phase4-verification.md) | investigation | Phase-4 capture-ring, graph-overlay, canonical converter, deterministic snapshot, and Vulkan-validation evidence |
| [harness-case-schema.json](tooling/harness-case-schema.json) | implemented | Draft-07 structural contract mirrored by the strict runtime case parser |
| [harness-profile-schema.json](tooling/harness-profile-schema.json) | implemented | Draft-07 execution-profile contract mirrored by the strict runtime parser |
| [harness-report-schema.json](tooling/harness-report-schema.json) | implemented | Draft-07 aggregate/child report contract for machine validation |

Rationale for the two decisions this series depends on:
[ADR-014](architecture/adr/014-offscreen-present-target.md) (present target
seam) and [ADR-015](architecture/adr/015-metrics-module.md) (metrics registry).

## Memory

| Document | Status | Purpose |
|---|---|---|
| [allocator-migration-plan.md](memory/allocator-migration-plan.md) | partial | Scope-based allocator migration; scopes wired but under-used |
| [temp-allocation-tracking-proposal.md](memory/temp-allocation-tracking-proposal.md) | proposed | Temporary allocation tracking |

Working rules for allocator choice and lifetime:
[`.codex/skills/vkr-memory/SKILL.md`](../.codex/skills/vkr-memory/SKILL.md).

## Scene and ECS

| Document | Status | Purpose |
|---|---|---|
| [scene-system-design.md](scene/scene-system-design.md) | partial | Scene model ships; historical renderer integration remains |
| [ecs-system-refactor-plan.md](scene/ecs-system-refactor-plan.md) | implemented | `vkr_entity` archetype ECS refactor |
| [scene-text-and-shapes-design.md](scene/scene-text-and-shapes-design.md) | partial | 3D text/shapes ship; historical examples and checklist remain |

## Editor

| Document | Status | Purpose |
|---|---|---|
| [editor-viewport-and-picking-design.md](editor/editor-viewport-and-picking-design.md) | partial | Viewport/picking ship; deferred example uses old APIs |
| [transform-gizmo-design.md](editor/transform-gizmo-design.md) | implemented | Translate/rotate/scale gizmo |

## Text and fonts

| Document | Status | Purpose |
|---|---|---|
| [font-system-design.md](text/font-system-design.md) | partial | Font system ships; planned snippets/checklist remain |
| [text-rendering-api-design.md](text/text-rendering-api-design.md) | implemented | Public text rendering API |
| [bitmap-font-loader-design.md](text/bitmap-font-loader-design.md) | partial | Bitmap loader ships; planned snippets/checklist remain |
| [mtsdf-font-loader-design.md](text/mtsdf-font-loader-design.md) | partial | MTSDF loader ships; historical integration/checklist remain |
| [system-font-loader-design.md](text/system-font-loader-design.md) | partial | System-font loader ships; planned snippets/checklist remain |
| [multi-variant-system-font-plan.md](text/multi-variant-system-font-plan.md) | partial | Multi-variant loading ships; pre-implementation baseline remains |
| [ui-text-implementation-design.md](text/ui-text-implementation-design.md) | partial | UI text ships; historical examples/checklist remain |

## UI

| Document | Status | Purpose |
|---|---|---|
| [ui-system-overview.md](ui/ui-system-overview.md) | partial | UI architecture overview and roadmap |
| [ui-element-primitives-design.md](ui/ui-element-primitives-design.md) | partial | UI element primitives |
| [ui-layout-engine-design.md](ui/ui-layout-engine-design.md) | proposed | Layout engine |
| [ui-components-library-design.md](ui/ui-components-library-design.md) | proposed | Component library; no code exists |
| [ui-docking-system-design.md](ui/ui-docking-system-design.md) | proposed | Docking system; no code exists |

## Effects system

The whole series is `proposed`. Compute passes in the JSON graph currently
orchestrate graphics and CPU work; real compute dispatch is not exercised, and
no tagging or effects code exists.

| Document | Status | Purpose |
|---|---|---|
| [00-overview.md](effects-system/00-overview.md) | proposed | Series overview |
| [01-tagging-system.md](effects-system/01-tagging-system.md) | proposed | Entity/mesh tagging |
| [02-compute-pipeline-support.md](effects-system/02-compute-pipeline-support.md) | proposed | Compute pipeline support |
| [03-effects-system-design.md](effects-system/03-effects-system-design.md) | proposed | Effects system core |
| [04-wave-effect-demo.md](effects-system/04-wave-effect-demo.md) | proposed | Wave effect demo |
| [05-scene-integration.md](effects-system/05-scene-integration.md) | proposed | Scene integration |
| [06-implementation-checklist.md](effects-system/06-implementation-checklist.md) | proposed | Implementation checklist |

## Archive

Historical material: superseded designs, completed progress logs, and closed
investigations. Retained for context; never current. Full listing and the
legacy `.spec/` pile are in [archive/README.md](archive/README.md).

| Document | Status | Superseded by |
|---|---|---|
| [view-layer-system-refactor.md](archive/view-layer-system-refactor.md) | superseded | Render graph design |
| [layer_event_communication_refactor.md](archive/layer_event_communication_refactor.md) | superseded | Render graph design |
| [renderer_frontend_refactoring.md](archive/renderer_frontend_refactoring.md) | superseded | Architecture spec |
| [render-graph-progress.md](archive/render-graph-progress.md) | superseded | Render graph design |
| [stateless_renderer_progress.md](archive/stateless_renderer_progress.md) | superseded | Stateless renderer spec |
| [pipeline-layout-reflection-and-cache-progress.md](archive/pipeline-layout-reflection-and-cache-progress.md) | superseded | Pipeline reflection spec |
| [multithreaded-vulkan-backend-spec.md](archive/multithreaded-vulkan-backend-spec.md) | superseded | Architecture spec |
| [multithreaded-vulkan-backend-progress.md](archive/multithreaded-vulkan-backend-progress.md) | superseded | Architecture spec |
| [multithreaded-vulkan-backend-validation-matrix.md](archive/multithreaded-vulkan-backend-validation-matrix.md) | superseded | `vkr-validation` skill |
| [multithreaded-vulkan-backend-performance-matrix.md](archive/multithreaded-vulkan-backend-performance-matrix.md) | superseded | `vkr-performance` skill |
| [memory_leak_resolution.md](archive/memory_leak_resolution.md) | investigation | `vkr-memory` skill |
| [skybox-implementation-postmortem.md](archive/skybox-implementation-postmortem.md) | investigation | Architecture spec |
| [csm-implementation-analysis.md](archive/csm-implementation-analysis.md) | investigation | CSM design |
| [csm-debugging-postmortem-and-next-steps.md](archive/csm-debugging-postmortem-and-next-steps.md) | investigation | CSM design |
| [csm-shadow-cutoff-investigation.md](archive/csm-shadow-cutoff-investigation.md) | investigation | CSM design |
| [world-text-picking-investigation.md](archive/world-text-picking-investigation.md) | investigation | Editor viewport and picking |
| [frustum-culling-design.md](archive/frustum-culling-design.md) | superseded | ADR-013 draw-submission strategy |
| [spec-legacy/](archive/spec-legacy/) | superseded | 46 files — the pre-render-graph `.spec/` pile |
