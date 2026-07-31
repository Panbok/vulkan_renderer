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
| [ADR-001](001-frontend-backend-separation.md) | Frontend/backend separation via function-pointer interface | Accepted |
| [ADR-002](002-render-graph.md) | Compiled render graph with declared resource access | Accepted (partial) |
| [ADR-003](003-json-authored-render-graph.md) | JSON-authored render graph with named executors | Accepted |
| [ADR-004](004-stateless-render-packet.md) | Versioned render-packet submission | Accepted (partial) |
| [ADR-005](005-reflection-driven-pipelines.md) | SPIR-V-reflected resource layouts with declarative manifests | Accepted |
| [ADR-006](006-cpu-memory-allocators.md) | Lifetime-specific CPU allocators behind a common interface | Accepted |
| [ADR-007](007-gpu-memory-allocation.md) | Per-resource device memory allocation | Accepted (partial) |
| [ADR-008](008-cpu-gpu-communication.md) | Lifetime-tiered CPU↔GPU data paths | Accepted (partial) |
| [ADR-009](009-frame-synchronization.md) | Per-image present semaphores and bounded frames in flight | Accepted (partial) |
| [ADR-010](010-ecs-scene-system.md) | Archetype ECS as authoritative scene state | Accepted |
| [ADR-011](011-vulkan-1-2-baseline.md) | Vulkan 1.2 baseline with classic render passes | Accepted |
| [ADR-012](012-texture-compression-pipeline.md) | Offline KTX2/UASTC packing with runtime transcode | Accepted |
| [ADR-013](013-draw-submission-strategy.md) | Measured draw submission: culling, instancing, and MDI | **Proposed** |

## Relationship to the Specification

The [renderer architecture specification](../renderer-architecture-spec.md)
describes *what the system is* and its current status. These ADRs describe
*why it is that way*. Where they overlap, the spec is the status authority and
the ADRs are the rationale authority.
