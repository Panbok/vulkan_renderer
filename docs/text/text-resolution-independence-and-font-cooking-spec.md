---
status: partial
updated: 2026-09-01
authority: design
---

# Text resolution independence and font cooking specification

**Document status:** Accepted and implemented through the F3 core. The defect
inventory below is the pre-implementation baseline. Native Vulkan/Windows,
mixed-display transition, cross-backend comparison, and alpha-fallback A/B
evidence remain open, so this document is not marked fully implemented.

**Scope:** Establish the evidence for VKR's text scaling defects, define the
runtime and offline target, and sequence the migration. Rationale for the three
accepted directions lives in
[ADR-034](../architecture/adr/034-offline-cooked-font-artifacts.md),
[ADR-035](../architecture/adr/035-canonical-mtsdf-screen-pixel-range-shading.md),
and [ADR-036](../architecture/adr/036-dpi-derived-ui-text-scale.md).

Related documents:
[mtsdf-font-loader-design.md](mtsdf-font-loader-design.md),
[system-font-loader-design.md](system-font-loader-design.md),
[font-system-design.md](font-system-design.md),
[ui-text-implementation-design.md](ui-text-implementation-design.md), and
[presentation-dpi-and-transfer-function-spec.md](../rendering/presentation-dpi-and-transfer-function-spec.md).

## Implementation status

| Stage | Status on 2026-09-01 | Remaining acceptance work |
| --- | --- | --- |
| F0 | Core implemented | The production shaders use pure canonical RGB MSDF. The optional alpha-SDF blend remains unaccepted until matched Metal/Vulkan A/B captures exist; native Vulkan execution is unavailable on the current host. |
| F1 | Implemented | The POSIX path passes locally. The checked-in Windows wrapper and build integration still require a native Windows run. |
| F2 | Core implemented | Cooked Ubuntu Mono is the default scalable UI face, and UI/world layout share float em metrics, integer lookup, and glyph-ID kerning. Fresh native Vulkan output and validation remain open. |
| F3 | Core implemented | Windows and macOS publish OS content scale, offscreen cases author it explicitly, and UI applies it before layout. A real mixed-scale display transition and native Windows witness remain unavailable. |
| F4 | Deferred by design | No authoritative profile identifies text geometry or upload work as a limiter. |

The remaining items are evidence or optional-policy gates, not permission to
restore the pre-F0 storage, shader, metric, lookup, or extent-derived scaling
paths. The legacy JSON-plus-PNG MTSDF loader remains only as a bounded rollback
until native Vulkan evidence closes.

---

## 1. Conclusion first

The reported resolution-scaling symptom has four causes with different owners:

1. **The default UI uses a fixed raster, not MTSDF.** On Windows the system
   font is rasterized at 128 pixels per em, while the primary overlays request
   32 logical pixels before an extent-derived transform. At the 800 by 600
   reference extent, the atlas is minified 4 times without mips. Small windows
   increase the minification. macOS follows separate application sizing
   branches.
2. **The MTSDF runtime quantizes scalable metrics.** Em-space advances,
   bearings, and line metrics are converted to integer pixels at one configured
   size, then rescaled. Half-texel atlas bounds are truncated as well.
3. **The stored field and shader violate the MTSDF sampling contract.** The
   atlas sidecar is tagged sRGB, block-compressed, and mipmapped. The shader uses
   `fwidth()` of the decoded median instead of reconstructing screen-pixel range
   from UV derivatives. A CPU range value uses the wrong denominator and is
   uploaded but ignored.
4. **Window extent is being used as UI density.** The scale is applied through
   the transform after layout. It changes with aspect and resize, does not
   follow the OS content scale, and cannot reflow text correctly.

Font production is also not reproducible. The repository contains generator
outputs but no tracked producer recipe or provenance. The empty kerning array
does not prove which generator option was used, and the runtime ignores kerning
in any case.

The target is a pinned offline cooker, a validated `.vkfa` artifact containing
float em metrics and linear uncompressed MTSDF pixels, canonical
derivative-based range reconstruction, direct glyph-ID lookup, and OS content
scale applied to logical UI dimensions before layout.

The safe order is strict: repair the atlas input and shader atomically, build
the artifact path, migrate float layout and default UI, then add window content
scale and retire proven-dead paths. Instancing remains optional and needs a
profile that identifies CPU text geometry as a limiter.

## 2. Evidence

### 2.1 Default UI raster scaling

`vkr_font_system_init()` selects
`assets/fonts/NotoSansCJK-Windows.fontcfg` as the default system font on
Windows. Its configured size is 128. `application_init_ui_texts()` requests 32
for the primary overlays and `application_init_memory_text()` requests 24.

`vkr_system_font_rasterize_glyphs()` rasterizes U+0020 through U+00FF with
stb_truetype into one RGBA atlas. The texture has one uploaded mip and
`VKR_MIP_FILTER_NONE`.

`vkr_ui_system_text_content_scale()` returns
`min(width / 800, height / 600)` on Windows, and retained text transforms carry
that value. The primary 32-unit overlay therefore has this nominal scaling:

| Client extent | extent scale | rendered em in device px | 128 px atlas minification |
| --- | ---: | ---: | ---: |
| 480 by 270 | 0.450 | 14.4 | 8.9x |
| 800 by 600 | 1.000 | 32.0 | 4.0x |
| 1280 by 720 | 1.200 | 38.4 | 3.3x |
| 1600 by 900 | 1.500 | 48.0 | 2.7x |
| 1920 by 1080 | 1.800 | 57.6 | 2.2x |
| 2560 by 1440 | 2.400 | 76.8 | 1.7x |
| 3840 by 2160 | 3.600 | 115.2 | 1.1x |

A single-mip bilinear sample cannot integrate the source texel footprint under
heavy minification. Aliasing or unstable coverage is therefore expected, but
the visual degree must come from captures rather than inference.

The `min()` rule also makes aspect ratio affect size. Because the rule is a
transform, wrapping and clipping are solved at the authored size and stretched
afterward. The layout does not reflow in the dimensions ultimately rendered.

On macOS the primary overlay uses `font->size * 2.0f` and the memory overlay
uses `font->size * 1.5f`. VKR has no production Linux window backend. The
platforms in production do not share one authored-size contract.

### 2.2 MTSDF metric quantization

`vkr_mtsdf_build_font()` computes a target-size scale and writes line metrics
as rounded `int32_t` values. It writes advance and offsets into `int16_t`
fields. At an 18-pixel configured size, rounding can contribute up to half a
pixel per stored value before later layout scaling. The error accumulates along
a run and is magnified by the retained transform.

The UI geometry path mixes conventions. MTSDF quad extents come from float
`planeBounds * font_size`, while the origin and advance use quantized stored
fields. The glyph plane and advance box can therefore disagree even before
transform scaling.

The source values from msdf-atlas-gen are em-normalized. If `.vkfa` stores them
in em units, the correct runtime conversion is:

```text
device_value = em_value * requested_device_pixels_per_em
```

Dividing em-normalized values by the source font's TrueType `unitsPerEm` would
be a second, incorrect normalization. Source `unitsPerEm` is useful provenance,
not the runtime scale denominator for this representation.

### 2.3 Atlas-bound truncation

msdf-atlas-gen emits fractional atlas bounds. U+0021 in
`assets/fonts/Ubuntu-2d.json` includes:

```json
"atlasBounds": { "left": 948.5, "bottom": 973.5, "right": 965.5, "top": 1023.5 }
```

The loader casts these bounds into integer glyph fields. The stored origin is
half a texel away from the generated rectangle. `uv_inset_px` cannot repair a
coordinate conversion error without also changing the relationship between
the distance field and plane bounds.

The cooked representation stores normalized float UV bounds with the vertical
convention already applied.

### 2.4 JSON field lookup escapes its object

`vkr_json_find_field()` scans forward without tracking brace depth.
`vkr_mtsdf_parse_atlas()` searches from the atlas object for `emSize`, which is
not an msdf-atlas-gen atlas field, and finds `metrics.emSize` later in the
document. That value is `1` in the checked-in artifacts, so the bug currently
produces the correct em-to-pixel multiplier by accident.

`atlas.size`, 64 for `Ubuntu-2d.json`, is parsed and validated but unused. It is
quality/provenance metadata and is the denominator in an axis-aligned
pixels-per-em estimate. Canonical shader range reconstruction instead needs the
distance range and actual atlas page dimensions.

### 2.5 Dead and incorrect CPU range

UI and world text preparation compute:

```c
screen_px_range = Clamp(
    font->sdf_distance_range * (render_size / font->em_size), 1.0f, 4.0f);
```

The Vulkan path uploads the result, but the shader does not read it. For the
Ubuntu artifact at 18 pixels per em, the expression computes `8 * 18 / 1` and
clamps 144 to 4. An axis-aligned estimate based on the generated 64 atlas pixels
per em would be `8 * 18 / 64 = 2.25`.

A correct draw-level constant is not a screen range. It is the two-component
atlas unit range:

```text
unitRange = (distanceRange / atlasWidth, distanceRange / atlasHeight)
```

The fragment shader combines it with UV derivatives, which also covers
perspective and rotation for world text.

### 2.6 Shader reconstruction and alpha policy

Both backends currently compute alpha from the median RGB distance divided by
`fwidth(distance)`. `median3` changes its selected channel around corners, so
the derivative follows a piecewise channel selection. The msdf-atlas-gen
reference instead derives a projected texel footprint from the component-wise
magnitude of `ddx(uv)` and `ddy(uv)`, then converts through `unitRange`. It
documents `1 / fwidth(uv)` only as an approximation.

The alpha channel of MTSDF contains a true single-channel signed distance. It
can be useful under minification, but the specific `saturate(2 - range)` blend
in `nuri` is project policy, not part of the canonical upstream range formula.
It needs an A/B comparison with pure RGB MSDF and pure alpha SDF before VKR
accepts a threshold.

### 2.7 Atlas storage corruption

The generic texture packer scans `assets/textures/`. The Ubuntu atlas filename
matches no data-texture token, so its sidecar records `color_srgb`, UASTC, and a
box-filtered mip chain. The loader supplies no explicit request class, allowing
sidecar metadata to select that path.

sRGB decoding is categorically wrong for an encoded signed distance because it
changes the location and slope around the nominal 0.5 boundary. Lossy block
compression and generic box mips can also change relative color-channel values
and encoded distances. They are not accepted inputs without visual and numeric
evidence for a specific MTSDF-aware scheme.

A `?cs=linear&tc=data-mask` query does not complete F0. `data-mask` may still
select a lossy GPU block format, and a stale sidecar remains discoverable. F0
must delete the sidecars and move loose atlases out of the generic texture
input set, or add a verified exclusion, in the same change.

### 2.8 Runtime and ownership defects

- The MTSDF loader does not populate kerning. The checked-in JSON's empty
  kerning list has unknown provenance.
- `vkr_ui_text_find_glyph()` formats codepoints as decimal strings during
  geometry generation and falls back to a linear scan. The loader also creates
  string keys per glyph. Cooked text needs an integer codepoint-to-glyph-ID
  map.
- All font loaders populate `VkrFont::atlas_cpu_data`, but no current consumer
  reads it. The MTSDF loader decodes the PNG again for this copy. A 2048-square
  RGBA8 image needs 16 MiB and fails in the 6 MiB font pool chunk.
- Bitmap, system, and MTSDF paths maintain separate loaders, caches, metric
  conventions, and fallback behavior. An unresolved UI font handle falls back
  to the bitmap default rather than the intended scalable UI asset.

These are migration targets, not permission to delete every legacy path in F0.
Usage and rollback evidence decide retirement.

### 2.9 Window content-scale boundary

Per-Monitor V2 setup is already shipped on Windows, including
`GetDpiForWindow()` and `WM_DPICHANGED` handling. macOS uses backing-coordinate
conversion. The public window API exposes pixel extent but not content scale or
a scale revision, so UI cannot reliably distinguish resize from a scale-only
display transition.

`dpi / 96` and macOS backing scale are logical-to-device coordinate scales.
They are not physical-DPI measurements and do not convert typographic points.
The target promises consistent OS-scaled logical UI size, not identical
physical inches across displays.

## 3. Target design

### 3.1 Reproducible `.vkfa` artifact

`vkr_font_cooker` links a pinned `vendor/msdf-atlas-gen` only in an offline C++
tool. Its `.fontcfg` input fixes source face, collection index, charset,
variable axes, field type, atlas size and range, padding, edge-coloring settings
and seed, fallback policy, and pixel format.

The versioned little-endian `.vkfa` file contains:

- a bounded header and section directory with file size, version, field kind,
  fallback glyph ID, counts, source-and-settings identity, and checksums;
- finite float em metrics where one em equals `1.0`;
- sorted unique glyph-ID records with float plane bounds, advance, normalized
  UVs, and page index;
- a sorted unique codepoint-to-glyph-ID map and glyph-ID-pair kerning table;
- symmetric distance range and atlas pixels per em as separate metadata;
- a page directory and embedded, checksummed atlas payload.

The exact byte layout is frozen during F1 in a shared format header and tested
with golden bytes and round trips. No implementation dumps native structures.
Version 1 production loading accepts one MTSDF page. The schema can describe
pages, but runtime multi-page support waits for a draw-partition design.

The baseline atlas is linear, uncompressed, single-mip RGBA8 UNORM. RGBA16F is
an opt-in experiment because it doubles page residency. It is not the default
until same-glyph captures show a material improvement after all other defects
are fixed.

### 3.2 Cold loader and runtime representation

The C11 loader validates the complete file before publishing a font or GPU
atlas: bounds and overflow, section overlap, checksums, finite values, sorted
uniqueness, glyph and page references, UV order and range, field type, pixel
format, row stride, exact payload size, and the v1 one-page restriction.
Partial failure releases every temporary allocation and GPU handle through one
cleanup path.

Cooked glyph records remain in float em units. Layout multiplies them by device
pixels per em. Lookup uses integer codepoints and glyph IDs. No formatting,
allocation, file access, validation, or atlas query occurs per glyph or per
draw.

The prepared draw carries `float2 unit_range`. It does not carry the current
CPU-computed `screen_px_range`.

### 3.3 MTSDF shading contract

Both backends reconstruct:

```hlsl
float2 dx = ddx(uv);
float2 dy = ddy(uv);
float2 gradient_squared = max(dx * dx + dy * dy,
                              float2(1e-12f, 1e-12f));
float2 screen_tex_size = rsqrt(gradient_squared);
float range = max(0.5f * dot(unit_range, screen_tex_size), 1.0f);
float sd_msdf = median3(atlas.rgb) - 0.5f;
float alpha = saturate(range * sd_msdf + 0.5f);
```

The candidate alpha fallback is evaluated independently. UI stays blended.
Picking uses the same reconstructed coverage. Depth-writing world text may use
an explicit domain-specific discard threshold represented in both backend root
ABIs.

No canonical-range shader change may land against the current packed sidecar.
Clean atlas storage, field declaration, range metadata, and both backend ABI
changes form one F0 landing boundary.

### 3.4 Asset and build policy

Loose transition atlases and cooked font pages never enter the generic texture
packer. Stale `.vkt` sidecars are deleted when the exclusion lands. Generated
`.vkfa` files are ignored build products; source fonts and cooker configuration
are committed only when their licenses permit redistribution.

Cooking is deterministic and skips an unchanged source-and-settings identity.
Whether root wrappers invoke it automatically or production assets are prepared
through an explicit wrapper is settled during F1. Either choice must stage the
artifact after a successful cook and fail the requesting workflow on cook or
copy error.

### 3.5 Logical UI scale before layout

The window publishes a finite positive content scale and a change revision.
Windows uses `GetDpiForWindow() / 96`; macOS uses the relevant backing scale;
offscreen execution uses an explicit configured value that defaults to `1`.
There is no Linux contract until a Linux window backend exists.

The UI converts all authored logical dimensions before layout:

```text
device_pixels = authored_logical_units * content_scale
```

This includes font size, letter and line spacing, maximum dimensions, clipping,
anchor padding, and slot offsets. The retained text transform does not also
carry content scale. A scale revision dirties layout even if the pixel extent
does not change.

The code and documentation must stop calling these authored values points.
`dpi / 96` is a logical-UI scale. It is not the `dpi / 72` conversion required
for typographic points.

### 3.6 Deliberate non-goals

This plan does not add shaping, bidirectional text, dynamic glyph generation,
full CJK coverage, a multi-page draw strategy, a speculative design-extent
policy, or instancing without measurement. Glyph IDs and an explicit charset
keep those options open without placing them in current hot paths.

## 4. Five-stage plan

### F0: Correct loose-atlas input and shading

Land as one rollback unit:

1. move the loose MTSDF atlas outside generic texture inputs or add a verified
   packer exclusion;
2. delete stale atlas `.vkt` sidecars and request linear, uncompressed,
   single-mip sampling;
3. preserve float atlas bounds and validate explicit atlas size, symmetric
   range, and MTSDF field kind;
4. replace the scalar CPU range with two-component `unit_range` in UI, world,
   Vulkan, and Metal contracts;
5. implement canonical UV-derivative reconstruction and A/B the candidate alpha
   fallback.

F0 is a short-lived quality stopgap. If F1 is ready to land immediately, F0 may
be folded into it, but the atlas correction still precedes shader evaluation
inside the change.

### F1: Add cooker, format, and loader

Pin the dependency; implement deterministic `.fontcfg` parsing, artifact
identity, and cooker output; freeze the byte layout; add the validated C11
loader; integrate Windows and POSIX preparation/staging; cook the explicit
production charset; and keep JSON-plus-PNG as rollback.

Version 1 ships RGBA8 and exactly one page. Unsupported page counts, field
types, versions, or asymmetric ranges fail at the cold boundary.

### F2: Migrate float layout and default UI

Add float em glyph records to `VkrFont`, direct integer lookup, glyph-ID-pair
kerning, and float layout. Convert UI and world geometry to the same metric
contract. Switch the default UI to the cooked MTSDF asset after coverage and
fallback-glyph tests pass.

Do not delete the system raster path in the same first migration commit. Keep a
bounded rollback until the default UI, world text, picking, backend matrix, and
required character coverage pass.

### F3: Add content scale and retire proven-dead paths

Expose per-window content scale and revision, record it in harness reports,
scale every authored UI length before layout, remove the hidden transform
scale, and remove platform-specific application sizes. Test scale-only changes
separately from resize.

After migration evidence, remove only paths with no remaining owner: obsolete
JSON parsing, write-only CPU atlas copies, string glyph keys, and unused default
font fallbacks. Retain bitmap or system-raster loading if a real caller still
requires fixed-pixel or dynamic coverage.

### F4: Optional measured instancing

First add focused CPU timing or counters that distinguish layout, glyph lookup,
geometry rebuild, upload, and draw submission. Convert retained text to a unit
quad plus glyph instances only if an authoritative profile shows text geometry
or upload work on the critical path. Preserve picking, world depth semantics,
and both backend contracts.

Without that evidence, F4 is not work to schedule.

## 5. Evidence gates

The following are implementation gates. Editing this proposed document does not
satisfy them.

### 5.1 F0 gates

- Assert the font atlas resolves to linear non-sRGB, uncompressed, one-mip
  storage on Vulkan and Metal. Record the reported GPU format.
- Prove no stale atlas `.vkt` is selected after a clean and an incremental
  build.
- Unit-test rectangular-page `unit_range` and known transform derivatives.
- Capture pure MSDF, pure alpha SDF, and candidate blends at several projected
  ranges, including corner glyphs such as `M`, `W`, `4`, and `@`.
- Run `local.font.downsized_snapshot` and `local.font.maxsized_snapshot` for
  observational reports. They have no accepted baselines today.
- Run the offscreen text snapshot with final-color and picking-ID captures on
  both backends.
- Run one focused Vulkan validation pass and one focused single-process Metal
  API/shader validation pass. Keep validation out of baseline and performance
  runs.

No baseline is accepted automatically. A changed capture is proposed and
reviewed through the normal owner workflow.

### 5.2 F1 gates

- Two cooks from identical inputs produce byte-identical output and the second
  normal cook skips work.
- Changing font bytes, face index, charset, an axis, a generator setting, tool
  version, or pinned commit changes artifact identity.
- Golden-header and round-trip tests fix the exact v1 layout on supported host
  compilers.
- Truncated, oversized, overlapping, trailing, checksum-invalid, non-finite,
  unsorted, duplicate, bad-reference, bad-UV, unsupported-version,
  unsupported-format, and multi-page v1 fixtures fail without publishing a
  handle or leaking a GPU resource.
- Windows and POSIX wrappers stage a usable artifact on clean and incremental
  builds and report failures.
- Record cook time, `.vkfa` bytes, peak temporary decode bytes, and GPU resident
  atlas bytes separately.

### 5.3 F2 gates

- Pure layout tests cover advances, negative bearings, kerning, fallback glyph,
  multiline height, wrapping, clipping, non-integer sizes, UTF-8 mapping, and
  long-run accumulation against known em metrics.
- Assert that geometry rebuild performs no per-glyph formatting, heap
  allocation, file access, or linear full-table fallback.
- Capture the default overlays, world text, and picking IDs on Vulkan and Metal.
- Verify the selected production charset and fallback glyph against every
  shipped overlay string. Do not infer full CJK coverage from the source font's
  filename.
- Run `./build_test.sh`, the cold/warm production pipeline-cache sequence, and
  focused validation as required by the touched shader and resource paths.

### 5.4 F3 gates

- Pure tests inject `1.0`, `1.25`, `1.5`, and `2.0` content scale and verify
  size, advance, wrapping, clipping, padding, and slot offsets. Assert that the
  transform does not apply the factor again.
- Verify a scale-only revision dirties layout and an extent-only resize does not
  invent a density change.
- Offscreen reports explicitly record `content_scale = 1.0` unless a case
  overrides it.
- On available hardware, move a window between mixed-scale Windows displays or
  1x and 2x macOS displays. Record actual reported scale and stale-layout
  behavior. If the host cannot provide the transition, mark the gate
  unavailable and retain the pure seam test as non-equivalent evidence.
- Re-run windowed resize cases as behavior witnesses, not performance evidence.

### 5.5 F4 gates

- Use a Release performance case and profile with documented warmup,
  repetitions, effective configuration, and validation disabled.
- Compare matched before and after runs. Report CPU layout/rebuild/upload time,
  GPU text-pass time when available, draw count, instance count, upload bytes,
  and allocator deltas.
- Keep the implementation only if the identified limiter improves without a
  frame-time, hitch, memory, picking, or backend regression.

Retained harness output is not the record. Carry commands, report digests, and
numbers into the task record, then remove regenerable artifact trees in the
same implementation turn.

### 5.6 Implementation record — 2026-09-01

- `./build_test.sh` passes under the default Debug ASan/UBSan configuration.
  It includes exact VKFA golden bytes, exhaustive malformed-container
  rejection, atomic-publication collision/retry coverage, production
  charset/overlay coverage, float layout, derivative range, atlas request, and
  content-scale tests. It also forces two independent production cooks,
  byte-compares them, and verifies that the following normal cook skips. The
  incremental pack step discovers 2,093 generic textures and neither discovers
  nor recreates a font atlas sidecar.
- `./build_release.sh` passes and compiles both production text shaders. The
  cooker identity self-test covers source bytes, face index, charset, axes,
  generator settings, cooker version, and both pinned generator commits.
- Forced cooks from identical inputs produced byte-identical 4,205,472-byte
  artifacts with SHA-256
  `b13580443fd0eaa4ec8c229173eee73a109b9cfc48c4de080a135931f96c3279`.
  A warm forced Release cook reported `cook_ms = 463` and
  `format_temp_bytes = 4,205,878`; a normal follow-up cook skipped. The identity
  is
  `bb33e34134d874db1abdd8bfa5b8e49eda61357cf9536296f0fb30923a4f2fd4`;
  Debug runtime decode scratch peaks at 4,216,309 bytes, and the 1024-square
  RGBA8 page occupies 4,194,304 GPU bytes.
- Two normal Release application launches against one fresh explicit pipeline
  archive both exited successfully. The cold launch created a non-empty
  4,044,208-byte Metal archive; the warm launch loaded the existing archive and
  left the same size and SHA-256
  `ff69ccef1c9ebe3e2380cabe0a8c68d80a20c37415353bc9533400e52a24cc78`.
- The validation-disabled Metal offscreen text fixture passes with report
  `sha256:c0f03d9ee1c4e44ac86669e044b8849879e7b0ff03fb326be4f18c411891f326`.
  Its 960 by 540 final-color and picking captures were inspected, and its
  effective configuration records `content_scale = 1.0`.
- A strictly serial Metal API plus GPU/shader-validation run passes with report
  `sha256:ecc6389a75c53836b05d04020efa31177fdcec0ea8fd443fe5818d1b94ee0bcc`.
  The child log confirms both validators were enabled and contains no
  validation error.
- The maximized-class and downsized native resize witnesses pass with reports
  `sha256:a9d7b4e82c6a10e198e926ba179ef47bd32d3b203cfc9f56d7bf6e8f076c2779`
  and
  `sha256:e41bde6b0ee4111e5e2171abdd0c2adb7e7614037758437dee6dacc4a52477a4`.
  Both aggregate reports preserve the observed macOS content scale of `2.0`;
  captures are 3200 by 1800 and 960 by 540 respectively and were inspected.
- A Vulkan-pinned run under `VK_LAYER_KHRONOS_validation` is unavailable on
  this macOS build before device creation. Report
  `sha256:6b3ceb68d3e4b718afc12fbc0930f0abdbbf5c5f00527ccb7b7cfcc9dd242420`
  records zero completed repetitions and the explicit unavailable-backend
  error. It is not counted as Vulkan validation evidence.

All harness reports above are local, dirty-tree, and baseline-free diagnostic
or behavior evidence. No baseline was accepted, and the regenerable run trees
were removed after these values were transcribed.

## 6. Comparison with `nuri`

| Area | `nuri` evidence | VKR transfer | Caveat or rejection |
| --- | --- | --- | --- |
| Producer | Links msdf-atlas-gen into an offline compiler | Pin the dependency in `vkr_font_cooker` | Record full settings identity and licensing |
| Runtime dependency | Runtime does not link generator code | Same separation | Required, not optional |
| Metrics | Stores generator float metrics and scales at layout | Store float values in em units | Do not divide em-normalized values by source `unitsPerEm` |
| Glyph identity | Carries codepoint mapping with serialized glyph records | Use codepoint to glyph ID plus glyph-ID kerning | Supports future shaping without adding it now |
| UVs | Serializes normalized float UVs | Apply the vertical convention offline | Loader still validates order and bounds |
| Atlas | Embeds RGBA16F pixels | Embed pages in `.vkfa` | Start with RGBA8; 16F needs A/B and residency evidence |
| Range shader | Uses `unitRange` and the documented `1 / fwidth(uv)` approximation | Start with the exact upstream derivative magnitude | Store two components for rectangular pages; approximate only after visual and GPU evidence |
| Alpha fallback | Blends toward true SDF below range 2 | Evaluate as a candidate VKR policy | Not part of the upstream canonical formula |
| DPI | Caller supplies a pixel size | Apply window content scale before VKR layout | VKR must define and publish the window-scale boundary |
| Instancing | Uses a scalable draw representation | Consider only after profiling | Current retained geometry may already be adequate |
| Feature breadth | Supports effects and larger text systems | Preserve glyph IDs and field kind | Do not import unused effects, shaping, or dynamic atlases |

The transferable lesson is offline ownership of generator output and a float
runtime contract. The comparison does not establish VKR's pixel format, alpha
threshold, scale semantics, or performance result.

## 7. Resolved decisions and remaining questions

1. **Production coverage and fallback (resolved):** Ubuntu Mono Regular is
   retained under the Ubuntu Font Licence 1.0. One 1024-square page covers
   U+0020 through U+00FF, every shipped overlay string is checked in the CPU
   suite, and U+003F is the fallback.
2. **Cook integration (resolved):** Root Debug, Release, run, and test wrappers
   invoke the incremental cooker after a successful compile and fail on cook
   failure. Standalone POSIX and Windows wrappers remain available.
3. **Alpha fallback (open):** No blend is accepted. Production uses pure
   canonical RGB MSDF until matched pure-MSDF, pure-SDF, and candidate-blend
   evidence exists on both native backends.
4. **Atlas precision (resolved for v1):** RGBA8 remains the version-1 baseline.
   RGBA16F requires new same-glyph visual and residency evidence.
5. **Multi-page need (not demonstrated):** Current accepted coverage fits one
   page. A larger coverage set must justify a separate draw-partition or array
   design.
6. **Legacy loaders (bounded):** Bitmap and system-raster fonts still have real
   harness fixture callers. JSON-plus-PNG MTSDF remains a rollback path until
   native Vulkan acceptance; write-only CPU atlas copies are removed.
7. **Instancing (deferred):** No measured limiter justifies F4.

Shaping, bidirectional layout, dynamic glyph generation, speculative
design-extent scaling, and full CJK are not current open implementation tasks.
They require a product need and separate evidence before entering scope.
