---
status: investigation
updated: 2026-07-31
authority: investigation
---

> **Archived.** Superseded by [`../editor/editor-viewport-and-picking-design.md`](../editor/editor-viewport-and-picking-design.md). Retained for history; do not treat as current.
# World-space (3D) text picking: investigation + fix options

## Context

The renderer uses **GPU picking** by rendering per-object IDs into an offscreen
`R32_UINT` texture (`Renderpass.Builtin.Picking`) and reading back a pixel under
the cursor (plus optional probe pixels).

2D UI text picking works, but **3D world text picking** does not: when hovering
visible world text, the pick result often returns the **scene mesh behind the
text**, ignoring the text.

This document consolidates the runtime evidence gathered in `.cursor/debug.log`
and outlines the most likely root causes + concrete solution paths.

## Resolution (what actually fixed it)

- Split picking text pipelines: world text uses `VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT`,
  UI text stays on `VKR_PIPELINE_DOMAIN_POST`.
- Draw world text picking first, UI picking last to match visible composition.
- Result: world text respects scene depth, and UI text no longer overwrites world text IDs.

## Current picking pipeline structure (as implemented)

- **Picking renderpass**: `Renderpass.Builtin.Picking`
  - Color: `R32_UINT`
  - Depth: enabled + cleared
- **Mesh picking**: `assets/shaders/picking.slang` → `picking.spv`
  - Writes `object_id` directly to `R32_UINT`
  - Pipeline domain: `VKR_PIPELINE_DOMAIN_PICKING` (depth test/write enabled)
- **Text picking (UI + World)**: `assets/shaders/picking_text.slang` → `picking_text.spv`
  - Writes `object_id` directly to `R32_UINT`
  - Pipeline domain currently: `VKR_PIPELINE_DOMAIN_POST` (depth test/write disabled)
  - Used by both:
    - `vkr_world_resources_render_picking_text()`
    - `vkr_ui_system_render_picking_text()`

Picking pass render order in `vkr_picking_render()`:

1. Draw meshes (picking pipeline)
2. Draw world text (picking_text pipeline)
3. Draw UI text (picking_text pipeline)
4. End picking render pass
5. Request pixel readback(s)

## Key runtime evidence (high-signal)

### E1 — Picking text pipeline is created and compatible with picking renderpass

From logs:

- **H17**: `Picking text pipeline meta` shows domain `3` (POST) and a renderpass pointer.
- **H18**: `Picking pass + pipeline renderpass compatibility` shows both
  `picking_text_pipeline_renderpass_ptr` and `picking_pipeline_renderpass_ptr`
  match the active `picking_pass_ptr`.

This rules out “pipeline built against the wrong renderpass” as the reason for
text not appearing in the picking texture.

### E2 — World-text picking draw call is executed (buffers + draw call happen)

From logs:

- **H25**: `World pick pre-draw pipeline+buffers` includes non-null VB/IB handles,
  non-zero `quad_count` and `index_count`.
- **H2**: `World picking text submitted` reports `rendered_slots=1` for world text.

This rules out “world text is never submitted to picking”.

### E3 — World text can write into the picking texture (forced full-screen test)

The strongest proof came from a shader-side isolation test:

- **H29 (test)**: modified `picking_text.slang` so WORLD_TEXT emits a full-screen triangle
  (independent of geometry / transforms) and `fragmentMain` outputs a WORLD_TEXT-tagged
  constant ID.
- **H22** then consistently showed `decoded_kind:2` for cursor and probe samples.

This confirms:

- the picking texture is being rendered into,
- readback works,
- the WORLD_TEXT path in `picking_text` is functional,
- the “missing world text” is not caused by renderpass incompatibility.

### E4 — In the *normal* geometry path, cursor samples return SCENE IDs

After reverting the full-screen override and using normal vertex transforms again,
cursor samples returned SCENE IDs:

- **H22**: `Pick readback sample READY` shows `decoded_kind:0` (SCENE) under the cursor
  while hovering where world text is expected.

This is the symptom: world text is not “winning” the pixel in the pick buffer.

### E5 — Transparent materials can corrupt picking (alpha not respected)

Picking for meshes does **not** sample alpha; any “cutout”/transparent area still
writes depth and object ID.

Evidence:

- **H12**: winning SCENE mesh reported `transparent_submesh_count:35`.

Mitigation attempt:

- **H13**: picking pass skipped transparent submeshes (count logged), but the world
  text issue persisted. This suggests transparency is a contributor (and is still a
  real correctness problem) but not the sole root cause.

### E6 — Picking and world render use the same MVP math for the probed glyph center

Instrumentation compared the NDC of the first glyph center in:

- **H28**: world pick path (computed inside `vkr_world_resources_render_picking_text()`)
- **H30**: world render path (computed inside `vkr_text_3d_draw()`)

In multiple samples, **H28 and H30 match** for the same glyph center. That reduces
the likelihood that the picking pass uses stale/different view/projection/model than
the visible render.

## What’s most likely happening (based on evidence)

We can state with high confidence:

- World text picking **can** write to the picking texture (E3).
- In the real geometry path, it **does not win** the cursor pixel (E4),
  despite the draw executing (E2).

Therefore the failure is almost certainly due to one (or a combination) of:

1. **Visibility mismatch between “what you see” and “what picking thinks is in front”**
   - Example: world text is rendered without depth test (or with special ordering) in
     the visible pass, so it appears on top of geometry, but in picking the underlying
     geometry still “wins”.
   - This can be amplified by transparent/cutout geometry (E5).

2. **Geometry coverage mismatch between the world text glyphs and sampled cursor pixels**
   - The draw exists (E2), but doesn’t cover the sampled pixel where the user expects.
   - This can still happen even if the text is visible if coordinate mapping for picking
     is subtly wrong, or if the visible world text and the probed slot are not the same
     (multiple world texts).

3. **Depth-related interference despite intending “depth off” for picking_text**
   - The code sets picking_text pipeline domain to POST (depth off), but if an unexpected
     depth state ends up active (or a separate pass/state write happens), meshes can win.
   - The H29 “force z / fullscreen” test was compatible with a depth-related failure mode
     (but does not prove it alone).

4. **UI text vs World text overlap in the picking target (single-layer ID buffer)**
   - The picking target stores only **one** `uint32_t` per pixel.
   - If **UI text and world text overlap in screen space**, whichever one is drawn
     **later** in the picking pass will overwrite the earlier ID at that pixel.
   - This can look like “UI text works but world text doesn’t” (or vice-versa) depending
     purely on draw order, even when both are correctly rendered into the picking target.
   - In that case, this is not a shader/pipeline bug; it’s an inherent “single topmost ID”
     limitation. The fix is to match the *visible* layering order (typically: World → UI),
     or to implement multi-layer/priority picking (see Options below).

## Recommended fix path (practical + correct)

### Option A (recommended): Make mesh picking respect alpha for cutout/transparent materials

**Goal**: picking should match what’s actually visible per pixel.

Approach:

- For meshes with alpha/cutout textures, in the picking shader (`picking.slang`)
  sample the diffuse alpha (or mask) and **discard** when alpha < threshold.
- This requires binding the relevant material texture(s) for picking, and a consistent
  “alpha cutoff” convention (or treat “has transparency” as “needs alpha test”).

Pros:

- Correct picking for *all* objects, not just text.
- Removes “invisible triangles occlude everything behind” (E5), which is a fundamental
  correctness issue for picking.

Cons:

- More expensive (texture sampling in picking pass).
- Requires plumbing material textures into picking pipeline (descriptor set layout etc.).

### Option B: Treat world text as a true overlay in picking (match visible text if it’s rendered on top)

If world text is intentionally rendered without depth test in the visible render,
then picking should follow the same rule:

- Ensure world text in picking is drawn last and **always wins**.
- Keep picking_text depth test off (POST-like depth state), but ensure it is actually
  applied, and that no later draw overwrites it.

This path is simpler but may allow picking text “through” geometry if that geometry
is meant to occlude text.

### Option C: Depth-correct world text picking (if world text is meant to be occluded)

If world text is meant to be occluded by geometry:

- Use depth test **enabled** but depth write **disabled** for world text picking.
- This makes world text win only when it is closer than geometry at that pixel.

This needs a pipeline state variant that is not currently expressible via domains
without adding a new pipeline domain or configurable depth flags.

### Option D: Priority picking for overlapping UI vs World text

If UI and world text can overlap and you want predictable results:

- **Match visible composition**: render world text first, then UI text in the picking pass.
  - Pros: matches “what you see is what you pick”.
  - Cons: you cannot pick world text through UI overlays at the same pixel (by design).

If you *do* want to pick world text even when UI overlaps it, you need a multi-step or
multi-layer approach, for example:

- **Two-step picking**: pick UI first; if no UI hit, pick world/scene.
- **Separate picking targets**: one texture for UI IDs, one for world/scene IDs, read UI
  first then fall back.
- **CPU pre-test for UI**: do a cheap UI hit test on the CPU and only request a GPU
  picking readback for the world/scene when UI is not hit.

## Verification plan (for whichever option is chosen)

1. Create a minimal scene with:
   - a mesh with a cutout/transparent texture (holes)
   - a world text label behind and in front of that mesh
2. Picking pass correctness:
   - Hover over visible glyphs and confirm WORLD_TEXT ID returns
   - Hover over cutout holes (where you see through) and confirm the object behind wins
3. Keep the existing instrumentation until verified:
   - **H22** should show `decoded_kind:2` when cursor is over world text
   - **H12/H13** should confirm whether transparent submeshes are no longer falsely “winning”

## Notes / cleanup

This investigation introduced multiple debug logs and temporary shader modifications
to isolate rendering vs readback failure modes. After a real fix is implemented and
verified, those debug logs should be removed to keep code clean.

## Final fix (implemented)

World and UI text were previously rendered into the same picking target using the same
`picking_text` pipeline configured as `VKR_PIPELINE_DOMAIN_POST` (no depth test). That made
results order-dependent: whichever text draw happened last would overwrite the ID buffer,
and world text could “steal” picks from UI.

The fix was:

- **Split picking text pipelines**:
  - **UI picking text** keeps `VKR_PIPELINE_DOMAIN_POST` (overlay behavior).
  - **World picking text** uses a new pipeline in `VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT`
    (depth test enabled, depth write disabled) so world text respects scene depth.
- **Match visible composition** in the picking pass: render **world picking text first**,
  then render **UI picking text last**.

Implementation: `lib/src/renderer/systems/vkr_picking_system.c` and
`lib/src/renderer/systems/vkr_picking_system.h`.

### DO NOT REMOVE (exact locations of the fix)

If you’re doing cleanup and want to avoid accidentally breaking picking again, keep these
exact code blocks:

- **New context field (world text pipeline handle)**:
  - `lib/src/renderer/systems/vkr_picking_system.h` **L72–L76**
    - Field: `picking_world_text_pipeline`

- **Pipeline creation (two text pipelines)**:
  - `lib/src/renderer/systems/vkr_picking_system.c` **L383–L433**
    - Creates:
      - UI text picking pipeline: `VKR_PIPELINE_DOMAIN_POST` → `ctx->picking_text_pipeline`
      - World text picking pipeline: `VKR_PIPELINE_DOMAIN_PICKING_TRANSPARENT` + name
        `picking_world_text` → `ctx->picking_world_text_pipeline`

- **Picking pass draw order (world first, UI last)**:
  - `lib/src/renderer/systems/vkr_picking_system.c` **L795–L798**
    - Calls:
      - `vkr_world_resources_render_picking_text(rf, ctx->picking_world_text_pipeline);`
      - `vkr_ui_system_render_picking_text(rf, ctx->picking_text_pipeline);`

- **Shutdown releases the new pipeline**:
  - `lib/src/renderer/systems/vkr_picking_system.c` **L944–L947**
    - Releases: `ctx->picking_world_text_pipeline`
