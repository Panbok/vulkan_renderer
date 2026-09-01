---
status: partial
updated: 2026-09-01
authority: adr
---

# ADR-036: DPI-derived UI text scale

## Status

**Accepted (partial).** Windows publishes `GetDpiForWindow() / 96`, macOS
publishes the window backing scale, and offscreen execution uses an explicit
finite positive scale defaulting to `1.0`. The window snapshot includes a
revision. UI consumes it only at resize/offscreen configuration boundaries,
scales every authored layout dimension before layout, and keeps the retained
text transform at unit scale.

Pure tests cover 1×, 1.25×, 1.5×, and 2× plus scale-only and extent-only
invalidation. Offscreen reports preserve 1×, and local macOS downsized and
maximized-class witnesses preserve the observed 2× scale in aggregate reports.
A real mixed-scale display transition and native Windows execution are not
available on the current host, so those acceptance gates remain open.

## Context

`vkr_ui_system_text_content_scale()` returns
`min(width / 800, height / 600)`, and
`vkr_ui_system_position_slot()` applies the result as a retained text-transform
scale. That path is compiled only for Windows. The primary overlays request 32
pixels on Windows and twice the loaded font size on macOS; the memory overlay
requests 24 pixels on Windows and 1.5 times the loaded font size on macOS. The
two production platforms therefore do not share a sizing model.

The current Windows scale has three independent problems:

- it treats output extent as display density, so resizing a window changes the
  nominal UI size even when the OS content scale is unchanged;
- `min()` against an 800 by 600 reference makes aspect ratio affect text size;
- applying scale after layout stretches advances, clipping, and wrap decisions
  that were solved at a different size.

The existing `font_size` comments call the value points, but current layout
treats it as pixels. The proposed `dpi / 96` formula is not a conversion from
typographic points. A true point is 1/72 inch. Windows effective DPI divided by
96 and macOS backing scale instead convert logical UI units into backing or
device pixels according to OS display policy. macOS also documents backing
scale as a coordinate conversion, not a measurement of physical panel density.

Per-Monitor V2 support provides most of the platform input but not the public
contract this decision needs. Windows establishes
`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`, sizes with
[`GetDpiForWindow()`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getdpiforwindow),
and handles `WM_DPICHANGED`. macOS converts through the window's
[`backingScaleFactor`](https://developer.apple.com/documentation/appkit/nswindow/backingscalefactor).
`vkr_window_get_pixel_size()` exposes physical output pixels, but `VkrWindow`
does not currently expose content scale or a content-scale revision to the UI
system. A resize event alone is not proof of a scale change, and a scale-only
change must not leave layout stale.

## Decision

Represent authored UI dimensions as logical UI units. Convert them to device
pixels with a per-window OS content scale before text layout. Do not apply the
same factor again through the text transform.

### Scale source and lifecycle

The window layer stores and exposes a finite positive `content_scale`:

- Windows: `GetDpiForWindow(hwnd) / 96.0f`;
- macOS: the relevant window or view backing scale;
- deterministic offscreen and headless execution: an explicit configured
  value, defaulting to `1.0f`.

Linux behavior is not specified because VKR has no production Linux window
backend. A future backend must define its compositor-scale mapping and event
semantics before joining this contract.

The window layer also publishes a monotonically changing scale revision, or an
equivalent event payload containing both pixel extent and content scale.
`WM_DPICHANGED`, macOS backing-property or screen changes, and initial window
creation update the stored value. The UI system consumes the new scale before
rebuilding layout. A change to scale dirties text and anchor layout even if the
pixel extent is unchanged; an extent change does not imply a scale change.

Harness reports record the effective content scale. Offscreen reports use the
configured value, so a fixed offscreen baseline does not vary with the machine
running it. Authored offscreen scale participates in the workload fingerprint.
Observed window scale participates in the environment fingerprint, and a
snapshot is incomplete if its child repetitions report different scales.

### Application point and unit contract

For each authored UI length:

```text
device_pixels = authored_logical_units * content_scale
```

Conversion occurs before `vkr_text_layout_compute()`. It applies consistently
to font size, letter and line spacing, maximum width and height, clipping
bounds, anchor padding, and slot offsets. Glyph em metrics are then multiplied
by the resulting device pixels per em.

Text transforms remain available for semantic animation or scene placement,
but they do not carry OS content scale. Removing the hidden transform factor
prevents double scaling and lets wrapping, clipping, kerning, and line breaks
operate in the dimensions that are rendered.

The implementation updates misleading `font_size` documentation to say
logical UI units for UI configuration and device pixels per em at the layout
boundary. It must not call these values typographic points unless a separate
72-DPI point conversion is actually implemented.

### Platform and application convergence

The platform-conditional UI scale and font-size branches are removed. The
application authors one logical size per overlay on Windows and macOS. The
window's content scale supplies the platform-dependent conversion.

This decision does not add a `VkrUiTextScalePolicy` abstraction. There is no
current second caller that requires design-extent sizing. If a shipped HUD
needs resolution-relative text, its accepted design may add an explicit
policy; retaining the current implicit 800 by 600 rule as a speculative option
would preserve unnecessary state and branches.

### Relationship to float metrics

Float em metrics from ADR-034 remove the current baked-size quantization and are
the preferred implementation sequence. They are not a semantic prerequisite
for applying content scale before layout: integer metrics would still produce
correct scale and wrap ordering, with lower precision. F3 follows the float
metric migration to reduce simultaneous variables and avoid preserving that
quantization in a new API.

## Consequences

UI text follows the operating system's logical UI scale. This aims for
consistent OS-scaled logical size across displays; it does not promise equal
physical inches on different panels.

Resizing a window at a fixed content scale no longer changes nominal text size.
Moving between displays with different OS scale rebuilds layout at the new
device-pixel size. Aspect ratio no longer changes text size implicitly.

Layout invalidation becomes a cold display-change operation. All retained text
and anchor geometry affected by a scale revision must rebuild once. No content
scale query, validation branch, allocation, or string work enters per-glyph or
per-draw hot paths.

Existing windowed output can change. The local downsized and maxsized cases are
observational cases with no accepted baselines. They can show resize behavior,
but they cannot prove mixed-DPI behavior unless the harness can request and
report an actual scale transition. Baseline changes, if proposed, require
normal owner review.

The authoritative mixed-scale gate therefore has two parts: a pure layout test
with injected scale values and a windowed witness on available 100, 125, 150,
or 200 percent Windows displays, or 1x and 2x macOS displays. Unsupported host
combinations are reported as unavailable, not silently treated as passed.

## Alternatives Considered

**Keep design-extent scaling but apply it before layout.** This fixes stretched
layout but continues to make window extent and aspect ratio determine UI size.
Rejected for desktop-style overlays.

**Use physical monitor DPI.** Physical size data is often absent or unreliable
and does not represent the user's accessibility scale. Rejected.

**Let every application pre-scale `font_size`.** This duplicates platform
policy and leaves the UI system without the same scale for padding and slots.
Rejected.

**Infer scale from framebuffer divided by logical window extent.** This can be
equivalent on some platforms, but becomes ambiguous when an internal render
scale exists. The window layer's explicit OS content scale is the authority.

**Render UI into a fixed-size target and composite it.** This adds a pass and
resamples already antialiased text. Rejected.

## Revisit When

- An internal render extent is added. UI content scale continues to follow the
  output window, not the internal rendering resolution.
- ADR-027's immediate-mode grid UI ships and owns scaling for every widget.
- A real game HUD requires a design-extent policy.
- A future Linux backend defines per-output fractional scaling.
