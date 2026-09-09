---
status: proposed
updated: 2026-09-09
authority: proposal
---

# Windows/Vulkan verification checklist

This checklist records evidence still required on a Windows Vulkan host. The
ADRs define the feature contracts. A successful build or compiled SPIR-V
reflection does not prove native Vulkan execution, synchronization, display
behavior, or Metal/Vulkan pixel parity.

## Build and shader gates

- [ ] Set `VCPKG_ROOT`, install `freetype:x64-windows-static`, and run
  `build_release.bat`; inspect the Release output and confirm the Vulkan 1.4
  capability profile from [ADR-023](../adr/023-vulkan-1-4-bindless-capability-profile.md).
- [ ] Run `build_editor.bat Release` and confirm the editor Vulkan target starts
  with the same cooked assets and shader set.
- [ ] Run `build.bat Debug` for the diagnostic build. Confirm that
  `VK_LAYER_KHRONOS_validation` is initialized by the child, then inspect
  stderr/stdout for validation errors; a Debug build without the layer is not
  a validation result.
- [ ] Confirm that the Release wrappers compile every production Slang/native
  entry point and that Vulkan reflection validates the generated SPIR-V roots,
  bindings, push constants, and dispatch sizes under
  [ADR-044](../adr/044-shader-cross-backend-contract.md). Record module and
  reflection output; this remains a compiled-contract gate.
- [ ] Confirm build wrappers compile cooker tools without invoking cooking; run
  cooker jobs only through Bakery or an explicit cooker wrapper.
- [ ] Run the focused synchronization witness after the Debug build:

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
  reads and graph bindings 17/18. Trace adds the hit image at binding 9 while
  retaining its 336-byte root. Check RGBA32_UINT sampled/storage format support.
  Capture version-5 `ssr_reflection` is full-resolution incoming radiance;
  composite applies current material/GTAO exactly once and removes the current
  probe term. Prior shaded-history captures are not numerically comparable.
  Check the expanded geometry/identity histories and raw-hit image: +98.4375 MiB
  logical payload at source 1280×720 with three slots/five histories, or
  +203.90625 MiB with eight slots/ten histories, excluding allocation overhead.
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
  reuse nine guided samples (four on mirrors) and dominant metadata adds two
  reads. Composite reads one history pixel and shades it at the current receiver.
  Trace retains fractional linear-clamp sampling, half-resolution rays,
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

- [ ] Add and run the matching eight-image Vulkan witness. The repository has
  four-image and three-image cases, but no checked-in eight-image case yet;
  this item stays pending until that manifest is added. Exercise startup,
  repeated acquire/submit/present, resize, history, retained images, and clean
  shutdown at eight images. Pair it with the existing
  `p21_vulkan_offscreen_2image.case.json` and
  `p21_vulkan_offscreen_4image.case.json` lifetime witnesses.
- [ ] Run the hidden-window WSI case under Vulkan validation:

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
