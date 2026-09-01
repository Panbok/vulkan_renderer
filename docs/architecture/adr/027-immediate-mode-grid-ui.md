---
status: accepted
updated: 2026-09-01
authority: adr
---
# ADR-027: Immediate-mode grid UI with a composited editor viewport

**Status:** Accepted

## Context

At proposal time, VKR had no general UI system. `VkrUiSystem` was a fixed array
of 16 corner-anchored text slots with no rectangle primitive, hit testing,
clipping, or UI input model. Both an editor and in-game HUD were required, and
the choice of layout model, update mode, and rendering strategy constrained
every later widget.

Four facts about the renderer shaped the decision.

**The editor topology was authored, but not fully plumbed.** The JSON graph
branches on `editor_enabled`: the scene renders offscreen into `scene_color` at
`extent:{mode:viewport}`, `Editor.Composite` blits it into a panel rect, then
`UI.Editor` draws over the swapchain. However
`VkrRenderGraphFrameInfo.editor_enabled` was not assigned in either backend;
both patched only `picking_pending` and `shadow_cascade_count` before
`vkr_rg_begin_frame()`. The condition was therefore permanently false, and
`scene_color`, `Editor.Composite`, and `UI.Editor` were never built.
`viewport_width/height` was likewise pinned to window size.

**The GPU path was ahead of the CPU path.** `pass.ui` was registered in both
backends and could draw screen-space text, while the application submitted its
general draw payload empty.

**Two required capabilities were absent.** Scissor was set once to the full
render area, and Metal had no `setScissorRect` call. There was no input focus,
capture, or arbitration layer;
the camera controller, picking, and gizmo drag each poll `InputState` and
hand-arbitrate with ad-hoc button checks.

**The former `docs/ui/` tree designed a different system**: a retained-mode
element tree with a flexbox engine and no tiling across roughly 4,700 lines,
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
because it is never scaled with the scene image. A dock tree owns the panel
rectangle.

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
`VkrUiVertex` ABI record, which world text aliases, so no second vertex format
enters reflection validation.
Tile hashing yields a damage set. The damage set's use for a cached `ui_color`
target is gated behind measurement (see Consequences).

## Consequences

**Packet contract.** The implementation landed against a newer renderer. Version
25 to 26 replaced the editor quad with one compositor rectangle; version 26 to
27 introduced `VkrPreparedUiDrawList`. The 16-slot `VkrUiTextSlot` API and
`VKR_PREPARED_TEXT_DRAW_MAX` retired, while `VkrTextUpdatesPayload` narrowed to
world text.

**New backend surface, written twice.** Per-batch scissor, dedicated quad/text and
rounded-rectangle pipeline pairs, and one semantic 64-byte UI root ship on Metal
and Vulkan. `VkrUiVertex` is the canonical shared 32-byte record and world text
aliases it. Metal validates the UI root field by field through native
reflection; Vulkan pins its size and offsets alongside the shader declaration.

**A per-draw defect is fixed rather than inherited.** UI batches allocate and
initialize only the 64-byte UI root rather than zeroing the 512-byte Vulkan
utility root.

**Input arbitration changes the update order.** UI capture is computed first and
read by everyone; nothing consumes events, since edge detection queries an
`InputState` the UI does not mutate. Picking must not cancel an in-progress gizmo
drag when the cursor crosses a panel, and the camera controller is told it is
blocked rather than skipped so its smoothing does not snap.

**Text fields require committed character input.** A fixed 64-codepoint queue,
Windows `WM_CHAR` surrogate lowering, macOS committed-text lowering, and
platform-independent key repeat now ship. IME composition remains out of scope.

**DPI is handled up front.** Engine coordinates are backing pixels. Style
constants are authored in logical points and scaled once at resolve time from
the platform or explicit offscreen content-scale snapshot.

**Tile hashing ships; the cached target does not.** Retained, reusable CPU-side
64-pixel bins, ordered hashes, motion damage, and dirty metrics ship while the
direct pass still redraws the complete stream. ADR-029 later supplied retained
per-image graph state, but the prescribed early measurement found the direct UI
pass at 0.017583 ms GPU p50 in a local static retained-UI workload. Adding persistent
per-image storage and an unconditional full-screen composite was not justified,
so the cached `ui_color` path and its A/B were declined. This local dirty-tree
measurement is a scope gate, not a portable speed claim.

**Any future tiled path selects by graph file, not by graph condition.**
`VkrRgJsonConditionKind` is closed and its parser hard-errors on unknown
conditions, so a new condition would touch nine files and break older binaries
reading newer graphs. A second authored graph file selected through the existing
`graph_path` config avoids that and gives each arm distinct pass names for
measurement.

**Documentation.** `ui-system-overview.md`, `ui-layout-engine-design.md`, and
`ui-element-primitives-design.md` are superseded and archived. The UI
specification is the status authority for the shipped direct path and the
declined cached target.

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

- A future workload and paired evidence show that a cached UI target overcomes
  the direct pass's measured local 0.017583 ms GPU p50 plus the added composite
  and retained-image costs.
- Multiple OS windows become a requirement, which would invalidate the in-window
  docking assumption and reopen device selection, surface creation, and the
  graph's single present output.
- Non-rectangular clipping becomes necessary, which would move clip state into
  the fragment shader and into the tile hash.
- A second layout model is genuinely needed, for example if text flow or
  baseline alignment across containers cannot be expressed as grid tracks.
