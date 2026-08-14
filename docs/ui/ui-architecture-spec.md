---
status: proposed
updated: 2026-08-12
authority: design
---
# UI Architecture Specification

Authoritative design for the VKR user-interface system covering both editor and
in-game UI. Supersedes [ui-system-overview.md](../archive/ui-system-overview.md),
[ui-layout-engine-design.md](../archive/ui-layout-engine-design.md), and
[ui-element-primitives-design.md](../archive/ui-element-primitives-design.md).
Rationale is recorded in
[ADR-027](../architecture/adr/027-immediate-mode-grid-ui.md).

`status: proposed` — no production code implements this document. The existing
`VkrUiSystem` is a 16-slot corner-anchored text array and is not an
implementation of anything below.

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

Section 8 records the correctness hazards in the tile-cached rendering half and
the measurement that must precede shipping it. Section 2 records a status defect
that must be fixed before decision 1 has any implementation behind it.

## 2. Prerequisite: the editor graph path is not plumbed

`assets/render_graphs/main.rendergraph.json` declares the full editor topology,
conditioned on `editor_enabled`: `scene_color`, `scene_depth` and
`scene_pre_transmission` at `extent:{mode:viewport}`, the `Editor.Composite`
pass, and `UI.Editor`.

`VkrRenderGraphFrameInfo.editor_enabled` is **never assigned** in either backend.
`vkr_bindless_vulkan_renderer_submit_packet()` and the Metal equivalent in
`vkr_metal_packet_frame.inc` patch only `picking_pending` and
`shadow_cascade_count` into `prepared_frame` before `vkr_rg_begin_frame()` — the
Metal site under a comment reading "Patch packet-derived graph conditions here,
immediately before graph build". `packet.frame.editor_enabled` reaches only
capture-channel selection in `vkr_bindless_vulkan_capture.c` and
`vkr_metal_packet_graph.inc`.

Consequences, all derivable from the authored graph:

- `scene_color`, `scene_depth`, `scene_pre_transmission` are never created.
- `Editor.Composite` and `UI.Editor` are never built.
- `UI.Fullscreen`, conditioned on `!editor_enabled`, always builds.
- The editor snapshot case requests a `scene_color` capture channel that is not
  in the built graph.

`prepared_frame.viewport_width/height` is likewise initialized to the window
size and never updated from `packet.frame.viewport_*`, so
`extent:{mode:viewport}` resolves to window extent. The packet's viewport values
reach only shader constants.

Phase P0 below plumbs both. Until it lands, decision 1 is a design statement with
no running code, and the architecture spec's §4 "Editor viewport and picking"
row is qualified accordingly.

## 3. Layering and ownership

```
lib/src/core/ui/          pure CPU, no renderer dependencies, unit-testable
  vkr_ui_id.h/.c          ID stack and stable hashed widget identity
  vkr_ui_grid.h/.c        grid track solver
  vkr_ui_style.h          style and box model; constants in logical points
  vkr_ui_draw.h/.c        draw-command buffer to vertices/indices/batches
  vkr_ui_tile.h/.c        tile binning, per-tile hashing, damage set

lib/src/renderer/systems/vkr_ui_system.c
                          retained node arena; builds the packet payload from
                          per-frame scratch; injected at submit time
```

The `core/ui/` split exists so the grid solver, ID stack, and tile hashing are
testable under `./build_test.sh` with no GPU and no renderer state.

**Allocator discipline** (see `.codex/skills/vkr-memory/SKILL.md`):

| Data | Allocator | Reason |
|---|---|---|
| Per-widget retained state, per-tile hashes, last-frame AABBs | `VkrDMemory` | Hash-table keys and individually-freed entries; must not live in an arena |
| Per-frame vertices, indices, batches, tile bins | `renderer.scratch_allocator` | The main loop already opens a scope around the whole frame in `application.h`; nothing is freed individually |

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
from the previous frame reuses its resolved rects and is not re-measured. This is
the primary payoff of the retained cache, and it is independent of the tile
rendering in §8.

### 5.4 Units and DPI

**All style constants are authored in logical points** and multiplied by
`content_scale` at resolve time. This is not deferrable:

- Engine coordinates are backing pixels — macOS window code returns
  `convertRectToBacking` results and scales mouse positions by
  `layer.contentsScale`.
- Windows has no `SetProcessDpiAwarenessContext` or `GetDpiForWindow` anywhere in
  the tree, so at 150% scaling the process is DPI-virtualized.

The existing hardcoded editor gutters in `vkr_editor_viewport.c` (`max(32, …)`,
`max(220, …)`) are already effectively half-size on a Retina display, which is
the concrete symptom. `content_scale` is added to `VkrWindow` and consumed by the
UI context.

## 6. Coordinate conventions

Three conventions are in play and the spec fixes each one:

| Space | Convention |
|---|---|
| UI geometry (vertex positions) | **Y-up**, origin bottom-left |
| Mouse input | **Y-down** backing pixels, origin top-left |
| Scissor rectangles | **Y-down** attachment pixels (both Vulkan and Metal) |

UI geometry is Y-up because both existing text shaders already map `y = 0` to the
bottom of the target — with opposite NDC signs, since the Vulkan and Metal clip
conventions differ, which is why the two shaders look contradictory and are not.

`VkrUiDrawBatch.scissor_rect_px` is defined as **Y-down attachment pixels**, and
the frontend performs the single conversion `scissor.y = attachment_height -
(rect.y + rect.h)` where the true attachment extent is known.

`RendererFrontend.globals.ui_projection` and `ui_view` are written by
`vkr_ui_system.c` with a Y-down orthographic matrix and are **read nowhere** —
the shaders do their own Y-up mapping. They are deleted rather than left as a
second, contradictory convention.

## 7. Rendering — one stream, scissored batches

### 7.1 Packet contract

Packet version bumps 10 → 11. The per-draw borrowed-buffer shape is replaced by
one list per frame:

```c
typedef struct VkrUiDrawBatch {
  uint32_t first_index;
  uint32_t index_count;
  VkrTextureHandle texture;     /* id == 0 means untextured */
  Vec4 scissor_rect_px;         /* Y-down attachment pixels */
  uint32_t mode;                /* see below */
  float32_t screen_px_range;    /* MTSDF only */
} VkrUiDrawBatch;

typedef struct VkrPreparedUiDrawList {
  const VkrTextVertex *vertices; uint32_t vertex_count;
  const uint32_t *indices;       uint32_t index_count;
  const VkrUiDrawBatch *batches; uint32_t batch_count;
} VkrPreparedUiDrawList;
```

Modes: `0` solid or textured quad, `1` MTSDF text, `2` bitmap-alpha text,
`3` rounded rect.

Vertices reuse `VkrTextVertex { Vec2 position; Vec2 texcoord; Vec4 color; }` — a
shared, static-asserted ABI record in `vkr_gpu_abi` that world text also uses.
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

Written twice — bindless Vulkan and Metal.

**Per-batch scissor.** Scissor is currently set once to the full render area.
Vulkan already declares `VK_DYNAMIC_STATE_SCISSOR`, so this is one
`vkCmdSetScissor` per batch. Metal has no `setScissorRect` call anywhere in the
tree, so this is new API surface there rather than a parity change.

Nested clips are intersected on the CPU into a single rect per batch. **Clipping
is rectangular**: a rounded panel clips its children square. Adding an SDF clip
in the fragment shader is possible later but would also have to enter the tile
hash, so it is out of scope here.

**Pipelines.** Modes 0–2 reuse the existing `text_vertex`/`text_fragment` entry
points, which already do screen-space projection and read vertices through a GPU
pointer.

Mode 3 requires **its own vertex/fragment pair** (`ui_rect_vertex` /
`ui_rect_fragment`). The forcing constraint is the vertex stage, not blend state
or formats: a rounded-rect SDF needs rect-local coordinates, and `texcoord` is
already taken by the atlas. Since `VkrTextVertex` cannot grow, the parameters
travel in the batch root.

The untextured case must branch or bind a white texture — `text_alpha`
unconditionally samples `g_textures[...]`.

**Two ABI surfaces, not one.** The 512-byte Vulkan utility root is not
reflection-validated and has ample unused fields. Metal's
`VkrMetalPacketTextRoot` is a *different* 176-byte struct that **is**
field-by-field reflection-validated, so growing it touches the header, the
`.metal` source, the ABI field table, and a size assertion. Budget both.

**A dedicated small UI root.** `vkr_bindless_vk_packet_utility_root()` allocates
and zeroes 512 bytes per draw. Hundreds of UI batches × 512 B of `MemZero` per
frame is precisely the per-draw cost AGENTS.md classifies as a defect. UI batches
use a 32–64 byte root instead.

**Frame-upload budget.** The per-slot upload arena is 16 MB and is shared with
world and shadow instances, text geometry, and all roots; exhaustion rejects the
whole frame. A single-list UI upload is strictly better than today's per-draw
uploads, but a large scrolling list can still exhaust it. The UI vertex/index
budget is capped and degrades by dropping trailing batches with one warning —
never by losing the frame.

## 8. Tiling and grid hashing

### 8.1 Binning and hashing

A fixed tile grid covers the UI target. Each draw command's clipped AABB is
binned into every tile it overlaps, from per-frame scratch. Each tile receives an
FNV-1a hash over its ordered command contents (type, geometry, colour, texture
slot, clip). Comparing against the previous frame's hashes yields the dirty-tile
damage set.

Two correctness requirements a naive implementation misses:

1. **A command that moves from tile A to tile B must dirty both.** Binning
   therefore uses the union of the previous frame's AABB and this frame's, which
   means the retained cache stores last-frame AABBs, not only hashes.
2. **MTSDF text bleeds past its geometric AABB** by the filter footprint. Every
   AABB is dilated by `screen_px_range` before binning, or tile seams show
   one-pixel artefacts.

### 8.2 Uses that do not depend on a cached target

Tile bins independently provide hit-test acceleration (candidate widgets under
the cursor in one tile rather than a full tree walk), batch grouping, and the
ability to skip the UI pass entirely when no tile is dirty. These are available
from §8.1 alone and are not contingent on §8.3.

### 8.3 The cached target, and why it is gated

The full scheme renders UI into a persistent `ui_color` image with `LOAD`,
scissored to the damage rects, then composites `ui_color` over the swapchain, so
clean tiles keep their previous pixels.

**As the graph stands this is incorrect on Vulkan.** `vkr_rg_compile.c` re-seeds
every non-imported graph image to `UNDEFINED` at the start of each frame, so the
pre-barrier is `UNDEFINED → COLOR_ATTACHMENT_OPTIMAL`, which the specification
explicitly permits to discard contents; `LOAD_OP_LOAD` then reads garbage.
`VKR_RG_RESOURCE_FLAG_TRANSIENT` describes the *allocation*, not the contents.

Metal has no image layouts and reuses the same `MTLTexture`, so contents survive
naturally. **The scheme would therefore look correct on macOS and be wrong on
Windows** — the worst available failure mode, and the reason this section is
explicit rather than left to implementation.

Metal additionally does not implement `PER_IMAGE` at all: `realize_images`
creates one `MTLTexture` per graph image regardless of the flag.

Prerequisites before any `ui_color` work:

1. A `PRESERVE_ACROSS_FRAMES` image flag seeded from the previously computed
   `final_layout`, plus a **per-instance `initialized` bit** in the backend graph
   image, so an instance that has not yet completed a frame forces full damage.
2. `ui_color` is `PER_IMAGE`, sized by **target image count**, not by the three
   frame slots. An image's staleness is "since that image was last acquired",
   which under FIFO or MAILBOX is not a fixed period. Damage therefore
   accumulates per image index in the CPU-side retained cache and assumes no
   period:
   ```
   accumulated[image_index] |= dirty_this_frame;
   render_damage = accumulated[image_index];
   accumulated[image_index] = 0;
   ```
   This needs no graph change.
3. Invalidation is driven from the frontend on resize and on any viewport-extent
   change — all-damage for every instance, all tile hashes cleared — rather than
   by detecting the graph's destroy/recreate.
4. A **per-frame-variable load op**, which the JSON graph cannot express: `load`
   is parsed statically. Full-damage frames want `CLEAR` (which sidesteps the
   undefined-contents question entirely); partial-damage frames want `LOAD`.
5. `PER_IMAGE` implemented on Metal, or the tile cache does not ship on Metal. A
   single texture written while a prior frame reads it is a real hazard.

A single non-`PER_IMAGE` persistent `ui_color` is rejected outright: it is a
write-after-read hazard against in-flight frames.

### 8.4 Selecting the path without touching the condition enum

`VkrRgJsonConditionKind` is closed, and its parser hard-errors on unknown
condition strings — so an older binary reading a newer graph fails to boot, and
adding a condition touches the enum, parser, evaluator,
`VkrRenderGraphFrameInfo`, both backend patch sites, the Metal frame-info
initializer, the authored graph, and the graph barrier test.

Instead, the tiled path ships as a **second authored graph file**,
`assets/render_graphs/main.ui_tiled.rendergraph.json`, selected through the
existing `graph_path` configuration (currently hardcoded at three literals in
`renderer_frontend.c`). Each arm then owns distinct pass names, which makes the
per-pass comparison in §8.5 unambiguous.

"Always run both passes with an empty damage set" is rejected: it pays the
composite unconditionally, which is exactly the cost under test.

### 8.5 Measurement

No claim about the speed of either path appears in this document. Per AGENTS.md
an unmeasured performance claim is not a result. The order of work is prescribed.

**First, one run against the current build.** `sponza_orbit.case.json` under
`performance-windowed-gpu.json`; read `UI.Fullscreen`'s `gpu_ms` p50 from
`report.json` `passes[]` — not `summary.csv`, which emits metric rows only. If it
is already at the noise floor, a scheme that adds an image, a pass, and an
unconditional full-screen alpha blend cannot pay for itself, and §8.3 is dropped
for the cost of one harness run.

**If it survives, the A/B.** A new `ui_editor_static.case.json` with a static
camera so world cost is constant, in **two mandatory variants**: one where a
large fraction of the UI changes per frame, one where only an FPS counter changes
(≈1 dirty tile). One variant cannot settle this, because the composite is fixed
cost while the saving is proportional to the clean fraction.

| Arm | Metric |
|---|---|
| A (direct) | `UI.Fullscreen` `gpu_ms.p50` |
| B (tiled) | `UI.Draw` + `UI.Composite` `gpu_ms.p50` |
| Both | `cpu.render_submit` p50 — binning and hashing are CPU work paid every frame in the submit path |
| Both | exported dirty-tile ratio, without which the result is uninterpretable |

The two arms have different workload fingerprints by construction. That is an
honest paired comparison of two renderer configurations, not a baseline
comparison; equality of `draw.world.calls_issued` and `visibility.objects_tested`
proves the scene workload matches. Five repetitions, exclusive lane. The tiled
arm ships only on non-overlapping ranges across **all** repetitions in **both**
variants. Overlapping ranges mean no measured difference, which for a scheme that
adds an image, a pass, and a full-screen blend is a reason not to ship.

A `renderer.ui_tile_cache` harness key touches the closed allowlist in
`vkr_harness_manifest.c`, plus `vkr_harness.h`, `vkr_harness_child.c`,
`vkr_harness_fingerprint.c`, and `vkr_harness_report.c`.

## 9. Editor and docking

`vkr_editor_viewport_compute_mapping()` splits into a pure
`vkr_editor_viewport_mapping_from_panel_rect(panel_rect_px, fit_mode,
render_scale, out_mapping)`, with the current hardcoded-gutter function retained
as a default-layout convenience implemented on top of it.

The dock tree then owns the panel rect. Because `scene_color` is
`extent:{mode:viewport}` and `RESIZABLE`, dragging a split resizes the scene
target once §2 is plumbed, and the camera re-aspects through the path already in
`application.h`.

Docking is in-window: there is one window, one surface, one swapchain, and
`RendererFrontend` holds a single `VkrWindow *`. OS-level floating panels are out
of scope.

The dock tree is a binary split tree with tab groups at the leaves, serialized to
JSON so layouts persist. The scene viewport is a leaf of kind
`SCENE_VIEWPORT`, which is what makes it dockable like any other panel.

## 10. Input

### 10.1 Capture arbitration

Input is polled from `InputState`; the event handlers subscribed in
`application_create()` are effectively no-ops, and the camera controller,
picking, and gizmo drag currently hand-arbitrate with ad-hoc button checks.

`vkr_ui_end()` produces one record consumed by everyone:

```c
typedef struct VkrUiInputCapture {
  bool8_t mouse, keyboard, text;
  uint64_t hot_id, active_id;
} VkrUiInputCapture;
```

`application_update_ui()` becomes the **first** statement of
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

### 10.2 Missing platform capabilities

- **Character input does not exist.** `EVENT_TYPE_KEY_PRESS` carries a
  virtual-key code; there is no `WM_CHAR` or `NSTextInputClient` path. Text
  fields require `input_process_char(uint32_t codepoint)` plus platform hooks in
  `vkr_window_windows.c` and `vkr_window_macos.m`.
- **Key repeat does not exist.** `input_process_key` deduplicates on state
  change, so a held key produces one event. Repeat timing is implemented in the
  UI, platform-independently, rather than plumbed through the platform layer.
- **IME is out of scope.** Composition strings, candidate windows, and
  preedit rendering are a separate body of work and are not specified here.

## 11. Phasing

Each phase is independently shippable with its own evidence gate. Per AGENTS.md,
validation runs are never mixed into performance commands, and Metal shader
validation stays confined to minimal focused reproductions.

| Phase | Content | Gate |
|---|---|---|
| **P0** | Plumb `editor_enabled` and `viewport_*` from the packet into `VkrRenderGraphFrameInfo` in both backends (§2) | `./build_test.sh`; editor snapshot resolving `scene_color`; **Debug validation-layer run on both backends** — this enables three never-built passes and therefore a new barrier surface |
| **P0b** | Use the real attachment extent for text and picking draws instead of the configured window size | Text snapshot digest unchanged in fullscreen; editor snapshot correct |
| **P1** | `lib/src/core/ui/` — IDs, grid solver, style, draw-command buffer, content scale. Pure CPU, not wired up | `./build_test.sh` plus `tests/src/ui_layout_test.c` registered in `tests/CMakeLists.txt` and `test_main.c`; pins the `fr`+`minmax` iteration bound |
| **P2** | Packet v11, single draw list, per-batch scissor, `ui_rect_*` shaders, dedicated UI root, both backends. Retires the 16-slot `VkrUiTextSlot` array and `VKR_PREPARED_TEXT_DRAW_MAX` in the same bump | `./build_test.sh`; text snapshot digest changes intentionally, plus a screenshot as AGENTS.md requires for UI changes; Debug validation both backends; record `UI.Fullscreen.gpu_ms` and `cpu.render_submit` as the baseline later phases must beat |
| **P3** | Widgets — panel, label, button, checkbox, slider, scroll area, text field — and input capture arbitration (§10.1) | `./build_test.sh`, `tests/src/input_test.c` extension, interactive verification |
| **P4** | Tile binning and hashing, CPU only. Still draws everything; exports the dirty-tile ratio | `./build_test.sh` plus a harness run showing hashing cost in `cpu.render_submit` and the observed ratio. **If the ratio is not consistently low for a realistic editor UI, stop here** |
| **P5** | `PRESERVE_ACROSS_FRAMES`, Metal `PER_IMAGE`, `ui_color` + damage + composite behind the second graph file | The A/B of §8.5, plus a **mandatory** Vulkan validation run — the cross-frame layout carry is the likeliest source of validation errors in this plan |
| **P6** | Docking — dock tree, drag and drop, tab groups, JSON layout persistence | Interactive verification, layout round-trip test |
| **P7** | Character input and key repeat (§10.2) | `input_test.c`, interactive verification |

### Note on the P2 retirement

The two text-injection sites in `renderer_frontend.c` are byte-identical modulo
`false_v` versus `0`; they collapse into one helper with a count-only mode
(`out_draws == NULL` returns the required count) so the fixed
`VKR_PREPARED_TEXT_DRAW_MAX` stack arrays become exact scratch allocations.

The `text_draw_count == 0` fallback to `packet->world->text_draws` must be
preserved verbatim — it is what lets the harness text fixture inject draws
through the packet. On allocation failure the count falls back to zero and the
packet's own list, never a partial list.

Because this is a pure refactor, it has a strict cheap proof: the text snapshot
digest must be **byte-identical** before and after.

`VkrUiTextSlot` retirement is itself a packet-contract change —
`VKR_UI_SYSTEM_MAX_TEXTS`, the anchor/padding model, the
`vkr_ui_system_text_create/update/destroy` API, `VkrTextUpdatesPayload`, the
application's text ids, and the harness text fixture all move together — so it
happens inside the same version bump rather than causing a second one.

## 12. Open questions

- Whether rounded-panel children should eventually clip to an SDF rather than a
  rectangle (§7.2), which would require the clip to enter the tile hash.
- Whether scroll containers erode the tile-cache thesis enough to matter: a
  scrolled panel dirties every tile it covers, and scrolling is a common editor
  interaction. P4's exported ratio is what answers this.
- Whether the `PRESERVE_ACROSS_FRAMES` flag generalizes usefully beyond
  `ui_color`, or should stay a narrow capability.
