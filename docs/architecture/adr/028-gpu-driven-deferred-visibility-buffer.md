---
status: implemented
updated: 2026-08-21
authority: adr
---

# ADR-028: GPU-driven deferred visibility-buffer rendering

## Status

**Accepted and implemented.** The P20 boundary is satisfied on both backends,
and P21 has retired the legacy whole-frame renderer. Metal uses
GPU-frustum-classified four-bucket
ICB submission; Vulkan uses fixed-partition indirect-count submission. Both
backends provide opaque visibility raster, compute material resolve and
lighting, completion-compatible HZB history, requested-pixel picking,
camera-plus-cascade multi-view submission, and four-layer depth-peeled
transmission with completion-gated coverage. There is no runtime topology
selector or whole-frame fallback; unsupported structure and exhausted capacity
are explicit pre-recording errors, and the opaque/shadow and transmission
candidate streams retain independent bounds.

Vulkan opaque and transmission shading share punctual/environment helpers and
surface reconstruction with the retained forward model. Its resolver samples
normal, ORM, and emissive maps with analytic gradients. Homogeneous barycentric
and face-orientation reconstruction remove the Sponza eye-plane invalid-resolve
failure and the dominant three-row normal-X mismatch. Exact equality remains
open on 520 double-sided secondary-side pixels, 147 shared raster-edge pixels,
and 2,410 pixels in the unchanged transmission debug composite; the owner
accepted this classified remainder as the P20 visual threshold on 2026-08-20.

Final Windows Vulkan acceptance used a clean isolated source commit. Five
independent repetitions each of Bistro, the eight-bucket state matrix, and the
Sponza orbit report stable warmup, complete requested GPU timestamps, deferred
selected, and every named fallback, overflow, invalid-resolve, and publication-
rejection assertion at zero. The authoritative report IDs are
`20260820T152657.594Z-0043ef`, `20260820T153320.004Z-00146d`, and
`20260820T153351.227Z-004172`. Focused synchronization-validation report
`20260820T153902.776Z-000afd` passes two repetitions without a VUID, validation
error, device loss, renderer error, or fatal marker. The state matrix publishes
15,100 / 7,572 / 0 / 0 transmission-layer pixels only after frame-slot
completion. Fresh Sponza and packed-Bistro snapshots are populated. The owner
accepted the recorded Metal evidence and these Windows Vulkan results without
promoting a snapshot baseline, closing P20 on both backends.

Bistro acceptance requires packed `.vkt` siblings for generated
specular-glossiness derivative textures; the repository pack step owns that
precondition. Enlarging the publication arena is not an accepted substitute. A
480-frame complete raw-texture publication stress can exceed the bounded source
reserve and then the target GPU image budget; that is separate streaming and
capacity work, not the bounded P20 workload.

Metal's P19 `Transmission.Compact` candidate remains default-off. Its local
pass-time improvement did not improve the owner-level frame outcome, so no
speed claim or Vulkan P19 implementation is accepted. P21 was authorized and
implemented on 2026-08-20. Metal validation processes must remain strictly
serial.

The native Windows Vulkan P21 gate ran on 2026-08-21 and passed after one
repair. P21's deletion had collapsed the Vulkan candidate-packing tail into a
single hard error, which made the bounded geometry/material publication
boundary — a lifetime state every cold scene load passes through — fail command
recording instead of omitting the affected candidates for the frame. Metal has
no equivalent CPU-side resolve step, so a Metal-only retirement pass could not
observe it. The consequence recorded below now reads explicitly: unsupported
*structure* and exhausted *capacity* are pre-recording errors, while an
unpublished handle is not. The repaired Vulkan runs reproduce the accepted P20
work volumes exactly on the RX 6700 XT — 25 candidates over 27 GPU passes for
Sponza (`20260821T111229.346Z-001eff`), 254 opaque plus 18 transmission
candidates over 43 GPU passes for Bistro (`20260821T111300.634Z-003d93`), and
15,100 / 7,572 / 0 / 0 layer coverage over 46 GPU passes for the state matrix
(`20260821T111157.816Z-003b85`) — with every fallback, overflow,
invalid-resolve, and publication-rejection assertion at zero, a clean focused
synchronization-validation replay (`20260821T111524.783Z-003ffe`), and a
cold/warm pipeline-cache pair whose 204,280-byte cache is smaller than the
pre-P21 217,752 bytes. Those runs are non-authoritative (local profile, dirty
worktree) and carry no speed claim. SPEC §11.1 holds the complete table.

The same pass removed the retirement residue the deletion diff had missed: the
unreferenced CPU draw batcher `vkr_draw_batch.*`, two graph resources no
surviving pass used (`swapchain_depth` and the editor-only `scene_depth`), and
a Metal `depth` capture channel still naming them. The earlier megabuffer and
MDI proposals this decision subsumes are archived as `superseded`.

The implementation contract, evidence digests, and phase details are in
[deferred-visibility-buffer/SPEC.md](../../rendering/deferred-visibility-buffer/SPEC.md).

## Context

Before this migration, the renderer shaded opaque world geometry forward.
Material evaluation, lighting, shadows, IBL, and transmission were coupled to
rasterized fragments, so overdraw repeated the whole fragment workload.
Visibility and draw submission were CPU-driven per submesh, and each surviving
draw required backend root and state encoding.

The codebase already has GPU-addressed vertex data, immutable indexable
material rows, a working integer picking attachment/readback path, largely
derivative-free lighting helpers, and conditional render-graph passes. P0-P3
added typed graph-driven compute descriptors, realized graph buffers,
indirect-read synchronization, per-mip graph image uses/views, shared
geometry/candidate/visible rows, and stable-generation vertex/index
megabuffers. The later Metal and Vulkan slices now execute GPU-generated opaque,
transmission, and shadow commands through their backend-native indirect paths.

Earlier opaque-compaction and megabuffer/MDI proposals need the same
foundations, but their existence is not evidence that any of them ship. The
renderer architecture spec remains the status authority.

The requested direction is a GPU-driven deferred renderer using visibility and
G-buffers, developed Metal first without abandoning Vulkan. The design must
also preserve ADR-018's immutable pre-transmission source and ADR-019's current
light-assignment model.

The installed Metal 4 SDK exposes GPU-address indirect indexed draw, indirect
compute dispatch, ICB execution, and GPU-sourced ICB ranges. The open question
is therefore which capability-gated strategy is correct and reduces CPU work
on supported devices—not whether the API surface exists.

## Decision

Adopt a staged GPU-driven deferred visibility-buffer architecture for opaque
and alpha-cutout geometry. Ordinary alpha blend, world text, UI text, and UI
remain feature-local direct-raster compositing passes inside the deferred
renderer; they are not a selectable whole-frame forward topology. Transmission
may join the deferred path only if its explicit single-layer fidelity decision
is accepted; otherwise ADR-018 forward transmission remains valid beside
deferred opaque shading, but complete retirement of the legacy forward renderer
is blocked.

**Separate visibility, material evaluation, and lighting.** Rasterization
writes an encoded visible-draw row and primitive ID. A compute pass reconstructs
the winning surface with analytic barycentrics and explicit texture gradients,
then writes a compact linear G-buffer. A second compute pass reconstructs world
position and evaluates the existing lighting model. The initial 8-byte
visibility/12-byte G-buffer layout is capability- and fidelity-gated, not a
promise that quantization is invisible.

**Cull draw candidates, not bare instances.** The source table contains one row
per prospective instance × submesh draw, including geometry, material,
instance, index range, conservative bounds, and render-state bucket. GPU
compaction produces visible rows plus indirect arguments/counts partitioned at
least by opaque/cutout and single-/double-sided state. Input geometry for this
path is triangle-list with u32 indices.

**Cull over a view set, not a single camera.** One candidate array is tested
against N views, each owning its own visible-row range, argument range, count,
and capacity policy. The camera is view 0 and each shadow cascade is a further
view. This mirrors the existing CPU contract, where
`vkr_visibility_classify()` already derives camera and shadow visibility from a
single bounds test. It is required for retirement rather than for the camera
path: if cascades keep CPU visibility and merging, `vkr_visibility.c` and
`vkr_draw_merge_candidates()` survive with a live world caller and the
"CPU submission is deleted" clause cannot be met.

**Encode one command per visible draw candidate initially.** Each command uses
`instanceCount = 1` and `baseInstance = visible_draw_index`. The vertex stage
passes that index flat to the fragment stage, which stores
`visible_draw_index + 1`; zero is the empty visibility value. This avoids a
draw-ID dependency and primitive-ID ambiguity under instancing. Command volume
is measured and may justify a later explicit instanced encoding.

**Make GPU work and ownership graph-visible.** Compute writes to visible rows,
draw arguments, and count buffers are followed by declared shader reads and
`INDIRECT_READ`/`DRAW_INDIRECT` uses. Images declare mip/subresource ranges,
uses resolve by named binding, and all frame-slot, target-image, history, and
megabuffer generations are reused or retired only after GPU completion. Every
capacity is fixed for a recorded frame; before retirement, overflow uses the
retained legacy forward path or stops the phase instead of silently dropping
work. After retirement it grows between frames or rejects the frame explicitly;
there is no hidden rendering fallback.

**Keep the pre-transmission HDR topology.** Material resolve initializes
`hdr_pre_transmission` with emissive/zero and deferred lighting reads and writes
that image, adding lighting or writing sky. ADR-018's declared feedback copy
then seeds `hdr_scene_color`. Deferred transmission samples the immutable
pre-transmission image and composites into scene color; ordinary blend follows.
The editor path uses the equivalent
`scene_pre_transmission -> scene_color` ordering.

**Use a fused, initially full-screen transmission resolve.** A separate
visibility buffer retains only the nearest transmissive surface after a
declared opaque-depth seed. The compute pass exits on empty pixels. Pixel-list
compaction remains optional. The provisional Metal P19 branch uses an explicit
scan producer with bounded list/count/overflow/dispatch buffers, fuses the
required background copy into that scan, and retains full-screen resolve as the
default until owner-level performance evidence accepts the branch.

**Decide single-layer transmission fidelity before building on it, and resolve
a rejection with bounded depth peeling rather than a retained forward path.**
The decision is answerable on the current forward renderer by depth-testing
transmissive draws against each other, so it is scheduled before GPU submission
work rather than at the transmission phase. If rejected, a bounded N-layer
depth peel repeats the ADR-018 topology per layer, reusing the graph `repeat`
mechanism, and ships before deferred becomes the default. The owner selected
the rejection path. Metal P18 fixes the current bound at four ordered surfaces:
four opaque-depth seeds and peel visibility/depth layers feed four
back-to-front shade passes through a graph-declared RGBA16F ping-pong target.
That bound covers entry and exit surfaces for both closed transmissive meshes
in the current layered witness. It is an explicit capacity/fidelity limit;
Vulkan parity and P20 budget acceptance were separate gates and are now
satisfied by the accepted four-layer implementation.

Depth peeling is chosen over an order-independent accumulation scheme because
transmission here is not alpha blending: it refracts a background sample with
IOR, thickness, and Beer-Lambert attenuation, and a depth-weighted accumulation
cannot represent a per-layer refracted background at all. Peeling is exact in
ordering, bounded by construction, and reuses machinery that already exists.
Order-independent accumulation remains the appropriate tool for ordinary alpha
blend, which stays out of scope.

**Use dedicated, conservative HZB history.** Swapchain/offscreen per-image depth
is not assumed to be previous-frame depth. A completion-gated history ring
stores an authored mip chain. The baseline uses normal-Z maximum reduction and
invalidates occlusion on resize, view/projection change, camera cut, depth-
convention change, or occluder/world epoch change. Moving-camera, current-depth
two-phase occlusion is a later explicit graph topology, not part of the baseline
claim.

**Preserve picking resolve/readback.** A GPU resolve selects the nearest opaque
or transmissive visibility sample and copies `object_id` to a completion-gated
readback buffer. Feature-local picking raster remains for ordinary blend and
text, whose candidate is depth-compared with the deferred result. Only
redundant opaque/transmission mesh re-rasterization may be removed.

**Migrate in narrow vertical slices, then delete the legacy renderer.** Shared
graph/ABI/lifetime foundations landed on both backends. Each rendering feature
landed on Metal first and was then mirrored on Vulkan before the next layer. A
temporary `deferred_enabled` condition kept the legacy forward topology
selectable through P20, when deferred became the default and completed a
bounded cross-backend stability soak. P21 completely removed the legacy
graph branch, opaque/transmission shaders and pipelines, CPU
visibility/submission and fallback routes, selector/configuration state,
dual-path metrics/tests, and dead compatibility code. It began only after no
shipping opaque/transmission material, state, topology, capacity overflow, or
unresolved transmission-fidelity case needed that path.

After P21, the renderer has one world topology. Ordinary blend, world/UI text,
UI, post processing, and their picking coverage keep only their narrow
feature-local raster passes; none may preserve or call the retired world
renderer. Shadows are not among them: they are a view of the deferred
submission path once multi-view culling ships. Runtime rollback and per-frame rerouting end at P21. Rollback requires
reverting the retirement change in source control and rebuilding, and
reintroducing a permanent forward renderer requires a new architectural
decision.

## Consequences

- Opaque shading cost can become independent of depth complexity, but the
  design adds visibility, G-buffer, HDR seed/read-modify-write, and compute
  traffic. Whether this is faster is a measured outcome, not a consequence
  claimed in advance.
- GPU-driven submission can remove CPU per-submesh visibility and draw
  encoding from opaque/transmission hot paths. A Metal GPU-address indirect
  loop may still leave CPU command calls; GPU-encoded ICB execution is compared
  explicitly before claiming submission collapse.
- Alpha-cutout acceptance samples base color during raster and full material
  resolve evaluates it again. Opaque camera buckets use a discard-free fragment
  with forced early tests, and opaque shadow buckets omit the fragment stage.
  Cutout early-depth behavior remains implementation-dependent.
- Geometry publication becomes more complex. Megabuffer addresses must remain
  stable within a generation; growth, compaction, physical range reuse, and
  table retirement are completion-gated and observable.
- Logical opaque intermediates start at 20 bytes/pixel, but actual memory and
  bandwidth include target/frame multiplicity, depth, transmission, HZB,
  history, HDR traffic, and alignment. Resource statistics must report the
  complete cost.
- Colored dielectric F0 remains represented in RGB. Deferred debug mode 6 is
  `(max(f0), roughness, occlusion)`. Any 8-bit channel that fails visual
  evidence moves to a wider linear format.
- Compute material resolve must reproduce raster pixel-center, viewport Y,
  winding/front-face, two-sided normal, model/tangent transform, degenerate
  triangle, and explicit-gradient LOD semantics on both backends.
- Material resolve trusts GPU compaction bounds and the finite-geometry
  publication contract. It retains only genuine geometric-degeneracy rejection
  in the per-pixel path. Malformed GPU rows and non-finite published inputs are
  outside the accepted packet/publication contract.
- Viewport-to-NDC conversion is backend-owned. Vulkan's positive-height
  viewport plus the shared Y-inverted projection maps framebuffer top to NDC
  Y = -1; Metal uses its own inverted screen mapping. Vulkan visibility resolve
  formerly applied the Metal flip a second time, so it reconstructed material
  attributes at the vertically mirrored raster location. Its sample point and
  analytic Y derivative now use the Vulkan mapping shared by deferred world-
  position reconstruction, without adding per-pixel samples, branches, or
  resource work.
- Both backends store and composite four ordered transmission layers. More than
  four visible surfaces remain intentionally clipped by the documented bound.
- Bounded depth peeling adds one full transmission
  visibility/resolve/feedback iteration per layer. Cost scales linearly with the
  peel bound, so the accepted bound is a measured budget decision and the
  renderer gains a documented maximum transmissive depth rather than unbounded
  correctness.
- Shadow cascades become views of the GPU submission path rather than a retained
  CPU-driven feature. This extends culling to a view set with per-view capacity
  and overflow policy, and it is what makes deleting CPU world submission
  achievable at all. If it is not done, retirement must instead be amended to
  accept CPU shadow visibility as named residue.
- The case for deleting the forward renderer is maintenance cost, not speed. Two
  whole-frame topologies mean every material feature, shading fix, debug
  channel, capacity policy, metric, and validation case is built twice on two
  backends indefinitely. Retirement is therefore scheduled as a phase with an
  evidence gate in front of it, because a migration that ships the new path and
  never deletes the old one is the expected failure mode.
- HZB is optional for correctness. Any false-negative visibility omission
  disables it while frustum-only GPU culling remains available.
- ADR-019's bitmask grid, stable light table, exact range/cone rejection, and
  local probe model remain unchanged. This decision does not introduce
  clustered lighting.
- ADR-018's immutable-source rule remains and now records the four-layer Metal
  feedback chain selected by P18.
- ADR-013 is superseded for opaque/transmission and shadow submission. It
  remains in force only for feature-local blend/text/UI submission.
- P21 deliberately removes a compatibility surface rather than leaving dead
  code behind a default-off flag. This reduces graph, pipeline, cache,
  lifecycle, metric, and validation combinations, but post-retirement rollback
  becomes a source-control/build operation.
- Unsupported opaque/transmission input and exhausted capacity become explicit
  pre-recording errors after P21. Silently omitting work or reviving a hidden
  forward reroute is not an acceptable degradation mode. This covers malformed
  candidate structure and exhausted bounds only. A candidate whose geometry or
  material has not finished publishing is a bounded lifetime boundary, not
  unsupported input; it is omitted for that frame and recorded once by a
  one-shot warning. Conflating the two makes every frame of a cold scene load
  a hard failure on any backend that resolves publication before recording.
- The renderer architecture spec and harness/metrics documentation are updated
  only as individual phases ship; this ADR does not change status by existing.

## Alternatives Considered

**Keep the former forward renderer.** This was the fallback through P20. P21
rejected retaining it as a runtime option.

**Classic rasterized G-buffer deferred.** This is simpler and avoids analytic
surface reconstruction, but material evaluation still follows raster overdraw
and the G-buffer must carry the attributes that cannot be reconstructed. It
remains the fallback if analytic resolve cannot meet correctness or cost.

**Shade visibility directly without a materialized opaque G-buffer.** This
reduces intermediate bandwidth but fuses material and lighting scheduling and
loses directly inspectable material channels. It remains a measured fallback
if G-buffer traffic is the losing term. The transmission baseline deliberately
uses this fused form because its coverage is sparse and its channel set is
wider.

**CPU-driven visibility-buffer draws.** This is a coherent diagnostic and
fallback because visibility encoding only needs a stable visible-row index. It
does not satisfy the GPU-driven submission goal and does not justify retiring
the CPU visibility/draw path.

**Keep both whole-frame renderer topologies permanently.** Rejected after P20.
It preserves an attractive emergency switch, but every graph resource,
pipeline/cache lifecycle, material capability, metric, and validation case must
then remain correct in both combinations. Source-control rollback is sufficient
after a bounded stable-default period; the production renderer should carry one
world topology.

**Instanced indirect commands with a wider visibility encoding.** This can
reduce command count but requires an explicit per-instance discriminator and
more visibility bandwidth or another lookup. It is retained for measurement if
one-command-per-candidate becomes material.

**A conventional depth prepass.** The baseline visibility pass already writes
depth with minimal fragment work. A separate prepass doubles geometry work and
is added only if matched measurement shows a net benefit.

**Move alpha blend or lighting assignment into this decision.** Rejected as
scope expansion. Multi-layer order-dependent blend and a replacement for
ADR-019 require separate designs and evidence.

**Implement all Metal phases before Vulkan parity.** Rejected because it would
accumulate graph, ABI, shader, and lifetime assumptions without proving the
cross-backend contract. Metal still leads each feature, but Vulkan mirrors it
before the next feature layer.

## Revisit When

- The Metal strategy spike finds that neither GPU-address indirect execution
  nor GPU-encoded ICB execution is capability-correct and faster than CPU direct
  submission on supported hardware.
- Candidate/visible/command work-volume evidence diverges from the CPU/reference
  classification or any fixed-capacity path can silently omit work.
- Analytic barycentric, front-face, normal/tangent, or explicit-gradient LOD
  evidence cannot match the retained forward path.
- Complete opaque-path measurements on the repository's Sponza and Bistro
  workloads fail to pay for visibility/G-buffer/HDR traffic.
- The P20 stable-default soak exercises any legacy-forward, unsupported-input,
  or overflow fallback, or exposes a shipping opaque/transmission state with no
  deferred representation. Do not start P21 until the trigger is eliminated
  and the soak is repeated.
- Layered-transmission evidence makes the nearest-layer approximation
  unacceptable.
- HZB history produces a false-negative omission, or moving-camera occlusion is
  required strongly enough to justify the explicit two-phase topology.
- Temporal antialiasing adds velocity/history needs that change the G-buffer.
- Material divergence makes classification/tile binning worth a separate
  measured slice.
- Mesh shaders or meshlet culling become supported and measured enough to
  replace the per-candidate command model.
- ADR-019's own light-count/probe-overlap triggers fire; this decision does not
  move those thresholds.
- After P21, a new production requirement cannot be represented by the
  deferred contracts and explicit rejection is unacceptable. Reintroducing a
  selectable forward renderer still requires a new ADR and fresh correctness
  and performance evidence; this trigger does not silently restore deleted
  code.
