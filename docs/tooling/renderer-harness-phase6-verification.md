---
status: investigation
updated: 2026-08-02
authority: investigation
---

# Renderer Harness Phase-6 Verification

## Conclusion

Phase 6 is implemented. Renderer configuration now selects a windowed or
ordinary-image offscreen present target before Vulkan instance/device creation.
The offscreen path creates no native window, `VkSurfaceKHR`, swapchain, present
queue, WSI acquire/present semaphores, or present call. It still runs the same
application, packet, render-graph, pass, and capture path as the windowed
renderer.

The graph resolves its compatibility-named `swapchain` and `swapchain_depth`
imports through target-neutral attachment queries. Imported layout/access state
comes from the selected target, and the graph owns the terminal transition:
`PRESENT_SRC_KHR` for windowed output and retained color-attachment state for
offscreen output. Normal frame completion injects no hidden present barrier.

Local Apple M1 Pro/MoltenVK evidence is validation-clean. A two-repetition
windowed Sponza profile and a two-repetition offscreen counterpart reported the
same 640×480 drawable extent, BGRA8 sRGB color, D32 float depth, and three target
images. All 80 deterministic work-volume metrics were identical. Two-image
offscreen and three-image windowed snapshot runs produced byte-identical
canonical final-color and depth payloads.

These are dirty-tree local-profile correctness runs. They are not an
authoritative performance result or accepted visual baseline.

## Implemented contract

- `VkrPresentTargetConfig` reaches instance extension filtering, physical-device
  scoring, queue selection, logical-device extension selection, and final
  attachment creation before any target is opened.
- The backend holds two implementations behind one `VulkanPresentTargetOps`
  seam. Create/destroy/resize, per-frame begin/acquire/complete/cancel, the WSI
  capability, the recovery policy, and the terminal state are all declared by
  the bound implementation, so the frame path never tests the target kind.
  Only per-image state retention and reported provenance still name it.
- `vkr_renderer_window_*` and `vkr_renderer_get_swapchain_*` are removed. Graph
  compilation, frame setup, capture, shadow, picking, and the editor viewport
  reach attachments, counts, and formats only through
  `vkr_renderer_present_target_*`.
- Windowed initialization retains surface/swapchain support and present-queue
  requirements. Offscreen initialization omits all of them and selects supported
  ordinary color/depth formats, including transfer-source capability when
  capture is enabled.
- Image count and frame slots remain separate. Offscreen image selection is
  round-robin; every image waits on its last submit fence before reuse. Submit
  uses the frame-slot fence without WSI semaphores or presentation.
- Each offscreen color/depth pair retains explicit access/layout state for its
  next graph import. Cancel restores the pre-frame state before submitting the
  empty cancellation buffer.
- Explicit offscreen recreation is rejected during an active frame, waits for
  device idle, invalidates graph/backend framebuffers before imported image
  destruction, resets target-lifetime arena storage, and rebuilds image pairs,
  wrappers, command buffers, fences, and per-image fence references.
- Every offscreen harness child performs that explicit recreation before scene
  loading and outside readiness, warmup, and measurement windows.
- Reports and environment fingerprints carry actual target kind, image count,
  extent, color/depth formats, color space, and present mode. Offscreen color
  space and present mode correctly report `unknown` and `none` because ordinary
  images have no WSI color-space/present configuration.
- Camera projection, shadow fitting, UI layout, and editor target completion
  accept explicit target dimensions and no longer require a window.

## Evidence

| Gate | Result |
|---|---|
| `./build_test.sh` | Exit 0; every registered CPU suite passed, including the new offscreen/windowed import and graph-terminal barrier regression |
| `./build.sh Debug` | Exit 0; shaders, renderer library, application, texture packer, and complete harness built with implicit function declarations treated as errors |
| Backend matrix | `tools/validate_multithreaded_backend_matrix.sh` completed with `PASS=5`, `FAIL=0`; no validation/crash markers in its logs. An earlier attempt segfaulted inside the CPU event-system suite; the same binary then completed five consecutive clean runs and two later full matrix runs passed, so it is an unrelated CPU-suite flake rather than a target-path failure |
| Three-image offscreen profile | Two isolated processes passed after explicit target recreation; actual target was offscreen 640×480, BGRA8 sRGB/D32 float, `present=none` |
| Three-image windowed profile | Two isolated processes passed; actual target was hidden windowed 640×480, BGRA8 sRGB/D32 float, IMMEDIATE; both processes also completed live swapchain recreation |
| Work equivalence | All 80 metrics selected by the harness work-volume rule (`draw.*`, `visibility.*`, overflow, capture) had identical availability and aggregate values across targets |
| Two-image offscreen snapshot | Final color and depth completed after explicit target recreation with no validation/error markers |
| Offscreen `autotest` composition | Primary profile and auxiliary snapshot references both passed and the top report was schema-valid; the command returned the expected exit 4/`missing_baseline` because no accepted `local.offscreen` baseline exists |
| Canonical target equivalence | Final-color SHA-256 `sha256:7b22d79a6e1c996d47d84c623899309c3545a79e674a7d186a5cfb739f2bcf6c`; depth SHA-256 `sha256:5488545b8495e39c847443e796e6f97ff3e4edb2dae4046382df2429b290cacc`; each digest was identical across windowed and offscreen captures, and unchanged by the present-target seam extraction |
| Vulkan validation scans | No `validation layer:`, `VUID-`, `[ERROR]`, or `[FATAL]` markers in the final windowed/offscreen profile and snapshot artifact trees |
| Report shape | Effective configuration records actual target extent/count, color/depth formats, color space, and present mode; all four final reports and the autotest report passed Draft-07 validation with `ajv-cli`, and all new fixture JSON passed syntax validation |
| Existing application and pipeline cache | `./validate_pipeline_cache.sh` passed cold/warm create-load-save; a separate 30-second ordinary windowed application run initialized the full UI/editor configuration and shut down normally with no validation/error markers |
| Formatting | `clang-format` applied to changed C headers/sources; `git diff --check` passed |

Final profile reports:

- windowed: `build/_artifacts/profile/20260802T194343.483Z-00e560/report.json`
- offscreen: `build/_artifacts/profile/20260802T194240.899Z-00e034/report.json`

Final snapshot reports:

- windowed: `build/_artifacts/snapshot/20260802T194520.762Z-00e74f/report.json`
- offscreen: `build/_artifacts/snapshot/20260802T194452.071Z-00e517/report.json`

Offscreen autotest composition:

- report: `build/_artifacts/autotest/20260802T194617.327Z-00e657/report.json`
- report SHA-256:
  `sha256:9594bb07ac548e62913fd2c42bf9603385242fb9fd9f58386a92d8cd8fe3da2c`

Exact runtime commands:

```sh
./build_test.sh
./build.sh Debug

build/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_windowed_equivalent.case.json \
  --profile tools/profiles/local-windowed.json

build/tools/vkr_harness profile \
  --case tools/cases/smoke/sponza_offscreen.case.json \
  --profile tools/profiles/local-offscreen.json

build/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_windowed_snapshot_equivalent.case.json \
  --profile tools/profiles/local-windowed.json

build/tools/vkr_harness snapshot \
  --case tools/cases/smoke/sponza_offscreen_snapshot.case.json \
  --profile tools/profiles/local-offscreen.json

build/tools/vkr_harness autotest \
  --case tools/cases/smoke/sponza_offscreen_snapshot.case.json \
  --profile tools/profiles/local-offscreen.json

tools/validate_multithreaded_backend_matrix.sh
./validate_pipeline_cache.sh
VKR_AUTOCLOSE_SECONDS=30 build/app/vulkan_renderer

npx --yes ajv-cli@5 validate --spec=draft7 \
  -s docs/tooling/harness-report-schema.json \
  -d build/_artifacts/profile/20260802T194343.483Z-00e560/report.json \
  -d build/_artifacts/profile/20260802T194240.899Z-00e034/report.json \
  -d build/_artifacts/snapshot/20260802T194520.762Z-00e74f/report.json \
  -d build/_artifacts/snapshot/20260802T194452.071Z-00e517/report.json
```

## Residual evidence scope

macOS reports logical window points separately from Vulkan drawable pixels. The
windowed fixture requests 320×240 logical points and the local Retina drawable
is 640×480; the equivalent offscreen fixture therefore requests 640×480
ordinary images. Reports and fingerprints use the actual 640×480 target extent.
The camera path is explicit and both workloads use the same aspect, pose,
simulation delta, scene, renderer settings, and measured frame sequence.

MoltenVK exposed a three-image windowed swapchain on this host, so unavailable
two- and four-image windowed configurations could not be exercised. Two- and
three-image offscreen targets were exercised. The windowed runs covered live
resize-driven swapchain recreation, while an explicit minimize/restore action
was not automated in this local pass. Native Vulkan, other GPUs/drivers, distinct
graphics/present queue families, injected acquire/submit/present failures, and
repeated recreation stress remain broader validation-matrix work; none is hidden
by the Phase-6 implementation status.
