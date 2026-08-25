---
status: proposed
updated: 2026-08-25
authority: adr
---

# ADR-036: DPI-derived UI text scale

## Status

Proposed. No production code implements this decision.

## Context

`vkr_ui_system_text_content_scale()` returns
`min(width / 800, height / 600)` and `vkr_ui_system_position_slot()` applies
that value through `vkr_transform_set_scale()` on each retained text transform.
Both are compiled only under `PLATFORM_WINDOWS`; every other platform gets a
constant `1.0`.

The application compounds it. `application_init_ui_texts()` in `app/src/main.c`
sets `text_config.font_size = 32.0f` under `#if defined(_WIN32)` and
`font->size * 2.0f` otherwise. With the Windows default system font baked at
128 px by `vkr_system_font_rasterize_glyphs()`, that means the Windows HUD
minifies a 128 px raster by `128 / (32 × content_scale)` while macOS and Linux
magnify the same raster by a constant 2×.

The scaling table is in
[text-resolution-independence-and-font-cooking-spec.md](../../text/text-resolution-independence-and-font-cooking-spec.md)
§2.1. The short version: minification runs from 8.9× at a 480×270 client area
down to 1.1× at 3840×2160. Text is sharp only where the window extent happens to
put the effective size near the baked raster size, which is around 4K, and the
worst case is a small window rather than a large one.

Three further problems with the formula itself, independent of the raster.

`min()` against a 4:3 design extent means a 16:9 window is scaled by its height.
Going from 800×600 to 1600×900 doubles the horizontal pixel count but scales
text by 1.5, so text occupies a smaller fraction of a wide window than of a tall
one at equal height. Aspect ratio silently changes text size.

Applying the scale as a **transform** rather than to the authored size means
layout is solved at the design size and stretched afterward. Wrap points, line
breaks, and `max_width` clipping are all computed against 800×600 geometry and
then scaled, so a wrapped paragraph does not re-wrap when the window changes.
Combined with the integer-quantized advances the spec's §2.2 documents, the
per-glyph rounding error is multiplied by the scale factor and accumulates
along a run — up to ±1.8 device pixels per glyph at `content_scale = 3.6`.

And the scale tracks *resolution*, not *density*. Two 27-inch monitors at 1080p
and 4K get text at 1.8× and 3.6×, which is correct for a game HUD authored
against a fixed design extent and wrong for desktop UI, where the same physical
size is the expectation. VKR's overlays are the latter — a metrics readout, a
camera position, an FPS counter.

[presentation-dpi-and-transfer-function-spec.md](../../rendering/presentation-dpi-and-transfer-function-spec.md)
already made the correct input available. `vkr_platform_init()` establishes
`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` before window creation, the Windows
window layer uses `GetDpiForWindow()` with `AdjustWindowRectExForDpi()` and
handles `WM_DPICHANGED`, and `vkr_window_get_pixel_size()` reports physical
output pixels on Windows and macOS. The DPI is known and authoritative; the text
system does not consume it.

`nuri` has no design-extent scale at all. `TextStyle::pxSize` is a real
device-pixel size, `computeFontScaleInfo()` derives `scale = pxSize /
unitsPerEm`, and `scaleGlyphMetrics()` applies it to float metrics at layout
time. The caller supplies the density.

## Decision

Replace the design-extent transform scale with a DPI-derived scale applied to
the authored point size before layout.

**Scale source.** `VkrWindow` exposes a content scale. On Windows it is
`GetDpiForWindow(hwnd) / 96.0`; on macOS it is the view's `backingScaleFactor`;
on Linux it is the compositor's per-output scale. The value changes on
`WM_DPICHANGED` and on the equivalent platform events, and it reaches the UI
system through the existing resize path rather than a new one.

**Application point.** The scale multiplies `VkrUiTextConfig::font_size` before
`vkr_text_layout_compute()` runs. It does **not** multiply the text transform.
Layout therefore runs at the real device-pixel size: advances, kerning, wrap
points, line breaks, and `max_width` clipping are all solved where the text is
actually drawn. `vkr_transform_set_scale()` on a text slot returns to identity.

Anchor padding and slot positions are scaled by the same factor, so a 10-pixel
inset stays a 10-point inset.

**One path for all platforms.** The `#if defined(PLATFORM_WINDOWS)` guard in
`vkr_ui_system_update_text_content_scale()` is removed, and the
`#if defined(_WIN32)` font-size branches in `application_init_ui_texts()` and
`application_init_memory_text()` are removed. Overlays author one point size on
every platform.

**Design-extent scaling remains available as an explicit policy.** A game HUD
that genuinely wants text to grow with resolution keeps that behaviour through a
per-slot `VkrUiTextScalePolicy` on `VkrUiTextSlot` — `DPI` or `DesignExtent`
with an authored reference extent. The default is `DPI`. What this decision
removes is the *implicit, platform-conditional, aspect-sensitive* version of it
hardcoded in the system.

**Dependency.** This decision is only worth landing on top of float glyph
metrics. Scaling the authored size instead of the transform reduces layout error
only if `vkr_text.c` stops rescaling integer pixel advances, which
[ADR-034](034-offline-cooked-font-artifacts.md) supplies. Landing DPI scale
against the current quantized metrics moves the error rather than removing it.

## Consequences

Text renders at one physical size regardless of display resolution, which is the
correct behaviour for the overlays VKR actually draws. A 1080p and a 4K monitor
of the same physical size show the same text.

Layout becomes correct under resize. Because the authored size scales before
layout, a wrapped paragraph re-wraps when the window changes and clipped text
re-clips. The current code cannot do either.

The aspect-ratio sensitivity disappears. `min()` against a 4:3 reference is gone.

Platform behaviour converges. One expression, one code path, one authored size.
The macOS and Linux 2× magnification and the Windows minification ladder both
stop existing.

Layout runs more often. Today a resize only rewrites transforms; after this
change a DPI change dirties layout for every text slot. That is bounded — 16
corner-anchored slots per `VkrUiSystem` — and happens on a monitor move or a
scale change, not per frame. It is not a hot path, but the dirty-flag path in
`vkr_ui_text_set_config()` must actually be exercised by the DPI change rather
than bypassed.

Every windowed text golden changes, on every platform, including the two
existing harness cases. The spec's evidence gates enumerate the required runs;
fixed-extent offscreen goldens are unaffected, matching what
`presentation-dpi-and-transfer-function-spec.md` §2.3 already records.

The mixed-DPI evidence that spec lists as pending now has a second consumer.
Moving a window between 100%, 125%, 150%, and 200% displays must produce a
coherent text-size change with no stale layout, and that becomes part of the
same validation run rather than a separate one.

## Alternatives Considered

**Keep the design-extent scale, just fix the aspect handling and apply it to
size instead of transform.** Preserves current visual behaviour at 800×600 and
fixes the layout-stretch bug cheaply. Rejected as the default because it keeps
resolution, not density, as the input, which is wrong for desktop overlays. It
survives as the opt-in `DesignExtent` policy.

**Scale by the framebuffer-to-logical-pixel ratio instead of DPI/96.** On macOS
these are the same number; on Windows with Per-Monitor V2 they are also the
same, since the client area is reported in physical pixels. Rejected as the
stated source only because `dpi / 96.0` is the value the platform actually
publishes on Windows and is meaningful when the two diverge — for example under
a future internal render-scale feature, which the DPI spec explicitly keeps
separate from output extent.

**Let the application own the scale entirely and pass a pre-scaled
`font_size`.** Minimal renderer change; the app already computes platform
branches. Rejected because it pushes a platform-specific correctness concern
into every caller and because the UI system needs the scale anyway for anchor
padding and slot placement. The current `#if defined(_WIN32)` sizes in
`app/src/main.c` are the evidence that this does not work.

**Render UI text into a fixed-extent offscreen target and composite.** Makes UI
scaling a single blit parameter and decouples it from the swapchain entirely.
Rejected: it costs a target and a pass, it resamples already-antialiased text,
and it makes crisp text at native resolution impossible — the exact property
SDF text exists to provide.

## Revisit When

- An internal render-scale feature lands. `presentation-dpi-and-transfer-function-spec.md`
  §1 explicitly reserves internal render extent as a separate field from output
  extent; UI text must scale against the output extent, not the internal one,
  and that distinction needs stating once the field exists.
- The immediate-mode grid UI of [ADR-027](027-immediate-mode-grid-ui.md) is
  implemented. A real layout engine owns scaling for all widgets, not just text,
  and the per-slot policy here should fold into it rather than coexist.
- A shipped game needs HUD text that scales with resolution across the whole UI.
  That is a request to change the default policy, not to change this decision.
- Linux windowing gains fractional per-output scaling that the platform layer
  cannot report as a single number.
