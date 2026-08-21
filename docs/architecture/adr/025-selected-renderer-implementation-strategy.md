---
status: implemented
updated: 2026-08-21
authority: adr
---

# ADR-025: One Selected Renderer Implementation Strategy Replacing the Backend-Type Ladder

## Status

**Accepted** — `VkrRendererImpl` selects one immutable capability
record and coarse operation strategy for Metal or production Vulkan.
The Vulkan strategy now owns a Vulkan 1.4 device, offscreen and window-target state,
descriptor heaps, prepare/submit execution, completion, memory/heap metrics,
the validated V4 asset-publication and dynamic-memory boundary, and the V5
authored graph, asynchronous capture, IBL, and pass-timing path.
The neutral submit result carries capture, memory, material, and pass-timing
data; renderer behavior no longer tests
backend type after factory selection; and the normal frame path contains only
the declared prepare and submit indirect calls. This ADR acts on
[ADR-020](020-bindless-backend-seam.md)'s contingency that "if sharing
`renderer_frontend.c` produces branch duplication, split it." The production
Vulkan strategy now passes the V3 offscreen and native RX 6700 XT
window/resize gates, so the ADR's final acceptance condition is met; V4
completeness is not an acceptance condition for this selection seam. V2 is
complete on both required platforms. The matched clean Release Metal profile passes its
predeclared no-regression tolerance. On macOS, Debug
legacy-Vulkan report `20260808T204053.961Z-01220d` passes two explicit native
window resize round trips with renderer-observed extents and clean validation,
startup, and shutdown diagnostics. On Windows, report
`20260809T084713.791Z-003636`
(`sha256:0e4964fa31bb7e9dd575e868986fda82a3d50b61989ef6d38aa9ced969a0d5f7`)
passes the complete CPU suite plus two equivalent hidden-window runtime
children against a tracked text fixture, with clean validation and lifecycle
diagnostics. After the V3/V4 production slice and shared-core extraction,
current-tree Windows report `20260809T113259.325Z-003da8`
(`sha256:fbb9ec7686f3cd0fc6ba999c962e5566802366a9bed695bd3d7ff78bf488e12b`)
repeats the complete CPU, build, and two-child legacy resize/lifecycle gate.
The original bindless window witness used only core surface, Win32-surface, and
swapchain extensions. The portable path now treats a completed submit that
consumed the reacquired image's acquire semaphore as the presentation proof;
when maintenance1 is available, per-image present fences provide explicit
completion. Neither path adds a backend-type branch or per-draw dispatch.
Windows selects Vulkan by default and `--renderer vulkan` names that
same implementation explicitly. ADR-026 V7 removed the legacy adaptor,
temporary `vulkan-bindless` spelling, and the capability boolean that existed
only to distinguish retained pipeline state. The normal frame path still has
exactly the coarse prepare and submit dispatches and no per-pass or per-draw
strategy call.
The post-V7 native Windows focused synchronization/GPU-assisted diagnostics and
Debug/Release whole-graph text runs pass, confirming that deletion did not
reintroduce a backend-type ladder or widen the frame dispatch seam.

## Context

[ADR-001](../../archive/adr-001-frontend-backend-separation.md) established
`VkrRendererBackendInterface`, an 87-entry function-pointer table, as the
frontend-to-backend seam. [ADR-020](020-bindless-backend-seam.md) then chose to
add the bindless path as a *parallel renderer implementation selected once at
initialization*, rather than filling that table with Metal, because the table's
render-pass, descriptor-instance, vertex-binding, layout, and telemetry
contracts would have forced Metal to reproduce the Vulkan 1.2 data model.

That decision was right, but its implementation took a shortcut. On Metal the
87-entry table is left **entirely NULL** and the frontend short-circuits above
it to call the Metal packet renderer directly. The mechanism for
short-circuiting was an `if (backend_type == VKR_RENDERER_BACKEND_TYPE_METAL)`
test. The pre-V2 inventory found 46 of them:

| Location | Sites |
|---|---|
| `renderer_frontend.c` | 27 |
| `vkr_renderer_metrics.c` | 4 |
| `vkr_capture.c`, `vkr_ui_system.c`, `vkr_text_3d.c` | 2 each |
| `vkr_world_resources.c`, `vkr_skybox_system.c`, `vkr_shadow_system.c`, `vkr_picking_system.c`, `vkr_ui_text.c`, `vkr_renderer.h` | 1 each |
| Outside `lib/`: `app/src/main.c`, `tools/harness/vkr_harness_common.c`, `tests/src/harness_test.c` | 1 each |

Adding a third backend to that structure would have turned every one into a
three-way ladder across 14 files, most of them outside the renderer frontend.
That is the concrete cost ADR-020 anticipated but did not yet have to pay.

Reading every site rather than counting them reveals something more useful:
**most of them are not asking which backend is running.** The comment above the
shadow-system branch states its actual question outright — the subsystem "must
not recreate legacy renderpass, descriptor, or pipeline state." The branches in
`vkr_ui_text.c`, `vkr_text_3d.c`, `vkr_ui_system.c`, and `vkr_world_resources.c`
all guard pipeline-registry acquisition or a pipeline-readiness check. "Metal" is
not why those sites behave differently; "this implementation owns its own
precreated pipelines" is.

There was also a latent defect that a third backend would have made concrete.
`RendererFrontend` stored `void *metal_timing_result`, and three sites cast it to
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

Metal and legacy Vulkan already provide two concrete implementations of these
coarse operations, even though the current dispatch shape exposes them
unevenly. V2 first proved they fit one ownership and completion contract through
the real implementations and a rejecting bindless stub; V3 then made the
production Vulkan backend the third caller. Only these coarse operations
satisfy ADR-020's extraction condition. They become a `VkrRendererImpl` struct
of function pointers carrying an opaque state pointer, the capability struct
from kinds 1 and 2, and the asset publisher from kind 3.

The implementation is selected once by a small factory keyed on backend type and
guarded by the platform macros. **After selection, renderer behavior never
branches on backend type.** CLI parsing, harness case decoding, reporting, and
factory selection may still inspect the enum; they do not choose frame behavior.

Explicitly **not** in the table: draw, indexed draw, buffer or texture binding,
viewport and scissor state, barriers, resource creation, pipeline creation, and
render-pass begin and end — anything per pass or per draw. This is a coarse
strategy whose frame path is two entries, not a resurrected 87-entry vtable.

### A typed submit result

`VkrRendererImplSubmitResult` replaces the untyped `void *metal_timing_result`
and its three casts. It carries only what the frontend and metrics actually read:
submit value, source frame index, executed pass count, draw counters, the
capture poll snapshot, generalized memory and material metrics, and the per-pass
timing array. Backend-specific evidence fields — exact probe colours, shadow
depth, picking identifiers, IBL prefilter samples, pipeline-archive warmth —
stay in each backend's own rich result, returned through its own focused
evidence entry point.

### Where legacy Vulkan 1.2 sat during V2–V6

Legacy Vulkan was one of V2's **two production implementations**. Its entries
forwarded into the then-existing `VkrRendererBackendInterface`, with
`uses_legacy_pipeline_state` set true. The bindless Vulkan entry became the third
production implementation in V3. The legacy entry was a mechanical adaptor.

Critically, the direct `rf->backend.*` call sites stayed in functions reached
only by the legacy implementation and gated by the capability boolean. Routing
them through the strategy table would have been the speculative-vtable mistake
ADR-020 rejects. That containment paid off at V7: ADR-026 deleted the adaptor,
the 87-entry table, the capability boolean, and their private call sites without
unpicking a behavior ladder across the frontend.

### Hot-path guarantee

After initialization a normal successful frame executes **two indirect calls**
— prepare frame and submit packet. Exceptional lifecycle, resize, and explicitly
requested capture/poll operations may add bounded coarse calls; none is
dispatched per pass or per draw. Inside submit the backend is direct-typed. No
per-pass indirect call, no per-draw indirect call, no backend-behavior type test,
no allocation, no lock, and no string construction. A source audit proving this
is required evidence in every stage that touches the seam.

### File split, only when it is earned

The umbrella specification sanctions splitting `renderer_frontend.c` if branch
duplication appears. That split — public lifecycle and packet validation; legacy
orchestration and its wrappers; bindless orchestration shared by both bindless
backends — happens **when the second bindless implementation exists and the
duplication is measurable, not in advance.**

## Consequences

**Positive**

- A third backend costs a factory entry and a capability struct, not 14
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
- It landed before the second bindless backend existed, but the operation set
  was shaped by two real implementations: Metal and legacy Vulkan. Bindless
  Vulkan fit that coarse boundary; repeated future growth would still be
  evidence that the partition is wrong.
- Three implementations coexisted during migration, the project's maximum
  maintenance cost. ADR-026 ended that temporary state.
- A capability struct can accrete fields the way a vtable accretes entries. V7
  removed `uses_legacy_pipeline_state`; adding a new behavior-selecting boolean
  would require renewed architectural scrutiny.
- `AGENTS.md` names `VkrRendererImpl` as the selected coarse strategy. The
  legacy adaptor seam was removed by ADR-026.

## Alternatives Considered

- **Implement bindless Vulkan through the existing 87-entry
  `VkrRendererBackendInterface`.** Rejected for the same reason ADR-020 rejected
  it for Metal: its render-pass, descriptor-instance, vertex-binding, layout, and
  descriptor-write-telemetry contracts encode the Vulkan 1.2 data model, and a
  bindless backend would have to reconstruct that model to satisfy them.
- **Add a third arm to the existing ladder.** Rejected. It is the cheapest change
  today and the most expensive one thereafter: 46 sites across 14
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

- The Vulkan implementation needs a coarse operation the table lacks. One
  addition is normal; a pattern of additions means the partition was wrong.
- The capability struct gains a behavior-selecting boolean, which would suggest
  implementation behavior is leaking back above the selected strategy.
- The Metal and Vulkan implementations share enough operation-level contracts that
  ADR-020's low-level `VkrGpuInterface` question can be reopened with evidence
  rather than speculation.
- Duplication between Metal and Vulkan orchestration becomes measurable, which is
  the trigger for the `renderer_frontend.c` split.
- A third selected implementation is added, which must preserve the same coarse
  boundary rather than reintroducing a behavior ladder.
- A measured profile shows the two per-frame indirect calls are a material cost,
  at which point replace the selected strategy with a cheaper coarse mechanism
  while preserving the no-per-pass/no-per-draw dispatch invariant.
