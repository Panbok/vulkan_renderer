---
status: partial
updated: 2026-08-20
authority: adr
---
# ADR-013: Measured Draw Submission — Culling, Instancing, and MDI

**Status:** Accepted; superseded in part by ADR-028

## Context

### Implemented behavior

Application world-payload construction emits one bounded instance × submesh
candidate stream. The Metal and Vulkan implementations classify camera and
shadow-cascade views on the GPU and compact opaque, cutout, and transmission
work into backend-native indirect buckets. There is no CPU opaque,
transmission, or shadow draw list and no direct world fallback.

Ordinary alpha blend remains the narrow exception: the application performs
conservative camera culling, sorts survivors back to front, and emits direct
feature-local draws. World text, UI text, and UI are likewise outside the
retired whole-frame submission path.

Integrated infrastructure and remaining boundaries:

| Module | Current state |
|---|---|
| `math/vkr_frustum.*` | Shared frustum math; CPU world use is limited to feature-local ordinary blend |
| `renderer/vkr_draw_batch.*` | Draw key/batch structures; no production caller |
| `renderer/vkr_visibility.*` | Alpha routing, conservative bounds, ordinary-blend sorting, and direct emission |
| Backend indirect submission | Metal ICB and Vulkan indirect-count implementations own GPU command compaction and execution |
| Geometry/submesh bounds | Transformed conservatively for TRS/non-uniform scale; no spatial hierarchy |
| Capacity policy | Invalid structure or exhausted fixed candidate capacity is rejected before recording |
| Submission metrics | GPU candidate, visible, bucket, command, and overflow diagnostics remain; CPU merge and fallback metrics are removed |

These modules are useful starting points, not proof that the final pipeline is
already designed. In particular, the existing key includes geometry/range; one
indirect command per such “batch” followed by one MDI call per batch would not
reduce draw calls.

ADR-028 P17 supersedes this ADR for shadow-cascade visibility and submission;
P21 supersedes it for opaque, cutout, and transmission submission. The ordered
ordinary-blend policy and its conservative-bound rules remain in force.

## Decision

Keep the following implemented stages independent and measurable.

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

## Implementation Evidence

Release measurements on Apple M1 Pro/MoltenVK found:

- Sponza: 36 tested submeshes, zero camera/light rejections;
- San Miguel: 282 tested, roughly 37% camera rejection as the camera moves;
- zero compatible world instancing runs on both measured scenes;
- 1,124 opaque shadow commands carried by 8 indirect calls;
- no measurable frame-time difference between MDI enabled/disabled because the
  full render-graph CPU slice is only about 0.3–0.5 ms of a ~20 ms frame.

The P2 review corrected four invariants before acceptance: the perspective
matrix now shares `mat4_look_at`'s right-handed convention; shadow visibility
tests every cascade rather than assuming the last contains earlier cascades;
instancing includes position-dependent descriptor compatibility; and indirect
submission binds the same compacted/default index buffer as direct fallback.

Native Vulkan hardware validation remains missing, so this ADR is accepted
partial rather than accepted.

## Consequences

**Positive**

- Culling removes packet and pass-resolution work for rejected objects.
- True instancing can reduce draws immediately for repeated identical geometry.
- State sorting reduces redundant pipeline/material/buffer work even before
  MDI.
- A correctly defined MDI group can submit several ranges with one call.
- Staged metrics reveal whether mega-buffers or bindless material access are
  actually worth their complexity.

**Negative / risks**

- Bounds/cascade-union errors cause visible or shadow popping; both are pinned
  by CPU regressions but still need broader scene/device validation.
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

Mark this ADR fully accepted after validation on at least one native Vulkan
target in addition to the supported MoltenVK path. Move local-probe selection
into per-instance data before allowing instancing across position-dependent
probe state. A later GPU-driven design should supersede command generation while
retaining the visibility/order correctness rules.

## Implementation Order

1. Fix frame-stream indexing and error propagation.
2. Add counters and representative baselines.
3. Integrate conservative camera culling with separate shadow candidates.
4. Compact identical opaque items into true instanced draws.
5. Sort remaining opaque work and measure binding groups.
6. Add MDI for groups with multiple compatible commands.
7. Evaluate mega-buffers/material tables/GPU culling from measured limits.
