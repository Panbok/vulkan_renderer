---
status: proposed
updated: 2026-09-12
authority: proposal
---

# Windows/Vulkan verification checklist

This checklist records evidence still required on a Windows Vulkan host. The
ADRs define the feature contracts. A successful build or compiled SPIR-V
reflection does not prove native Vulkan execution, synchronization, display
behavior, or Metal/Vulkan pixel parity.

## 2026-09-12 Windows execution record

The runnable native renderer subset now passes on an AMD Radeon RX 6700 XT,
driver 26.6.3, Vulkan API 1.4.315 and SDK 1.4.357. Debug witnesses loaded
`VK_LAYER_KHRONOS_validation` with synchronization validation enabled and no
API or synchronization errors. Reports remain local and non-authoritative
because the fixes are not yet committed.

| Coverage | Result | Report and SHA-256 |
| --- | --- | --- |
| SSGI emission on/off, Release | pass | `20260912T103829.989Z-000bd8` / `6980163ae7bcc5a7cfe9d93c42cbbc26c6154b05ebd448fdc423e2b6eed6dd62`; `20260912T103835.198Z-001187` / `21858b94498b2fe0d265a0ef6b0c0d2a172acef8d45ef8ebb56fa8fa04667ba6` |
| SSGI Bistro, Release | pass | `20260912T103625.242Z-002637` / `db8d1d6000d6407206037b0f718d145181c0e2e7b55bbe312211767fecc2e93c` |
| SSGI focused Debug validation | pass | `20260912T111516.304Z-004249` / `8368380187d0358eadf810dfe0b0a1e187db988ade6933ac00da749dc2018ac1` |
| 2/4/8-image offscreen lifetime, Debug | pass | `20260912T104041.035Z-000492`, `20260912T104101.370Z-0031b3`, `20260912T104206.851Z-000f86` |
| Hidden-window acquire/present/resize, Debug validation | pass | `20260912T104336.637Z-0001e4` / `301515fafe6df78a381b1933b03bf00dbb195ee406532234aa9245f4124fd78f` |
| Portable TAA reference and FSR static/motion, Release snapshots | pass | `20260912T104649.504Z-002149`, `20260912T104706.781Z-0022c8`, `20260912T104723.602Z-00356b` |
| Clearcoat, sheen, anisotropy, thin-sheet diffuse, DoF and motion-blur resize, Debug validation | pass | `20260912T104809.336Z-001bf5` through `20260912T104951.620Z-002541` |
| Profiled surface diffusion, Release snapshot and Debug resize validation | pass | `20260912T111422.875Z-003ffe` / `ce2ee2896257522f972aa200a9eeae78937ba9ed0be9decc77af81920ad9664b`; `20260912T111336.086Z-003163` / `9457566e5ba9fac63cf042b8e2e87fb041a780293cbfadc7dbb288743e474db9` |
| Full-feature Bistro texture streaming and 1258x752/866x555 resize, Release / Debug validation | pass | `20260912T115416.259Z-003742` / `b440a48dcd851e1657e8d0355e81b22d473d0bea8b87884c6d7fb94e2b79ddb1`; `20260912T115010.173Z-003268` / `6d02aed7706799c832c5a7b03685d7862c531385bc0ddf19cdb4bc4220f38008` |

The reported SSGI device loss was reproduced before repair. Its Vulkan path
mixed sampled and storage descriptor indices, AMD lost the device on the
unannotated variable trace loop, and SSGI/SSR composite allocated zeroed frame
roots before material access. The corrected path uses typed sampled descriptors,
an explicit loop policy and fully populated roots. The sweep also exposed and
fixed the same zeroed-root defect in subsurface gather, plus a publication race:
the subsurface graph now waits for profile-bank initialization to complete.
Harness children now fail when rendering records a
fatal error even if the host closes normally in that frame.

The later Bistro editor run exposed a separate CPU allocation boundary during
texture streaming. Each new Vulkan image-pool block needs a 47,640-byte
`VkrGpuMemoryCore` record, but graph resize and texture publication exhausted the
renderer DMemory's former 64 MiB reserve. The reserve is now 96 MiB while its
initial 2 MiB commit and GPU completion rules remain unchanged. The retained
regression case enables FSR 3.1, bloom, GTAO, SSR and SSGI, loads all 632 texture
assignments, and performs the reported 1258x752 to 866x555 resize round trip. Two
Release and two validation-enabled Debug runs ended with zero pending, failed or
demanded-missing assignments, zero omitted publication candidates, empty stderr,
and no Vulkan API or synchronization diagnostics.

This Windows host cannot complete the checklist's bilateral Metal comparisons,
HDR-display inspection, multi-DPI monitor transitions or manual editor/Bakery
UI actions. Those boxes remain open rather than being inferred from Vulkan.
The original eight-image WSI item also combined incompatible requirements:
windowed image count is selected by WSI and the harness rejects a forced count.
The new eight-image witness therefore covers offscreen frame-slot/history
lifetime; acquire/present/resize remains covered separately by the driver-selected
three-image hidden-window witness.

## Build and shader gates

- [x] Set `VCPKG_ROOT`, install `freetype:x64-windows-static`, and run
  `build_release.bat`; inspect the Release output and confirm the Vulkan 1.4
  capability profile from [ADR-023](../adr/023-vulkan-1-4-bindless-capability-profile.md).
  `vkr_vk_select_device` now emits the selected candidate's profile at INFO and
  every unmet requirement of every rejected candidate at ERROR; before this the
  report was only a pass/fail predicate, so no build could show it. Stock
  `build_release.bat` keeps `LOG_LEVEL=1`, so the INFO report needs a Release
  configure with `VKR_EDITOR_LOGGING=ON`. Recorded for an AMD Radeon RX 6700 XT
  on driver 26.6.3, api 1.4.315, 62 entries, every required entry present:
  [capability profile](../../assets/verification/renderer-features/windows-vulkan-capability-profile.txt).
  Target-device coverage beyond this one adapter is unmeasured.
- [x] Run `build_editor.bat Release` and confirm the editor Vulkan target starts
  with the same cooked assets and shader set. The Release editor reaches the
  frame loop and creates its FSR 3.1.4 context at 866x555 with no error or
  validation output. Startup only; no editor UI interaction was exercised.
- [x] Run `build.bat Debug` for the diagnostic build. Confirm that
  `VK_LAYER_KHRONOS_validation` is initialized by the child, then inspect
  stderr/stdout for validation errors; a Debug build without the layer is not
  a validation result. Both witnesses below report
  `Vulkan validation enabled: VK_LAYER_KHRONOS_validation (synchronization=1,
  GPU-assisted=0)` in child stdout with empty child stderr. Debug application
  targets need the 8 MiB main-stack reserve set in the root list file: an
  unoptimized FidelityFX dispatch overflows the Windows 1 MiB default about
  five seconds in, at the first FSR dispatch, with `0xC00000FD` and no log
  output. Release is unaffected, so a passing Release run does not cover it.
- [x] Confirm that the Release wrappers compile every production Slang/native
  entry point and that Vulkan reflection validates the generated SPIR-V roots,
  bindings, push constants, and dispatch sizes under
  [ADR-044](../adr/044-shader-cross-backend-contract.md). Record module and
  reflection output; this remains a compiled-contract gate. The startup gate in
  `vkr_vulkan_pipelines.c` had never passed on Vulkan: the UI root's bindless
  slot names, the SSGI push blocks' declared size, the froxel push block's
  untyped root address, and the SSR/SSGI composite address-padding names all
  disagreed with the host records. All roots now reflect and match. Reflection
  failures name their module, so a mismatch identifies its shader.
- [x] Confirm build wrappers compile cooker tools without invoking cooking; run
  cooker jobs only through Bakery or an explicit cooker wrapper. The Debug,
  Release and editor wrappers link every cooker executable and invoke none.
- [x] Build and run the CPU suite with `build_test.bat`. It had never compiled
  on Windows: `far`, `near` (minwindef.h) and `small` (rpcndr.h) are Windows SDK
  macros that captured local variables in four test files. These cannot be
  undefined, because the SDK's own `FAR`/`NEAR` expand to them and
  `DEFINE_GUID` then fails, so the locals were renamed. 537 cases pass.
- [x] Run the focused synchronization witness after the Debug build:

  ```powershell
  .\build_debug\tools\vkr_harness.exe profile `
    --case tools\cases\local\p20_vulkan_state_matrix_validation.case.json `
    --profile tools\profiles\validation-windowed.json
  ```

  With a Visual Studio multi-config build, use
  `build_debug\tools\Debug\vkr_harness.exe` instead.

  Require an actual validation-layer initialization, zero reported API or
  synchronization errors, passing manifest assertions, and a successful child
  report. Repeat with
  `deferred_state_matrix_vulkan_validation.case.json` for cutout and four-layer
  transmission coverage.

  Both pass on an AMD Radeon RX 6700 XT, driver 26.6.3. The state-matrix run
  reports `target_image_count: 3`, `present_mode: fifo`, empty child stderr, and
  passing `visibility.gpu_visible.overflow`,
  `visibility.transmission.gpu_visible.overflow` and
  `visibility.gbuffer.resolve_invalid` assertions; the deferred run reports
  `status: pass` with empty child stderr. Both carry `profile.local_only` and
  `provenance.dirty` diagnostics, so neither is authoritative: they ran against
  an uncommitted tree and must be repeated on a committed revision before the
  reports are cited as acceptance evidence.

## Native Vulkan feature matrix

For each item, run the smallest existing local case, inspect its assertions and
captures, and repeat the same revision on Metal for a bilateral comparison.
Record native Vulkan diagnostics, effective configuration, capture metadata and
the comparison result. Keep a feature UNALIGNED until both native runs and the
comparison satisfy [ADR-044](../adr/044-shader-cross-backend-contract.md).

- [ ] Energy-compensated GGX and shared DFG: [ADR-053](../adr/053-energy-compensated-ggx.md).
- [ ] Baked diffuse volumes, room boundaries, thick glass, multi-bounce diffuse
  transport, and photon caustics: [ADR-054](../adr/054-baked-diffuse-volumes.md).
- [ ] Opaque SSR: validate the 512-byte temporal and 416-byte composite roots,
  the 128-byte camera record at temporal offset 288, exact producer transform
  reads and graph bindings 17/18. Trace writes the hit image at binding 9 with
  a 336-byte root. Check the removed depth-base/trace/temporal receiver bindings
  3/5/1 and temporal hit/output indices at offsets 460/476/480/484.
  Trace/raw/hit use source extent; the depth pyramid remains floor-half with
  odd trailing rows/columns retained. Check RGBA32_UINT sampled/storage support.
  Capture version-5 `ssr_reflection` is full-resolution incoming radiance;
  composite applies current material/GTAO exactly once and removes the current
  probe term. Prior shaded-history captures are not numerically comparable.
  Check the expanded geometry/identity histories, full-source raw/hit images and
  removed receiver-coordinate image: +140.625 MiB logical payload at source
  1280×720 with three slots/five histories, or +316.40625 MiB with eight slots/ten
  histories, excluding allocation overhead. Full-resolution tracing accounts for
  42.1875/112.5 MiB of this increase over the prior reflected-hit implementation.
  Resize, cancellation, scene reload and retirement must preserve completion.
  Exercise no-TAA and TAA, subpixel jitter, camera translation, rigid reflected
  objects and receivers, nonuniform/mirrored models, material/coat boundaries,
  missing hits, foreign identities, depth discontinuities and normal rejection.
  Run `ssr_reflected_hit_motion.case.json` and its
  `tools/checks/check_ssr_reflected_hit.py` payload check: the moving emitter must
  stop leaving red history when its current reflection loses coverage.
  Run `ssr_reflected_hit_editor.case.json` for odd-size scaled editor bindings.
  Run `ssr_reflected_hit_flicker.case.json` at the later reported bar camera and
  `tools/checks/check_ssr_reflected_flicker.py` on its retained snapshot. Confirm
  that weighted RGB, rather than coverage alone, selects the history's reflected
  object. Assess the bar lip and upper woodwork before static accumulation;
  absent current hits and thin edges remain limitations on Metal. The separate
  `ssr_reflected_hit_flicker_motion.case.json` is MetalFX-specific; use a separately
  identified Vulkan/FSR witness for that mode, not a bilateral comparison.
  Unsupported correspondence must use current radiance/probes without fading
  an old reflection. Four history taps retain individual validation; raw bounds
  reuse nine guided rough samples at source offsets {-2,0,2}, weighted at half
  those offsets. Mirrors use their exact current ray. Dominant metadata adds one
  hit read; the gather retains its visible row. Composite reads one history pixel
  and shades it at the current receiver. Trace uses one ray per source pixel,
  fractional linear-clamp sampling,
  full-resolution leaves, absolute crossings and earliest rear-slab hits.
  Compare the under-bar in/out case and the separate SSGI flicker camera in
  `ssr_ghost_under_bar[_taa].case.json` and
  `ssgi_bistro_remaining_flicker.case.json`. Check filtered coat SSR against exact
  packed-roughness probe removal, sheen/anisotropy and disabled GTAO's 1×1 sentinel.
  Assess visible trails, flicker, reflection strength and measured temporal cost
  before claiming parity: [ADR-055](../adr/055-screen-space-reflections.md).
- [ ] Rectangular LTC lights and offline rectangle transport:
  [ADR-056](../adr/056-rectangular-ltc-lights.md).
- [ ] Analytic height fog and ordered transmission composition:
  [ADR-057](../adr/057-analytic-height-fog.md).
- [ ] Revision-baked atmosphere, sky IBL, and unified sun:
  [ADR-058](../adr/058-revision-baked-sky-atmosphere.md).
- [ ] Froxel volumetric fog, scattering history, and invalid-input fallback:
  [ADR-059](../adr/059-froxel-volumetric-fog.md).
- [ ] Independent SSGI, source classification, shared motion predecessor consistency,
  exact in-flight producer ordering, jitter-corrected four-tap depth/identity
  rejection, no-TAA zero jitter, and baked-volume suppression: [ADR-060](../adr/060-screen-space-diffuse-indirect-lighting.md).
- [ ] Extended-linear scRGB/EDR output, headroom mapping, and SDR fallback:
  [ADR-061](../adr/061-extended-linear-display-output.md).
- [ ] Layered clearcoat maps, coat energy, and coat-priority SSR:
  [ADR-062](../adr/062-layered-clearcoat.md).
- [ ] Charlie sheen, directional tables, and rectangle-light response:
  [ADR-063](../adr/063-charlie-sheen.md).
- [ ] Anisotropic GGX reflection, directional tables, and material transport:
  [ADR-064](../adr/064-anisotropic-ggx-reflection.md).
- [ ] Thin-sheet diffuse transmission, sidedness, cutout coverage, and offline
  transport: [ADR-065](../adr/065-thin-sheet-diffuse-transmission.md).
- [ ] Post-reconstruction depth of field and transparent/opaque depth behavior:
  [ADR-066](../adr/066-post-reconstruction-depth-of-field.md).
- [ ] Camera and rigid-object motion blur, motion vectors, and history reset:
  [ADR-067](../adr/067-post-reconstruction-motion-blur.md).
- [ ] Profiled RGB surface diffusion and offline BSSRDF transport:
  [ADR-068](../adr/068-profiled-surface-diffusion.md).

Use the existing feature cases under `tools/cases/local/` (for example
`*_bistro_*`, `*_resize_*`, `*_editor_*`, `*_furnace_*`, and the feature-named
fixtures). A passing case that only reports SPIR-V compilation remains
insufficient.

## Cooker and Bakery migration

- [ ] Run `tools\pack_vkt_textures.bat` and verify KTX2/UASTC outputs for base
  color, alpha-cutout coverage, and paired normal/roughness variants. Exercise
  opaque, single-sided cutout, double-sided cutout, transmission, and normal
  map fixtures through the Vulkan visibility and material assertions. The
  format and variant contract is [ADR-012](../adr/012-texture-compression-pipeline.md).
- [ ] Run `tools\cook_vkr_meshes.bat` and `tools\cook_vkr_fonts.bat`; verify
  versioned mesh hierarchy, cooked MTSDF fonts, and unchanged-output skipping
  under [ADR-030](../adr/030-offline-mesh-optimization-and-cooking.md) and
  [ADR-034](../adr/034-offline-cooked-font-artifacts.md).
- [ ] Exercise all nine Bakery recipes—mesh, font, single texture, texture
  directory, GGX DFG, Charlie, anisotropy, diffuse volume, and reflection
  probe. Verify each job records source identity, output path, status, and
  failure text, and that a failed job does not publish a partial artifact.
  Every recipe failed on the first Windows run, for four separate reasons now
  fixed: `write_file_atomic` reported nothing, so a cooker failure reached the
  UI as an exit code with an empty log; the table cookers held the destination
  open across their own atomic rename, which Windows refuses; the Charlie fit
  pruned a mixture lobe without handing its weight to the survivor, failing its
  own sum-to-one check; and `CreateProcessA` decoded the UTF-8 Python path with
  the system code page, so the two Python recipes never launched on a host whose
  profile directory is not ASCII. Cookers now build optimized in every
  configuration with unfused floating point, so a Debug-configured and a
  Release-configured cooker emit byte-identical tables; the Charlie fit still
  takes about 32 minutes. Table reproducibility is per-toolchain only: libm
  differences remain, and the Charlie fit in particular lands on a different
  mixture pruning decision per platform, which moves real coefficients rather
  than only rounding.
- [ ] Run a Windows Bakery job for an actual glass scene with multi-bounce
  transport and photon caustics. Verify valid-probe/cell counts, finite SH,
  nonzero caustic deposits, dependency manifest, CRCs, and `--check` freshness
  using the [diffuse-volume wrapper](../../tools/bake_diffuse_volume.py).
- [ ] Cancel a running Bakery job from the UI and from its owning process. The
  cancellation must reach nested cooker children, leave no orphan processes,
  remove only temporary outputs, and leave the last published artifact usable.
  Verify the Windows Job-object tree and the POSIX process-group equivalent
  before accepting the cross-platform contract.

## Temporal, target, and WSI checks

- [ ] Disable TAA with all post effects in Bistro and verify that the expanded
  graph fits the shared 163-pass native capacity. Port the Metal-pinned
  `ssr_bar_no_taa.case.json` settings to a Vulkan witness and inspect the actual
  native pass count and frame result. The CPU envelope check and
  [Metal capacity evidence](../../assets/verification/renderer-features/ssr-no-taa-capacity.txt)
  do not establish native Vulkan acceptance.
- [ ] With SSR and portable TAA enabled below 100% scale, exercise a camera turn
  followed by a hold beyond 256 unchanged frames. Port the Metal-pinned
  `ssr_bar_turn_stop.case.json`, `ssr_bar_turn_stop_settled.case.json` and
  `ssr_bar_scaled_taa.case.json` settings to Vulkan, then repeat with FSR 3.1.
  Check that the first 128 matching submitted frames keep ordinary accumulation
  and FSR composition masking, then begin the existing 128-sample static mean.
  Motion, SSR toggles, invalid history, scene/resource changes and resize must
  reset the settling counter. Failed submissions must not advance it. Verify the
  TAA root's history mode at offset 124 (144-byte root): SSR settling caps ordinary
  history at 0.9, SSR-off ordinary stationary retention stays 0.99, and mode 1
  alone enables the checked static integral. Compare the user under-bar in/out
  case `ssr_ghost_under_bar_taa` and separate `ssgi_bistro_remaining_flicker` case
  for shorter trails and the possible pre-settle shimmer tradeoff. Compare
  trails, reflection strength, the first static sample and the final held image;
  [Metal settling evidence](../../assets/verification/renderer-features/ssr-history-settling.txt)
  does not validate FSR SDK execution or native Vulkan history ordering.
- [ ] Run the portable TAA reference and Vulkan FSR 3.1 static case, then the
  moving-camera cases. Use `snapshot` and inspect final color, motion/depth
  channels, history age/validity assertions, and the effective render/output
  extents:

  ```powershell
  .\build_release\tools\vkr_harness.exe snapshot `
    --case tools\cases\local\fsr31_bistro_taa_reference.case.json `
    --profile tools\profiles\local-offscreen.json
  .\build_release\tools\vkr_harness.exe snapshot `
    --case tools\cases\local\fsr31_bistro_static.case.json `
    --profile tools\profiles\local-offscreen.json
  ```

  Repeat with `fsr31_bistro_motion.case.json`,
  `fsr31_bistro_motion_translation.case.json`, and
  `fsr31_editor_bistro_resize.case.json`. Require completed-submission history
  ordering, no stale history after camera or extent changes, and no temporal
  fallback hidden by a passing process exit. The contracts are
  [ADR-037](../adr/037-portable-same-resolution-temporal-antialiasing.md) and
  [ADR-052](../adr/052-vulkan-fsr31-upscaling.md).
- [ ] Run the existing Vulkan three-image Bistro witness and check the report's
  `effective_config` and `provenance` for `target_image_count: 3`:

  ```powershell
  .\build_release\tools\vkr_harness.exe snapshot `
    --case tools\cases\local\p20_vulkan_bistro_default.case.json `
    --profile tools\profiles\local-offscreen.json
  ```

  Pair it with `fsr31_editor_bistro_resize.case.json`, whose current manifest
  also requests three images, to cover resize and history invalidation at that
  count.

- [x] Add and run the matching eight-image offscreen Vulkan lifetime witness.
  The checked-in `p21_vulkan_offscreen_8image.case.json` exercises startup,
  repeated submit, history, retained images and clean shutdown at eight images.
  Pair it with the existing
  `p21_vulkan_offscreen_2image.case.json` and
  `p21_vulkan_offscreen_4image.case.json` lifetime witnesses. A windowed target
  cannot force eight swapchain images; WSI acquire/present/resize is the next
  separate item and uses the selected image count.
- [x] Run the hidden-window WSI case under Vulkan validation:

  ```powershell
  .\build_debug\tools\vkr_harness.exe profile `
    --case tools\cases\local\p21_vulkan_bistro_windowed_validation.case.json `
    --profile tools\profiles\validation-windowed.json
  ```

  Confirm actual present mode, image count, swapchain recreation, acquire and
  present results, and zero validation diagnostics across the orbit and resize.

## Presentation, DPI, and editor UI

- [ ] On an HDR-capable Windows display, run
  `edr_window_auto_capture_local.case.json` and
  `edr_window_editor_capture_local.case.json`; inspect the float16 capture
  sidecar for `extended_srgb_linear`, `display_headroom`, and
  `display_output_scale`.
- [ ] On an SDR-only display or with HDR disabled, run
  `edr_window_sdr_local.case.json` and verify AUTO selects the SDR path without
  changing scene-linear exposure, and that the report does not claim extended
  output. Preserve the fallback result as evidence under [ADR-061](../adr/061-extended-linear-display-output.md).
- [ ] Run app and editor at 100%, 125%, 150%, and 200% Windows DPI, including a
  monitor transition. Check physical-pixel UI layout, text baseline, pointer
  mapping, picking, resize mailbox coalescing, and retained Scene presentation
  under [ADR-036](../adr/036-dpi-derived-ui-text-scale.md),
  [ADR-043](../adr/043-presentation-dpi-and-color-transfer.md), and
  [ADR-047](../adr/047-event-payload-and-resize-mailbox-lifetimes.md).
- [ ] Verify Settings > Graphics has a left tab rail and clipped, scrollable
  right content for Display, Quality, Lighting, Effects, and Color. Check active
  tab state, keyboard/mouse focus, Restore defaults, and narrow-dock layout.
  Confirm the UI emits typed requests and the runtime owns validation/application.
- [ ] Change vsync, HDR, temporal upscaling, dynamic resolution, and render
  scale. Verify the restart-required notice and the started/effective values;
  change the remaining controls and verify live application plus shadow/history
  invalidation where applicable.
- [ ] Set `VKR_GRAPHICS_SETTINGS_PATH` to a temporary Windows path and verify
  missing-file defaults, invalid-file rejection, versioned JSON, 0.25-second
  idle debounce, atomic save, and exit flush. Relaunch and verify persistence;
  also check the project `.vkr-graphics-settings.json` default path.
- [x] Native macOS Graphics and Bakery acceptance is recorded in
  [ADR-027](../adr/027-immediate-mode-grid-ui.md): CPU/settings oracles,
  process-group cancellation, Release/Debug/editor wrappers without cooking,
  four unchanged shared-table hashes, final opacity coverage, GGX completion,
  and cancelled anisotropy with no cooker descendants all pass. Windows
  UI/process-tree behavior and native Vulkan remain unchecked below.

## Acceptance evidence

- [ ] For every native run, inspect `status`, `exit_code`, `effective_config`,
  `provenance`, `comparison`, required metrics, child stderr/stdout, and all
  capture records. Require finite payloads, complete channels, passing
  assertions, and the expected backend, target, present mode, image count, and
  feature settings.
- [ ] Use same-revision, same-case, same-profile captures for bilateral checks.
  Compare canonical float16 channels and their sidecars; do not compare PNG
  previews as radiance data. Keep captures and report digests until review, and
  use `compare --run` before proposing a baseline.
- [ ] Treat unavailable native execution, missing captures, incomplete GPU
  timestamps, validation-layer absence, or a fallback configuration as an
  evidence failure. Do not promote a baseline or call a feature aligned from
  compilation, a single backend, or a process exit alone.
- [ ] Record exact commands, binary/device/driver identity, report SHA-256,
  comparison limits, and the remaining unavailable gates in the owning ADR or
  task evidence. Optional refinements from the removed image-quality proposal
  are outside this checklist.
