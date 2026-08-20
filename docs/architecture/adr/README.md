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
| [ADR-027](027-immediate-mode-grid-ui.md) | Immediate-mode grid UI with a composited editor viewport | Proposed |
| [ADR-028](028-gpu-driven-deferred-visibility-buffer.md) | GPU-driven deferred visibility-buffer rendering | Accepted (partial); the P20 boundary is owner-accepted on Metal and Vulkan, deferred is the default, P19 remains a default-off Metal-only candidate, and P21 retirement remains unclaimed |

## Relationship to the Specification

The [renderer architecture specification](../renderer-architecture-spec.md)
describes *what the system is* and its current status. These ADRs describe
*why it is that way*. Where they overlap, the spec is the status authority and
the ADRs are the rationale authority.
