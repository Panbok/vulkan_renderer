# Documentation Index

Index of the active documentation tree. Every active document and non-Markdown
artifact is listed here. `archive/README.md` owns the recursive archive index;
the legacy pre-render-graph collection is deliberately indexed as one preserved
unit.

## Optional Metal validation

Debug builds and Metal validation are diagnostic configurations. Use them only
for a concrete issue reproduction, with the smallest case that exercises the
issue. Snapshot/baseline comparisons and performance runs must use the normal
Release configuration with validation environment variables unset; run any
needed validation as a separate diagnostic command.

Metal validation is opt-in on macOS, including for Debug builds:

- `MTL_DEBUG_LAYER=1` enables API validation for incorrect Metal API usage.
- `MTL_SHADER_VALIDATION=1` enables shader/GPU validation for faults such as
  invalid or out-of-bounds resource access.

Enable either mode independently or both together before launching:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
  ./build_run.sh Debug --renderer metal
```

**Warning:** these diagnostics can tank frame rate, especially shader
validation. In one local, non-authoritative Debug observation, enabling both
dropped the renderer from roughly 30–40 FPS to 6–7 FPS. Debug timings are not
performance evidence; never compare or profile performance with either mode
enabled. `build_run.sh` prints a warning whenever it detects a nonzero
validation variable. Do not run shader/GPU validation across a broad
multi-capture or baseline suite: a validation-enabled 14-capture snapshot was
immediately followed by a macOS watchdog kernel panic. A later bounded P18
reproduction was also followed by a 91-second `watchdogd` timeout with
`MTLCompilerService` active. This remains correlation rather than a proven
driver root cause. A later minimal P18/P19 run passed with both validators when
executed serially. Run exactly one validation-enabled Metal renderer process at
a time, never launch parallel validation children, and keep shader validation
to the smallest focused case.

The Metal backend bypasses pipeline-archive lookup while shader validation is
enabled because Apple's validator currently crashes in the Metal 4-to-3
archive-conversion path. These runtime variables must be present before the
process creates its first Metal device; when running
`build_debug/app/vulkan_renderer` directly, set them in the calling environment.

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
| [bindless-gpu-pointer-renderer-spec.md](architecture/bindless-gpu-pointer-renderer-spec.md) | partial | Metal 4 Stages 0–5 and modern Vulkan through V7 ship as the two selected strategies. Vulkan is the sole Vulkan renderer, the legacy migration surface is removed, cross-platform shared-core witnesses pass, and the post-V7 target Windows correctness rerun passes; Linux remains unsupported |
| [bindless-vulkan-backend-spec.md](architecture/bindless-vulkan-backend-spec.md) | implemented | Windows V0–V7 end state: authored graph, full pass/IBL path, shared asynchronous capture, timestamps, lifecycle, metrics, cache, selection, and audited Vulkan 1.2 deletion. Historical V0/V3 executables and fixture-only walking/diagnostic plumbing are removed; production harness and native Debug/Release validation pass; no performance claim is inferred |
| [d3d12-backend-evaluation.md](architecture/d3d12-backend-evaluation.md) | proposed | Evaluation of D3D12 as a third `VkrRendererImpl`. Recommends not scheduling it without a concrete Windows compatibility, tooling, or deployment requirement; defines the evidence and full-backend acceptance work needed to reopen the decision |
| [bindless-renderer-audit-spec.md](architecture/bindless-renderer-audit-spec.md) | investigation | Static audit and implementation record for both selected backends and the shared GPU cores. Recommendations 1–11 are implemented, including the Vulkan unit split, shared lighting/IBL/transmission flags, immutable GPU material rows, 432-byte pass roots, and the reflected 48-byte table-driven draw roots introduced by deferred P3. Windows CPU/SPIR-V/runtime-reflection evidence and exact captures pass for the audited pre-P3 root; native Apple M1 Pro reflection and focused Metal API validation pass for P3, while its Vulkan-native/pixel evidence remains open. No performance claim is inferred |

### Architecture Decision Records

| ADR | Status | Decision |
|---|---|---|
| [001](archive/adr-001-frontend-backend-separation.md) | superseded | Function-pointer backend interface, superseded by ADR-025 and removed by ADR-026 |
| [002](architecture/adr/002-render-graph.md) | partial | Compiled render graph with declared resource access |
| [003](architecture/adr/003-json-authored-render-graph.md) | implemented | JSON-authored render graph with named executors |
| [004](architecture/adr/004-stateless-render-packet.md) | partial | Versioned render-packet submission |
| [005](architecture/adr/005-reflection-driven-pipelines.md) | implemented | SPIR-V-reflected resource layouts with declarative manifests |
| [006](architecture/adr/006-cpu-memory-allocators.md) | implemented | Lifetime-specific CPU allocators behind a common interface |
| [007](architecture/adr/007-gpu-memory-allocation.md) | partial | Per-resource device-memory allocation |
| [008](architecture/adr/008-cpu-gpu-communication.md) | partial | Lifetime-tiered CPU↔GPU data paths |
| [009](architecture/adr/009-frame-synchronization.md) | partial | Per-image present semaphores, bounded frames in flight |
| [010](architecture/adr/010-ecs-scene-system.md) | implemented | Archetype ECS as authoritative scene state |
| [011](archive/adr-011-vulkan-1-2-baseline.md) | superseded | Vulkan 1.2 baseline, replaced by ADR-023 and removed by ADR-026 |
| [012](architecture/adr/012-texture-compression-pipeline.md) | implemented | Offline KTX2/UASTC packing with runtime transcode |
| [013](architecture/adr/013-draw-submission-strategy.md) | partial | Measured draw submission — culling, instancing, MDI; native Vulkan validation pending |
| [014](architecture/adr/014-offscreen-present-target.md) | implemented | Present target seam decoupling the frame path from the swapchain |
| [015](architecture/adr/015-metrics-module.md) | implemented | Centralized metrics registry with pre-registered slots |
| [016](architecture/adr/016-hdr-environment-format.md) | implemented | Equirectangular HDR delivery, cubemap runtime |
| [017](architecture/adr/017-prepared-specular-glossiness-lowering.md) | implemented | Prepared specular-glossiness lowering with retained dielectric F0/F90 response in the runtime PBR path |
| [018](architecture/adr/018-graph-declared-transmission-feedback.md) | implemented | Separate opaque, feedback-copy, transmission, and ordinary-blend stages; Metal P18 adds a four-layer ordered feedback chain |
| [019](architecture/adr/019-bounded-forward-spatial-lighting.md) | implemented | Stable punctual-light table, fragment-local bitmask grid, and fragment-aware local IBL |
| [020](architecture/adr/020-bindless-backend-seam.md) | partial | Parallel renderer implementation boundary for the bindless path |
| [021](architecture/adr/021-metal-first-bindless-backend.md) | implemented | Metal 4 selected on macOS; capability-gated Vulkan selected on Windows |
| [022](architecture/adr/022-gpu-pointer-resource-model.md) | partial | GPU-address resources, backend-native texture references, and backend-lowered graph dependencies |
| [023](architecture/adr/023-vulkan-1-4-bindless-capability-profile.md) | accepted | Vulkan 1.4 bindless profile; descriptor buffers required, with base-swapchain reacquisition completion on the RX 6700 XT window path |
| [024](architecture/adr/024-shared-bindless-gpu-cores.md) | implemented | Memory, submit-ring, ABI, slot-table, and capture-ring cores have real Metal and Vulkan callers with cross-platform regression witnesses |
| [025](architecture/adr/025-selected-renderer-implementation-strategy.md) | accepted | One coarse selected renderer strategy for Metal and Vulkan, replacing the backend-type ladder and legacy adaptor |
| [026](architecture/adr/026-vulkan-1-2-retirement.md) | implemented | Vulkan 1.2 backend, shaders, legacy frontend model, interface, and graph migration residue removed under explicit owner authorization |
| [027](architecture/adr/027-immediate-mode-grid-ui.md) | proposed | Immediate-mode UI over a retained cache, grid-only layout, batched scissored draw list with evidence-gated tile caching, and a composited offscreen editor viewport |
| [028](architecture/adr/028-gpu-driven-deferred-visibility-buffer.md) | implemented through P21 | GPU-driven deferred visibility buffer; both backends use one GPU-submitted topology with opaque/deferred shading, HZB, picking, four-layer transmission, and completion-gated layer coverage. The legacy world renderer and runtime fallback are retired; the measured eight-pass P19 compact path is the Metal default with a diagnostic full-screen rollback |
| [029](architecture/adr/029-retained-graph-resources.md) | partial | Retained graph-owned contents with per-instance, per-subresource content validity, seeded from the last successfully submitted state and committed only on proven submit. Prerequisite for directional cascade reuse; distinct from the `HISTORY` ring and from `PERSISTENT`, which only suppresses a diagnostic |
| [030](architecture/adr/030-offline-mesh-optimization-and-cooking.md) | accepted | Version-14 meshoptimizer-backed `.vkb` cooking, worker validation/decode, mandatory runtime source optimization, production conversion tooling, lifecycle/byte/locality metrics, and `EXT_meshopt_compression` input decode |
| [031](architecture/adr/031-versioned-packed-static-geometry-abi.md) | accepted | Implemented 32-byte static geometry ABI shared by Metal and Vulkan; float32 UVs replace the rejected 24-byte float16-UV candidate |
| [032](architecture/adr/032-two-phase-confirmed-visibility.md) | investigation | Declined for the measured workload after the predictor deferred zero candidates. The exact-gated one-phase path remains; revisit only for a materially different candidate population |
| [033](architecture/adr/033-occupied-depth-sdsm-feedback.md) | partial | Metal implements opt-in occupied-depth feedback with completion and source-projection metadata. Fixed splits remain the measured default; Vulkan lowering is open |
| [034](architecture/adr/034-offline-cooked-font-artifacts.md) | proposed | Replace untracked font-generator outputs and runtime metric conversion with a validated versioned offline-cooked artifact; no production implementation exists |
| [035](architecture/adr/035-canonical-mtsdf-screen-pixel-range-shading.md) | proposed | Adopt canonical derivative-based MTSDF range reconstruction and evidence-gate VKR's alpha minification fallback; no production implementation exists |
| [036](architecture/adr/036-dpi-derived-ui-text-scale.md) | proposed | Replace the Windows-only design-extent text transform with per-window OS content scale applied before layout; no production implementation exists |
| [037](architecture/adr/037-portable-same-resolution-temporal-antialiasing.md) | partial | Portable same-resolution TAA with shared Metal/Vulkan resolve, completion-safe history, stable output-grid reprojection, rigid transparent motion, stationary transparency accumulation, bounded moving-camera composition reactivity, output-space FXAA in the existing final draw, and native Apple validation; deformation/procedural/particle motion, broader motion evidence, and owner acceptance remain open |
| [038](architecture/adr/038-sh-l2-diffuse-irradiance.md) | proposed | Replace the baked 64² diffuse response cubemap with GPU-resident second-order SH coefficients for `E/pi`, using completion-safe slots; retains cubemaps for source, skybox, and specular prefilter. Gated on indirect-diffuse quality review and matched Release deferred-lighting measurement; no production implementation exists |

## Rendering

| Document | Status | Purpose |
|---|---|---|
| [shader-cross-backend-contract.md](rendering/shader-cross-backend-contract.md) | partial | Complete source inventory for the shared and Vulkan shader trees, with Metal parity witnesses, exact values/layouts/dispatches, snapshot evidence, and explicit unaligned gates for draw overflow, ABI coverage, IBL, shadows, temporal/deferred paths, tonemap, and text |
| [image-quality-roadmap.md](rendering/image-quality-roadmap.md) | partial | **Ordered plan for image-quality work.** Presentation correctness, portable same-resolution TAA, stationary transparency accumulation, post-TAA FXAA, automatic exposure E0-E3, bloom B0-B1, and GTAO G0-G2 ship. GTAO adds a dedicated current-frame view-depth pyramid, full-resolution horizon evaluation and edge-aware denoise, indirect-diffuse-only integration, and direct depth/normal/raw/final visibility captures on both backends. Metal and Windows Vulkan runtime evidence passes, including stable Vulkan fixed-camera exposure, non-vacuous post-effect smoke cases, and focused validation. Authored deformation motion, broader animation/disocclusion fixtures, authoritative post-effect performance, and final-color acceptance remain open; MSAA is outside the implemented slice |
| [presentation-dpi-and-transfer-function-spec.md](rendering/presentation-dpi-and-transfer-function-spec.md) | partial | Roadmap items 1 and 2 ship: Windows uses Per-Monitor V2 physical pixels, both backends use one linear-to-sRGB output contract, and retained UI/text colors blend in linear RGB. Mixed-DPI/display fixtures and replacement final-color golden acceptance remain pending; internal render scale remains a separate later feature |
| [visibility-buffer-msaa-spec.md](rendering/visibility-buffer-msaa-spec.md) | partial | Temporal phases T0-T2 and the portable TAA decision ship under ADR-037. FSR, MetalFX, and every visibility-buffer MSAA phase remain unimplemented |
| [post-exposure-bloom-and-ambient-occlusion-spec.md](rendering/post-exposure-bloom-and-ambient-occlusion-spec.md) | partial | Roadmap items 4 and 6. Automatic exposure E0-E3, bloom B0-B1, and GTAO G0-G2 ship. GTAO uses packet controls, a dedicated current-frame R16 view-depth pyramid, full-resolution 3-slice × 3-step evaluation, edge-aware R8 denoise, branchless white fallback, and indirect-diffuse-only integration with direct capture channels. Deterministic isolated-cold Release snapshots, moving thin-occluder coverage, timing attribution, focused Metal validation, and Windows Vulkan smoke/Bistro/validation evidence pass. Authoritative matched performance and final-color owner acceptance remain open |
| [windows-vulkan-post-effect-parity-investigation.md](rendering/windows-vulkan-post-effect-parity-investigation.md) | investigation | **Windows Vulkan post-effect parity investigation.** Records the shared positive-Z normal decode, BasisU BC5/EAC source-channel correction, Vulkan exposure descriptor fix, bounded EV adaptation, corrected deferred matrix direction, non-vacuous fixtures, and matched M1 Pro/RX 6700 XT GTAO-on/off closure. Exposure differs by at most `0.003181 EV` and common foreground normals have mean dot `0.999080`; final-color baseline publication and authoritative performance remain separate owner decisions |
| [vulkan-dx12-metal-comparison.md](rendering/vulkan-dx12-metal-comparison.md) | proposed | Corrected semantic comparison of the production Vulkan and Metal implementations and a possible D3D12 backend. Separates portable renderer contracts from backend-owned mechanisms, records capability limits for temporal upscaling and optional API features, and removes unsupported external-engine mappings |
| [deferred-visibility-buffer/SPEC.md](rendering/deferred-visibility-buffer/SPEC.md) | implemented through P21 | GPU-driven deferred visibility-buffer contract. Both backends use the sole GPU-submitted topology with opaque/deferred shading, HZB, picking, four-layer transmission, and completion-gated coverage. Metal additionally defaults to the measured eight-pass P19 compact path. Rationale in [ADR-028](architecture/adr/028-gpu-driven-deferred-visibility-buffer.md) |
| [render-graph-design.md](rendering/render-graph-design.md) | partial | Access/subresource synchronization implemented; IBL bake coverage remains incomplete |
| [render-graph-schema.json](rendering/render-graph-schema.json) | — | JSON schema for `assets/render_graphs/*.rendergraph.json` |
| [stateless_renderer/stateless_renderer_spec.md](rendering/stateless_renderer/stateless_renderer_spec.md) | partial | Packet API design; see ADR-004 for the real boundary |
| [pipeline-layout-reflection-and-cache-spec.md](rendering/pipeline-layout-reflection-and-cache-spec.md) | partial | Reflection-driven pipeline layout and cache |
| [pbr-material-system-design.md](rendering/pbr-material-system-design.md) | implemented | Metallic-roughness PBR materials, texture slots, and legacy diffuse compatibility |
| [texture-format-and-colorspace-design.md](rendering/texture-format-and-colorspace-design.md) | implemented | sRGB vs linear UNORM format selection |
| [cascading-shadow-mapping-design.md](rendering/cascading-shadow-mapping-design.md) | partial | CSM ships; historical view/layer integration examples remain |
| [csm/confirmed-assumptions.md](rendering/csm/confirmed-assumptions.md) | investigation | Verified CSM assumptions |
| [stable-csm-spec.md](rendering/stable-csm-spec.md) | proposed | Texel-snapped stable directional CSM |
| [lighting-system-design-plan.md](rendering/lighting-system-design-plan.md) | partial | Lighting across scene, ECS, and picking |
| [bistro-baseline-shading-investigation.md](rendering/bistro-baseline-shading-investigation.md) | investigation | Historical Bistro diagnosis, owner-audit corrections for punctual lights, materials, face orientation, and indoor IBL, plus validation-clean evidence |
| [shadow-transmission-transparency-improvements.md](rendering/shadow-transmission-transparency-improvements.md) | partial | Implemented transmission, cascade-fit, and transparency record; residual PCF/SDSM work is superseded by the current shadow rewrite spec. Its §2.1 glTF-parity claims overstate what ships — corrected in [transmission-shading-correctness-spec.md](rendering/transmission-shading-correctness-spec.md) |
| [transmission-shading-correctness-spec.md](rendering/transmission-shading-correctness-spec.md) | proposed | Source audit and staged T0-T5 correction plan for transmission inputs, lobe partition, backend parity, volume reprojection, roughness feedback, and peel-bound evidence. The existing peel ships, but the corrective work does not. OIT stays rejected per [ADR-028](architecture/adr/028-gpu-driven-deferred-visibility-buffer.md) |
| [shadow-cpu-cost-and-csm-rewrite-spec.md](rendering/shadow-cpu-cost-and-csm-rewrite-spec.md) | partial | P0/P1, P2 completion-safe candidate residency, P3 retained cascade reuse, P5 bounded refresh, P6 opt-in SDSM, and P7 receiver quality ship. P4 closed without a topology change; fixed splits and zero proactive refresh remain defaults, while P3/P7 retain Vulkan evidence gaps |
| [hdr-environment-ibl-spec.md](rendering/hdr-environment-ibl-spec.md) | implemented | HDR equirect loading, equirect→cubemap bake, half-float IBL storage, prefilter mip fix, and tonemap activation |
| [sh-l2-diffuse-irradiance-spec.md](rendering/sh-l2-diffuse-irradiance-spec.md) | proposed | Implementation plan for [ADR-038](architecture/adr/038-sh-l2-diffuse-irradiance.md). Stages SH0-SH3 define normalized `E/pi` coefficients, a completion-safe generation pool, selected-mip reference checks, temporary A/B ABI, indirect-diffuse captures, pre-retirement performance evidence, and final packet version 23. No production code exists |
| [uniform-buffer-std430-migration.md](rendering/uniform-buffer-std430-migration.md) | partial | DX cbuffer → std140/std430 migration; phase 0 done |
| [render-pass-and-target-improvements.md](rendering/render-pass-and-target-improvements.md) | proposed | Render pass and target system improvements |
| [terrain-rendering-design.md](rendering/terrain-rendering-design.md) | proposed | Terrain rendering; no code exists |

## Assets and GPU data

| Document | Status | Purpose |
|---|---|---|
| [gltf-loader-design.md](assets/gltf-loader-design.md) | partial | glTF import ships; nested texture resolution, UV orientation, and partial legacy material conversion are documented |
| [texture-compression-policy/SPEC.md](assets/texture-compression-policy/SPEC.md) | implemented | Capability-driven `.vkt` policy with transcodable `NORMAL_RG` fallbacks |
| [texture-compression-vkt-ktx2-uastc-spec.md](assets/texture-compression-vkt-ktx2-uastc-spec.md) | implemented | KTX2/UASTC `.vkt`, capability-driven transcode, compressed upload, and persistent target-transcode cache |
| [texture-compression-vkt-ktx2-uastc-implementation-tracker.md](assets/texture-compression-vkt-ktx2-uastc-implementation-tracker.md) | implemented | Completed implementation tracker, including normal-map fallback hardening |
| [parallel-asset-loading.md](assets/parallel-asset-loading.md) | partial | Async CPU prep; parallel upload needs an unsafe opt-in |
| [async-gpu-transfer-queue.md](assets/async-gpu-transfer-queue.md) | proposed | Independent-submit end state; in-frame uploads are deferred, out-of-frame uploads still wait |
| [resource_loading_analysis.md](assets/resource_loading_analysis.md) | proposed | Resource loading analysis |
| [meshoptimizer-geometry-pipeline-spec.md](assets/meshoptimizer-geometry-pipeline-spec.md) | implemented | Version-14 cooked geometry, mandatory source optimization, packed Metal/Vulkan ABI, observability, production assets, and glTF meshopt interop |
| [static-scene-batching-spec.md](assets/static-scene-batching-spec.md) | proposed | Static batching; the `VkrDrawBatcher` it targeted was deleted by ADR-028 P21 |

## Mesh, instancing, and performance

| Document | Status | Purpose |
|---|---|---|
| [mesh-system/mesh-assets-and-instances/SPEC.md](mesh-system/mesh-assets-and-instances/SPEC.md) | partial | Mesh asset dedup/instances ship; historical renderer names remain |
| [performance/ecs-hot-path-optimization-spec.md](performance/ecs-hot-path-optimization-spec.md) | partial | ECS hot-path changes ship; before/after performance is unmeasured |
| [performance/gpu-memory-baseline.md](performance/gpu-memory-baseline.md) | investigation | Captured device-memory baseline; concludes pooling is not justified by the numbers |
| [performance/p2-throughput-findings.md](performance/p2-throughput-findings.md) | investigation | Measured P2 plus review corrections for visibility, instancing, MDI, and camera handedness |

Performance workflow and evidence rules:
[`.codex/skills/vkr-performance/SKILL.md`](../.codex/skills/vkr-performance/SKILL.md).

## Tooling and automation

| Document | Status | Purpose |
|---|---|---|
| [renderer-harness-and-metrics-spec.md](tooling/renderer-harness-and-metrics-spec.md) | implemented | Metrics, deterministic profiling, capture/comparison/autotest, guarded baselines, and true window-free offscreen execution ship through Phase 6 |
| [renderer-metrics-phase1-verification.md](tooling/renderer-metrics-phase1-verification.md) | investigation | Phase-1 functional, Vulkan, pipeline-cache, and paired Release-overhead evidence |
| [renderer-metrics-phase1b-verification.md](tooling/renderer-metrics-phase1b-verification.md) | investigation | Phase-1b GPU-owner propagation, exactness, Vulkan, and paired Release-overhead evidence |
| [renderer-harness-phase2-verification.md](tooling/renderer-harness-phase2-verification.md) | investigation | Phase-2 parser, determinism, artifact, build, and isolated-profile evidence |
| [renderer-harness-phase2b-verification.md](tooling/renderer-harness-phase2b-verification.md) | investigation | Phase-2b benchmark-parity, authoritative-policy, retirement, and Release profile evidence |
| [renderer-harness-phase3-verification.md](tooling/renderer-harness-phase3-verification.md) | investigation | Phase-3 dependency-plan, full/automation work-equivalence, boot/residency, and Vulkan-validation evidence |
| [renderer-harness-phase4-verification.md](tooling/renderer-harness-phase4-verification.md) | investigation | Phase-4 capture-ring, graph-overlay, canonical converter, deterministic snapshot, and Vulkan-validation evidence |
| [renderer-harness-phase5-verification.md](tooling/renderer-harness-phase5-verification.md) | investigation | Phase-5 debug replay, canonical comparison, autotest separation, guarded baseline, and Vulkan-validation evidence |
| [renderer-harness-phase6-verification.md](tooling/renderer-harness-phase6-verification.md) | investigation | Phase-6 target seam, WSI-free offscreen lifecycle, target equivalence, recreation, and Vulkan-validation evidence |
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
| [text-resolution-independence-and-font-cooking-spec.md](text/text-resolution-independence-and-font-cooking-spec.md) | proposed | Evidence-driven plan for offline font cooking, canonical MTSDF shading, and DPI-derived UI text layout; no production implementation exists |

## UI

| Document | Status | Purpose |
|---|---|---|
| [ui-architecture-spec.md](ui/ui-architecture-spec.md) | proposed | **Authoritative UI design.** Immediate-mode API over a retained cache, grid-only layout, batched scissored draw list, tile binning and hashing, composited editor viewport. Rationale in [ADR-027](architecture/adr/027-immediate-mode-grid-ui.md) |
| [ui-components-library-design.md](ui/ui-components-library-design.md) | proposed | Component library under the spec (phase P3); component inventory still useful, retained-element lifetimes superseded. No code exists |
| [ui-docking-system-design.md](ui/ui-docking-system-design.md) | proposed | Docking system under the spec (phase P6); in-window only. No code exists |

The overview, layout-engine, and element-primitives designs are superseded by the
spec above and archived — see the [archive index](archive/README.md).

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
