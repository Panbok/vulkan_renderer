---
status: partial
updated: 2026-09-01
authority: design
---
# Presentation DPI and transfer-function specification

**Document status:** Implemented through C3 and Metal render-scale phases R1-R2.
Mixed-DPI/display fixture evidence, owner acceptance of replacement final-color
goldens and MetalFX moving quality, Vulkan scaling, and editor integration
remain pending.

**Scope:** Two independent presentation defects plus the Metal internal
render-scale boundary accepted by ADR-039.

## 1. Decision

Windows uses Per-Monitor V2 DPI awareness, and both renderers write linear
display color to an sRGB output attachment. The changes retain separate
evidence requirements.

Do not implement render scale by changing the physical window size returned by
`vkr_window_get_pixel_size()`. Internal render extent and output extent are
different facts and need different fields.

Metal now implements that separation. Scale `1.0` remains the default. Vulkan
and editor compositor mode reject non-unit scale.

## 2. Windows DPI correctness

### 2.1 Implemented behavior

`vkr_platform_init()` establishes
`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` before window creation and fails
with the Win32 error code if that request fails. `vkr_window_windows.c` uses the
new hidden window's authoritative `GetDpiForWindow` result with
`AdjustWindowRectExForDpi` before the first `ShowWindow` call, uses the same path
for later explicit resizes, and applies the `WM_DPICHANGED` suggested rectangle.

`WM_SIZE`, `GetClientRect`, `WM_MOUSEMOVE`, cursor recentering, and application
picking now share the Per-Monitor V2 physical client-pixel domain. Window
creation logs the effective awareness, window DPI, and output extent once.
`vkr_window_get_pixel_size()` therefore reports physical output pixels on both
Windows and macOS.

### 2.2 Required behavior

1. Set `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` before creating any
   top-level window. Put the call at a process or platform initialization
   boundary, not in a path that may run after the first `HWND` exists. A
   manifest is also valid, but do not declare both and ignore a conflict.
2. Fail initialization with the exact Win32 error if the requested awareness
   mode cannot be established. A silent fallback would make window extent
   ambiguous again.
3. Size new and existing windows with DPI-aware non-client calculations. Use
   the target monitor's DPI for initial placement and `GetDpiForWindow` for an
   existing window. Do not assume the system DPI equals the destination
   monitor's DPI.
4. Handle `WM_DPICHANGED`. Apply the suggested rectangle from `lParam` with
   `SetWindowPos`, then let the existing resize mailbox drive target
   recreation.
5. Keep `WM_SIZE`, `GetClientRect`, cursor coordinates, and picking in the
   same client-pixel domain. Audit capture-mode recentering, cursor warps,
   `ScreenToClient`, and the application path that writes the picking pixel.
6. Record the effective awareness context and output extent in a cold
   diagnostic. Do not log per event or per frame.

The Vulkan window-target path already has resize and reacquisition handling.
That does not prove mixed-DPI moves are correct. A monitor move changes the
extent at a different lifecycle point and needs its own validation run.

### 2.3 Evidence

- Move a window between 100%, 125%, 150%, and 200% displays where available.
- Confirm that the reported client extent matches a physical-pixel screenshot.
- Resize while crossing monitors and confirm there is one coherent resize
  sequence, no zero-size submit, and no stale target use.
- Pick fixed screen-space fixtures at each scale.
- Run a focused Vulkan synchronization-validation case for the monitor move and
  resize path.

Fixed-extent offscreen goldens do not change because of DPI awareness. Windowed
screenshots and performance results do change if the renderer now draws more
physical pixels. Compare performance only at matched internal extents.

## 3. Output transfer function

### 3.1 Implemented behavior

Vulkan and Metal now share one output contract:

- Vulkan requires an `*_SRGB` surface format, renders windowed and offscreen
  final color into `R8G8B8A8_SRGB`, and blits windowed output to the sRGB
  swapchain.
- Vulkan `fullscreen_fragment` returns the ACES-mapped linear value without a
  shader gamma approximation.
- Metal retains its sRGB window and offscreen attachments and linear tonemap
  output.
- `vkr_srgb_to_linear()` and `vkr_linear_to_srgb()` own the exact piecewise
  transfer functions. Retained UI and world-text constructors/setters decode
  public authored RGB through `vkr_srgb_color_to_linear()` once; alpha is
  unchanged.

UI and text therefore blend straight-alpha linear RGB on both backends, and the
sRGB attachment applies the only output encoding.

### 3.2 Selected contract

Use this contract on windowed and offscreen output targets:

1. Scene tonemapping returns linear display-referred RGB in `[0,1]`.
2. UI and text blending occurs in linear RGB.
3. The output attachment uses an sRGB format and performs the final sRGB
   encoding on store.
4. Public authored UI/text color values are sRGB display colors. Convert RGB to
   linear once at the cold CPU boundary that builds retained vertex/color data.
   Alpha remains linear.
5. HDR scene resources, material textures, and data attachments keep their
   existing format-specific contracts. This decision does not relabel arbitrary
   UNORM data as sRGB.

This means:

- prefer `VK_FORMAT_B8G8R8A8_SRGB`, then
  `VK_FORMAT_R8G8B8A8_SRGB`, with
  `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` for a Vulkan window target;
- use an sRGB `R8G8B8A8` offscreen final-color target on Vulkan as well;
- keep Metal's sRGB window and offscreen targets;
- remove the Vulkan shader `pow`; and
- make the UI/text input conversion explicit and shared.

If a window system cannot provide a usable sRGB surface format, reject it with a
capability report for now. Do not silently revive the gamma-2.2 path. A manual
exact-sRGB fallback would be a new accepted compatibility decision and would
also need a plan for correct linear UI blending.

### 3.3 Alpha and blend rules

The sRGB attachment converts stored RGB to linear before blending and encodes
the result afterward. That is the main correctness gain over shader-encoding to
a UNORM target. Keep the current straight-alpha convention unless a separate
blend audit changes it:

- fragment RGB is unpremultiplied linear RGB;
- fragment alpha is coverage/opacity;
- RGB uses source-alpha and one-minus-source-alpha factors; and
- the attachment encodes RGB only.

Test translucent glyph edges and overlapping UI, not just opaque swatches. An
opaque patch can match while blending is still in the wrong domain.

### 3.4 Capture contract

Name each capture by its domain:

- scene-linear HDR before exposure and tonemapping;
- linear display color after exposure and the tone curve but before the OETF;
- encoded final color as stored in the output target; and
- UI-only linear or encoded fixtures where needed.

The existing canonical scene-color capture applies exposure and the ACES curve,
so it is not scene-linear HDR despite its RGBA16F storage. Preserve its current
bytes unless the capture contract is deliberately versioned. The transfer fix
must change encoded final-color captures. It need not change the linear
post-tonemap capture.

### 3.5 Evidence

1. Unit-test the exact piecewise sRGB encode/decode at the linear segment,
   breakpoint, dark values, midtones, and endpoints.
2. Capture a linear ramp and dark patches through Vulkan and Metal at the same
   extent. Compare both the linear pre-OETF capture and encoded final color.
3. Capture opaque and translucent UI/text swatches over dark and mid-gray
   backgrounds.
4. Run Vulkan synchronization and GPU-assisted validation plus focused Metal API
   validation.
5. Regenerate final-color goldens only after explicit owner review.

## 4. Metal internal render scale

Render scale uses two extents:

- output extent, fixed by the physical window or harness target; and
- internal scene extent, derived from output extent and a validated scale.

Metal renders scene, visibility, depth, G-buffer, lighting, transmission, AO,
HZB, and temporal inputs at the internal extent. Spatial mode keeps temporal,
exposure, and bloom internal, then the existing tonemap draw linearly samples
HDR into the output extent and applies FXAA in output pixels. MetalFX mode
stages color, depth, and motion into native-sized inputs with an active internal
content rectangle, reconstructs native scene-linear HDR, and runs exposure,
bloom, and tonemap at native resolution. UI and text remain at output
resolution in both modes. Fullscreen picking maps output pixels to internal
pixels.

MetalFX motion targets the exact immediately preceding scaler encode, not the
portable resolver's newest completed history image. The transform address,
view-projection matrix, and source frame come from one retained instance. A
missing predecessor resets history; an in-flight producer is ordered with the
existing Metal shared event without a CPU wait.

Scaling the value returned by `vkr_window_get_pixel_size()` would lie about the
present target, resize swapchain resources unnecessarily, lower UI resolution,
and contaminate fixed-extent harness cases. Do not do it.

`render_scale` is immutable, finite, and in `(0,1]`; zero in an application
configuration selects `1.0`. The internal extent rounds the scaled physical
extent to the nearest integer and clamps each axis to at least one pixel. Metal
accepts the mode only for the fullscreen topology. The editor already owns an
independent panel scale and coordinate mapping, so a non-unit global scale with
`editor_enabled` is rejected until that topology is complete. Vulkan rejects
all non-unit values.

Spatial reconstruction remains the default. MetalFX temporal reconstruction is
an explicit Metal-only strategy and may use completion-driven dynamic scale.
The harness records output extent, renderer-reported scene extent, scale,
upscaler, dynamic policy, and observed transition range as separate effective
values. See [ADR-039](../architecture/adr/039-metal-internal-render-scale.md)
and [ADR-040](../architecture/adr/040-metalfx-temporal-dynamic-resolution.md).

## 5. Phases

| Phase | Work | Implementation | Evidence status |
| --- | --- | --- | --- |
| D1 | Per-Monitor V2 declaration and DPI-aware create/resize | Implemented | DPI-96 create/resize validation passes; mixed-DPI move pending |
| D2 | Cursor, capture-mode, and picking coordinate audit | Implemented | Client-pixel audit complete; multi-scale picking fixtures pending |
| C1 | Define UI/text public color semantics and add conversion tests | Implemented | Exact piecewise transfer and alpha-preservation tests pass |
| C2 | sRGB Vulkan window/offscreen targets and removal of shader gamma | Implemented | Window/offscreen sRGB reports and validation pass; matched dark ramp pending |
| C3 | Linear UI/text blending on both backends | Implemented | Validation-clean sRGB text capture passes; matched cross-backend translucent fixtures pending |
| C4 | Final-color golden review | Pending owner review | No baseline mutated |
| R1 | Metal internal scene scale with native output/UI | Implemented | Scale-0.4 Bistro clears the local 75 FPS p95 target; clean authoritative rerun and owner quality acceptance remain pending |
| R2 | MetalFX temporal reconstruction and completion-driven dynamic scale | Implemented; acceptance partial | Exact previous-encode motion removes the reproduced whole-scene camera warp in a fixed-scale Bistro diagnostic. A post-correction child averages 11.734 ms with 14.143 ms p95 at scales 0.40-0.45; its parent is incomplete because two repetitions register different shadow-pass catalogs. Solid 75 FPS, broader moving-image acceptance, and Metal validation remain open |

## 6. Primary references

- [Microsoft: setting process DPI awareness](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process)
- [Microsoft: WM_DPICHANGED](https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged)
- [Khronos data-format specification, sRGB transfer functions](https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html)
