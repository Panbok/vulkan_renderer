---
status: partial
updated: 2026-10-19
authority: adr
---

# ADR-127: Crash reports, GPU capture and the render graph description

## Status

Accepted (partial). Phase P3 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented: crash reports with the active pass on macOS and Windows, the
`debug.gpu_capture` request with Metal and RenderDoc backends, the
`debug.graph` description, the Render graph window, debug drawing, freeze
culling and the LOD, light count and overdraw views. Metal
programmatic capture is refused by macOS 26 for Metal 4 compute encoders,
and RenderDoc cannot capture this renderer on a driver without
`descriptorBufferCaptureReplay`, such as the Windows RX 6700 XT's.

## Context

A crash left only the session log and the system's `.ips` report, which
knows neither the frame, the scene nor the agent operation that ran. No
agent could start a native GPU capture. The compiled render graph was
visible to no tool: `vkr_rg_export_dot` had no caller.

## Decision

**Crash reports** ([vkr_crash.h](../../lib/src/platform/vkr_crash.h)).
`vkr_crash_install(directory, program)` installs handlers for SIGSEGV,
SIGBUS, SIGILL, SIGFPE, SIGABRT and SIGTRAP on an alternate signal stack
([vkr_crash_posix.c](../../lib/src/platform/vkr_crash_posix.c)).

- The handler writes `crash-<program>-<pid>-<unix seconds>.txt` with the
  signal, its code and address, the breadcrumbs, the newest 64 log records
  and a backtrace. It then restores the default action and raises again, so
  the system report and exit status stay as before.
- Breadcrumbs are fixed buffers any thread sets without locks:
  - the frame number and frame stage, from the application host loop;
  - the scene path, from the runtime each frame;
  - the running agent operation, from the editor's op dispatch;
  - the render thread's pass. Metal sets it for each encoded pass (a
    batched compute run names its first pass); Vulkan for each pass it
    prepares and records. A failed pass keeps its name; a finished frame
    clears it.
- Reports go into the editor's session log directory (`<workspace>/logs/`),
  or the player's settings folder; with no directory, to stderr. A second
  fault during a report goes straight to the default action.
- `vkr_crash_write_report(reason)` writes the same report without a signal.
  Both GPU completion timeouts call it before they exit (ADR-124).
- The log tail is read from the history ring without the logger lock
  (`log_history_emergency_write`), so a record being written can come out
  torn. `backtrace` is not on the POSIX async-signal-safe list. Everything
  before the backtrace is already written if it fails.
- Windows ([vkr_crash_windows.c](../../lib/src/platform/vkr_crash_windows.c)):
  an unhandled-exception filter, with a 64 KiB `SetThreadStackGuarantee` so
  a stack overflow still reports, writes `crash-<program>-<pid>-<time>.dmp`
  through `MiniDumpWriteDump` (`MiniDumpWithThreadInfo`; dbghelp loaded at
  install from System32 only) and the same text report: the exception name,
  code and address, for an access violation the access and its address,
  then the context and a backtrace as module+offset frames with the
  faulting instruction first. Large buffers are static, guarded by the
  re-entry flag, so the filter allocates nothing. It then hands the
  exception to the previous filter, normally the C runtime's, or answers
  `EXCEPTION_CONTINUE_SEARCH`, so Windows Error Reporting and the exit code
  stay as before.
- A `SIGABRT` handler, installed only over the default action, writes a
  dump and the report for `abort()` (every failed `assert`), then restores
  the default and calls `abort()` again, so the process still fast-fails
  with `0xc0000409`. `__fastfail` and `/GS` failures bypass both.

**GPU capture** ([vkr_renderer.h](../../renderer/src/vkr_renderer.h)).
`vkr_renderer_request_gpu_capture(path, frames)` and
`vkr_renderer_gpu_capture_status` drive a capture for a GPU debugger.

- On Metal the render thread starts an `MTLCaptureManager` capture of the
  device before the next frame encodes, and stops it after `frames`
  committed frames
  ([vkr_metal_packet_frame.inc](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)).
- Capture to a `.gputrace` document needs the capture layer, which a
  process outside Xcode gets only with `MTL_CAPTURE_ENABLED=1`. Without it,
  the request fails with that reason.
- The capture layer throws in the pipeline archive lookup (an unrecognized
  `supportsAIRNT` selector sent to a dispatch queue), as the shader
  validator does. So capture-enabled runs compile pipelines from source
  ([vkr_metal_packet_setup.inc](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc)).
- On Vulkan ([vkr_vulkan_gpu_capture.c](../../renderer/src/vulkan/vkr_vulkan_gpu_capture.c))
  RenderDoc captures through its implicit layer, which the loader enables
  when `ENABLE_VULKAN_RENDERDOC_CAPTURE` is 1 at instance creation:
  RenderDoc sets it for a process it launches, and `VKR_RENDERDOC_CAPTURE=1`
  makes the device set it through `vkr_platform_environment_set`. After the
  instance exists, the device opens the already loaded `renderdoc.dll` by
  name and binds the in-application API 1.6 from the vendored
  `renderdoc_app.h` (`vendor/renderdoc.md`). The render thread calls
  `StartFrameCapture` with the instance and no window before the next
  frame's first API call, and `EndFrameCapture` after the last captured
  frame's submission and present; the status answers the `.rdc` RenderDoc
  wrote. Without RenderDoc the request answers how to start it.
- RenderDoc exposes `VK_EXT_descriptor_buffer`, which this renderer
  requires, only on a driver with `descriptorBufferCaptureReplay`; on any
  other driver device creation fails under its layer, and the log says so.
- The frontend passes each backend a base path: Metal adds `.gputrace`,
  RenderDoc its frame number and `.rdc`.
- `debug.gpu_capture {name, frames}` (1 to 8) writes into the recording
  directory and answers the path, or the failure with its reason
  ([editor_ops.c](../../editor/src/editor_ops.c)).

**Debug drawing** ([vkr_debug_draw.h](../../runtime/src/core/vkr_debug_draw.h)).
The runtime owns a fixed list of world-space segments (8192) and labels
(64). Boxes, spheres (three 24-segment circles) and frusta add segments.

- Each shape has a named group, a colour from eight names and an expiry
  time (zero lasts until cleared). The frame loop expires shapes before the
  UI build; a full list stores what fits and counts the rest as dropped.
- The editor draws the list over the Scene, on top of the image without
  depth ([editor_debug_draw.c](../../editor/src/editor_debug_draw.c)). One
  path widget holds every segment, cut at the view's planes, and each label
  is a widget. Both are placed after the camera is final, as the recording
  route is. A path takes at most 16384 points: three per segment, two for
  one that continues the previous segment, so about 5,400 to 8,192 segments
  show.
- Shapes are in level coordinates; the projection adds the level's origin
  offset while Play rebases.
- `debug.draw {lines, boxes, spheres, labels, frustum, color, seconds,
  group}` adds shapes after checking every item, and `debug.clear {group,
  all}` removes them ([editor_ops.c](../../editor/src/editor_ops.c)).
  `frustum` adds the Scene camera's current frustum, which shows once the
  camera moves.

**Render graph description** ([vkr_rg_debug.h](../../renderer/src/vkr_rg_debug.h)).
`vkr_rg_describe` visits the compiled graph's passes, in execution order
and then culled, with the resources each reads and writes. It then visits
the resources with kind, extent, format, bytes, lifetime, instance domain
and first and last use.

- `vkr_renderer_describe_graph` joins the render thread, adds the backend's
  realized memory and, given a path, writes `vkr_rg_export_dot` output.
- `debug.graph {passes, resources, pass, dot}` answers it with the newest
  CPU and GPU milliseconds per pass, at most 512 rows per list.
- The graph never aliases memory, and one resource's backend instance count
  is not exposed, so only the backend's live totals count instances.
- Pass timings are matched by name through `vkr_renderer_metrics_pass_find`
  ([vkr_renderer_metrics.h](../../renderer/src/vkr_renderer_metrics.h)).

**Render graph window** ([editor_render_graph.c](../../editor/src/editor_render_graph.c)).
Develop > Render graph, or `window graph`, shows the same description.

- A Passes list (order, kind, CPU and GPU ms, reads, writes) and a
  Resources list (size, format, bytes, lifetime, first and last pass) with
  a name filter and sorting. The selected row's uses list below; clicking
  a use selects the other side.
- A summary line gives the pipeline, backend, viewport and render scale,
  scheduled and culled counts, declared against live memory, and whether
  GPU pass timing is on (F7).
- The window describes the graph only while it shows: four times a second,
  or on Refresh while paused, since each description joins the render
  thread. It keeps at most 512 passes, 512 resources and 4096 uses, and
  writes DOT through an icon button.

**Freeze culling.** `VkrWorldPassPayload.frozen_cull` (frame input
version 54, [vkr_frame_input.h](../../renderer/src/vkr_frame_input.h)) is an
optional `VkrCullCamera`: a view, projection and position that cull the
camera view in place of the frame's camera, which still rasterizes and
shades.

- The camera view's frustum comes from it on both backends
  ([vkr_metal_packet_resources.inc](../../renderer/src/metal/internal/vkr_metal_packet_resources.inc),
  [vkr_vulkan_deferred.c](../../renderer/src/vulkan/vkr_vulkan_deferred.c)),
  and `vkr_packet_write_lod_views` measures draw distance, smallest drawn
  size and levels from its position, without morph motion. The motion view
  copies the camera view.
- On Metal, occlusion tests the depth pyramid built at the frozen camera.
  `vkr_metal_packet_hzb_frozen_history` looks for a history instance of
  the frozen view-projection, world and extent before the graph is built;
  when one exists the frame skips the HZB build, so the instance keeps, and
  the new graph condition `hzb_read_enabled` keeps `Cull.Classify`'s read.
  Until one exists the frame builds as usual, and only a build at the
  frozen camera matches. A history needs an unjittered frame: under the
  MetalFX upscaler's jitter no pyramid is built, so live and frozen culling
  both run without occlusion, and frozen culling repeats what live culling
  did in each configuration. Vulkan turns occlusion off while frozen.
- The scene runtime keeps the camera of the first world payload after
  `freeze_culling` turns on and builds rooms, scatter proxies, the blend
  list and `view.cull_report` from it
  ([vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c)).
  Room-gated lights, shadows and light selection keep the live camera. A
  probe capture never freezes.
- The editor's switch is `VkrSampleViewState.freeze_culling`: the Scene's
  draw distance chip row Freeze culling (the chip then reads Frozen), Cmd
  `view.freeze_culling`, `view.freeze_culling = true`, and op
  `view.freeze_culling {on}`. While frozen, the sample runtime keeps the
  frozen frustum in debug group "freeze culling", in cyan.

**LOD and light count views.** Render modes `LOD` (20) and `LIGHT_COUNT`
(21) are tiled-pipeline data views beside the artist views of
[ADR-099](099-artist-views-and-luminance-queries.md), selected as `lod` and
`light-count` (Cmd `view.mode`, `view.capture`) or `lod` and `light_count`
(harness cases).

- `LOD` colours each draw by the level the camera view selected,
  `vkr_editor_lod_color` in
  [editor_view.slangh](../../renderer/src/shaders/shared/editor_view.slangh):
  grey 0, red 1, green 2, blue 3, yellow 4, magenta 5, cyan 6, orange 7.
  A geomorphing draw moves toward the next level's colour as it morphs.
- `LIGHT_COUNT` counts the local lights that reach each surface
  (`vkr_metal_tiled_light_count` in
  [tiled.metal](../../renderer/src/shaders/metal/msl/world/tiled.metal)):
  its light grid cell's punctual lights within range and cone, and the
  rectangle lights it faces whose diffuse bound passes the shading cut-off.
  `vkr_editor_light_count_color` ramps dark blue (none), blue (1), green
  (4), yellow (8), red (12) to white (16 or more). The sun is not counted.
- Both shade by the angle to the viewer so shapes read. The desktop
  pipeline draws no artist view, so neither runs on Vulkan.

**Overdraw view.** Render mode `OVERDRAW` (22, `overdraw`) counts the
surfaces that rasterize under each pixel, hidden or not. Hidden-surface
removal shades an opaque pixel once on the tiled GPU, so a counter in the
shading would measure something other than raster load; this view draws
the geometry itself.

- `Tiled.Opaque` draws every opaque, alpha-tested and Custom-graph bucket
  with `vkr_metal_tiled_overdraw_fragment`, an additive pipeline
  (`VKR_METAL_PACKET_BLEND_ADD`) under `tiled_overdraw_state` (no depth
  test, no depth write), and skips the sky draw. `Tiled.Blend` draws the
  blended list with `vkr_metal_tiled_overdraw_blend_fragment`. Each surface
  adds `VKR_EDITOR_OVERDRAW_LAYER` (1/32); the tonemap's
  `VKR_METAL_PACKET_TONEMAP_FLAG_OVERDRAW` colours the sum with
  `vkr_editor_overdraw_color`: dark blue 1, blue 2, green 4, yellow 8, red
  16, white 32 or more.
- Alpha-tested surfaces count whole triangles, and a Custom graph's draws
  take the Standard vertex function. The view writes no depth, so the
  editor's grid shows through, and it builds no depth pyramid.
- The desktop pipeline does not draw it.

## Consequences

- A crash or GPU timeout leaves a report that names the frame, the scene,
  the agent operation and the recent log.
- An agent can read the render graph without a debugger and hand a DOT file
  to Graphviz.
- An agent can mark positions, bounds and camera frusta in the Scene and
  check them in a window capture.
- An agent or a user can fly around a frozen camera's culling result, and
  see which levels draws select, where many lights overlap and where
  surfaces stack.
- A Metal GPU capture from inside the engine waits for Apple to support
  Metal 4 compute encoders in programmatic capture. Xcode's capture layer
  no longer crashes the editor at startup.

## Alternatives considered

- A separate crash-reporting process: catches more (other threads' stack
  overflows) but adds a process and IPC.
- Writing reports from the logger's file: needs the logger lock, which a
  crashing thread may hold.

## Revisit when

- macOS supports capturing Metal 4 compute encoders.
- RenderDoc or the AMD driver supports descriptor buffers for capture, so
  a Vulkan capture can run on the Windows machine.
- A crash on another thread's stack overflow needs a report (per-thread
  alternate stacks).

## Evidence

- `./build_release.sh`, `./build_editor.sh Release` and `./build_test.sh`
  (Debug, ASan) on the M1 Pro, 2026-10-18: 860 test cases passed, including
  `test_crash_write_report`. That test checks the reason, pid, frame and
  every breadcrumb, that a long breadcrumb is cut to fit, and that a
  backtrace follows.
- A scratch driver built against `libvkr_foundation.a` wrote reports for a
  null write (exit 139), a trap (exit 133), a main-thread stack overflow
  and a timeout-style report; macOS still wrote its `.ips` reports.
- A headless Bistro editor started with `MTL_CAPTURE_ENABLED=1` aborted
  before the archive change, and the editor's crash handler printed the
  SIGABRT report with its backtrace. After the change it started, and
  `debug.gpu_capture` answered "Capturing Metal 4 Compute Command Encoder
  is not supported." Without the variable it answered the
  `MTL_CAPTURE_ENABLED=1` reason.
- `debug.graph` on a headless Bistro editor answered the tiled pipeline's
  34 passes and 31 resources, declared image bytes 258 MB against 406 MB
  live, and wrote a 13 KB DOT file.
- 2026-10-19, same builds, 860 test cases passed. `run_debug_draw_tests`
  checks clearing by group, inclusive expiry in order, refused non-finite
  and overflowing shapes, label truncation and colour names.
  `test_crash_write_report` also sets a breadcrumb from an unterminated
  slice.
- The Render graph window on a headless Bistro editor showed 34 passes and
  31 resources, jumped from a use to its resource, filtered, and wrote a
  230-line DOT file.
- `debug.draw` on a headless Bistro editor stored a box, a sphere, three
  lines and two labels (89 shapes), refused a line without `to` and added
  nothing, and added a 12-segment camera frustum. A window capture showed
  every shape and label at its place; after `debug.clear {group: probe}`
  only the frustum remained.
- Windows crash reports, 2026-10-19: a scratch driver linked against the
  Release `vkr_foundation` wrote, for a null write, a 29 KB minidump and
  `EXCEPTION_ACCESS_VIOLATION (code 0xc0000005, …, write 0x0)` with
  `#0 crash_probe.exe+0x3cb7` (exit `0xc0000005`); for a stack overflow a
  1.0 MB dump and 54 frames (exit `0xc00000fd`); for `abort()` and
  `raise(SIGABRT)` a dump and `reason: SIGABRT (abort)` (exit
  `0xc0000409`, as without the handler); and `vkr_crash_write_report` a
  554-byte report with the frame and breadcrumbs. No WerFault dialog stayed
  open.
- Vulkan capture, RX 6700 XT, Adrenalin 26.6.3, RenderDoc v1.45: with
  `VKR_RENDERDOC_CAPTURE=1` the editor logged `RenderDoc capture available
  (API 1.7.0)`, then no device: the driver reports
  `descriptorBufferCaptureReplay` false, so the layer hid
  `VK_EXT_descriptor_buffer`, and the new log line named that cause. No
  `.rdc` was taken. Without the variable, `debug.gpu_capture` answered how
  to enable RenderDoc.
- `debug.graph` on a headless Release Bistro editor on Vulkan answered 200
  executed passes (135 graphics, 57 compute, 8 transfer), none culled, in
  unique ascending order and matching `perf.snapshot`'s 200 pass rows, and
  66 resources (29 transient, 17 per frame slot, 11 retained, 7 history, 1
  imported, 1 graph); it wrote a 153 KB DOT file. `IBL.Bake`,
  `DiffuseVolume.Compose` and `Clouds.SkyLight` list no reads or writes:
  they write backend-owned images the graph does not declare.
- The Vulkan pass breadcrumb on Windows, 2026-10-19: a Release editor built
  with a temporary trigger that wrote through a null pointer while
  recording `Cull.Classify` (removed afterwards) exited with `0xc0000005`
  on frame 2 of a headless Bistro run. Without a workspace its report went
  to stderr, naming the access violation, the scene, stage `frame`, `pass:
  Cull.Classify`, the log tail and render-thread frames from
  `vkr_editor.exe+0x7113fd`.
- On a headless Release Bistro editor on Vulkan, `debug.draw` stored a box,
  a sphere and a label (85 shapes, cyan, group `probe`) and a red line
  (group `keep`). A window capture showed them where the camera places
  them, drawn over the scene, and after `debug.clear {group: probe}` only
  the line remained. The Render graph window showed "Desktop pipeline ·
  Vulkan · 1275×700" with 75 passes and 66 resources, listed
  `Cull.Classify`'s two reads and two writes, jumped from
  `gpu_draw_classifications` to the resource (passes 9 to 13, written by
  `Cull.Classify`, read by `Cull.Encode`), and wrote a 1,694-line DOT
  file. `run_debug_draw_tests` passed on Windows.
- Freeze culling, 2026-10-19, same builds, 860 test cases passed, on a
  headless Bistro editor at the tiled pipeline's defaults. At eye
  (-12, 6, 14) `view.cull_report` counted 2567 candidates, 670 outside the
  view, 126 too small and 1771 drawn, and the GPU drew 1771 in the lit
  view: under MetalFX's jitter no HZB is built
  (`visibility.hzb.history_rejection.disabled`). Frozen there and moved to
  (-70, 60, 75), the report was unchanged with `frozen` true, the GPU drew
  exactly 1771 and the blend list kept its 277 draws. A capture showed only the frozen view's buildings and the cyan
  frustum. Unfrozen at the same eye the report counted 1054 drawn and the
  blend list 342.
- LOD view from (-70, 60, 75): Bistro's draws showed levels 0, 1 and 2.
  Light count view at (-12, 6, 14): ramps followed the lamps' ranges. The
  tiled shaders compiled at runtime with no error.
- Frozen occlusion, same editor: in the LOD view at (-12, 6, 14), which
  renders unjittered, the GPU drew 171 with `visibility.hzb.rejected`
  1600. Frozen there and moved to (-70, 60, 75), still in the LOD view, it
  drew 171 with 1600 rejected and `history_valid` 1, so the frame kept the
  frozen pyramid. Switched to the lit view (another extent, jittered) it
  drew 1771 with the HZB disabled, as live culling does there.
- Overdraw view at (-12, 6, 14): foliage and the tree reached 32 layers or
  more and walls 4 to 16; frozen and moved away, the frozen draws showed
  their layers under the cyan frustum. Live in that view the HZB stayed
  disabled.
- The Vulkan freeze culling path is compiled on the Mac only.
