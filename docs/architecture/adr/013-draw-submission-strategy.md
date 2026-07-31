---
status: proposed
updated: 2026-07-31
authority: adr
---
# ADR-013: Measured Draw Submission — Culling, Instancing, and MDI

**Status:** Proposed
**Priority:** High after ADR-004/008/009 correctness fixes.

## Context

### Current behavior

Application world-payload construction scans all live mesh-manager slots twice:
once to count and once to populate submesh draw/instance arrays. Apart from the
retained `visible` flag and load state, it performs no spatial rejection.

The world pass then resolves mesh/submesh, material, pipeline, range, and probe
state for every item, binds per-material descriptor state, and issues direct
indexed draws. Packet construction currently emits one instance record and
`instance_count = 1` for each submesh draw, including repeated compatible
objects. Opaque sort keys are not used to create batches. Transparent/cutout
items preserve distance ordering through a separate sort.

Existing but unintegrated infrastructure:

| Module | Current state |
|---|---|
| `math/vkr_frustum.*` | Sphere/frustum functions; no production caller |
| `renderer/vkr_draw_batch.*` | Draw key/batch structures; no production caller |
| `renderer/vkr_indirect_draw.*` | Initialized fixed mapped command ring; no allocation caller |
| Geometry/submesh bounds | Present, but must be transformed conservatively |
| Capability flags | MDI and indirect first-instance support are queried |
| Batch metrics | Fields exist, but current average/max batch size report one |

These modules are useful starting points, not proof that the final pipeline is
already designed. In particular, the existing key includes geometry/range; one
indirect command per such “batch” followed by one MDI call per batch would not
reduce draw calls.

## Decision (Proposed)

Implement and measure the following stages independently.

### 1. Establish baselines and correctness tests

Capture per-pass CPU/GPU time, submitted/visible draw count, pipeline/material/
buffer binds, instance count, and device capabilities on representative scenes.
Add image/visibility regression cases before changing culling or ordering. Do
not promise an order-of-magnitude improvement without these measurements.

### 2. Cull during render extraction

Cull world-camera candidates before final packet arrays are materialized.
Transform bounds correctly:

- transform centers to world space;
- under non-uniform scale, expand a sphere by the maximum basis scale or
  transform/test an AABB;
- use conservative bounds when geometry/submesh metadata is absent.

Do not reuse the camera-culled list for shadows. Off-camera objects can cast
visible shadows, so each cascade needs conservative light-space caster tests or
an unculled candidate list. Picking should follow the viewport visibility
policy without weakening editor selection expectations.

### 3. Add true instancing for identical draw state

When opaque draws share pipeline, material descriptor state, vertex/index
buffers, and index range, write their instance records contiguously and issue
one indexed draw with `instance_count > 1` and the correct `first_instance`.
This reduces direct draw calls without requiring MDI.

Keep blended transparent draws on a depth-ordered direct path unless a batching
scheme can preserve the required order. Alpha-cutout policy can be evaluated
separately because it is generally depth-writing rather than blended.

### 4. Sort opaque work by binding-state key

Define two explicit keys rather than overloading one:

- an **instancing key** including pipeline, material/descriptors, vertex/index
  buffers, and index range;
- an **MDI group key** including only state that must remain bound across one
  `vkCmdDrawIndexedIndirect` call: pipeline, descriptors, and compatible
  vertex/index buffers.

Track state changes and batch effectiveness with real metrics.

### 5. Introduce MDI only where it combines commands

Within one MDI group, write multiple `VkrIndirectDrawCommand` records for
different index ranges/instance groups and issue one indirect call with
`drawCount > 1`. Keep a direct fallback for unsupported devices and for groups
containing only one command where MDI has no benefit.

Current per-material descriptor sets prevent grouping across material changes,
and independently allocated geometry buffers prevent grouping across those
buffer changes. Large MDI wins may therefore require:

- shared/mega vertex and index buffers with offset ranges;
- a material table or descriptor-indexing/bindless model;
- GPU-generated compacted commands in a later phase.

Before enabling the existing indirect stream, also fix its frame-resource
indexing described by ADR-008/009.

## Consequences (Expected)

**Positive**

- Culling removes packet and pass-resolution work for rejected objects.
- True instancing can reduce draws immediately for repeated identical geometry.
- State sorting reduces redundant pipeline/material/buffer work even before
  MDI.
- A correctly defined MDI group can submit several ranges with one call.
- Staged metrics reveal whether mega-buffers or bindless material access are
  actually worth their complexity.

**Negative / risks**

- Bounds errors cause visible popping; shadow-caster culling is especially
  sensitive.
- Culling/sorting adds CPU work and scratch memory; small scenes may not win.
- Opaque batching and transparent ordering require separate paths.
- Existing geometry/material ownership may limit batch size more than expected.
- Indirect commands are harder to inspect and validate than direct calls.
- Fixed stream capacities require explicit overflow policy and telemetry.

## Alternatives Considered

- **Culling only.** A valid first milestone; reduces off-screen work but not
  visible draw count.
- **Instancing only.** High-value for repeated assets, but does not combine
  different ranges.
- **Bindless first.** Removes descriptor changes across materials but does not
  itself reduce draw count. Complementary.
- **GPU-driven culling immediately.** Desirable end state, but the graph does
  not yet provide complete compute/access synchronization. CPU results should
  provide the correctness baseline first.
- **Static scene mega-mesh only.** Can reduce calls for immutable content but
  complicates per-object visibility, materials, picking, and edits. Useful for
  selected content, not a universal replacement.

## Revisit When

Mark this ADR accepted only after implemented stages include captured
before/after metrics and validation on at least one native Vulkan target and the
supported MoltenVK path. A later GPU-driven design should supersede the command
generation portion while retaining the visibility/order correctness rules.

## Implementation Order

1. Fix frame-stream indexing and error propagation.
2. Add counters and representative baselines.
3. Integrate conservative camera culling with separate shadow candidates.
4. Compact identical opaque items into true instanced draws.
5. Sort remaining opaque work and measure binding groups.
6. Add MDI for groups with multiple compatible commands.
7. Evaluate mega-buffers/material tables/GPU culling from measured limits.
