---
status: investigation
updated: 2026-08-24
authority: adr
---

# ADR-032: Two-phase current-depth-confirmed visibility

## Status

**Declined for the measured workload.** A transient Metal predictor deferred
zero candidates across the 600 measured moving-camera frames. With no predicted
set to confirm, the required second classify, raster, and HZB topology cannot
produce a net win. The experiment was removed;
[ADR-028](028-gpu-driven-deferred-visibility-buffer.md) and its exact one-phase
history gates remain in force.

The passing local report digest is
`sha256:f54a508cbb1adac7233738b682cb6ac12c017b1bb263e595eaf8d0671e3b5f0e`.
It was a dirty-tree, warmup-unstable observation, so this decision applies to
the measured workload rather than claiming universal predictor behavior.

Design source: section 8 of
[shadow-cpu-cost-and-csm-rewrite-spec.md](../../rendering/shadow-cpu-cost-and-csm-rewrite-spec.md).

The benefit was measured as zero at the predictor boundary. Revisit only when a
materially different candidate population produces a nonzero deferred set.

## Context

`vkr_vk_deferred_cull_root()` builds a hierarchical depth buffer and then almost
never uses it. Two gates reject the history:

```c
candidate->history_world_epoch != slot->gpu_world_epoch ||
MemCompare(&candidate->history_view_projection,
           &current_view_projection, sizeof(Mat4)) != 0
```

There is no reprojection, so any camera motion fails the `MemCompare`.
`gpu_world_epoch` is an FNV hash over every camera-opaque candidate, so one
object moving invalidates the HZB for the entire scene. The test is also
camera-only: `vk_gpu_draw_classify` runs it under `view_index == 0u`, so cascade
views get frustum rejection alone.

Phase 0 measured this directly on the Bistro orbit case:
`visibility.hzb.history_valid` is 0 and `visibility.hzb.rejected` is 0 on every
orbiting frame. On a moving camera the HZB is built and never consulted.

### Why the gates cannot simply be deleted

The obvious change — drop the gates, reject against previous depth — is not
safe. Previous depth cannot prove a candidate is occluded *now*:

- camera motion can reveal a static object that was occluded last frame;
- a moving occluder can reveal a static object without the receiver moving at
  all.

Bounds-inside guards prevent invalid projections. They do not prevent
disocclusion. A one-phase reject from previous depth therefore drops geometry
that is genuinely visible, and the artifact is a hole in the frame rather than a
mis-shaded pixel.

### What is not yet known

Nothing can currently be rejected, because history is never valid under motion.
The size of the prize is therefore unmeasured. The measured workload is also
small — 254 candidates on Bistro, against a 262,144 candidate capacity — and the
renderer is GPU-bound on geometry submission rather than on classification.
Occlusion culling may or may not pay for a second classify, a second raster
phase, and an extra HZB reduction at that scale.

## Decision

Previous-frame depth becomes a **predictor only**. Correctness comes from a
confirmation phase against current-frame depth. No previous-frame result is ever
the final authority.

### Prediction

Project candidate bounds through the matrix that produced the previous HZB, and
partition frustum-visible candidates into:

- **probable-visible** — rendered in the first raster phase;
- **deferred** — predicted occluded by previous depth.

The predictor bails to probable-visible when any corner crosses the previous
near plane, falls outside `[0, 1]`, leaves the HZB UV rectangle, has invalid
bounds, or uses an incompatible projection convention. VKR's projection uses NDC
Z in `[0, 1]`; the `[-1, 1]` guard from the reference implementation must not be
copied. Dynamic candidates bypass prediction entirely and enter
probable-visible: this improves first-phase depth and reduces confirmation work,
but it is an optimization, not the correctness argument.

### Confirmation

The graph topology becomes:

1. classify and encode probable-visible candidates;
2. `VBuffer.Opaque.Initial`, clearing depth and visibility;
3. build `HZB.Provisional` from that current-frame depth;
4. classify the deferred set against `HZB.Provisional` using the **current**
   view-projection;
5. encode survivors into supplemental indirect buckets;
6. `VBuffer.Opaque.Confirm`, loading depth and visibility and drawing survivors;
7. run G-buffer resolve and downstream shading only after confirmation;
8. build or refresh the final history HZB from completed depth when the next
   frame, or SDSM, requires complete depth.

The provisional test may fail open and draw extra work. It may **not** reject a
candidate whose current projected bounds fall outside the valid HZB domain.
Every frustum-visible candidate rejected by previous history reaches either the
confirmation draw or a current-depth occlusion proof.

### Structural rules

- Compaction state, overflow counters, and visible-table ranges are separate for
  the initial and confirmation phases.
- Supplemental commands obey the same pipeline, material, and geometry
  compatibility rules as the primary buckets.
- G-buffer resolve and picking see the union of both phases.
- Overflow at supplemental-buffer capacity is an explicit frame error or a
  conservative draw fallback. It is never a silent drop.
- This topology applies to camera opaque and cutout candidates only.
  Transmission runs after confirmed opaque depth exists and may test that
  current HZB directly; an implementation that instead uses previous depth for
  transmission keeps the exact history gates until it has an equivalent
  confirmation path.
- Shadow cascades are explicitly out of scope. Cascade occlusion has the same
  disocclusion problem and would add up to four reduction chains; retained
  cascade reuse ([ADR-029](029-retained-graph-resources.md)) is the first
  cascade optimization, and any later cascade-HZB proposal must define its own
  per-cascade confirmation topology and beat reuse in matched GPU evidence.

### Stop condition

Ship only when matched Release profiles show a **net win** after the second
classify, the confirmation raster, and the provisional HZB build. If
confirmation overhead consumes the saved work, retain the exact-gated one-phase
path and close the phase without changing correctness policy. A partial
implementation is not landed "for later".

## Consequences

- The frame gains a second classify dispatch, a second opaque raster pass, and
  an extra HZB reduction chain. All three are paid every frame; the saving is
  paid only when the predictor is right.
- Depth and visibility gain a `LOAD` in the confirmation pass, which changes
  attachment load/store actions and barriers on both backends.
- Compaction and overflow state doubles, and the visible table gains a second
  range. Picking and G-buffer resolve must read the union.
- New counters are required: predicted-deferred, confirmation-tested,
  confirmation-survived, current-depth-rejected, and overflow.
- Exact moving-camera captures must exercise disocclusion behind both static and
  moving occluders. Vulkan and Metal validation must cover the extra depth
  `LOAD`, visibility-buffer writes, barriers, and indirect state.
- The one-phase path's exact history gates can finally be deleted for the camera
  view — but only once confirmation ships, never before.
- Two backends must implement the same topology. Vulkan runtime evidence is
  already the standing gap for ADR-029; this ADR widens that surface.

## Alternatives Considered

**Delete the gates and reject from previous depth.** Cheapest by far, and
incorrect: disocclusion produces missing geometry. Rejected on correctness.

**Reproject previous depth instead of confirming.** Reprojection fixes the
matrix mismatch but still cannot prove a candidate is unoccluded *now*. It
narrows the failure without removing it. Rejected.

**Loosen the gates with conservative bounds expansion.** Expanding bounds
reduces false rejects but does not bound disocclusion, which is a visibility
change rather than a precision problem. Rejected.

**Keep the status quo.** Legitimate, and currently in force. The HZB costs its
build every frame and returns nothing under motion, so the honest alternative to
this ADR is to delete the HZB for the camera view rather than to keep paying for
an unused structure. That option should be measured alongside this one.

**Extend occlusion to shadow cascades in the same change.** Rejected for now;
see the structural rules above.

## Revisit When

- Matched Release profiles show confirmation overhead exceeding the rejected
  work on the reported workload. Close the phase and keep one-phase.
- The candidate count grows by an order of magnitude, changing the balance
  between classification cost and rejected raster work.
- A cheaper source of the same saving lands — LOD, meshlet culling, or mesh
  shaders — making per-candidate occlusion rejection redundant.
- Either backend gains a native two-phase or predicated-draw primitive that
  removes the second classify.
