---
status: proposed
updated: 2026-08-25
authority: design
---
# Text resolution independence and font cooking specification

**Document status:** Proposed. No production code implements any part of this
document. The measurements and defect claims below were read from current code
and current assets; the remediation is a plan.

**Scope:** Why VKR text degrades as the window extent changes, why the font
generation pipeline is manual, and what replaces both. Rationale for the three
load-bearing decisions lives in [ADR-034](../architecture/adr/034-offline-cooked-font-artifacts.md),
[ADR-035](../architecture/adr/035-canonical-mtsdf-screen-pixel-range-shading.md),
and [ADR-036](../architecture/adr/036-dpi-derived-ui-text-scale.md).

Related current documents: [mtsdf-font-loader-design.md](mtsdf-font-loader-design.md),
[system-font-loader-design.md](system-font-loader-design.md),
[font-system-design.md](font-system-design.md),
[ui-text-implementation-design.md](ui-text-implementation-design.md), and
[presentation-dpi-and-transfer-function-spec.md](../rendering/presentation-dpi-and-transfer-function-spec.md).

---

## 1. Conclusion first

The reported symptom — text that does not scale well with resolution — is not
one defect in the MTSDF path. It is three independent facts:

1. **The shipping UI does not use the MTSDF path.** Windows UI overlays resolve
   `VkrFontSystem::default_system_font_handle`, which is a stb_truetype raster
   baked at 128 px with no mip chain. The application authors those overlays at
   32 px. Every HUD glyph is a 4× minification of an unmipped raster at the
   800×600 design extent, and the minification factor moves with the window.
   Text is sharp only where the window extent happens to drive the scale near
   the baked raster size, which is around 4K.
2. **The MTSDF path quantizes resolution-independent data into integer pixels
   at a baked size.** `vkr_mtsdf_build_font()` converts em-normalized advances,
   bearings, and line metrics into `int16_t`/`int32_t` pixel counts at one
   authored size, and `vkr_text_layout_compute()` rescales those integers.
   Atlas UVs are truncated from non-integer `atlasBounds`, giving every glyph a
   systematic half-texel bias.
3. **The MTSDF shader does not implement the msdf-atlas-gen contract.** The
   correct screen-pixel range is computed on the CPU, uploaded, and never read.
   `text/default.slang` instead antialiases from `fwidth()` of the distance
   value, ignores the `.a` channel that exists precisely for minification, and
   samples an atlas that the texture packer has block-compressed and tagged
   sRGB.

Generation is manual because there is no cooker. `Ubuntu-2d.json` and its PNG
were produced by hand with the msdf-atlas-gen CLI, checked in, and are now
consumed by two systems that disagree about their coordinate conventions. The
checked-in JSON contains `"kerning": []` because nobody passed `-kernpairs`,
and the loader would not read it if it were there.

`nuri` already solves all three. Section 6 records what it does and which parts
transfer.

---

## 2. Evidence

### 2.1 The shipping Windows UI text path

`vkr_font_system_init()` loads `assets/fonts/NotoSansCJK-Windows.fontcfg`
(`type=system`, `size=128`) as the default system font on Windows.
`application_init_ui_texts()` and `application_init_memory_text()` in
`app/src/main.c` set `text_config.font` to that handle and
`text_config.font_size` to 32 and 24 respectively.

`vkr_system_font_rasterize_glyphs()` rasterizes U+0020..U+00FF with
`stbtt_MakeGlyphBitmap()` at the requested pixel height into a single RGBA
atlas. The atlas is created with `mip_filter = VKR_MIP_FILTER_NONE` and
`upload_mip_levels = 1`.

`vkr_ui_system_text_content_scale()` returns `min(width/800, height/600)` and
`vkr_ui_system_position_slot()` applies it through `vkr_transform_set_scale()`
on each retained text transform. Both are compiled only under
`PLATFORM_WINDOWS`; every other platform gets `1.0`.

Device-pixel em height for the 32 px HUD text is therefore
`32 × content_scale`, and the atlas minification factor is
`128 / (32 × content_scale)`:

| Client extent | `content_scale` | HUD em (device px) | Atlas minification |
|---|---|---|---|
| 480×270 | 0.450 | 14.4 | 8.9× |
| 800×600 | 1.000 | 32.0 | 4.0× |
| 1280×720 | 1.200 | 38.4 | 3.3× |
| 1600×900 | 1.500 | 48.0 | 2.7× |
| 1920×1080 | 1.800 | 57.6 | 2.2× |
| 2560×1440 | 2.400 | 76.8 | 1.7× |
| 3840×2160 | 3.600 | 115.2 | 1.1× |

A bilinear tap set covering a 9×9 texel footprint discards most of the glyph.
The FPS and metrics overlays rewrite their content several times per second, so
the discarded texels change frame to frame and the result shimmers. This is the
reported symptom, and it is worst at small windows, not large ones.

Two further consequences of the same formula. `min()` means a 16:9 window is
scaled by its height against a 4:3 design extent, so text occupies a smaller
fraction of a wide window than of a tall one at equal height. And because the
whole quad is scaled by a matrix, layout — advances, wrapping, line breaks — is
solved at the design size and then stretched, so wrap points do not move when
the window does.

On macOS and Linux the same overlays take the `#else` branch and request
`font->size * 2.0f`, i.e. 256 px from a 128 px raster: constant 2×
magnification at every resolution. The platforms do not share a scaling model.

### 2.2 MTSDF metric quantization

`vkr_mtsdf_build_font()` computes `scale = target_size / metadata->em_size` and
writes:

- `out_font->line_height`, `ascent`, `descent`, `baseline` as rounded `int32_t`
  pixel counts;
- `VkrFontGlyph::x_advance` as a rounded `int16_t`;
- `VkrFontGlyph::x_offset` and `y_offset` as `int16_t`, and `x_offset` uses a
  bare cast, which truncates toward zero. `planeBounds.left` is negative for
  several Ubuntu glyphs, so the sign of the error flips with the glyph.

`vkr_text_glyph_base_advance()` and `vkr_text_font_get_kerning()` in
`lib/src/core/vkr_text.c` then multiply those integers by
`font_size / font->size`. For `UbuntuMono-2d.fontcfg` the bake size is 18, so
each advance carries up to ±0.5 px of error at 18 px/em — ±2.8% of an em — and
that error is multiplied by the render scale and accumulates along a run. At
`content_scale = 3.6` a single glyph contributes up to ±1.8 device pixels of
tracking error.

`vkr_ui_text_generate_geometry()` makes the inconsistency visible: quad
*extents* for MTSDF come from exact `planeBounds × font_size`, while the quad
*origin* comes from the quantized `x_offset`/`y_offset`. The glyph image and
the advance box no longer agree, and the disagreement scales.

### 2.3 MTSDF atlas UV bias

msdf-atlas-gen emits half-texel `atlasBounds`. From `assets/fonts/Ubuntu-2d.json`,
U+0021:

```json
"atlasBounds": { "left": 948.5, "bottom": 973.5, "right": 965.5, "top": 1023.5 }
```

`vkr_mtsdf_build_font()` stores `dst->x = (uint16_t)948.5 = 948` and
`dst->y = (uint16_t)(1024 - 1023.5) = 0`. `vkr_ui_text_generate_geometry()`
divides those integers by the atlas extent. Every glyph is sampled half a texel
up and half a texel left of where it was rasterized. `VkrUiTextConfig::uv_inset_px`
exists to fight the resulting bleed; it treats a systematic bias as if it were
a filtering problem, and insetting an MTSDF quad's UVs also shrinks the
distance field relative to the plane bounds.

### 2.4 The `emSize` read escapes its object

`vkr_json_find_field()` scans forward to end of document. It tracks no brace
depth, so it has no concept of object scope. `vkr_mtsdf_parse_atlas()` resets to
`atlas_start` and looks for `emSize`, which does not exist in msdf-atlas-gen's
`atlas` object. The search runs past the closing brace and returns
`metrics.emSize`, which msdf-atlas-gen sets to `1`.

The result happens to work: metrics are em-normalized, so `scale = target_size`
is the correct em→pixel conversion. It works for the wrong reason, and any
generator that emits a non-unit `emSize` breaks the font silently.

Meanwhile `atlas.size` — 64 for `Ubuntu-2d.json`, the pixels-per-em the atlas
was rasterized at, and the exact denominator the screen-pixel range needs — is
parsed into `VkrMtsdfFontMetadata::size`, validated, and never used again.

### 2.5 The screen-pixel range is computed, uploaded, and ignored

`vkr_ui_system_prepare_text_draws()` and
`vkr_world_resources_prepare_text_draws()` both compute:

```c
screen_px_range = Clamp(font->sdf_distance_range * (render_size / font->em_size), 1.0f, 4.0f);
```

`vkr_vulkan_draws.c` writes it to `root->material_alpha.x` and the font mode to
`root->material_flags`. `text/default.slang` reads `material_flags` and never
reads `material_alpha`.

The formula is also wrong. The msdf-atlas-gen contract is

```
screenPxRange = distanceRange × (glyph size in screen px) / (glyph size in atlas px)
```

which for a whole-atlas constant is `distanceRange × render_size / atlas.size`.
With `distanceRange = 8`, `atlas.size = 64`, and `render_size = 18` the correct
value is 2.25. The shipped expression divides by `em_size = 1`, producing
`8 × 18 = 144`, which the clamp pins to `4.0` for every font at every size. The
clamp is what hides the error.

Because the shader uses `fwidth()` of the distance value instead, none of this
is currently observable. It becomes load-bearing the moment the shader is
corrected, so both must move together.

### 2.6 The MTSDF shader

`text_alpha()` in `lib/src/renderer/shaders/vulkan/slang/text/default.slang`:

```hlsl
float distance = max(min(atlas.r, atlas.g), min(max(atlas.r, atlas.g), atlas.b)) - 0.5f;
return saturate(distance / max(fwidth(distance), 1e-6f) + 0.5f);
```

Three problems.

`fwidth()` of the *median* is not the screen-space gradient of a continuous
field. `median3` switches which channel it returns across a corner, so its
derivative is discontinuous exactly where MSDF corners live. The correct
reference form takes `fwidth()` of the *UV*, which is smooth everywhere, and
converts through the known unit range.

The `.a` channel is never sampled. In an MTSDF atlas `.a` holds the true
single-channel signed distance. It is the channel that stays well-behaved under
minification, and it is the reason to pay for four channels instead of three.
Without it there is no graceful path for text below roughly two screen pixels
of range.

At high magnification `fwidth(distance)` approaches zero, the guarded divide
saturates, and the edge becomes a hard step with no antialiasing.

### 2.7 The atlas is block-compressed and sRGB-tagged

`tools/pack_vkt_textures.sh` walks all of `assets/textures` with no exclusions.
`vkr_vkt_packer.cpp` classifies by filename; `Ubuntu-2d.png` matches no normal
or data token, so it takes the `color-srgb` default. The KTX2 key/value block in
`assets/textures/Ubuntu-2d.png.vkt` records it:

```
vkr.texture_class   color_srgb
vkr.colorspace_hint srgb
vkr.pack_settings   asset=1;shape=2d;class=color_srgb;uastc=faster;mips=rgba8-box-v1;flip=vertical
```

The file is 1,398,864 bytes for a 1024² source — UASTC at one byte per pixel
plus a full mip chain.

`vkr_mtsdf_font_loader_load()` requests the atlas with no query string, so the
request carries no explicit class and the KTX2 `vkr.texture_class` value wins.
The atlas is transcoded to an sRGB block format and bound with a full mip
chain.

Both halves are wrong for a distance field:

- Block compression correlates the R, G, and B channels inside each 4×4 block.
  `median3` depends on those channels being independent; msdfgen documents that
  MSDF atlases must not be block-compressed. "faster" UASTC makes it worse.
- An sRGB view applies the sRGB EOTF to distance values. The glyph edge at 0.5
  decodes to about 0.214, so `median − 0.5` is negative across most of the
  glyph and strokes erode. The erosion is non-linear in the distance, which
  removes the linearity the technique is built on.
- Box-filtered mips of an MSDF are not valid MSDFs.

The runtime *asks* for linear/UNORM — `vkr_texture_parse_request()` defaults
`prefers_srgb` to false — and gets sRGB anyway because the cooked sidecar
overrides an unspecified class. That is the packer classifying an asset it
should not have touched.

### 2.8 Smaller defects found along the way

- **No MTSDF kerning.** `vkr_mtsdf_parse_glyphs()` has no kerning counterpart.
  `VkrMtsdfFontMetadata::kernings` is never populated, so
  `vkr_mtsdf_build_font()` always takes the empty branch. `Ubuntu-2d.json`
  contains `"kerning": []` anyway, because the atlas was generated without
  `-kernpairs`.
- **Per-glyph `snprintf` in geometry generation.** `vkr_ui_text_find_glyph()`
  formats the codepoint into a decimal string to probe a string-keyed hash
  table, for every glyph, every time geometry is rebuilt — which for the FPS and
  metrics overlays is several times a second. AGENTS.md names per-draw string
  construction as a defect, not a style preference. `vkr_mtsdf_build_font()`
  builds the same table by allocating one `String8` per glyph.
- **Linear-scan fallback.** When the hash probe misses,
  `vkr_ui_text_find_glyph()` scans the whole glyph array. Harmless for 224
  Latin glyphs; not harmless for a CJK face.
- **`VkrFont::atlas_cpu_data` is write-only.** All three loaders populate it;
  nothing in `lib`, `app`, `tests`, or `tools` reads it.
  `vkr_mtsdf_font_load_atlas_cpu_data()` opens the source PNG a second time,
  decodes it with stb_image, and copies 4 MiB into the font's 6 MiB pool chunk.
  For `Ubuntu-3d.json` the 2048² atlas needs 16 MiB, the allocation fails, and
  the loader logs `out of memory for CPU atlas copy` at every startup. It also
  decodes unflipped while the GPU copy is flipped, so the two would disagree if
  anything did read it.
- **Three parallel font paths.** Bitmap, system, and MTSDF each carry their own
  loader, cache format, metric convention, and quality envelope, and
  `vkr_ui_text_create()` falls back to the *bitmap* default when a handle does
  not resolve.

---

## 3. Target design

### 3.1 One cooked artifact

A new offline tool, `vkr_font_cooker`, links msdf-atlas-gen and emits one
versioned binary `.vkfa` container per font. This mirrors `vkr_mesh_cooker`
(ADR-030) and `vkr_vkt_packer` (ADR-012), including the build-wrapper
integration and the content-hash skip. ADR-034 owns the rationale, the
dependency pin, and the container ABI.

The container holds, in one file:

- format magic `VKFA`, version, and a source content hash;
- `distanceRange` in atlas pixels and `atlasPxPerEm`, both as `float32_t`;
- em-normalized `FontMetrics`: ascender, descender, line height, underline
  position and thickness, `unitsPerEm`;
- a glyph table with `float32_t` advance, plane bounds, and **UVs already
  normalized to [0,1] with the V flip applied**, plus a page index;
- a sorted codepoint→glyph-index map;
- a sorted kerning table with `float32_t` amounts in em units;
- the atlas pages as uncompressed pixels, `R16G16B16A16_SFLOAT` by default.

Nothing in the container is expressed in pixels of a chosen render size. The
runtime never divides by an atlas extent, never truncates a bound, and never
guesses a convention.

### 3.2 Runtime consumption

`VkrFont` gains a float glyph representation for cooked fonts. `VkrFontGlyph`'s
integer fields stay for the bitmap loader, which is genuinely a
fixed-pixel-size format; the MTSDF path stops borrowing them.

Glyph lookup becomes a direct `uint32_t` codepoint key. The decimal-string hash
table goes away on both the build and the lookup side.

`vkr_text_layout_compute()` scales em-normalized floats by
`pxSize / unitsPerEm` at layout time, the way `computeFontScaleInfo()` and
`scaleGlyphMetrics()` do in nuri's `text_layouter.cpp`. Advances, bearings,
kerning, and line height all become float. Nothing rounds until the vertex
position is written.

The `?size=` query parameter is removed from MTSDF font requests. A cooked font
has no baked size; requesting one is meaningless and is the root of §2.2.

### 3.3 Shading contract

ADR-035 owns this. `text/default.slang` and `metal/msl/text/default.metal`
adopt the reference form:

```hlsl
// unitRange = pxRange / atlasSize, computed once per vertex
float screenPxRange(float2 uv, float2 unitRange)
{
    float2 screenTexSize = 1.0f / max(fwidth(uv), 1e-6f);
    return max(0.5f * dot(unitRange, screenTexSize), 1.0f);
}

float3 msd  = atlas.rgb;
float  sdM  = median3(msd) - 0.5f;
float  sdS  = atlas.a - 0.5f;
float  fall = saturate(2.0f - range);          // SDF below ~2 px of range
float  sd   = lerp(sdM, sdS, fall);
float  alpha = saturate(range * sd + 0.5f);
```

`pxRange` reaches the shader as the cooked `distanceRange`, not a CPU-side
product. The `screen_px_range` field in `VkrPreparedTextDraw` is repurposed to
carry it, and `vkr_ui_system_prepare_text_draws()` and
`vkr_world_resources_prepare_text_draws()` stop computing a render-size
product. The `[1, 4]` clamp is deleted; the clamp that matters is the
`max(..., 1.0)` inside `screenPxRange`.

The world/3D fragment shader additionally discards below a threshold so that
text participates correctly in depth.

### 3.4 Atlas asset policy

Font atlases leave the `vkr_vkt_packer` input set. ADR-034 makes the cooked
`.vkfa` the only delivery path for cooked fonts, so there is no loose PNG for
the packer to find and no classification heuristic to get wrong.

For the transition period, and for any font atlas that remains a loose texture,
the packer gains an explicit skip and the MTSDF loader requests
`?cs=linear&tc=data-mask` so an existing sidecar cannot override the class.

Atlas pages upload as `R16G16B16A16_SFLOAT`, single mip, `LINEAR`/`LINEAR`,
`MIP_FILTER_NONE`, clamp-to-edge. Sixteen-bit float removes both the 8-bit
distance quantization and the sRGB question. A 1024² RGBA16F page is 8 MiB
resident against 4 MiB for RGBA8; for the small number of font atlases a build
carries, that is the right trade, and §5 records it as a measured gate rather
than an assumption.

`VkrUiTextConfig::uv_inset_px` is deleted. Exact float UVs make it unnecessary,
and it actively harms a distance field.

### 3.5 UI text scale

ADR-036 owns this. `vkr_ui_system_text_content_scale()` is replaced by a scale
derived from the window's DPI, which
[presentation-dpi-and-transfer-function-spec.md](../rendering/presentation-dpi-and-transfer-function-spec.md)
already makes available and authoritative through Per-Monitor V2:

```
ui_scale = dpi / 96.0
```

The scale multiplies the authored **point size** before layout, not the text
transform after it. Layout then runs at the real device-pixel size, so wrap
points, advances, and line breaks are solved where the text is actually drawn.
The transform scale returns to 1.

The same expression compiles on every platform. macOS supplies its backing
scale factor; Linux supplies its per-output scale. The `#if PLATFORM_WINDOWS`
divergence and the hardcoded `#if defined(_WIN32)` font sizes in `app/src/main.c`
both go away.

A game HUD that wants design-extent scaling rather than DPI scaling keeps that
option, but it becomes an explicit per-slot policy on `VkrUiTextSlot`, not a
platform `#if` in the system.

---

## 4. Staged plan

Each stage is independently shippable and independently verifiable. Stages F1
and F2 are the ones that change what the user sees.

**F0 — Stop the bleeding, no format change.**
Exclude `assets/textures/Ubuntu-*.png` and `UbuntuMono-mtsdf-atlas.png` from the
packer; delete their `.vkt` sidecars; request the atlas with
`?cs=linear&tc=data-mask`. Correct `screen_px_range` to
`distance_range * render_size / atlas.size` and store `atlas.size` instead of
discarding it. Land the ADR-035 shader. This alone makes the MTSDF path usable
and is a few hundred lines.

**F1 — Cooked artifact.**
Vendor msdf-atlas-gen, add `vkr_font_cooker`, define and implement the `.vkfa`
container, add the loader, cook `UbuntuMono` and `NotoSansCJK`, wire the build
wrappers. Keep the JSON+PNG loader behind the existing `can_load` so nothing
breaks while assets migrate.

**F2 — Float metrics and DPI scale.**
Move `VkrFont`'s MTSDF representation to floats, delete `?size=`, convert
`vkr_text.c` layout to float, land ADR-036, retire the `#if defined(_WIN32)`
font sizes in `app/src/main.c`. Point the Windows default UI font at the cooked
MTSDF font. This is where the reported symptom is actually fixed.

**F3 — Cleanup.**
Delete `VkrFont::atlas_cpu_data` and its three writers. Replace the
string-keyed glyph table with a `uint32_t` key. Delete `uv_inset_px`. Retire the
`system` font type, or reduce it to a cooker input rather than a runtime
rasterizer. Retire the JSON+PNG MTSDF loader once assets are migrated.

**F4 — Optional, behind measurement.**
Per-glyph instancing in the style of nuri's `GlyphInstance` buffer: 6 vertices,
`gl_InstanceIndex`, one 32-byte record per glyph in a BDA buffer, no CPU vertex
array and no index buffer. Worth doing only if F3's profile shows text geometry
on the critical path.

Shaping with HarfBuzz is explicitly **out of scope**. VKR's current text is
Latin overlays and world labels. Adding a shaper is a separate decision with its
own dependency cost; the cooked container reserves a glyph-id space so it can be
added later without a format break.

---

## 5. Evidence gates

Two harness cases already exist and target exactly this:
`tools/cases/local/font_downsized_snapshot.case.json` (800×600 → 480×270) and
`tools/cases/local/font_maxsized_snapshot.case.json` (800×600 → 1600×900). Both
capture `final_color` after a live resize round trip against
`assets/scenes/fixtures/text_rendering.scene.json`.

Required before any stage is called done:

1. Both existing cases, plus a new `local.font.native_snapshot` at 1920×1080 and
   a 3840×2160 variant where hardware allows. Text must be legible and
   stroke-weight-consistent at every extent. Replacement goldens need owner
   acceptance; the current goldens encode the defect.
2. A per-stage MTSDF comparison at 12, 18, 32, 64, and 128 px against the same
   string, confirming that stem weight and tracking are stable across sizes.
   The tracking test is the one that catches §2.2 regressions: measure the
   rendered advance of a 40-character run and confirm it matches
   `Σ advance × pxSize / unitsPerEm` within a quarter pixel.
3. A corner-fidelity check on a glyph with a sharp junction — `M`, `W`, `4` —
   confirming the ADR-035 shader removes the `fwidth(median)` notching. Compare
   against a same-size msdfgen CPU render.
4. Cooker determinism: two runs on the same input produce byte-identical
   `.vkfa`, matching the ADR-030 cooker contract.
5. `./build_test.sh` green, with new CPU tests in `tests/src/text_test.c`
   covering `.vkfa` round-trip, float layout advance accumulation, and
   codepoint lookup.
6. A focused Vulkan validation run on the text pass after the shader and format
   changes. Per AGENTS.md this is a separate diagnostic run, never folded into a
   baseline or performance command.
7. Release frame-time comparison at matched internal extents before and after
   F2, since RGBA16F atlas pages change resident bytes and sampler bandwidth.
   An unmeasured claim that this is free is not a result.

Delete the artifact tree in the same turn that produces it; carry out the
numbers and the reproducing command.

---

## 6. What nuri does, and what transfers

`nuri` is the other renderer in this workspace. It solved the same problem and
its shape is the reference for §3.

| Concern | nuri | VKR today |
|---|---|---|
| Generation | `compileNFontFromFontFile()` links msdf-atlas-gen: `FontGeometry::loadCharset` → `edgeColoringInkTrap` → `TightAtlasPacker` → `ImmediateAtlasGenerator<float,4,mtsdfGenerator>` | Manual CLI invocation; JSON + PNG checked in |
| Delivery | One binary `.nfont`: metrics, pxRange, glyphs, cmap, embedded atlas pages | Two files, parsed by a brace-unaware JSON scanner |
| Glyph metrics | `GlyphMetrics` — all `float`, UVs pre-normalized and pre-flipped | `int16_t` advances and bearings baked at one size; UVs truncated from float bounds at runtime |
| Layout scale | `scale = pxSize / unitsPerEm`, applied to float metrics per layout | Integer pixel metrics rescaled by `font_size / font->size` |
| Atlas format | `RGBA16_FLOAT` by default (`useRgba16fAtlas`), uncompressed | RGBA8, UASTC block-compressed, sRGB-tagged, mipmapped |
| Screen px range | `unitRange = pxRange / atlasSize` per vertex; `max(0.5 * dot(unitRange, 1/fwidth(uv)), 1.0)` per fragment | `fwidth(median(rgb) - 0.5)`; the CPU-computed range is uploaded and never read |
| Minification | Blends MSDF median toward the `.a` SDF channel below 2 px of range | `.a` never sampled |
| Multi-page / fallback | `AtlasPageHandle`, `localPageIndex`, validated fallback chains | Single page, `page_count = 1` |
| Kerning | Shaper-driven, float | Absent for MTSDF |
| Geometry | 6 vertices × `gl_InstanceIndex`, 32-byte `GlyphInstance` from a BDA buffer | 4 verts + 6 indices per glyph, rebuilt on the CPU |
| Build integration | msdf-atlas-gen as an `external/` submodule linked **only into the editor target**; the runtime library has no msdf dependency | None |

Transfers directly: the container concept, float glyph metrics with baked UVs,
the layout scale formula, the shader, the RGBA16F atlas, and the
editor/tool-only dependency boundary.

Does not transfer as-is: `std::pmr`, `Result<T, E>`, and the C++ pimpl shape.
VKR is C11 with `VkrAllocator`, arena pools, and `bool8_t`/error-enum returns.
The cooker is a tool and may be C++ — `vkr_vkt_packer.cpp` and
`vkr_meshoptimizer_bridge.cpp` set that precedent — but the runtime loader is
C11 behind the existing `VkrResourceLoader` vtable.

Also worth noting: nuri's `MtsdfParams` carries `outlineWidth` and `glow`, and
its 3D path carries `maxScreenSizePx` and billboard modes. VKR has no equivalent
and this document does not propose adding them. They are listed so the container
can reserve space rather than break later.

---

## 7. Open questions

1. **Does the `system` font type survive?** If the cooker can consume a `.ttc`
   with a CJK charset in reasonable time and atlas budget, the runtime
   stb_truetype rasterizer has no remaining purpose and F3 deletes it. If CJK
   coverage makes the atlas impractical, the system loader stays for that case
   and needs its own mip chain at minimum. Requires a cooker run against
   `NotoSansCJK-Regular.ttc` to answer.
2. **Charset policy.** nuri exposes only `Ascii` and `Latin1` presets. VKR's
   `.fontcfg` should carry an explicit charset or glyph-range list so coverage
   is an asset decision, not a tool default.
3. **`.vkfa` versus extending `.vkt`.** The atlas page is a texture and `.vkt`
   already handles versioned texture delivery. Embedding keeps a font one file
   and one hash, which ADR-034 argues is worth the duplication; the alternative
   is recorded there.
4. **Whether F0 is worth landing separately** or whether the team goes straight
   to F1. F0 is cheap and fixes real corruption, but it touches code F1 replaces.
