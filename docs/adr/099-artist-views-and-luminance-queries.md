---
status: partial
updated: 2026-10-09
authority: adr
---

# ADR-099: Artist views and luminance queries

## Status

Accepted (partial). The Metal tiled pipeline draws seven artist views:
- base colour;
- roughness;
- metallic;
- normals;
- material cost;
- texel density;
- exposure false colour.

Captures take a view by name, and `query.luminance` measures the scene's
HDR colour at points and over regions. The desktop pipeline does not draw
these views yet.

## Context

The editor offered Lit, Unlit, Detail lighting, Lighting only and Wireframe
([ADR-044](044-shader-cross-backend-contract.md#editor-inspection-views)).
An artist checking a material pass, or an agent checking it through
captures, could not see a material's data, its cost, its texel density or
the scene's exposure. A capture's 8-bit display image also cannot say that
one corridor is two stops darker than a hall. [The artist toolkit
proposal](../proposals/artist-toolkit.md) (Part 7 and Agent operations)
asks for these views and a luminance query.

## Decision

1. **Modes.** `VkrRenderMode` adds:
   - `BASE_COLOR` (13), `ROUGHNESS` (14), `METALLIC` (15),
     `MATERIAL_COST` (16) and `TEXEL_DENSITY` (17), the data views;
   - `EXPOSURE` (18).

   `NORMAL` (2) joins the data views on the tiled pipeline.
   `vkr_graphics_pipeline_draws_render_mode` reports them for the tiled
   pipeline only. The desktop pipeline draws the modes before 13.
2. **Data views.** The tiled inspection variant writes the value instead of
   light, pre-exposed:
   - the shading normal (normal map included) as `n * 0.5 + 0.5`;
   - base colour;
   - roughness before the shading floor, as grey;
   - metallic, as grey;
   - material cost: the textures a pixel samples, its base colour plus each
     map its row flags enable, plus ten for layers and their mask, coloured
     green (one) through yellow to red (eight or more);
   - texel density: the base colour texture's texels per metre, from the UV
     and world-position derivatives along both screen axes, against
     `art.lint`'s default range of 128 to 2048. Blue is below, green
     inside (brighter toward the top), red above.

   A Custom graph's pixels in the cost view show its fallback's cost,
   tinted magenta. The sky shows black, and the atmosphere and fog pass
   leaves data views alone. The tonemap shows them with unit exposure and
   without grading, bloom or the tone curve, so the image is the value in
   display encoding.
3. **Exposure.** The exposure view shades as usual. The tonemap
   (`VKR_METAL_PACKET_TONEMAP_FLAG_FALSE_COLOR`) colours each pixel by whole
   stops of its exposed luminance from middle grey (0.18), five stops either
   way:
   - purple and blues below;
   - green at middle grey;
   - yellow, orange and red above;
   - white at five stops or more.

   Bloom and the tone curve are left out.
4. **Shared kernel.**
   [editor_view.slangh](../../renderer/src/shaders/shared/editor_view.slangh)
   holds the mode predicates and the cost, density and stop ramps, so the
   desktop pipeline can draw the same views later.
5. **Editor.**
   - The Scene toolbar's render menu lists the views.
   - Cmd `view.mode` takes `base-color`, `roughness`, `metallic`,
     `normals`, `material-cost`, `texel-density` and `exposure`.
   - The Lighting palette adds Exposure.
   - Harness cases name them `base_color`, `roughness`, `metallic`,
     `material_cost`, `texel_density` and `exposure`.
6. **Captures.** `view.capture` takes `mode`, per view in a sheet, and
   restores the designer's mode afterwards.
7. **Luminance.** `query.luminance` takes a view as `view.capture` does,
   `marks` and a `region` ([x0, y0, x1, y1] fractions of the image, default
   the whole image).
   - It captures the scene's HDR colour (`hdr_post_transmission`, RGBA16F)
     through the editor's capture request (`scene_hdr`).
   - Luminance is the Rec. 709 weighted sum divided by the capture's
     pre-exposure, in the renderer's scene-linear units.
   - Each mark and the region answer `luminance`, `ev100` (log2 of L × 100 /
     12.5) and `stops` from middle grey after the frame's exposure, the
     bands of the exposure view.
   - The region adds `log_average`, `peak` and its pixel count. A mark
     outside the view answers null.

## Consequences

- An agent checks a material pass with a capture sheet of data views. It
  checks lighting with numbers rather than by reading pixels.
- Luminance is independent of exposure, and stops follow it. Comparing two
  places compares luminance; judging a frame compares stops.
- The texel density view measures the base colour texture only, and a
  Custom graph's cost is its fallback's.
- On Vulkan the artist views draw the lit image, and `query.luminance` is
  unverified. Both wait for the desktop phase.

## Evidence

On a headless Bistro editor (Release, Metal), on 2026-10-09
(`.scratch/artist/phase5c_check.py` in the working tree):
- **Captures.** Two capture sheets of one street view showed lit, base
  colour, roughness, metallic, normals, material cost, texel density and
  exposure. Mostly green density showed the façades inside the range and
  the trees above it. An unknown `mode` failed with the list of modes.
- **Luminance.** `query.luminance` gave these values:
  - wall mark: 0.0112;
  - sky mark: 0.0610;
  - a mark behind the camera: null;
  - the central fifth: mean 0.0469, log-average 0.0224, peak 3.65.

  A look volume adding 2 EV of exposure compensation then changed:
  - the frame's exposure from 5.27 to 21.08;
  - the wall's stops from -1.61 to 0.39;
  - its luminance by under 0.2%.

  A falling region was refused.
- **Validation.** The same run under Metal API validation (`MTL_DEBUG_LAYER=1`,
  one process) reported no error and shut down cleanly.
- **CPU suites.** `run_harness_tests` passes with the new mode names, and
  `run_renderer_impl_tests`, `run_exposure_tests` and
  `run_metal_packet_abi_tests` pass.
- **Timing.** Not measured. The lit path's only change is a uniform flag
  test in the tonemap; the data views change only the inspection variant.
- **Unavailable:** Vulkan native drawing of the views.

## Alternatives considered

- **False colour in the forward shader.** Rejected: only the tonemap knows
  the frame's exposure, which the bands follow.
- **Luminance from the display image.** Rejected: 8-bit display values after
  the tone curve cannot give stops or compare places.

## Revisit when

- The desktop pipeline draws the views (with its own HDR channel for
  `query.luminance`).
- Custom graphs report their own sample counts to the cost view.
- Projects set their own texel density range.
