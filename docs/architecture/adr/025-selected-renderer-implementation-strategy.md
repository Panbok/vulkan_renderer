---
status: proposed
updated: 2026-08-08
authority: adr
---

# ADR-025: One Selected Renderer Implementation Strategy Replacing the Backend-Type Ladder

## Status

**Proposed** — no code. This ADR acts on
[ADR-020](020-bindless-backend-seam.md)'s contingency that "if sharing
`renderer_frontend.c` produces branch duplication, split it," and on its
"Revisit When" trigger that a second bindless implementation reveals which
operations are genuinely shared.

## Context

[ADR-001](001-frontend-backend-separation.md) established
`VkrRendererBackendInterface`, an 86-entry function-pointer table, as the
frontend-to-backend seam. [ADR-020](020-bindless-backend-seam.md) then chose to
add the bindless path as a *parallel renderer implementation selected once at
initialization*, rather than filling that table with Metal, because the table's
render-pass, descriptor-instance, vertex-binding, layout, and telemetry
contracts would have forced Metal to reproduce the Vulkan 1.2 data model.

That decision was right, but its implementation took a shortcut. On Metal the
86-entry table is left **entirely NULL** and the frontend short-circuits above
it to call the Metal packet renderer directly. The mechanism for
short-circuiting is an `if (backend_type == VKR_RENDERER_BACKEND_TYPE_METAL)`
test, and there are now 46 of them:

| Location | Sites |
|---|---|
| `renderer_frontend.c` | 27 |
| `vkr_renderer_metrics.c` | 4 |
| `vkr_capture.c`, `vkr_ui_system.c`, `vkr_text_3d.c` | 2 each |
| `vkr_world_resources.c`, `vkr_skybox_system.c`, `vkr_shadow_system.c`, `vkr_picking_system.c`, `vkr_ui_text.c`, `vkr_renderer.h` | 1 each |
| Outside `lib/`: `app/src/main.c`, `tools/harness/vkr_harness_common.c`, `tests/src/harness_test.c` | 1 each |

Adding a third backend to this structure turns every one of them into a
three-way ladder, in fifteen files, most of them outside the renderer frontend.
That is the concrete cost ADR-020 anticipated but did not yet have to pay.

Reading every site rather than counting them reveals something more useful:
**most of them are not asking which backend is running.** The comment above the
shadow-system branch states its actual question outright — the subsystem "must
not recreate legacy renderpass, descriptor, or pipeline state." The branches in
`vkr_ui_text.c`, `vkr_text_3d.c`, `vkr_ui_system.c`, and `vkr_world_resources.c`
all guard pipeline-registry acquisition or a pipeline-readiness check. "Metal" is
not why those sites behave differently; "this implementation owns its own
precreated pipelines" is.

There is also a latent defect that a third backend would make concrete.
`RendererFrontend` stores `void *metal_timing_result`, and three sites cast it to
the Metal-specific result type — one in `vkr_renderer_metrics.c` and two in
`renderer_frontend.c`. A second bindless backend has nowhere to put its result
without either a second untyped pointer or a second cast.

## Decision

Replace the backend-type ladder with **plain data plus one coarse strategy**,
selected once at initialization. The partition below is derived from reading the
call sites, not from a target operation count.

### Kind 1 — implementation properties, not dispatch (roughly a dozen sites)

The question these sites ask is "does this implementation use the retained
legacy descriptor, render-pass, and pipeline subsystems?" One immutable boolean
answers all of them:

```c
caps.uses_legacy_pipeline_state
```

Every such site becomes a struct-field read. A third bindless backend sets the
field false and all of them are correct with no further edits. **This is the
highest-leverage part of the decision and it introduces no dispatch mechanism at
all.**

A small number of neighbouring sites ask a related but distinct question — "does
this implementation load its own default cubemap / own its own picking target?"
Those are implementation-owned-resource questions and are resolved by moving the
resource ownership into the implementation, not by adding a second boolean.

### Kind 2 — implementation-derived constants (roughly eight sites)

Frames in flight, frame-in-flight index, present-target image count, present
-target kind, present-target colour and depth formats, shadow depth format, and
the renderer arena and scratch-arena sizes are **values**, not operations. They
become fields of a capability struct filled at initialization. A function
pointer that returns a constant is worse than a struct field on every axis.

### Kind 3 — asset publication (already solved)

`VkrAssetPublisher` is already a backend-neutral table of eleven function
pointers, already documented as never dispatched from a frame loop, and already
filled by Metal. The bindless Vulkan backend fills the same table. No new
mechanism.

**Do not widen it.** The Metal renderer exposes more non-hot entry points than
the publisher has — focused constructors used by tests and the harness. Those
stay backend-private.

### Kind 4 — genuinely coarse operations (roughly twenty sites)

Initialization, destruction, wait-idle, submit serial, completed submit serial,
upload wait statistics, command-slot wait count, device memory statistics,
bindless memory metrics, device information, prepare frame, submit packet,
cancel frame, resize, present-target recreate, capture reserve, capture record
item, capture poll, capture release, pass-timing poll, and the backend allocator
accessor.

Every one of these will have **two concrete implementations with the same
ownership and completion contract** once the bindless Vulkan backend exists.
That is precisely ADR-020's stated extraction condition — and it is satisfied
*only* for these operations. They become a `VkrRendererImpl` struct of function
pointers carrying an opaque state pointer, the capability struct from kinds 1
and 2, and the asset publisher from kind 3.

The implementation is selected once by a small factory keyed on backend type and
guarded by the platform macros. **That factory is the only place backend type is
tested at runtime after initialization.**

Explicitly **not** in the table: draw, indexed draw, buffer or texture binding,
viewport and scissor state, barriers, resource creation, pipeline creation, and
render-pass begin and end — anything per pass or per draw. This is a coarse
strategy whose frame path is two entries, not a resurrected 86-entry vtable.

### A typed submit result

`VkrRendererImplSubmitResult` replaces the untyped `void *metal_timing_result`
and its three casts. It carries only what the frontend and metrics actually read:
submit value, source frame index, executed pass count, draw counters, the
capture poll snapshot, generalized memory and material metrics, and the per-pass
timing array. Backend-specific evidence fields — exact probe colours, shadow
depth, picking identifiers, IBL prefilter samples, pipeline-archive warmth —
stay in each backend's own rich result, returned through its own focused
evidence entry point.

### Where legacy Vulkan 1.2 sits

It becomes a **third implementation** whose entries forward into the existing
`VkrRendererBackendInterface`, with `uses_legacy_pipeline_state` set true. A
mechanical adaptor.

Critically, **the direct `rf->backend.*` call sites stay exactly where they
are.** They live in functions that only the legacy implementation reaches,
because their callers are already gated by the capability boolean. Routing them
through the strategy table would be the speculative-vtable mistake ADR-020
rejects. The adaptor's value is containment: at the final retirement gate,
deleting the 86-entry table means deleting one implementation and its adaptor
rather than unpicking a ladder across fifteen files.

### Hot-path guarantee

After initialization the frame path executes **exactly two indirect calls per
frame** — prepare frame and submit packet. Inside submit the backend is
direct-typed. No per-pass indirect call, no per-draw indirect call, no
backend-type test, no allocation, no lock, and no string construction. A source
audit proving this is required evidence in every stage that touches the seam.

### File split, only when it is earned

The umbrella specification sanctions splitting `renderer_frontend.c` if branch
duplication appears. That split — public lifecycle and packet validation; legacy
orchestration and its wrappers; bindless orchestration shared by both bindless
backends — happens **when the second bindless implementation exists and the
duplication is measurable, not in advance.**

## Consequences

**Positive**

- A third backend costs a factory entry and a capability struct, not fifteen
  files of three-way branches.
- Roughly a dozen sites lose their branch entirely, replaced by a field read, so
  the majority of the ladder disappears without any dispatch mechanism.
- The untyped timing pointer and its three casts are removed before a second
  backend would have forced a second cast.
- Backend dispatch is provably outside every draw and pass loop, and the audit
  that proves it is part of the evidence rather than an assumption.
- The set of coarse operations is derived from real call sites, so it is evidence
  for what a future low-level seam would and would not need — which is what
  ADR-020 asked for.
- Systems keep their backend-neutral CPU responsibilities and stop knowing which
  renderer is selected.

**Negative / risks**

- This is a wide, behaviour-preserving refactor across the frontend, metrics,
  capture, and six systems files. Its review value depends almost entirely on the
  snapshot and validation witnesses, not on reading the diff.
- It lands **before** the second bindless backend exists, so the operation set is
  shaped by one real implementation plus one designed one. If the Vulkan backend
  needs an operation the table lacks, the table grows — and growth is the failure
  mode this ADR is trying to avoid.
- Three implementations coexist during migration, which is the project's maximum
  maintenance cost, as ADR-021 already noted.
- A capability struct can accrete fields the way a vtable accretes entries.
  `uses_legacy_pipeline_state` is defensible because it names a real subsystem
  cluster; a second boolean per behavioural difference would not be.
- `AGENTS.md`'s statement that the frontend "reaches the Vulkan backend only
  through the `VkrRendererBackendInterface`" becomes false when this lands. Per
  that document's own rule, it must be amended in the same change — not before,
  since it is accurate today.

## Alternatives Considered

- **Implement bindless Vulkan through the existing 86-entry
  `VkrRendererBackendInterface`.** Rejected for the same reason ADR-020 rejected
  it for Metal: its render-pass, descriptor-instance, vertex-binding, layout, and
  descriptor-write-telemetry contracts encode the Vulkan 1.2 data model, and a
  bindless backend would have to reconstruct that model to satisfy them.
- **Add a third arm to the existing ladder.** Rejected. It is the cheapest change
  today and the most expensive one thereafter: forty-six sites across fifteen
  files, most of which are not backend questions, and each of which is an
  independent opportunity to get the third case wrong.
- **Extract a full low-level `VkrGpuInterface` now.** Rejected, consistent with
  ADR-020. The bindless Vulkan design surfaces concrete evidence that a low-level
  seam designed ahead of the second implementation would encode the wrong
  contract: Metal has three barrier forms and Vulkan has an analogue for only
  two.
- **Function pointers for the implementation-derived constants, for
  uniformity.** Rejected. An indirect call returning a constant is worse than a
  field read in code size, inlining, and readability, and it hides that the value
  is fixed at initialization.
- **Widen `VkrAssetPublisher` to cover the coarse operations too.** Rejected. The
  publisher has a specific documented contract — load and unload finalization
  only, never a frame loop — and merging frame-path operations into it would
  destroy the property that makes it safe to reason about.
- **Keep the untyped result pointer and add a second cast for Vulkan.** Rejected;
  it is the defect this ADR exists partly to remove.
- **Split `renderer_frontend.c` immediately.** Rejected as premature. The split is
  justified by measured duplication between two bindless orchestrations, which
  does not exist until the second one does.

## Revisit When

- The bindless Vulkan implementation needs a coarse operation the table lacks. One
  addition is normal; a pattern of additions means the partition was wrong.
- The capability struct grows a second behavioural boolean, which would suggest
  the cluster `uses_legacy_pipeline_state` names is not actually one cluster.
- The two bindless implementations share enough operation-level contracts that
  ADR-020's low-level `VkrGpuInterface` question can be reopened with evidence
  rather than speculation.
- Duplication between Metal and Vulkan orchestration becomes measurable, which is
  the trigger for the `renderer_frontend.c` split.
- The legacy implementation is deleted per
  [ADR-026](026-vulkan-1-2-retirement.md), at which point the adaptor and the
  capability boolean it exists to serve are both removed and the strategy has two
  implementations instead of three.
- A measured profile shows the two per-frame indirect calls are themselves a cost,
  which would contradict the premise that coarse dispatch is free at frame
  granularity.
