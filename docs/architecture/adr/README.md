# Architecture Decision Records

This directory records the significant architectural decisions in the VKR
renderer: what was decided, why, what was given up, and what would cause the
decision to be revisited.

An ADR is written when a decision constrains future work. Decisions that are
purely local or trivially reversible do not get one.

## Format

Each ADR follows: **Status → Context → Decision → Consequences → Alternatives
Considered → Revisit When**.

`Status` is one of:

- **Accepted** — in force and implemented.
- **Accepted (partial)** — decided and implemented, but with known unfinished
  integration; the gap is stated in the ADR.
- **Proposed** — recommended, not yet implemented.
- **Declined** — measured or reviewed and deliberately not adopted.
- **Superseded by ADR-NNN** — no longer in force.

## Index

| ADR | Title | Status |
|---|---|---|
| [ADR-001](../../archive/adr-001-frontend-backend-separation.md) | Frontend/backend separation via function-pointer interface | Superseded by ADR-025 |
| [ADR-002](002-render-graph.md) | Compiled render graph with declared resource access | Accepted (partial) |
| [ADR-003](003-json-authored-render-graph.md) | JSON-authored render graph with named executors | Accepted |
| [ADR-004](004-stateless-render-packet.md) | Versioned render-packet submission | Accepted (partial) |
| [ADR-005](005-reflection-driven-pipelines.md) | SPIR-V-reflected resource layouts with declarative manifests | Accepted |
| [ADR-006](006-cpu-memory-allocators.md) | Lifetime-specific CPU allocators behind a common interface | Accepted |
| [ADR-007](007-gpu-memory-allocation.md) | Per-resource device memory allocation | Accepted (partial) |
| [ADR-008](008-cpu-gpu-communication.md) | Lifetime-tiered CPU↔GPU data paths | Accepted (partial) |
| [ADR-009](009-frame-synchronization.md) | Per-image present semaphores and bounded frames in flight | Accepted (partial) |
| [ADR-010](010-ecs-scene-system.md) | Archetype ECS as authoritative scene state | Accepted |
| [ADR-011](../../archive/adr-011-vulkan-1-2-baseline.md) | Vulkan 1.2 baseline with classic render passes | Superseded by ADR-023/026 |
| [ADR-012](012-texture-compression-pipeline.md) | Offline KTX2/UASTC packing with runtime transcode | Accepted |
| [ADR-013](013-draw-submission-strategy.md) | Measured draw submission: culling, instancing, and MDI | **Accepted (partial)** |
| [ADR-014](014-offscreen-present-target.md) | Present target seam for offscreen rendering | Accepted |
| [ADR-015](015-metrics-module.md) | Centralized metrics registry with pre-registered slots | Accepted |
| [ADR-016](016-hdr-environment-format.md) | Equirectangular HDR delivery, cubemap runtime | Accepted |
| [ADR-017](017-prepared-specular-glossiness-lowering.md) | Prepared specular-glossiness lowering with retained dielectric reflectance | Accepted |
| [ADR-018](018-graph-declared-transmission-feedback.md) | Graph-declared transmission feedback | Accepted |
| [ADR-019](019-bounded-forward-spatial-lighting.md) | Stable-table, fragment-local bitmask-grid lighting | Accepted |
| [ADR-020](020-bindless-backend-seam.md) | Parallel renderer implementation boundary for the bindless path | Accepted (partial) |
| [ADR-021](021-metal-first-bindless-backend.md) | Metal 4 first; modern Vulkan for Windows and Linux | Accepted and implemented |
| [ADR-022](022-gpu-pointer-resource-model.md) | GPU-address resources, native texture references, backend-lowered dependencies | Accepted (partial) |
| [ADR-023](023-vulkan-1-4-bindless-capability-profile.md) | Vulkan 1.4 bindless profile with descriptor buffers and base-swapchain reacquisition completion | Accepted |
| [ADR-024](024-shared-bindless-gpu-cores.md) | Backend-neutral memory, submit-ring, ABI, slot-table, and capture-ring cores extracted with real Metal and Vulkan callers | Accepted |
| [ADR-025](025-selected-renderer-implementation-strategy.md) | One selected renderer implementation strategy replacing the backend-type ladder | Accepted |
| [ADR-026](026-vulkan-1-2-retirement.md) | Vulkan 1.2 retirement and bindless-only end state | Accepted |
| [ADR-027](027-immediate-mode-grid-ui.md) | Immediate-mode grid UI with a composited editor viewport | Accepted; direct retained-cache UI and docking ship, while the measured cached-target variant is declined |
| [ADR-028](028-gpu-driven-deferred-visibility-buffer.md) | GPU-driven deferred visibility-buffer rendering | Accepted and implemented through P21; the legacy world topology is retired, and both backends lower the Metal-measured eight-pass P19 compact path with a diagnostic full-screen rollback; native Vulkan compact validation remains pending |
| [ADR-029](029-retained-graph-resources.md) | Retained graph resources with per-subresource content validity | Accepted (partial); load-bearing for Metal P3B cascade reuse, while the Vulkan path remains unexecuted and unvalidated at runtime |
| [ADR-030](030-offline-mesh-optimization-and-cooking.md) | Offline mesh optimization and cooked geometry artifacts | Accepted; version-15 per-range packed cooker/load, mandatory runtime source optimization, observability, production conversion, lifecycle stress, and glTF meshopt decode ship |
| [ADR-031](031-versioned-packed-static-geometry-abi.md) | Tight packed static-geometry GPU ABI | Accepted and implemented as one branchless 32-byte static record on Metal and Vulkan |
| [ADR-032](032-two-phase-confirmed-visibility.md) | Two-phase current-depth-confirmed visibility | Declined for the measured workload |
| [ADR-033](033-occupied-depth-sdsm-feedback.md) | Occupied-depth SDSM through completed asynchronous feedback | Accepted; opt-in on Metal and Vulkan, fixed splits remain default |
| [ADR-034](034-offline-cooked-font-artifacts.md) | Offline-cooked font artifacts | Accepted (partial); VKFA v1, the pinned cooker, licensed production recipe, cooked loader, float em records, and default UI migration ship; native Windows/Vulkan acceptance and JSON rollback retirement remain open |
| [ADR-035](035-canonical-mtsdf-screen-pixel-range-shading.md) | Canonical MTSDF screen-pixel range with evidence-gated fallback | Accepted (partial); both shaders compile the canonical derivative contract and corrected atlas policy, while native Vulkan evidence and optional alpha-fallback A/B remain open |
| [ADR-036](036-dpi-derived-ui-text-scale.md) | DPI-derived UI text scale | Accepted (partial); platform/offscreen scale snapshots drive pre-layout UI sizing without transform double-scale, while native Windows and mixed-display transition evidence remain open |
| [ADR-037](037-portable-same-resolution-temporal-antialiasing.md) | Portable same-resolution temporal antialiasing | Accepted (partial); shared Metal/Vulkan resolve, rigid transparent motion, bounded composition reactivity, output-space FXAA, and native Apple validation ship, while deformation/procedural/particle motion and broader quality evidence remain open |
| [ADR-038](038-sh-l2-diffuse-irradiance.md) | Second-order spherical-harmonic normalized diffuse response replacing the baked irradiance cubemap | Partial; final packet-version-23 Metal/Vulkan implementation preserves the `E/pi` convention while cubemaps remain for source, skybox, and specular prefilter. Focused native validation, compiled-root reflection, deterministic same-case execution, and the retained numeric payload comparison pass; matched quality, deterministic GPU-reference, lifetime-stress, and authoritative performance acceptance evidence remain open |
| [ADR-039](039-metal-internal-render-scale.md) | Metal internal scene scale with folded spatial upscale | Accepted; Metal keeps the physical output and UI extent while viewport-domain scene work uses an explicit scale in `(0,1]`. Vulkan and the unfinished editor topology reject non-unit scale |
| [ADR-040](040-metalfx-temporal-dynamic-resolution.md) | MetalFX temporal reconstruction with completion-driven dynamic resolution | Accepted; Metal stages scene-linear HDR, depth, and exact previous-encode motion into a native-resolution MetalFX path, synchronizes its untracked textures through the public scaler fence, orders in-flight transforms on the GPU, and selects bounded internal tiers from completed GPU intervals. Metal validation uses an explicit portable diagnostic path; Vulkan retains the portable temporal consumer |

## Relationship to the Specification

The [renderer architecture specification](../renderer-architecture-spec.md)
describes *what the system is* and its current status. These ADRs describe
*why it is that way*. Where they overlap, the spec is the status authority and
the ADRs are the rationale authority.
