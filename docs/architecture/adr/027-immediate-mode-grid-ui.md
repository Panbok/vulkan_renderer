---
status: proposed
updated: 2026-08-12
authority: adr
---
# ADR-027: Immediate-mode grid UI with a composited editor viewport

**Status:** Proposed

## Context

VKR has no UI system. `VkrUiSystem` is a fixed array of 16 text slots positioned
by a four-way corner anchor; that switch statement is the entire layout engine.
There is no rectangle primitive, no hit testing, no clipping, and no UI input
model. Both an editor and in-game HUD are required, and the choice of layout
model, update mode, and rendering strategy constrains every widget written
afterwards — so it belongs in an ADR rather than in whichever file gets written
first.

Four facts about the existing renderer shape the decision.

**The editor topology is already authored, but not plumbed.** The JSON graph
branches on `editor_enabled`: the scene renders offscreen into `scene_color` at
`extent:{mode:viewport}`, `Editor.Composite` blits it into a panel rect, then
`UI.Editor` draws over the swapchain. However
`VkrRenderGraphFrameInfo.editor_enabled` is never assigned in either backend —
both patch only `picking_pending` and `shadow_cascade_count` before
`vkr_rg_begin_frame()`. The condition is therefore permanently false, and
`scene_color`, `Editor.Composite`, and `UI.Editor` are never built.
`viewport_width/height` is likewise pinned to window size.

**The GPU path is ahead of the CPU path.** `pass.ui` is registered in both
backends and already draws screen-space text. The `draws` half of
`VkrUiPassPayload` is plumbed end to end but the application always submits it
empty.

**Two capabilities a UI needs are absent.** There is no scissor support anywhere —
scissor is set once to the full render area, and Metal has no `setScissorRect`
call in the tree at all. There is no input focus, capture, or arbitration layer;
the camera controller, picking, and gizmo drag each poll `InputState` and
hand-arbitrate with ad-hoc button checks.

**The existing `docs/ui/` tree designs a different system** — a retained-mode
element tree with a flexbox engine and no tiling — across roughly 4,700 lines,
none of it implemented, with stale paths referencing the removed view/layer
system. Leaving it in place would leave two competing designs on file.

## Decision

Adopt four positions, specified in
[docs/ui/ui-architecture-spec.md](../../ui/ui-architecture-spec.md).

**1. The editor composites an offscreen scene into a panel rect.** Keep the
authored topology and plumb it, rather than moving to translucent panels floating
over a full-window scene. The scene renders at viewport resolution, not window
resolution, so editor chrome costs no scene fill; picking already maps window
pixels to target pixels through `VkrViewportMapping`; and UI text stays crisp
because it is never scaled with the scene image. Docking later replaces the
hardcoded gutters with a dock tree that owns the panel rect.

**2. An immediate-mode API over a retained internal cache.** Callers write
per-frame widget calls. Behind them, a persistent table keyed by a stable hashed
identity holds scroll offset, focus, text-edit state, the last-resolved rect, and
last-frame draw bounds. A pure stateless rebuild is rejected because it
forecloses incremental layout and any form of damage tracking, and pushes scroll,
focus, and caret state onto every caller.

**3. Grid is the single layout model.** Track lists in `px`, `fr`, `auto`, and
percent, with `minmax()`, gaps, spans, and row-major auto-placement. A 1×N grid
is a row stack and an N×1 grid is a column stack, so grid subsumes the linear
cases that would otherwise justify a flex engine. No flex engine is written.

**4. Rendering is one vertex stream per frame, split into scissored batches, with
a tile grid and per-tile content hashing.** Vertices reuse the existing shared
`VkrTextVertex` ABI record so no new vertex format enters reflection validation.
Tile hashing yields a damage set. The damage set's use for a cached `ui_color`
target is gated behind measurement (see Consequences).

## Consequences

**Packet contract.** Version 10 → 11. `VkrPreparedUiDrawList` replaces the
per-draw borrowed-buffer shape; the 16-slot `VkrUiTextSlot` array,
`VKR_PREPARED_TEXT_DRAW_MAX`, and `VkrTextUpdatesPayload` retire in the same
bump so the version moves once.

**New backend surface, written twice.** Per-batch scissor (new API surface on
Metal, a dynamic-state call on Vulkan), one new vertex/fragment pair for rounded
rects, and a small dedicated UI root. The rounded-rect pipeline is forced by the
vertex stage: the SDF needs rect-local coordinates and `texcoord` is taken by the
atlas, and `VkrTextVertex` cannot grow because world text shares it. Note that
the Vulkan utility root is not reflection-validated while Metal's text root is,
so these are two distinct ABI surfaces.

**A per-draw defect is fixed rather than inherited.** The current utility-root
helper zeroes 512 bytes per draw; at UI batch counts that is exactly the per-draw
cost AGENTS.md classifies as a defect.

**Input arbitration changes the update order.** UI capture is computed first and
read by everyone; nothing consumes events, since edge detection queries an
`InputState` the UI does not mutate. Picking must not cancel an in-progress gizmo
drag when the cursor crosses a panel, and the camera controller is told it is
blocked rather than skipped so its smoothing does not snap.

**Text fields are blocked on platform work.** There is no character-input path
(`WM_CHAR` / `NSTextInputClient`) and no key repeat. IME is explicitly out of
scope.

**DPI must be handled up front.** Engine coordinates are backing pixels, and
Windows has no DPI-awareness call, so style constants are authored in logical
points and scaled at resolve time. Retrofitting this after widgets exist would
touch every constant.

**The tile cache is designed in but gated on evidence.** Tile bins pay for
themselves immediately in hit-test acceleration, batch grouping, and skipping the
UI pass when nothing is dirty. The cached `ui_color` target is a separate,
riskier step:

- It is **incorrect on Vulkan as the graph stands**. The compiler re-seeds every
  non-imported graph image to `UNDEFINED` each frame, so `LOAD` reads
  potentially-discarded contents. Metal has no layouts and reuses the texture, so
  the same code would look correct on macOS and be wrong on Windows. It requires
  a `PRESERVE_ACROSS_FRAMES` flag with a per-instance initialized bit.
- Metal does not implement `PER_IMAGE` at all.
- Damage must accumulate **per swapchain image**, not per frame slot, because an
  image's staleness is "since it was last acquired" and is not a fixed period.
- It needs a per-frame-variable load op, which the JSON graph cannot express.
- It adds an unconditional full-screen alpha blend whose cost is fixed while its
  saving is proportional to the clean-tile fraction.

The specification therefore requires one harness run against the current build
before any of this is written, and a two-variant paired A/B before it ships. No
claim about which path is faster appears in either document.

**The tiled path selects by graph file, not by graph condition.**
`VkrRgJsonConditionKind` is closed and its parser hard-errors on unknown
conditions, so a new condition would touch nine files and break older binaries
reading newer graphs. A second authored graph file selected through the existing
`graph_path` config avoids that and gives each arm distinct pass names for
measurement.

**Documentation.** `ui-system-overview.md`, `ui-layout-engine-design.md`, and
`ui-element-primitives-design.md` are superseded and archived. The components and
docking designs remain proposed under the new spec. The architecture spec's §4
"Editor viewport and picking" row is corrected to `partial`.

## Alternatives Considered

**Full-window scene with translucent floating panels.** Rejected: the scene
renders at full window resolution behind opaque chrome, wasting fill; panels
occluding the viewport make precise editing harder; and it discards an authored
topology that already exists. The offscreen model also keeps UI text unscaled.

**Supporting both presentation models at runtime.** Rejected for now: it doubles
the composite path and the viewport-mapping and picking cases to validate, for a
benefit no current requirement asks for.

**Pure stateless immediate mode.** Rejected: no incremental layout, no damage
tracking, and scroll, focus, and caret state become every caller's problem.

**Retained-mode element tree** (the archived design). Rejected: it requires
explicit create/destroy lifetimes for transient editor UI, and its generational
handles duplicate what hashed immediate-mode identity provides for free.

**Flexbox, or flexbox plus grid.** Rejected: roughly double the layout code and
edge cases, and `fr` tracks already cover the proportional-distribution case that
motivates flex. The archived layout design shows how large that surface is.

**Adding a graph condition for the tiled path.** Rejected: the condition enum is
closed and its parser hard-errors, so a new value is a nine-file change that
breaks version skew between binary and authored graph.

**Always running both UI passes with an empty damage set**, to avoid a second
graph file. Rejected: it pays the composite unconditionally, which is precisely
the cost the measurement is trying to isolate.

**Reusing GPU picking for UI hit testing.** Rejected: picking is asynchronous and
request-gated so it cannot drive hover, and its 29-bit payload cannot hold hashed
immediate-mode identities collision-free.

**Extending `VkrTextVertex` with rounded-rect attributes.** Rejected: it is a
shared, static-asserted ABI record that world text also uses, and it is validated
by reflection on both backends. The parameters travel in the batch root instead.

## Revisit When

- The dirty-tile ratio exported in P4 is not consistently low for a realistic
  editor UI — in which case the cached `ui_color` target in §8.3 of the spec is
  dropped and only the binning uses in §8.2 are kept.
- The initial harness run shows `UI.Fullscreen` GPU time is already at the noise
  floor, which retires the cached-target work before it is written.
- Multiple OS windows become a requirement, which would invalidate the in-window
  docking assumption and reopen device selection, surface creation, and the
  graph's single present output.
- Non-rectangular clipping becomes necessary, which would move clip state into
  the fragment shader and into the tile hash.
- A second layout model is genuinely needed — for example if text flow or
  baseline alignment across containers cannot be expressed as grid tracks.
