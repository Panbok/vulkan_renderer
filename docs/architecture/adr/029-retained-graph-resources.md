---
status: partial
updated: 2026-08-23
authority: adr
---

# ADR-029: Retained render-graph resources with per-subresource content validity

## Status

Accepted (partial). Implemented on both selected implementations and in force:
`shadow_map` is declared `RETAINED, PER_IMAGE, RESIZABLE`, and the flag,
provider, per-subresource seeding, compile-time read-before-write error, and
proven-submit commit all ship.

**The gap: the Vulkan path has never executed.** Vulkan's
`initialization_supported` is Windows-only in `vkr_renderer_impl_select()`, so
Metal is the selected implementation on the development machine. Vulkan's
retained provider compiles and is covered through the provider interface by CPU
tests, but no Vulkan frame has run against it and its barriers are unverified at
runtime. Validation at two and four target images passed on Metal only. A
Windows validation run is required before the Vulkan half is trusted.

Retention is load-bearing on Metal through Phase 3B. The shadow resolver uses a
backend-neutral token for the selected physical image, and repeated cascade
passes whose render-mask bit is clear are absent from the graph. Downstream
readers therefore consume retained layers no pass wrote in that frame. Pending
shadow history and retained terminal state both commit only after successful
submit. The Vulkan runtime gap above now covers this production
read-without-write path as well as the provider itself.

## Context

Directional shadow cascades re-render the whole scene every frame. Measured on
Bistro at 2560x1440 with the Metal implementation, the four `Shadow.Cascade.*`
passes cost 9.4 ms of a 28.6 ms GPU frame — 33%. Halving the shadow map to 1024
changed that total by 3% (9.402 ms to 9.126 ms), so the cost is geometry
submission, not fill: every cascade re-submits ~250 draws covering
approximately 4.2M vertices, and per-view frustum rejection removes almost
nothing (cascade 0 draws 211.7 of 254 candidates).

The largest available saving is therefore not to render a cascade at all when
its contents are still correct. That requires the shadow image to keep its
contents across frames, which the graph cannot currently express.

### What already exists, and why it is not enough

The graph has three flags that sound applicable. None of them provides retained
contents for this case.

| Flag | What it actually does | Why it does not serve cascade reuse |
|---|---|---|
| `PERSISTENT` | Only suppresses the read-before-write diagnostic, in `vkr_rg_image_allows_read_without_write()`. It preserves nothing. Declared in the JSON parser but used by no resource in `main.rendergraph.json`. | Silencing a warning is not a content guarantee. A pass reading it would sample undefined memory. |
| `HISTORY` | A real cross-frame mechanism, and the only one. Resolves to a `PER_FRAME_SLOT` instance ring; the backend records `history_producer_submit_value`, `history_world_epoch`, `history_view_projection`, and extent per instance, then selects a completed instance to read and a different one to write. Used by `hzb_history`. | It is a ping-pong ring: each frame writes a new instance and reads an older one. Cascade reuse must write *nothing* and keep sampling the instance it wrote N frames ago. Validity is also whole-image, and it is special-cased in backend code (`vkr_vk_deferred_cull_root()` issues its own barrier) rather than being general graph machinery. |
| `PER_IMAGE` | One instance per target image. `shadow_map` uses it. | `vkr_rg_resource_instance_domain()` returns `PER_IMAGE` before it tests `HISTORY`, so the two cannot combine today. A retained shadow image must be both. |

`shadow_map` is currently `TRANSIENT, PER_IMAGE, RESIZABLE`. The compiler seeds
every non-imported image as `VKR_TEXTURE_LAYOUT_UNDEFINED` each frame, which
discards contents by construction. Physical image allocation is cached by
descriptor equality in `vkr_vk_realize_graph_images()`, but allocation reuse is
not content preservation: an `UNDEFINED` seed permits the driver to discard.

Cascade reuse additionally needs validity per **layer**, not per image. Cascade
2 can remain valid while cascade 0 must re-render, and all four cascades are
layers of one array image.

## Decision

Add a `RETAINED` resource flag whose contents survive across frames in place,
with content validity tracked per physical instance and per subresource.

1. **Flag.** `VKR_RG_RESOURCE_FLAG_RETAINED`. The initial contract is
   image-only, graph-owned, and non-aliasable; JSON and direct C construction
   reject retained buffers rather than implying unsupported buffer state.
   Mutually exclusive with `TRANSIENT`, `EXTERNAL`, `HISTORY`, and
   `PER_FRAME_SLOT`. Composable with `PER_IMAGE` and `RESIZABLE`.
   `vkr_rg_resource_instance_domain()` keeps returning `PER_IMAGE` for a
   retained per-image resource; `RETAINED` describes content lifetime, not
   instance domain, and the two are independent axes.

2. **Backend-owned state.** The selected implementation stores, beside each
   realized physical instance and for each subresource (layer, mip), the
   terminal access mask, stage mask, layout, and a content-valid bit. The
   per-frame graph carries only the selected seed and the pending terminal
   state, so nothing cross-frame lives in frame-local structures.

3. **Compilation.** Compilation receives the selected resource-instance index
   and its seed, and initializes retained subresources from the last
   *successfully submitted* state for that instance rather than from
   `UNDEFINED`.

4. **Read-before-write is an error, not a warning.** Reading a retained
   subresource whose content-valid bit is false fails compilation. Validation
   uses the exact mip/layer slice of each scheduled access; writing one layer
   cannot authorize reading another. The frame must schedule a writer instead.
   This is deliberately stricter than `PERSISTENT`'s warning, because the whole
   point is that a reader may skip its producer.

5. **Commit only on proven submit.** A successful submit commits terminal states
   and content-valid bits. A skipped, rejected, or failed frame commits nothing.
   Staging then committing is what keeps a cancelled frame from advertising
   contents that were never written.

6. **Invalidation.** Resize, format change, layer or mip change, target
   recreation, image-count change, and graph generation change all clear
   content-valid for the affected instances. Physical destruction still waits
   for proven GPU completion.

7. **Synchronization.** A later submit may rely on same-queue ordering against
   the recorded submit value. CPU code must never block waiting for older
   contents. Cross-queue use requires an explicit semaphore dependency and is
   not supported by this ADR.

`shadow_map` becomes `RETAINED, PER_IMAGE, RESIZABLE` only after both selected
implementations persist and seed the same subresource state. Phase 3A ships the
contract and renders every cascade exactly as before; pass omission is Phase 3B
and a separate change.

## Consequences

- The graph gains a third content-lifetime concept beside frame-local and
  history-ring. That is a real increase in surface area, and it is why
  `PERSISTENT` should be removed or renamed in the same change rather than left
  as a fourth thing that sounds like this one.
- Barrier planning must seed from backend state, so the compiler stops being a
  pure function of the frame's declarations. Compilation now depends on which
  physical instance was selected.
- A reader can be scheduled in a frame with no writer, which the current
  compiler treats as suspicious. The content-valid bit becomes the authority
  for whether that is legal.
- Retained buffers remain outside this decision. Adding them requires a
  backend-owned buffer-range state contract rather than reusing image
  subresources by analogy.
- Memory is unchanged by this ADR. `shadow_map` at 2048 with four D32 layers is
  64 MiB per target image, 192 MiB at three images, retained or not. Retention
  does not add an instance; it stops discarding one.
- Debugging changes character: a frame's output can now depend on a previous
  frame's contents, so a wrong image may have no producer in the current frame's
  graph. Per-instance content-valid state must be inspectable.
- Both implementations must agree. A retained contract honoured by one backend
  and not the other is worse than no retention, because reuse decisions are made
  in backend-neutral frontend code.
- Variable graph topology is now normal for retained consumers. Harness pass
  aggregation treats absent conditional passes as omissions rather than invalid
  timestamps and grows its pass-name catalog when a later frame renders a
  previously omitted cascade.

## Alternatives Considered

**Generalize `HISTORY` instead of adding a flag.** Rejected for this slice.
`HISTORY` means "ring of N, read the previous, write the current", and the HZB
depends on exactly that. Widening it to also mean "one instance, write
sometimes, read always" would make one flag carry two incompatible selection
rules, and would put the HZB's proven path at risk for a shadow feature.
Revisit once retained state has shipped and the two mechanisms can be compared
with real call sites.

**Reuse `PERSISTENT`.** Rejected. It only suppresses a diagnostic and preserves
nothing, so adopting the name would mean silently changing what existing
declarations promise. It should be deleted or renamed to
`ALLOWS_READ_WITHOUT_WRITE`, which is what it does.

**Static cache image plus per-frame copy.** Keep a second retained array holding
static-only cascade contents and copy it into a working image each frame.
Rejected for now: it needs `TRANSFER_SRC` on the cache, `TRANSFER_DST` on the
working image, a `LOAD` dynamic pass, another 64 MiB per cache instance, and
moves about 32 MiB per copied layer. Spec §7.6 defers it to its own design, and
it cannot be evaluated before basic reuse is measured.

**Per-cascade separate images instead of array layers.** Would make whole-image
validity sufficient and avoid per-subresource tracking. Rejected because it
multiplies image count and barrier work, and the spec (§3) records that one
array with per-layer barriers is the better fit here.

**Do nothing and attack the cost with shadow LOD.** Not mutually exclusive, and
possibly cheaper per unit of work: rendering reduced-detail geometry into
distant cascades attacks the same geometry-bound cost and helps every frame,
including the correctness-forced renders reuse cannot avoid. This ADR does not
claim reuse is the best available option, only that reuse is impossible without
retained contents. If LOD is pursued first, this ADR can wait.

## Revisit When

- Cascade reuse is measured and does not produce a net frame-time win, in which
  case retained resources have no remaining production caller and should be
  removed rather than kept for a hypothetical user.
- A second feature wants retained contents. Two callers would show whether the
  per-subresource granularity chosen here is the right one.
- Cross-queue use becomes necessary, which requires the semaphore dependency
  this ADR excludes.
- Shadow LOD or finer draw granularity reduces cascade cost enough that reuse
  is no longer worth its complexity.
