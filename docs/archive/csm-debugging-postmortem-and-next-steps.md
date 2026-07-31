---
status: investigation
updated: 2026-07-31
authority: investigation
---

> **Archived.** Superseded by [`../rendering/cascading-shadow-mapping-design.md`](../rendering/cascading-shadow-mapping-design.md). Retained for history; do not treat as current.
# Cascaded Shadow Mapping (CSM) Debugging Postmortem + Next Steps (RenderDoc-Ready)

## Purpose

This document captures what we tried while debugging CSM, what symptoms we observed, which hypotheses are still plausible, and what data to collect next time (including a RenderDoc capture plan). It is written to be directly consumable by an LLM: it includes concrete reproduction steps, expected observations, and a checklist of artifacts to attach.

## Current Situation (Executive Summary)

We have a CSM implementation wired into the renderer, but shadows are not correct in the default Sponza scene:

- Shadows frequently appear as a **small “patch”** instead of covering the visible scene.
- Shadows may appear to **“swim” / move** noticeably as the camera moves (more than the typical minor cascade shimmer).
- Debug modes that visualize the shadow maps show **nearly identical results for multiple cascades**, often with only a small visible silhouette against a mostly-cleared background.
- A separate regression was introduced during debugging that made the world render appear **fully black** (later fixed).

At the moment, the system has enough logging and shader debug modes to narrow this down, but we need a more structured data-collection pass and a RenderDoc capture to definitively confirm which stage is wrong (shadow map generation vs shadow sampling vs coordinate conventions).

### Important constraint: what an LLM can and cannot consume

- An `.rdc` capture is the best source of truth, but it is a binary format and can be large (often hundreds of MB).
- For collaboration with an LLM, prefer exporting a small “capture bundle”:
  - a few texture exports (PNG for quick-look, EXR for numeric depth),
  - pipeline state export (HTML),
  - a short capture context note (text),
  - and a handful of in-app screenshots (our debug modes).

## What We’re Trying To Achieve

Baseline expectations for directional-light CSM in our scene:

- Curtains and other geometry should **cast** shadows onto nearby surfaces.
- With a fixed directional light, moving the camera should not change the *world-space* shadowing, except for:
  - minor shimmering at cascade boundaries and due to quantization, and
  - expected changes in which cascades cover which screen-space regions.

If shadows look like a “projected box behind the camera”, that is not expected behavior and typically indicates coordinate-space or projection-range mistakes.

## Reproduction Context

- Scene: `assets/scenes/default.scene.json` (directional light + point lights).
- Primary target: macOS M1 Pro (MoltenVK).
- Renderer uses view/layer system; shadows are produced by a shadow layer and consumed by the world layer.

## Debug Instrumentation That Exists (Use It)

### Runtime logging (already present)

From the shadow layer (`lib/src/renderer/passes/vkr_pass_shadow.c`):
- Per-cascade log includes: cascade index, image index, render target handle, shadow map handle, split far, light direction, radius, units-per-texel, and a few matrix diagonal terms.

From the world layer (`lib/src/renderer/passes/vkr_pass_world.c`):
- Periodic log includes: whether shadow frame data is valid, enabled flag, cascade count, and pointers for `shadow_maps[0..3]`.
- Periodic log includes: the viewport used for screen-space debug UV mapping (`screen_params`).

### Shader debug modes (world shader)

In `assets/shaders/default.world.slang`, the global UBO field `shadow_debug_mode` selects debug outputs:

- `0`: off (normal lighting)
- `1`: visualize cascade selection (solid per-cascade colors)
- `2`: visualize shadow factor (grayscale; 0=shadowed, 1=lit)
- `3`: visualize shadow projection + depth compare inputs
  - outputs `{receiver_z, map_depth, |dz|}` and tints UV-out red and Z-out blue
- `4..7`: visualize raw shadow maps `shadow_map_0..3` (grayscale)

If `4..6` look identical, that is a strong signal that either:
- all cascades render into the same underlying image/view, or
- all cascades use effectively the same light VP, or
- the debug visualization isn’t actually showing the correct map (binding or UV issue).

## What We Tried (Chronological) and Outcomes

### 1) “Shadows move with the camera”

**Symptom:** shadows shift strongly when moving the camera; looked like a projected box behind the camera.

**Attempt:** added cascade stabilization (snap light-space center to texel grid; fixed-span ortho).

**Outcome:** did not resolve the “large swim” symptom; in many views shadows still appeared as a small patch or vanished.

**Notes:** Some shimmer is expected in camera-relative CSM; the observed magnitude suggests a deeper issue (projection conventions, bounds, or sampling rejection).

### 2) “No shadows / tiny patch”

**Symptom:** in normal render, shadows were absent; debug map views showed only a tiny silhouette on a mostly white screen.

**Attempt:** extensive shader debug modes to inspect cascade selection, shadow factor, projected UVs, and raw maps.

**Outcome:** debug modes confirmed that:
- cascade selection does produce multiple regions on screen (mode `1`),
- but the raw maps often look very sparse and/or cascades appear identical.

### 3) “Black scene regression”

**Symptom:** entire scene became black (not just shadowed).

**Cause:** we tried compiling Slang SPIR-V with `-fvk-use-dx-layout` (added to `build.sh`). This changes UBO layout rules and caused CPU-side uniform packing to mismatch shader reads.

**Fix:** reverted `build.sh` to compile shaders without that flag; rebuilt shaders. Scene rendering returned to normal.

**Takeaway:** do not change Slang layout mode without also changing how we compute and upload uniform offsets.

## High-Probability Root Causes (Hypotheses)

This section lists plausible failure modes with the *specific evidence we saw* and a *direct way to confirm or falsify* them.

### H1: Light projection uses the wrong clip-space depth convention (0..1 vs -1..1, or reversed-Z mismatch)

**Why it fits**
- In the world shader, we early-out and return “fully lit” when projected `z` is outside `[0,1]`.
- If the light VP produces OpenGL-style depth (`[-1,1]`) while we treat it as Vulkan-style (`[0,1]`), most samples will be rejected → “no shadows”, except in a small region where values coincidentally land in range.
- Debug mode `3` is designed to show this: Z-out-of-range tints blue, and the raw `z` will look clamped.

**How to confirm (without RenderDoc)**
1. Enable `shadow_debug_mode = 3`.
2. Look for a mostly blue-tinted screen (meaning most fragments are Z-out).
3. Capture the per-cascade logs from `vkr_pass_shadow.c` and verify whether the computed VP matrices are expected to output Vulkan NDC depth.

**How to confirm (with RenderDoc)**
- In the world drawcall, inspect the constant buffer values of `shadow_view_projection[cascade]`.
- In the shadow pass drawcall(s), inspect the VS output `gl_Position.z / w` (or equivalent) range and compare against the depth attachment range.

### H2: Shadow pass renders correctly, but world pass samples the wrong descriptor bindings (maps/samplers)

**Why it fits**
- Raw map debug modes `4..6` sometimes looked identical and showed a small region; this can happen if all `shadow_map_N` bindings point to the same image view or if samplers are mis-bound.
- We previously saw validation errors related to descriptor lifetimes and image layouts in earlier iterations (sampler destroyed / image view invalid on reload). Those kinds of issues can silently result in sampling from “nothing”.

**How to confirm**
- Add one-time (or periodic) logs from the material system path where shadow maps are bound:
  - `vkr_material_system_set_shadow_maps()`
  - `vkr_material_system_apply_instance()` for the world shader instance set
  - Log each `shadow_map_N` handle plus its underlying VkImage/VkImageView if available.
- In RenderDoc: inspect descriptor set 1 bindings 4..7 and samplers 11..14 for the world drawcall and verify they reference distinct views for cascades 0..2.

### H3: Shadow pass uses different matrices per cascade on CPU, but they are not actually uploaded per pass (stale UBO)

**Why it fits**
- Logs showed different `vp00/vp11/vp22` across passes, but the resulting maps still appeared identical.
- If shadow VP is written into a CPU structure but not uploaded (or the wrong uniform name/offset is used), GPU will use the previous pass’s VP.

**How to confirm**
- In RenderDoc, for each shadow-pass drawcall group (cascade 0/1/2), inspect the shadow shader’s constant buffer contents and verify the VP matrix differs per cascade.

### H4: Viewport/scissor state is wrong during shadow rendering (rendering into only a small region)

**Why it fits**
- A “tiny patch” in the shadow map can be caused by rendering with a viewport/scissor that is not the full shadow-map extent.
- If the viewport is set to the window size (or some stale state) and the shadow map has a different size, the rendered region may be clipped unexpectedly.

**How to confirm**
- RenderDoc: in the shadow-pass pipeline state, inspect viewport and scissor for each cascade render pass. They should match the shadow map resolution.
- Also check for dynamic viewport/scissor state and whether it is set per pass.

### H5: Curtains not casting shadows because they are excluded from the shadow-caster set (domain/material filtering)

**Why it fits**
- Curtains might be routed through a transparent pipeline/domain and could be skipped in the shadow layer’s caster list.
- If the shadow pass only renders “opaque world” objects, thin cloth/curtain geometry can be missing entirely.

**How to confirm**
- RenderDoc: in the shadow pass drawcall list, search for curtain submeshes (by mesh name, drawcall labels, vertex counts, or bound textures if visible).
- If absent, inspect the shadow layer’s “which meshes/submeshes are submitted” logic and confirm whether transparent meshes are included.

### H6: Coordinate-system mismatch (Y flip / handedness / matrix multiply order) between CPU math and shader expectations

**Why it fits**
- The world shader assumes `clip.xy` maps to UV with `clip.xy * 0.5 + 0.5`.
- If the light VP produces a flipped Y compared to the shadow map rasterization, shadows can appear mirrored or offset; with tight bounds this can look like a moving patch.

**How to confirm**
- Debug mode `3`: check whether projected UVs cluster near edges or mirror unexpectedly as camera moves.
- RenderDoc: inspect shadow VS outputs and compare against depth attachment orientation.

## What We Need Next Time (Data Collection Checklist)

This is the “setup for the next prompt”. Capture *exactly* the following, attach it to the next debugging request, and include the commit hash / local diff summary if possible.

### A) App-side artifacts

1) **Screenshots (same camera pose)**
- Normal render: `shadow_debug_mode = 0`
- Cascade visualization: `shadow_debug_mode = 1`
- Shadow factor: `shadow_debug_mode = 2`
- Projection/depth debug: `shadow_debug_mode = 3`
- Raw maps: `shadow_debug_mode = 4`, `5`, `6` (cascade 0..2)

2) **Logs**
- Enable the periodic logs already present (they print every 240 frames).
- Provide a continuous log excerpt that includes:
  - at least one full set of `vkr_pass_shadow.c` per-pass prints for passes 0/1/2
  - the matching `vkr_pass_world.c` “frame_valid/enabled/cascades/maps” print
  - the `viewport window/last/used` print

3) **Camera + light state**
- Record camera position/rotation (already on-screen).
- Confirm directional light direction and intensity from `default.scene.json` (or paste that snippet).

4) **One controlled test**
- Freeze the camera and toggle only `shadow_debug_mode` through the modes above.
- Then move the camera slightly and capture the same set again.
  - Goal: determine whether the patch moves due to cascade coverage changes, UV/z rejection, or map content changes.

### B) RenderDoc artifacts (preferred)

> Note: RenderDoc capture is planned on Windows (or Linux) where it is supported. Use a native Vulkan backend if possible.

#### Capture requirements

1) Capture a frame where:
- `shadow_debug_mode = 0` shows the problem (missing/patchy/swimming shadows), and
- `shadow_debug_mode = 4..6` shows the shadow maps.

2) Save:
- the `.rdc` capture (recommended as an archive; optional if too large to share)
- exported images of the shadow maps (cascade 0..2) from the texture viewer (steps below)
- pipeline state exports (HTML) for one shadow-pass draw and one world-pass draw
- a short “capture context note” (copy/paste template below)

If the `.rdc` is too large to share:
- keep it locally, and share only the exports above (textures + HTML + context note + in-app screenshots).

#### First-time RenderDoc quickstart (Windows, Vulkan)

Goal: produce a single `.rdc` that contains both the shadow pass and the world pass, plus a few exported images.

1. Install RenderDoc from https://renderdoc.org/ and launch it.
2. Open the **Launch Application** tab.
3. Set:
   - **Executable Path**: your built app binary (example: `build\\app\\vulkan_renderer.exe`).
   - **Working Directory**: the folder that contains the executable (example: `build\\app\\`).
     - This matters: our app expects to find assets relative to the working directory.
   - **Command line arguments**: leave empty unless you have a custom scene/flags.
4. Click **Launch**.
5. In the running app:
   - load `assets/scenes/default.scene.json` (or confirm it is the active scene),
   - move the camera to a view where shadows should be obvious,
   - set `shadow_debug_mode = 0` (normal render) to confirm the symptom is visible.
6. Trigger a capture:
   - default hotkey is usually **F12** (confirm in RenderDoc → **Tools → Settings → Keyboard Shortcuts**).
7. Back in RenderDoc, click the capture thumbnail to open it.
8. Save it: **File → Save Capture As…** → `csm_<date>_<scene>.rdc`.

If the capture comes out empty (common first-time pitfall):
- Confirm you launched the app from RenderDoc (not from Explorer/terminal).
- Confirm the app is using Vulkan (not a different backend).
- Disable overlays/injectors that can interfere (Steam overlay, Discord overlay, etc.).

#### Exported textures (exact steps)

Goal: provide a small set of images that can be inspected quickly without opening RenderDoc.

Export these resources:
- the depth attachment for `Depth-only pass #1` (cascade 0)
- the depth attachment for `Depth-only pass #2` (cascade 1)
- the depth attachment for `Depth-only pass #3` (cascade 2)

What you are looking for in the Texture List:
- typically `2D Depth Attachment <id>` resources with size `1024x1024` and format like `D32`
- one distinct resource per cascade pass (they should not all be the same ID)

Steps (RenderDoc UI):
1. Open the `.rdc` capture in RenderDoc.

2. Identify the shadow map textures (two ways; pick whichever is easiest):

   **Way A (recommended for first-time users): find by looking at textures**
   - Open the **Texture Viewer** tab.
   - In the left resource list, look for 2D textures that match your shadow resolution (commonly square, e.g. `1024x1024`, `2048x2048`).
   - Click candidates and look for depth-like images (mostly white with darker silhouettes).
   - Once you find one, note its size and resource name/id. Find the other cascade textures the same way.

   **Way B: find by selecting a shadow-pass drawcall**
   - Open the **Event Browser** tab.
	   - Scroll until you find a sequence of drawcalls that look like a depth-only pass (often many draws, minimal outputs).
	   - Click a drawcall within that pass.
	   - Open **Pipeline State → FB** (Framebuffer). In RenderDoc v1.42 this is the “Output Merger” equivalent for Vulkan (there is no separate “Output Merger” panel).
	   - In the **FB** page, find the depth attachment (Depth / Depth-Stencil).
	   - Click the bound depth resource; RenderDoc will jump to it in **Texture Viewer**.

Practical tip (what you’re usually looking for):
- In our captures the shadow cascades commonly show up as **square depth textures** (e.g. `1024x1024`, `D32` or `D32_SFLOAT`-like formats).
- The swapchain/world depth is typically **window-sized** (non-square, e.g. `1493x938`) — don’t export that one as a “shadow map”.

3. Export `shadow_map_0` (cascade 0):
   - In **Texture Viewer**, select the cascade 0 texture.
   - Set:
     - **View**: 2D
     - **Mip**: 0
     - If it is a depth attachment: enable **Depth** display (RenderDoc has a Depth toggle/button next to the channel controls).
       - If it is an `R32_SFLOAT`-style depth texture, viewing channel **R** is equivalent.
   - If it looks blank/solid:
     - use the histogram/range controls in Texture Viewer to adjust the display range (depth often lives near 1.0).
   - Export:
     - click the **Save Texture** / disk icon (or right-click the texture → **Save…**).
     - choose **EXR** (preferred) or **PNG** (quick-look).
     - save as `shadow_map_c0.exr` (or `.png`).

4. Export cascade 1 and 2:
   - Repeat step 3 for the other two cascade textures.
   - Save as `shadow_map_c1.exr` and `shadow_map_c2.exr`.

If your capture shows labeled regions like `Depth-only pass #1/#2/#3`:
- Treat each `Depth-only pass #N` as “one cascade”.
- Click a drawcall inside `Depth-only pass #1`, export its depth attachment as `shadow_map_c0.*`.
- Repeat for `#2` → `shadow_map_c1.*`, and `#3` → `shadow_map_c2.*`.

If you have 4 cascades enabled, export `shadow_map_c3.exr` as well. Otherwise, skip cascade 3.

Do you need to “change the cascade value” in the app to export each cascade?
- No. If the renderer renders all cascades in one frame, the capture already contains all cascade passes; export one depth texture per `Depth-only pass #N`.
- If you want to reduce capture complexity (optional), temporarily set `cascade_count = 1` to capture just one cascade, but then you lose cross-cascade comparisons.

Sanity check (prevents exporting the wrong thing):
- After exporting, verify the exported image dimensions match the shadow-map resolution.
  - Shadow cascades are usually **square** (e.g. `1024x1024`, `2048x2048`).
  - If your exported “shadow map” is **window-sized** (e.g. `1920x1080` or `1493x938`), you almost certainly exported the swapchain color/depth, not the cascade map.
- Also verify the source texture in RenderDoc:
  - Cascades come from `Depth-only pass #N` and are typically listed as **2D Depth Attachment** (format like `D32`/`D32_SFLOAT`) or a single-channel float texture.

Interpreting what you see in the exported shadow maps:
- “Mostly white” usually means the depth buffer is near the clear value (commonly 1.0). This can be normal for a depth map, but if only a tiny region contains geometry silhouettes, it often means the light frustum is not covering the expected scene region.
- If cascades 0 and 1 look *pixel-identical*, treat it as a bug until proven otherwise:
  - you may have exported the same resource twice, OR
  - the renderer is accidentally rendering multiple cascades into the same depth attachment, OR
  - the per-cascade `light_view_projection` is not actually changing on GPU.

**Important (confirmed in our captures):** the shadow shader’s `light_view_projection` was **identical across cascades on GPU**, even though CPU logs printed different matrices.

Root cause (most likely): our Vulkan backend updates the global UBO via **host writes** (`vkMapMemory`/`memcpy`) into a per-frame region. When we “update” the same region multiple times while recording a command buffer (one time per cascade), the GPU ends up seeing the **last written value** for *all* cascades at execution time.

Minimal fix (implemented): move per-cascade `light_view_projection` out of the global UBO and into **push constants** for the shadow shader, so each drawcall records its own matrix value (no “last-write-wins” hazard).

Depth-map export gotcha:
- RenderDoc may export depth attachments to EXR with **UINT** channels that store float depth *bit patterns*.
- If you want numeric depth stats from EXRs in this repo, use `tools/exr_scanline_stats.py`:
  - `python3 tools/exr_scanline_stats.py "shadow_map_c0.exr" --channel R --uint-as-f32-bits`
  - This prints min/max/percentiles and counts of near-0/near-1 values.

Depth-map visualization gotcha (why your PNGs can look “empty”):
- A depth attachment can be numerically valid while still looking “all white” under a 0→1 display range (values clustered near 1.0).
- PNG export is a visualization; it can hide detail due to clamping/normalization.
- For a human-readable PNG preview:
  - In **Texture Viewer**, adjust the range to something like `0.98..1.0` (or use histogram/auto-range), then export PNG.
- For correct numeric inspection:
  - Export EXR and (optionally) run the stats script above.

Optional (very helpful when maps look “tiny”):
- In **Pipeline State → Rasterizer**, take a screenshot of the viewport/scissor values for the shadow pass.
- In **Pipeline State → Resource Bindings**, take a screenshot showing the bound shadow map images/samplers for the world drawcall.

Optional (even better than screenshots): export Pipeline State as HTML
- RenderDoc can export the current drawcall’s pipeline state to an HTML file, which is easy to share and search.

Steps:
1. In **Event Browser**, select:
   - one representative **shadow-pass** drawcall (cascade 0 is fine), then export HTML.
   - one representative **world-pass** drawcall that should receive shadows, then export HTML.
2. Export pipeline state:
   - Try **File → Export → Pipeline State (HTML)** (menu wording can vary by RenderDoc version), or
   - Look for an **Export** / **Save** button inside the **Pipeline State** tab.
3. Name files clearly:
   - `pipeline_state_shadow_pass_c0.html`
   - `pipeline_state_world_pass_receiver.html`
4. In `csm_capture_context.txt`, record the **event ID** for each exported HTML file.

#### Capture context note (exact contents)

Create a small text file next to the `.rdc`, e.g. `csm_capture_context.txt`, and fill it in. This prevents the “rdc with no story” problem.

Copy/paste template:

```
CSM RenderDoc Capture Context

Platform:
- OS: Windows <version>
- GPU: <vendor/model>, driver <version>
- Vulkan: native / via <loader>, validation layers: on/off

Build:
- Build type: Debug/Release
- Git: commit <hash> (or "uncommitted changes", include `git diff --stat`)
- App executable path: <path>

Scene + camera:
- Scene: assets/scenes/default.scene.json (or other)
- Camera position: <x y z>
- Camera yaw/pitch: <yaw pitch>

Directional light:
- Direction (world): <x y z>
- Color/intensity: <rgb * intensity>

CSM settings (from logs if available):
- enabled: 0/1
- cascade_count: N
- split_far: [a b c d]
- shadow_map_resolution: <w x h> (or "unknown")
- shadow_map_inv_size: <float>
- bias / normal_bias: <float>/<float>
- pcf_radius: <float>
- stabilization: on/off (and which method)

App debug mode at capture time:
- shadow_debug_mode: <0..7>

RenderDoc navigation notes:
- Shadow pass event range: <event id start>..<event id end> (or describe labels)
- Cascade 0 render target resource id/name: <id/name>
- Cascade 1 render target resource id/name: <id/name>
- Cascade 2 render target resource id/name: <id/name>
- World drawcall used for sampling inspection: <event id>, mesh/material: <name if known>

Observed symptom in this capture:
- <1–2 sentences: e.g. "only tiny shadow patch", "maps identical", "z-out everywhere", etc>
```

#### What to inspect inside RenderDoc (checklist)

**1) Find the shadow pass**
- Identify the render pass / drawcall region corresponding to the shadow layer (depth-only output).

**2) Shadow map outputs**
- Open the depth attachment textures (or sampled images) for each cascade.
- Confirm:
  - they are not all the same resource/view
  - they contain plausible silhouettes for the scene region that cascade covers
  - clear values (typically 1.0) are consistent with our sampling logic

First-time tip: make the depth map visible
- Depth maps often look “all white” under a 0→1 display range because most values are clustered near 1.0.
- In **Texture Viewer**, use the range/histogram controls to zoom the display range:
  - try setting range to `0.95..1.0` or `0.99..1.0`
  - or use the “auto range” / histogram buttons (varies by RenderDoc version)
- Export PNGs *after* adjusting the range if you want a human-readable preview.

**3) Pipeline state (shadow pass)**
- Viewport + scissor: match the shadow map resolution.
- Depth state: depth test enabled, correct compare op (usually LESS or GREATER depending on depth convention).
- Culling: check front/back cull mode matches expectation (wrong cull can remove thin geometry).

**4) Constants / uniforms (shadow pass)**
- Confirm the per-cascade light VP differs between cascade 0/1/2 drawcall groups.

Current implementation note:
- The shadow shader’s per-cascade `light_view_projection` is provided via **push constants** (not a global UBO). This avoids “last-write-wins” hazards when recording multiple cascades per frame.

How to capture the matrix values (explicit steps):
1. In **Event Browser**, click a representative drawcall in `Depth-only pass #1`.
2. Go to **Pipeline State → Vertex Shader**.
3. Find the **Push Constants** section (RenderDoc may show it under the shader stage page).
4. Expand it and locate the `light_view_projection` values, then:
   - take a screenshot, OR
   - export via pipeline state HTML (recommended).
5. Repeat for `Depth-only pass #2` and `#3`.
6. In `csm_capture_context.txt`, write down the event IDs used for these screenshots/exports.

**5) World pass sampling**
- Pick a world drawcall over a surface where a shadow should be visible.
- Inspect descriptor set 1 bindings:
  - `shadow_map_0..2` image views
  - `shadow_sampler_0..2`
- Confirm they are valid, distinct, and in the correct layout.

**6) Validate coordinate conventions**
- In the world fragment shader, inspect the computed `clip` values (or intermediate values if the shader is debuggable).
- Specifically check `clip.z` distribution:
  - If most fragments are outside `[0,1]`, the “no shadows” early-out will dominate.

#### Minimal metadata to include with the capture

- Platform (macOS/Windows/Linux), GPU, and whether Vulkan is native or via MoltenVK.
- Build type (Debug/Release) and whether validation layers are enabled.
- The exact `shadow_debug_mode` used at capture time.

## Next-session artifact bundle (what to attach)

When filing the next debugging prompt, attach a single folder/zip with these names (or close equivalents):

- `csm_capture_context.txt` (filled using the template in this doc)
- `shadow_mode_0.png`, `shadow_mode_1.png`, `shadow_mode_2.png`, `shadow_mode_3.png`
- `shadow_mode_4_map0.png`, `shadow_mode_5_map1.png`, `shadow_mode_6_map2.png` (and `shadow_mode_7_map3.png` if 4 cascades)
- `shadow_map_c0.png`, `shadow_map_c1.png`, `shadow_map_c2.png` (and EXR versions if available)
- `pipeline_state_shadow_pass_c0.html`
- `pipeline_state_world_pass_receiver.html`
- optional: `csm_<date>_<scene>.rdc` (keep it even if you can’t share it)

## Updated hypotheses (what still looks wrong after the push-constant fix)

We found and fixed one confirmed issue:
- In RenderDoc, the shadow-pass `light_view_projection` was identical across cascades (GPU saw the last-written UBO value).
- Fix: moved shadow-pass `light_view_projection` to push constants so each cascade drawcall records its own matrix.
- After this, the cascade debug map modes (`4/5/6`) became distinct, but the shadow maps can still look mostly-clear with only a small silhouette region, and the world pass still does not show plausible scene-wide shadows.

The remaining likely failure points are:

### H7: Light frustum coverage is wrong (bounds are far too small or badly centered)

**Why it fits**
- Exported cascade maps can appear mostly-clear with a small “box” silhouette occupying only a tiny region.
- This matches an ortho projection that does not cover the intended receiver region (wrong center, wrong extents, or wrong space for bounds).

**What to collect**
- RenderDoc: for each cascade pass, record the push-constant `light_view_projection`.
- App logs: keep the per-cascade `radius` and `units_per_texel` prints.

**How to confirm**
- Compare the logged `radius` against the matrix diagonal:
  - for a symmetric ortho, `abs(m00) ≈ 1/radius` and `abs(m11) ≈ 1/radius` (sign depends on Y convention).
  - if the matrix implies an extent much larger/smaller than `radius`, the CPU math and the GPU matrix do not match.
- Use world debug mode `3`:
  - if most pixels are UV-out (red) or Z-out (blue), the light VP likely does not cover the visible receiver area.

### H8: World shader receives incorrect `shadow_view_projection[]` / `shadow_split_far[]` (upload/packing mismatch)

**Why it fits**
- Shadow maps can be valid, but if the world pass uses wrong matrices/splits, sampling will be out-of-range or compare against the wrong cascade.
- This is especially easy to break with mat4 arrays due to std140 alignment/stride requirements.

**What to collect**
- RenderDoc world-pass drawcall:
  - inspect the world shader global UBO values for `shadow_split_far` and the `shadow_view_projection` array.
  - inspect descriptor set bindings for `shadow_map_0..2` and their samplers.
- App logs:
  - print `shadow_split_far[]` and a few elements of each `shadow_view_projection[i]` right before uploading.

### H9: Frustum-corner reconstruction uses the wrong NDC Z range (Vulkan 0..1 vs OpenGL -1..1)

**Why it fits**
- CSM frustum-corner code often assumes NDC Z `[-1,1]`.
- If our projection matrix/math library uses Vulkan-style Z `0..1` (or vice versa), cascade bounds can be wrong and unstable.

**How to confirm**
- Add a one-frame debug log that prints the eight frustum corners for cascade 0 in world space and their AABB.
- In RenderDoc, check whether the cascade depth map content changes in nonsensical ways when moving the camera slightly (beyond normal shimmer).

## Suggested Next Debugging Prompt Template

Use this as the next prompt to an LLM (attach artifacts):

1) Problem statement: “CSM shadows missing / tiny patch / swimming; need root cause.”
2) Attach:
  - screenshots for modes `0..6` (as listed above)
  - the log excerpt containing one full cascade print set
  - the RenderDoc `.rdc` capture (or note why it couldn’t be captured)
  - the snippet of `default.scene.json` directional light definition
3) Ask explicitly:
  - “Determine whether the issue is (a) shadow map generation, (b) sampling/binding, or (c) coordinate convention mismatch.”
  - “Propose the smallest code change(s) to validate the hypothesis (extra logs/asserts, one shader tweak, or a RenderDoc inspection).”
