---
status: implemented
updated: 2026-09-01
authority: design
---
# UI Architecture Specification

Authoritative design for the VKR user-interface system covering both editor and
in-game UI. Supersedes [ui-system-overview.md](../archive/ui-system-overview.md),
[ui-layout-engine-design.md](../archive/ui-layout-engine-design.md), and
[ui-element-primitives-design.md](../archive/ui-element-primitives-design.md).
Rationale is recorded in
[ADR-027](../architecture/adr/027-immediate-mode-grid-ui.md).

`status: implemented` means the direct-rendering architecture in P0-P4, P6,
and P7 ships. P5 was conditional, and the prescribed measurement retired it:
the direct UI pass measured 0.0176 ms GPU p50 locally, while the cached path
would add persistent per-image storage and an unconditional full-screen
composite. Section 8 records that decision and its evidence limits.
Native Vulkan runtime validation remains unavailable on the current macOS host,
so shader parity stays `UNALIGNED` rather than being inferred from compilation.

## 1. Summary

Four decisions define the system:

1. **Editor presentation** — the scene renders offscreen at viewport resolution
   and is composited into a panel rect; editor chrome surrounds it. This is
   already the shape of the authored render graph. It is *not* translucent
   panels floating over a full-window scene.
2. **Mode** — an immediate-mode API over a retained internal cache. Callers write
   per-frame widget calls; the system keeps persistent per-widget state keyed by
   a stable hashed identity.
3. **Layout** — CSS-Grid-like track solving as the *single* layout model. No flex
   engine. A 1×N grid is a row stack; an N×1 grid is a column stack.
4. **Rendering** — one vertex stream per frame split into scissored batches, with
   a tile grid and per-tile content hashing that yields a damage set.

Section 8 records why tile binning ships but a persistent cached UI target does
not. Section 2 records the completed editor-graph prerequisite.

## 2. Editor graph prerequisite

`assets/render_graphs/main.rendergraph.json` declares the full editor topology,
conditioned on `editor_enabled`: `scene_color`, `scene_depth` and
`scene_pre_transmission` at `extent:{mode:viewport}`, the `Editor.Composite`
pass, and `UI.Editor`.

P0 is complete. Both selected implementations copy `editor_enabled` and the
packet viewport extent into `VkrRenderGraphFrameInfo` immediately before graph
realization. Zero viewport dimensions retain the target-extent fallback. The
editor branch therefore realizes its viewport-domain images and executes
`Editor.Composite` followed by `UI.Editor`.

The application derives one scene panel rectangle from the dock tree. That
rectangle drives the viewport target extent, camera aspect, picking and gizmo
coordinate mapping, and the compositor viewport/scissor. The obsolete retained
editor mesh and material were removed, so there is no second representation of
the destination rectangle. Fullscreen rendering keeps `UI.Fullscreen`.

## 3. Layering and ownership

```
lib/src/core/ui/          pure CPU, no renderer dependencies, unit-testable
  vkr_ui_types.h/.c       rectangles, edges, and texture references
  vkr_ui_id.h/.c          ID stack and stable hashed widget identity
  vkr_ui_grid.h/.c        grid track solver
  vkr_ui_style.h/.c       style and box model; constants in logical points
  vkr_ui_draw.h/.c        draw-command buffer to vertices/indices/batches
  vkr_ui_tile.h/.c        tile binning, per-tile hashing, damage set
  vkr_ui_dock.h/.c        fixed-capacity dock tree and JSON persistence

lib/src/renderer/systems/vkr_ui_system.c
                          retained widget and tile-bin storage; builds the
                          packet payload from per-frame scratch; injected at
                          submit time
```

The `core/ui/` split exists so the grid solver, ID stack, and tile hashing are
testable under `./build_test.sh` with no GPU and no renderer state.

**Allocator discipline** (see `.codex/skills/vkr-memory/SKILL.md`):

| Data | Allocator | Reason |
|---|---|---|
| Per-widget state, per-tile hashes, last-frame AABBs, reusable tile offsets and command indices | `VkrDMemory` | Entries are reclaimed individually, and clean-frame bins must remain valid across scratch-scope resets |
| Per-frame vertices, indices, batches, command fingerprints, changed-frame bin workspaces, damage maps | `renderer.scratch_allocator` | The main loop already opens a scope around the whole frame in `application.h`; nothing is freed individually |

Building the payload inside `submit_packet` from the scratch allocator yields
exactly the "valid until submit returns" lifetime the packet contract requires.
`rf` is `&application->renderer`, so this is the same allocator the application
scopes.

## 4. Mode — immediate API over a retained cache

Per-frame shape is `vkr_ui_begin()` → widget calls → `vkr_ui_end()`.

**Identity.** An ID stack; each widget's identity is an FNV-1a hash of (parent
ID, caller-supplied label or pointer, loop index). Identity must be stable across
frames and independent of sibling count, so list items hash their own key rather
than their position where one is available.

**Retained state**, keyed by that identity in a `VkrDMemory`-backed table:
scroll offset, focus, text-edit cursor and selection, animation phase, the
last-resolved rect, the last frame's draw AABBs, and the subtree build hash.

**Hit testing resolves against the previous frame's rects.** This is inherent to
the model and is accepted. It is invisible except for a one-frame lag on the
first frame a widget appears at a new position.

Widget state is reclaimed when an identity is absent for a configurable number of
consecutive frames, so a collapsed panel does not leak and a briefly hidden one
does not lose its scroll position.

## 5. Layout — grid only

### 5.1 Model

```c
typedef enum VkrUiTrackUnit { VKR_UI_TRACK_PX, VKR_UI_TRACK_PCT,
                              VKR_UI_TRACK_FR, VKR_UI_TRACK_AUTO } VkrUiTrackUnit;
typedef struct VkrUiTrack { float32_t value; VkrUiTrackUnit unit;
                            float32_t min_px, max_px; } VkrUiTrack;
```

A container declares a column track list, a row track list, `gap`, and
`padding`. An item places at `(column start, span)` / `(row start, span)`, or
auto-flows row-major into the next free cell. `min_px`/`max_px` express
`minmax()`; `max_px == 0` means unbounded.

A 1×N grid is a row stack and an N×1 grid is a column stack, so one solver covers
toolbars, lists, property grids, and dock splits. This is the reason no flex
engine is specified.

### 5.2 Algorithm

Three passes, `O(n)` in nodes:

1. **Measure** — bottom-up intrinsic sizing. Text intrinsics reuse
   `vkr_text_measure()` in `lib/src/core/vkr_text.c`; nothing re-implements
   shaping.
2. **Resolve tracks** — fixed `PX` first, then `PCT` of the resolved container,
   then `AUTO` from measured intrinsics, then `FR` distributing the remainder in
   proportion, each clamped by `minmax`.
3. **Arrange** — top-down rect assignment with per-item align and justify
   (`start | center | end | stretch`).

**`fr` distribution under `minmax` is a fixed-point iteration**: clamping one
`fr` track changes the remainder available to the others. The solver runs at most
**3 iterations and then freezes**, accepting a slightly-off distribution rather
than looping. Unit tests assert that bound explicitly, including a construction
that would not converge.

### 5.3 Incremental layout

Each subtree carries a build hash over its declared tracks, placement, style, and
content identity. A subtree whose build hash *and* available size are unchanged
from the previous frame reuses its resolved rects and skips top-down arrangement.
Bottom-up measurement still folds cached leaf intrinsics through the container
tree each frame; text shaping and geometry rebuilds remain dirty-bit gated. This
reuse is independent of the tile rendering in §8.

### 5.4 Units and DPI

**All style constants are authored in logical points** and multiplied by
`content_scale` at resolve time. The UI reuses ADR-036's existing atomic window
snapshot rather than adding a competing scale source. macOS publishes backing
scale; Windows enables Per-Monitor V2 awareness, publishes `GetDpiForWindow /
96`, and updates it on `WM_DPICHANGED`; offscreen automation supplies an
explicit scale. Engine layout, mouse, viewport, and scissor coordinates remain
backing pixels.

## 6. Coordinate conventions

Four conventions are in play and the implementation fixes each one:

| Space | Convention |
|---|---|
| UI layout and draw commands | **Y-down**, origin top-left |
| UI geometry (vertex positions) | **Y-up**, origin bottom-left |
| Mouse input | **Y-down** backing pixels, origin top-left |
| Scissor rectangles | **Y-down** attachment pixels (both Vulkan and Metal) |

UI geometry is Y-up because both existing text shaders already map `y = 0` to the
bottom of the target — with opposite NDC signs, since the Vulkan and Metal clip
conventions differ, which is why the two shaders look contradictory and are not.

`vkr_ui_draw_build()` performs the only Y conversion while expanding each
Y-down command rectangle into Y-up vertices. `VkrUiDrawBatch.scissor_rect_px`
stays in Y-down attachment pixels and is passed directly to both native APIs.
This keeps layout, input, clips, and docking in one convention while preserving
the shared text/UI vertex convention.

`RendererFrontend.globals.ui_projection` and `ui_view` are written by
`vkr_ui_system.c` with a Y-down orthographic matrix and are **read nowhere** —
the shaders do their own Y-up mapping. They are deleted rather than left as a
second, contradictory convention.

## 7. Rendering — one stream, scissored batches

### 7.1 Packet contract

The implementation advanced the current packet from version 25 to 26 for the
editor compositor rectangle, then from 26 to 27 for the UI stream. The old
per-draw borrowed-buffer shape is replaced by one list per frame:

```c
typedef struct VkrUiDrawBatch {
  uint32_t first_index;
  uint32_t index_count;
  VkrUiTextureRef texture;      /* id == 0 means untextured */
  VkrUiRect scissor_rect_px;    /* integral Y-down attachment pixels */
  VkrUiDrawMode mode;           /* see below */
  float32_t screen_px_range;    /* MTSDF only */
  Vec2 sdf_unit_range;          /* MTSDF only */
  Vec2 rect_extent_px;          /* rounded rect only */
  Vec4 corner_radius_px;        /* rounded rect only */
} VkrUiDrawBatch;

typedef struct VkrPreparedUiDrawList {
  const VkrUiVertex *vertices; uint32_t vertex_count;
  const uint32_t *indices;       uint32_t index_count;
  const VkrUiDrawBatch *batches; uint32_t batch_count;
} VkrPreparedUiDrawList;
```

Modes: `0` solid or textured quad, `1` MTSDF text, `2` bitmap-alpha text,
`3` rounded rect.

`VkrUiVertex { Vec2 position; Vec2 texcoord; Vec4 color; }` is the canonical
shared, static-asserted 32-byte ABI record; `VkrTextVertex` aliases it for world
text.
**No vertex attributes are added**, because changing that record changes an ABI
validated by reflection on both backends.

One upload per frame. Each batch is one indexed draw differing only in scissor
and root fields. Validation lives alongside `vkr_renderer_validate_text_draws()`
in `renderer_frontend.c` and follows its existing shape: reject malformed
vertex/index ranges, out-of-range `max_index`, non-finite floats, and unknown
modes.

Deliberate omissions:

- **No `object_id`.** GPU picking is asynchronous and request-gated, so it cannot
  drive hover, and the 29-bit picking payload cannot hold hashed
  immediate-mode identities collision-free. UI hit-testing is CPU-side against
  retained rects. `VKR_PICKING_ID_KIND_UI_TEXT` remains for world-space text
  selection and is not extended to widgets.
- **No `revision`.** The field is write-only in `VkrPreparedTextDraw` today, and
  one frame-global revision is strictly weaker than the per-tile hashes of §8.

### 7.2 Backend work

Written twice — Vulkan and Metal.

**Per-batch scissor.** Vulkan issues one `vkCmdSetScissor` per batch through its
existing dynamic state. Metal issues one `setScissorRect` per batch. Both use
the integral CPU-intersected attachment rectangle.

Nested clips are intersected on the CPU into a single rect per batch. **Clipping
is rectangular**: a rounded panel clips its children square. Adding an SDF clip
in the fragment shader is possible later but would also have to enter the tile
hash, so it is out of scope here.

**Pipelines.** Modes 0-2 use the dedicated `ui_vertex`/`ui_fragment` pair. Mode
3 uses `ui_rect_vertex`/`ui_rect_fragment`. Both pairs consume the same vertex
stream and dedicated 64-byte UI root; rounded-rectangle parameters travel in the
batch root because the shared vertex record does not grow.

The untextured case branches on flag bit 0 before any texture access; textured
quads multiply the sampled RGBA value.

**Two native ABI surfaces, one semantic root.** Vulkan and Metal each lower the
same semantic 64-byte UI root. Vulkan validates its static size/offset contract;
Metal also validates every field against native pipeline reflection. UI batches
do not use the 512-byte Vulkan utility root or grow the 176-byte world-text root.

**Frame-upload budget.** The per-slot upload arena is 16 MB and is shared with
world and shadow instances, text geometry, and all roots; exhaustion rejects the
whole frame. A single-list UI upload is strictly better than today's per-draw
uploads, but a large scrolling list can still exhaust it. The UI vertex/index
budget is capped and degrades by dropping trailing batches with one warning —
never by losing the frame.

## 8. Tiling and grid hashing

### 8.1 Binning and hashing

A fixed tile grid covers the UI target. Each draw command's clipped AABB is
binned into every tile it overlaps. Each command receives one FNV-1a
fingerprint over its type, geometry, colour, texture slot, and clip. A tile hash
folds those fingerprints in draw order. Comparing against the previous frame's
hashes yields the dirty-tile damage set without rehashing a large command once
for every tile it covers.

Tile offsets and command indices live with the retained tile cache. When the
ordered command stream and all old/current AABBs are unchanged, the system
reuses those bins and returns a zeroed damage map. Changed frames rebuild the
same ordered bins using scratch counts and command fingerprints, then retain the
result for the next clean frame.

Two correctness requirements a naive implementation misses:

1. **A command that moves from tile A to tile B must dirty both.** Binning
   therefore uses the union of the previous frame's AABB and this frame's, which
   means the retained cache stores last-frame AABBs, not only hashes.
2. **MTSDF text bleeds past its geometric AABB** by the filter footprint. Every
   AABB is dilated by `screen_px_range` before binning, or tile seams show
   one-pixel artefacts.

### 8.2 Shipped use without a cached target

P4 ships 64-pixel bins, deterministic ordered command hashes, old/current AABB
damage, and MTSDF dilation. It exports `ui.dirty_tile_ratio`, `ui.dirty_tiles`,
and `ui.tile_count`. Clean command streams reuse their retained bins; this is a
CPU construction optimization, not pixel caching. The direct path still redraws
the complete UI stream because the swapchain receives fresh scene output every
frame. Batch coalescing remains an ordered-command operation, and hit testing
remains against retained previous-frame widget rectangles. The bins are
therefore an observability and future damage-selection structure, not a claim
that clean swapchain pixels can be preserved.

### 8.3 The cached target was declined

The full scheme renders UI into a persistent `ui_color` image with `LOAD`,
scissored to the damage rects, then composites `ui_color` over the swapchain, so
clean tiles keep their previous pixels.

The renderer gained `RETAINED` graph images and per-image realization under
ADR-029 after this proposal was written. Those facilities supersede the proposed
`PRESERVE_ACROSS_FRAMES` name and the old claim that Metal lacks `PER_IMAGE`.
They make a correct cached target possible, but they do not make it free.

If this decision is revisited, `ui_color` must still be `RETAINED | PER_IMAGE`,
sized by target image count rather than frame-slot count. Damage must accumulate
per acquired image because an image's staleness is not a fixed period:
```
accumulated[image_index] |= dirty_this_frame;
render_damage = accumulated[image_index];
accumulated[image_index] = 0;
```

Resize and viewport-extent changes would force full damage for every instance.
The path would also need a per-frame-variable load operation: full-damage frames
want `CLEAR`, while partial-damage frames want `LOAD`. The authored JSON load
operation is static today.

A single non-`PER_IMAGE` persistent `ui_color` is rejected outright: it is a
write-after-read hazard against in-flight frames.

No `ui_color`, second graph, damage-scissored pass, or composite was added. The
measurement in §8.5 retired that work before it expanded the renderer lifetime
surface.

### 8.4 Selecting the path without touching the condition enum

`VkrRgJsonConditionKind` is closed, and its parser hard-errors on unknown
condition strings — so an older binary reading a newer graph fails to boot, and
adding a condition touches the enum, parser, evaluator,
`VkrRenderGraphFrameInfo`, both backend patch sites, the Metal frame-info
initializer, the authored graph, and the graph barrier test.

If revisited, the tiled path must use a **second authored graph file** selected
through the existing `graph_path` configuration. Each arm would then own
distinct pass names, making a paired comparison unambiguous. No tiled graph file
ships today.

"Always run both passes with an empty damage set" is rejected: it pays the
composite unconditionally, which is exactly the cost under test.

### 8.5 Measurement and decision

The first prescribed Sponza orbit had no UI commands, so it established only the
empty-pass CPU cost. Its local dirty-tree report completed two repetitions and
600 samples, but Metal did not support a GPU timestamp for the empty scope and
warmup was unstable. Report digest:
`sha256:ffde92678be256f9da26cd444d3584b2b2c3c8c81c2119970362cb39217f48c7`.

A corrected static retained-UI workload ran at 2560x1440 on an Apple M1 Pro /
Metal 4 with the exclusive lane, two repetitions, 600 measured frames, and
stable warmup. It covered 920 tiles and settled at zero dirty tiles after
warmup. `UI.Fullscreen` measured 0.017583 ms GPU p50 and 0.019167 ms p95;
the UI pass CPU scope measured 0.009208 ms p50; `cpu.render_submit` measured
0.618791 ms p50. Report digest:
`sha256:04adf82ffb49fec44e980ca338fb413566070de860c51c3be2b96f4e32083bd5`.

The second report is non-authoritative because it uses a local profile and a
dirty tree. It is still the prescribed early rejection gate: adding persistent
per-image storage plus an unconditional full-screen blend to avoid a direct pass
at 0.0176 ms is not justified. P5 was therefore dropped without implementing an
A/B arm. This is a scope decision, not a portable speed claim.

## 9. Editor and docking

`vkr_editor_viewport_compute_mapping()` is implemented on top of the pure
`vkr_editor_viewport_mapping_from_panel_rect(panel_rect_px, fit_mode,
render_scale, out_mapping)` helper. The proportional default helper remains for
focused fixtures; the application uses the dock-owned scene content rectangle.

The dock tree owns the panel rect. Because `scene_color` is
`extent:{mode:viewport}` and `RESIZABLE`, dragging a split resizes the scene
target and the camera re-aspects through the frame packet.

Docking is in-window: there is one window, one surface, one swapchain, and
`RendererFrontend` holds a single `VkrWindow *`. OS-level floating panels are out
of scope.

The dock tree is a binary split tree with tab groups at the leaves, serialized to
JSON so layouts persist. The scene viewport is a leaf of kind
`SCENE_VIEWPORT`, which is what makes it dockable like any other panel.
The shipped representation is bounded to 31 nodes and 8 tabs per leaf. It
supports tab activation, center/edge drops, split resizing, empty-leaf collapse,
validation, a default editor layout, a 16 KiB JSON load bound, and atomic save.
`VKR_EDITOR_LAYOUT_PATH` opts the application into load-at-start and
save-at-shutdown; invalid input falls back to the default tree.

## 10. Input

### 10.1 Capture arbitration

Input is polled from `InputState`. The UI computes one capture record before
camera, picking, gizmo, and application hotkey handling.

`vkr_ui_end()` produces one record consumed by everyone:

```c
typedef struct VkrUiInputCapture {
  bool8_t mouse, keyboard, text;
  uint64_t hot_id, active_id;
} VkrUiInputCapture;
```

`application_update_ui()` is the **first** statement of
`application_update()`, before `application_handle_input()`. Rules:

- Picking early-returns on `capture.mouse && !gizmo_drag.active`. The second
  clause is essential: a drag begun outside the UI must not cancel because the
  cursor crossed a panel.
- Symmetrically, once `active_id != 0` the UI holds the mouse until button-up
  regardless of cursor position. That rule is what makes sliders work.
- The camera controller receives an `input_blocked` flag rather than being
  skipped, so its smoothing state does not snap on release.
- `KEY_Q`/`KEY_E` are handled in `application_update()` outside
  `application_handle_input()` and need the same guard.
- `KEY_TAB` is the mouse-capture toggle and collides with focus traversal; the UI
  gets first refusal only when `capture.keyboard`.
- **Nothing consumes events.** Edge detection queries `InputState`, which the UI
  does not mutate. Capture is decided once per frame and read by all consumers;
  otherwise a single click is observed twice in one frame.
- Hover picking can leave a pending request when capture transitions false→true;
  cancel it explicitly.
- UI hit-testing ignores the mouse entirely while `vkr_window_is_mouse_captured()`
  is true, because macOS integrates unclamped virtual deltas in that mode and
  would otherwise produce phantom hovers.

### 10.2 Character input and repeat

- `InputState` owns a fixed 64-codepoint committed-character queue populated by
  `input_process_char()`. It clears at the frame boundary and counts dropped
  characters without allocating.
- Windows lowers `WM_CHAR`, including UTF-16 surrogate pairs. macOS lowers
  committed text from its `NSTextInputClient` implementation into Unicode
  scalars.
- Text fields insert UTF-8, replace selections, and support Backspace, Delete,
  Left, Right, Home, and End. The platform-independent repeat policy waits 0.4 s
  and repeats every 0.05 s.
- **IME is out of scope.** Composition strings, candidate windows, and
  preedit rendering are a separate body of work and are not specified here.

## 11. Phasing

Each phase is independently shippable with its own evidence gate. Per AGENTS.md,
validation runs are never mixed into performance commands, and Metal shader
validation stays confined to minimal focused reproductions.

| Phase | Result | Evidence boundary |
|---|---|---|
| **P0** | Implemented in both backends: editor condition and viewport extent reach graph realization | CPU suite, editor `scene_color`/final capture, focused Metal validation. Native Vulkan runtime validation remains open |
| **P0b** | Implemented: attachment extents drive screen-space roots, picking, and compositor placement | Fullscreen text and editor compositor Release captures |
| **P1** | Implemented: IDs, grid, style, draw commands, shared vertex, and content scale | `ui_layout_test.c` covers track units, bounded `fr` iteration, placement, clipping, style, and truncation |
| **P2** | Implemented in packet v27: one draw list, per-batch scissor, two UI pipelines, 64-byte native roots, exact scratch allocation, and retirement of the 16-slot UI text API | CPU suite, Debug/Release builds, Release text capture, and one-process Metal API/GPU validation. Vulkan compiles; native execution remains open |
| **P3** | Implemented: panel, label, button, checkbox, slider, scroll area, text field, retained state, incremental layout, and capture arbitration | UI/text/input tests plus application integration |
| **P4** | Implemented: CPU tile binning and hashing with dirty metrics; direct rendering remains complete-frame | Deterministic tile tests and the two local profiles in §8.5 |
| **P5** | Declined by the prescribed early measurement; no cached target or second graph ships | §8.5. No speedup claim is made |
| **P6** | Implemented: bounded dock tree, tab/split interaction, scene-panel mapping, validation, and atomic JSON persistence | Dock mutation, collapse, validation, mapping, and round-trip tests |
| **P7** | Implemented: committed Unicode queue, Windows/macOS lowering, editing, and repeat | Input and text-field tests with the production event/input state |

### Note on the P2 retirement

The world-text injection sites in `renderer_frontend.c` use one helper with a
count-only mode (`out_draws == NULL` returns the required count), so the retired
`VKR_PREPARED_TEXT_DRAW_MAX` stack arrays become exact scratch allocations.

The `text_draw_count == 0` fallback to `packet->world->text_draws` must be
preserved verbatim — it is what lets the harness text fixture inject draws
through the packet. On allocation failure the count falls back to zero and the
packet's own list, never a partial list.

The helper refactor preserves world-text payload semantics. The aggregate text
snapshot changed intentionally because retained UI MTSDF text entered the same
case, so its current digest is recorded as a new execution witness rather than
claimed byte-identical to the pre-UI frame.

`VkrUiTextSlot` retirement is itself a packet-contract change:
`VKR_UI_SYSTEM_MAX_TEXTS`, the anchor/padding model, the
`vkr_ui_system_text_create/update/destroy` API, `VkrTextUpdatesPayload`, the
application's text ids, and the harness text fixture all move together — so it
happened in packet v27. `VkrTextUpdatesPayload` now carries world-text updates
only; retained UI widgets build their geometry directly.

## 12. Deferred and revisit questions

- Whether rounded-panel children should eventually clip to an SDF rather than a
  rectangle (§7.2), which would require the clip to enter the tile hash.
- Whether a future workload can justify reopening the cached-target decision.
  It requires new evidence that overcomes the direct path's measured local
  0.0176 ms GPU p50, not only a low dirty-tile ratio.
- Native Vulkan runtime validation and a crossed same-revision UI/text capture
  remain required before the new shader row can become `ALIGNED`.
- IME composition, candidate windows, and preedit rendering remain out of scope.
