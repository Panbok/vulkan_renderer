---
status: partial
updated: 2026-10-20
authority: adr
---

# ADR-130: Performance window tabs and the Tier 1 inspection streams

## Status

Accepted (partial). The editor's Performance window has six tabs, and Tier 1
recordings carry the data the five new ones read; Apple GPU counters from an
Instruments trace can be imported into a recording for the HW tab, and
AMD GPUs on Vulkan sample per-pass hardware counters into Tier 1
recordings through GPUPerfAPI. Metal evidence is on this Mac, and the HW
tab's Vulkan pipeline statistics ran on an RX 6700 XT; the Vulkan backend's
census pass, readback and command counters have no native run yet (Windows
machine). The AMD counters ran natively on Bistro with Adrenalin 26.10.1;
the one-frame sessions before them hung the GPU on Adrenalin 26.6.3.

## Context

The Performance window ([ADR-122](122-recordings.md)) showed a recording's
frame times, CPU stages, pass times, spikes and memory on one page. A
recording held no render graph shape, no backend command counts, no CPU
allocation counts in Release (the build sets `VKR_ALLOCATOR_DISABLE_STATS`, so
the global statistics stay zero), no per-object culling result, and no object
bounds. Questions such as "which pass feeds this one", "what did the
encoder record", "which arena grows" and "what did this frame draw and cull"
needed a live editor, a GPU capture or a debugger.

## Decision

1. **Tabs.** The window shows a tab row under its header
   ([`perf_tabs`](../../editor/src/editor_recording_perf.c)): Overview (the
   previous page, unchanged), the backend named after the recording's
   pipeline class (Metal for tiled, Vulkan for desktop, from the IDENTITY
   chunk), Memory, Draws, Render graph and HW. Every tab but Overview shows a
   frame-time strip that selects and zooms like the Overview charts, and
   describes one frame: the playhead while a playback plays, else the
   selected frame, else the playhead, else the frame under the pointer
   (`vkr_editor_perf_focus`). Selection, zoom and hover are shared by all
   tabs. Each tab lives in its own file
   (`editor/src/editor_recording_perf_{backend,memory,draws,graph,hw}.c`) and
   shows what a recording lacks and how to record it instead of empty charts. The
   agent operation `rec.analyze` takes `tab`, `frame` and `flow` to show one.
2. **Render graph shape.** When a Tier 1 recording's frame runs a pass that
   no recorded shape holds, the recording asks the renderer for a snapshot
   (`vkr_recording_graph_wanted`, `vkr_renderer_request_graph_snapshot`);
   the snapshot covers its scheduled passes and the passes that asked for
   it. Culled passes come and go every few frames (local shadow faces,
   cascade copies) and the pass rows already show them, so they make no new
   shape: asking on every change of culling gave 284 shapes in a 722-frame
   Bistro walk, asking on new passes gave 2.
   The thread that renders the next frame fills it with `vkr_rg_snapshot`
   (passes in execution order with their uses, declared resources and every
   name, fixed capacities, no pointer into the graph) without the frame loop
   joining the render thread; the frame loop takes it a frame later and
   writes a `graph` record under the recorded frame that rendered it
   ([`vkr_recording_format.h`](../../runtime/src/assets/vkr_recording_format.h)).
   The reader keeps every version by frame and joins its passes to the pass
   rows by name (`vkr_recording_data_graph_at`).
3. **Stepped pass list and flow.** The Render graph tab lists the shown
   version's passes in execution order. Values change only when the update
   window steps (every frame, 100 ms, 250 ms or 1 s of the recording) and show
   the window's mean or max, or the frame's value; a value that moved more
   than 6 % flashes, and each pass keeps a trend of its last 60 updates. The
   flow lays passes out top to bottom by dependency depth (a pass sits one
   row under the deepest pass whose writes it reads), draws one wire per
   producer and consumer pair widened by resource size, and marks the
   selected pass's inputs and consumers.
4. **Tier 1 measurements.** Tier 1 recordings (not rolling ones, which keep
   no start-time records) carry the catalog metrics named by
   `VKR_RECORDING_TIER1_MEASUREMENTS` (GPU memory by owner, draws, culling,
   pipeline, backend, render graph memory, uploads and streaming) in the
   existing measurement streams; all measurements still carry the whole
   catalog. `summary.json` gains `all_measurements`, so the Library tells
   the two apart.
5. **Backend counters.** Both backends count per frame, from one submit to
   the next, the `backend.*` metrics: command buffers, queue submits,
   encoders or render passes, ICB executions, direct and indirect draw
   calls, dispatches, barriers, descriptor writes and pipeline binds. A
   counter a backend cannot count reports UNAVAILABLE/UNSUPPORTED (Metal 4
   copies on compute encoders, so it has no blit encoders). `pipeline.binds`
   and `pipeline.frame_changes` are measured. The Backend tab lists every
   `backend.`, `pipeline.`, `draw.world.`, `gpu.present.`, `upload.` and
   command-slot series, the frame's passes as a strip and a table coloured
   by kind, the CPU cost of building and submitting each frame, and the CPU
   functions of recordings with zones, reduced per frame by zone and thread
   class at load.
6. **Named allocators.** The lib's allocator registry
   ([`vkr_allocator.h`](../../lib/src/memory/vkr_allocator.h)) counts
   allocations, frees, scopes, live bytes and arena footprints of allocators
   and arenas registered by name, with relaxed atomics and only when
   registered, independently of `VKR_ALLOCATOR_DISABLE_STATS`. The runtime
   names its frame arenas (`frame.0`, `frame.1`), `app.persistent`,
   `app.metrics`, the job workers' scratch (`job.scratch.N`), the renderer's
   `render.persistent` and each backend's `render.graph`. A Tier 1 recording
   takes every registered allocator's counts at its start, samples the
   registry once per frame on the frame loop and writes per-frame
   differences (`allocator_name`, `allocators`); one registered while the
   recording runs counts from its first sample. The Memory tab stacks RAM by
   named allocator and VRAM by GPU owner on separate axes and never sums
   them; allocations and frees per frame; and lists where memory lives and
   each arena's used, committed and reserved bytes, peak, scopes and depth.
7. **Draw census.** While a Tier 1 recording runs, the runtime sets
   `VkrGpuDebugPayload.draw_census`; a census pass after the camera's culling
   writes each candidate's 4-bit state (drawn, deferred by the tiled first
   phase then drawn, occluded, outside the frustum, past the draw distance)
   and its object id, read back with the culling counters' latency. The
   classification words carry reason codes always; the census pass and its
   buffers are culled otherwise. The recording writes object boxes when the
   world's generation changes (`vkr_world_object_bounds`), the candidate ids
   when they change, and the states every frame (`draw_bounds`, `draw_ids`,
   `draw_states`). The Draws tab maps objects top down, coloured by their
   most visible candidate's state, under the camera's path coloured by frame
   time, the frustum at the frame and spike pins; beside it the culling
   funnel, the frame's draw counters by pass and cascade or objects by cell
   of the ground (the world partition's cell size, else 32 m), and below the
   candidates by state over the shown frames.
8. **Hardware (HW tab).** The renderer owns a pipeline registry
   ([`vkr_pipeline_registry.h`](../../renderer/src/vkr_pipeline_registry.h)):
   each backend adds every pipeline it creates, on any thread, with its
   compile time and what its API reports about the hardware it needs.
   Metal adds compute and tile pipelines' `maxTotalThreadsPerThreadgroup`,
   `threadExecutionWidth` and static threadgroup memory; an Apple GPU lowers
   the first below the device's 1024 when a kernel holds many registers, the
   only public sign of register pressure there, and reports nothing about
   render stages. Vulkan, with the graphics setting Capture shader
   statistics (off by default, applies at the next start, hidden on the
   tiled pipeline) on a device with `VK_KHR_pipeline_executable_properties`,
   creates pipelines with `VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR` and
   adds each executable's driver statistics, summarized into vector
   registers, spill bytes, instructions and shared memory used by name
   fragment because the names are driver-defined (AMD: `numUsedVgprs`,
   `scratchMemUsageInBytes`, `ldsUsageSizeInBytes`; its
   `ldsSizePerLocalWorkGroup` is the limit and is left out). The HW tab
   adds an SGPRs column from the statistics whose name holds `sgpr`
   (`numUsedSgprs`), which needs no recording field. `VkrDeviceInformation` gains the GPU
   architecture (Metal) or driver name (Vulkan), unified memory, the
   working set, threadgroup memory and thread limits and the SIMD width. A
   Tier 1 recording writes the device's facts once (`hardware`, with the
   OS's GPU model and core count) and the registry's pipelines as they
   appear (`pipeline`, those created before it under its first frame). The
   lib's GPU monitor reads, on macOS, the Apple GPU driver's statistics
   every 250 ms on its own thread ([ADR-125](125-platform-telemetry-metrics.md)),
   recorded as `os.gpu.*` Tier 1 measurements. The HW tab shows cards of
   utilization, the parameter buffer, scene splits, GPU recoveries and
   register-limited pipelines; charts of device, renderer and tiler
   utilization and of the tiled scene, parameter buffer and splits; the
   device's facts; the pipelines, filtered by kind, with only the columns
   the recording has values for; the selected pipeline's driver statistics;
   and a note that occupancy and limiter counters of desktop GPUs reach only
   vendor profilers.
9. **Imported GPU counters.** Apple's occupancy and limiter counters reach
   only Instruments, so a recording imports them after the fact. With
   signposts on (`perf.set {signposts}`), the frame loop emits an
   os_signpost event `Clock` per frame carrying
   `vkr_platform_get_monotonic_ns` ([ADR-126](126-cpu-zones-and-gpu-pass-timeline.md)).
   A trace of the Metal GPU Counters and os_signpost instruments, taken
   while the recording runs, holds a sample of each of 31 counters about
   every 39 us from each of two counter ring buffers.
   [`vkr_recording_counters_import`](../../runtime/src/assets/vkr_recording_counters.c)
   streams the trace's os-signpost, `gpu-counter-info` and
   `gpu-counter-value` tables out of `xcrun xctrace export` through
   `vkr_platform_process_run`'s stdout callback (a pipe on macOS and
   Windows), resolving xctrace's id and ref values as it reads, maps trace time
   to the CPU clock by the median offset of the `Clock` events, averages
   the samples of both ring buffers into buckets (50 us by default) and
   writes the bundle's `gpu_counters.bin`
   ([`vkr_recording_format.h`](../../runtime/src/assets/vkr_recording_format.h)).
   `vkr_recording_counters_trace` runs `xcrun xctrace record` on a process.
   The editor takes the trace itself
   ([editor_recording_counters.c](../../editor/src/editor_recording_counters.c)):
   with the Recordings window's GPU counters checkbox (macOS, tiled
   pipeline), `rec.start {gpu_counters: seconds}` or `rec.counters
   {seconds}` during a recording, it turns signposts on, traces its own
   process for 3 s (1 to 10) and imports into the recording on a thread of
   its own; a Scene chip under the REC chip shows tracing or the samples
   imported (a click cancels), a toast the outcome, and `rec.status`
   `gpu_counters` the state. `vkr_rec counters TRACE --recording BUNDLE`
   imports a trace taken by hand.
   The loader ([`vkr_recording_data_finish_counters`](../../runtime/src/assets/vkr_recording_analysis.c))
   reads each frame's GPU span from `pass_starts` and averages each counter
   over it, and over each pass's span on GPU-timed frames (one in 8 on the
   tiled pipeline), mapping `pass_starts` rows to the kept pass rows; buckets
   without samples do not count, and a file that does not parse leaves the
   recording loaded without counters. The HW tab switches between Driver and
   GPU counters (counters by default when the recording has them): cards of
   the four limiters with the highest means and the fragment and compute
   occupancy; limiter and occupancy charts over the frames with samples; a
   table of every counter's mean, p95, max and focused-frame value; and the
   passes with the limiter that bounds each, the two top limiters and
   occupancy. Without counters it says how to take them.
10. **Scene frame time and rendered frames.** With the editor presenter
   ([ADR-111](111-editor-presenter.md)) the editor loop runs at the
   display's rate while the Scene renders on some of its iterations, so
   `frame.wall` is not what the Scene shows. The runtime records
   `frame.scene_interval` (Tier 0): the time between the GPU finishing one
   editor Scene frame and the next, from `vkr_renderer_scene_frames_completed`,
   restarted when the Scene stops. The analysis treats a frame with
   `cpu.graph_build` as one that rendered a Scene, and when under 95 % of
   frames did, takes `frame.scene_interval` as the frame time, or for older
   recordings the derived `frame.scene`, the loop time between frames that
   built a Scene; a series that is never non-zero on the other frames
   (draws, passes, GPU) leaves them unsampled, so means and medians cover
   rendered frames. Each frame keeps its GPU span from `pass_starts`, and
   the pass chart stacks the GPU time outside any timed pass on top of its
   passes, so a column reaches the GPU frame. The window's focus snaps to
   the newest rendered frame at or before it, and frame steps move between
   rendered frames. Metal reads the world's draw counts back with its GPU
   diagnostics on some frames; frames without them leave `draw.world.*`
   unsampled instead of counting only the editor's overlay draws.
11. **One update period.** The header's Update control (Frame, 200 ms, the
   default, or 1 s of the recording) holds a playing recording's focus for
   the period, so the tabs change at most that often, and sets the span the
   Render graph averages over, which had its own control. Paused, stepped
   or selected frames show exactly.
12. **Map fit and labels.** The Draws map fits the camera's route with room
   around it (Route), or the level (Level): the 1st to 99th percentile of
   object centres with the route, so one kilometre-wide terrain does not
   shrink the level to a dot. A census over the loader's word budget keeps
   every k-th frame across the recording, and the map and chart read the
   nearest kept frame, where the cut used to drop the recording's end. The
   Draws and Metal counter tables name metrics in words
   (`vkr_editor_perf_metric_label`), the raw name in the tooltip; the
   Overview's cards name Scene percentiles and a GPU frame p50 / p95, and
   the inspector lists Scene frame, GPU frame and editor loop, then the
   passes' share of the GPU frame with the time outside any pass.
13. **UI primitives.** `vkr_ui_path` takes filled rectangles under its strokes,
   so a map draws up to 16,384 footprints per widget within the frame's
   widget budget. Editor tables take per-cell text colours, fills and share
   bars.
14. **Sampled AMD counters.** Desktop GPUs' occupancy and limiter counters
   reach only vendor libraries; on AMD under Vulkan the renderer samples
   them with GPUPerfAPI 4.4.1.3 (MIT). CMake downloads its release on
   Windows x86-64 at configure time, pinned by SHA-256
   ([`cmake/vkr_gpa.cmake`](../../cmake/vkr_gpa.cmake)), keeps the headers,
   `GPUPerfAPIVK-x64.dll` and the licence, and copies the DLL beside the
   editor and the harness; a failed download builds without counters
   (`VKR_HAS_GPA=0`). The library must initialize before the device and
   its queue exist, so device creation, on an AMD candidate (vendor
   0x1002) with `VK_AMD_gpa_interface` and
   `VK_AMD_shader_core_properties`, loads it and calls `GpaInitialize`
   first, then enables those extensions (the instance enables
   `VK_KHR_get_physical_device_properties2`); the device report's
   `amdGpuCounters` line says whether it did. It is always on for such a
   GPU and costs nothing until a request.
   GPUPerfAPI's Vulkan backend offers discrete counters only (its
   streaming counters return `ApiNotSupported`), so
   [`vkr_vulkan_amd_counters.c`](../../renderer/src/vulkan/vkr_vulkan_amd_counters.c)
   brackets every graph pass as one sample beside its timestamps. It asks
   for 16 counters (the units' `*Busy`, `MemUnitStalled`,
   `WriteUnitStalled`, the L0, L1 and L2 hit rates, `CSVALUUtilization`,
   `CSLDSBankConflict`, `PSExportStalls`) and skips names a generation
   lacks, and splits them into groups the hardware reads in one replay,
   planned with probe sessions that never begin. A session samples one
   group over up to 30 frames: frame k's pass p is sample
   k × 416 + p, and passes match across frames by name. The counter
   context keeps the driver's own clocks. Once the GPU finished a session,
   the render thread reads 1,024 samples a frame without waiting and
   publishes each pass's mean through a one-slot handoff; a session that
   fails is deleted only 4 frames later, after the frames that may still
   run its commands.
   While a Tier 1 recording runs, the runtime writes each published
   session into the `gpu_counter_name` and `gpu_counters` streams
   ([`vkr_recording_format.h`](../../runtime/src/assets/vkr_recording_format.h))
   and the device's vendor into a `Vendor` fact. It places a session's
   first and last frame through the renderer frame indices of the last 128
   recorded frames, since a session arrives about 40 frames after its
   first. The Recordings window's
   GPU counters checkbox (on an AMD GPU), `rec.start {gpu_counters:
   seconds}` and `rec.counters {seconds}` request sampling for 1 to 10 s;
   `rec.status` reports `gpu_counter_sessions`. The loader packs the
   sampled names like an imported file: a pass's value is its mean over
   the sessions that sampled it; a frame's percentage is the mean of its
   passes weighted by their GPU time and a frame's count their sum;
   frames outside a session stay unsampled. With the `Vendor` fact AMD,
   the HW tab takes the `*Busy` counters other than `GPUBusy` as the
   limiters and `MemUnitStalled`, `WriteUnitStalled` and `L2CacheHit` in
   place of occupancy. On other desktop GPUs a recording without counters
   hides the GPU counters view.

## Evidence

Metal, M1 Pro, Release editor, headless, 2026-10-20: a Tier 1 recording with
zones of a 722-frame Bistro walk (`rec.start {tier: "1", zones: true}`, 720
gliding `view.camera` steps on a 16 m circle around the Bistro orbit centre)
wrote a 582 KB stream with 396 Tier 1 measurements, 2 graph shapes, 0
dropped and 0 lost records. `rec.analyze` with each `tab` captured every tab
without UI layout failures. At frame 400 the census reported 2,567
candidates, 637 outside the frustum, 352 past the draw distance, 1,502
occluded, 76 drawn and 1 deferred then drawn late; the frame's
`draw.world.opaque_draws` was 75, consistent with the drawn count. The
backend counters read 16 render and 7 compute encoders, 104 ICB executions
and 92 pipeline binds. CPU tests: `vulkan_renderer_tester` (113 suites) with
`test_recording_graph_allocators_census`, `run_allocator_registry_tests` and
`run_draw_census_tests`. Unavailable: the Vulkan backend's census pass,
counters and readback on hardware (they compile, and the SPIR-V builds,
on this Mac), recording overhead timing at Tier 1, and the census pass's
GPU cost.

Metal, M1 Pro, Release editor, headless Bistro, 2026-10-20: a Tier 1
recording of a 2,539-frame walk (`rec.start {tier: "1"}`, 600 gliding
`view.camera` steps) held 478 pipelines (593 ms of compiling; the slowest,
the animation preview, 11.1 ms) and the device's facts (Apple M1 Pro,
`applegpu_g13s`, 14 cores, 11.84 GiB working set, 32 KiB threadgroup
memory). Three compute kernels, among them the cloud sky-light SH and the
atmosphere multiple scattering at 896 threads, report fewer than the
device's 1,024 threads per threadgroup: their registers limit them. The driver read 100 % device, renderer and tiler
utilization (the headless editor renders without a frame limit), an 87 MiB
parameter buffer, no scene splits and no recoveries. `rec.analyze {tab:
"hw"}` and the kind filter captured without layout failures.
`test_recording_hardware_pipelines` round-trips both streams; it failed with
the reader's pipeline kind offset moved by one byte. Unavailable: the Vulkan
statistics path on hardware (it compiles here; MoltenVK lacks the
extension) and the driver's own refresh period of its utilization figures.

Vulkan, AMD Radeon RX 6700 XT, AMD driver 26.6.3, Windows 10 19045, Release
(clang and lld-link, MSVC ABI), 2026-10-20, built at `a06a6906` with the
fixes of `032cbbc5` and `854ebf05`: two headless Bistro runs, Capture
shader statistics off and on. Off, the HW tab listed 119 pipelines with
compile times, hid the register columns and pointed at the setting; the
feature report said `pipelineExecutableInfo` "available with Capture
shader statistics". On, it said "enabled", every pipeline showed
registers, the selected pipeline listed `ShaderProperties.cs|gs|ps:
<statistic>` rows, and the device facts read Captured. AMD reports five
statistics per executable and no instruction count: `vk_deferred_lighting`
228 VGPRs, 102 SGPRs, 512 B of LDS and 27,040 B of scratch. Cold-cache
compiles took 254,513 ms off and 255,101 ms on (+0.2 %), 24 ms warm; the
slowest pipeline, `vk_transmission_shade_partitioned`, 62 s. The run found
the TG memory column showing `ldsSizePerLocalWorkGroup` (64 KiB on every
pipeline) and the utilization chart drawn empty for a series with no
valid sample; both are fixed. Unavailable there: the counter import
(macOS only) and a build of `99cdc379`.

Metal, M1 Pro, Release editor, headless Bistro, 2026-10-20: a 14 s Tier 1
recording (2,974 frames, `perf.set {signposts: true}`) with a 3 s trace
(`xcrun xctrace record --instrument "Metal GPU Counters" --instrument
os_signpost --attach <pid> --time-limit 3s`, 137 MB) imported with
`vkr_xctrace.py counters` in 2 min 23 s, nearly all of it `xctrace export`
(2 min 21 s alone): 5,460,991 samples of 31 counters into 98,863 buckets
of 50 us, a 12.3 MB file, 760 `Clock` events whose offsets spread 0.2 us.
Instruments sampled 3.09 s, paused 1.54 s while it stopped and sampled
0.31 s more, so 720 frames from 1,591 to 2,636 carry counters. Means over
them: ALU limiter 26 %, texture sample limiter 19 %, last-level cache 17 %,
fragment occupancy 8 %, compute occupancy 14 %. Tiled.Opaque (1.75 ms, 90
GPU-timed frames) read ALU 19 %, texture sample 3 %, fragment occupancy
5.5 %: no unit near its peak at low occupancy, a latency-bound pass;
Post.Bloom.Downsample read texture sample 98 %. `rec.analyze {tab: "hw"}`
captured both views without layout failures. `test_recording_gpu_counters`
checks frame and pass means, buckets without samples, rows dropped before a
pass and a cut file; it failed with `pass_starts` rows mapped by position.

Metal, M1 Pro, Release editor, headless Bistro, 2026-10-20, the editor's own
trace: `rec.start {tier: "1", gpu_counters: 3}` reported `tracing` at once;
`xctrace record` (start, 3 s, save) took about 35 s, the import about 2 min
(5.1 M samples), and `rec.status` reported `last_imported` 154 s after
`rec.stop`, with the trace removed from `$TMPDIR`. The 11.7 s recording's
683 frames from 772 to 1,775 carry counters, 750 `Clock` events within
0.1 us; Tiled.Opaque again read ALU 19 % and fragment occupancy 5.5 %. The
chip and the HW tab captured without layout failures.
`test_recording_sampled_gpu_counters` writes two sampled sessions over
four frames through the recording API and checks the loader's per-pass
means, the GPU-time-weighted frame percentage, the summed frame count and
a counter one session lacks staying NaN; the recording suite passed (36),
and the editor, harness and tester built with the libc boundary check
passing. Native AMD runs on an RX 6700 XT with Adrenalin 26.6.3 found three
faults. A session spanning several replays could not end before every
replay ran (`Not all passes have been executed`), and deleting it while
its frame was in flight lost the device; one-replay groups and delayed
deletion replaced both. One-frame sessions then hung the GPU three times
(Windows event 4101): a breadcrumb run showed the last finished submit
run all 66 passes and the next three never start their first, about 28
sessions in, inside `GpaBeginCommandList`'s commands. GPUPerfAPI lists a
hang after a few dozen consecutive sessions on AMD's Windows driver before
Adrenalin 26.7.1, so sessions span up to 30 frames.

On Adrenalin 26.10.1 a headless Release editor on Bistro recorded at
Tier 1 with `rec.start {gpu_counters: 3}` and then `rec.counters
{seconds: 2}`: GPUPerfAPI found 14 of the 16 counters and split them into
5 one-replay groups, and seven 22- to 30-frame sessions ran and were read
without a hang (201 to 203 passes while the scene still loaded, then 76 to
79). The first run recorded none of them: the recording placed a
session's first frame through only its last 32 frames, and a 30-frame
session arrives later than that. The history now holds 128 frames,
`test_recording_sampled_gpu_counters` writes a 40-frame session 18 frames
after its last, and the rerun recorded all seven (`rec.status`
`gpu_counter_sessions` 3, then 7). `rec.analyze {tab: "hw"}` opened GPU
counters with 202 traced frames, `Lighting.Deferred` bound by CS (100 %),
the shadow cascades by VsGs (99 %) and `UI.Editor` by DepthStencilTest.
`TexUnitBusy` and `MemUnitBusy` read the same on every pass on this
RDNA 2 GPU. Unavailable: a check on a non-AMD desktop GPU, where only
reasoning shows the view hidden.

`test_recording_counter_import` feeds a synthetic three-table export in
7-byte pieces through a source callback and checks the file byte for byte;
its first run found a row lost when a chunk ended inside `<row>`.

Metal, M1 Pro, Release editor, headless, 2026-10-20, the designer's
190 s windowed recording of the Level Design Test (22,789 loop frames,
`rec-20261020-201539`): 12,637 frames built a Scene. Before, the window
showed a frame time of p50 8.33 ms and p95 8.69 ms (`frame.wall`, the
editor loop at 120 Hz) and 5 frames over 20 ms; with the derived Scene frame
time it shows p50 16.59, p95 17.28, p99 25.00 and worst 90.45 ms with 495
frames over 20 ms, as the designer saw. `gpu.submission` read p50 14.8 and
p95 18.5 ms; at frame 3242 the passes summed to 9.89 of a 15.75 ms GPU
frame, 5.85 ms outside any pass. The census needed about 42 M words
(30,555 candidates on about 11,150 frames) against a 16 M budget and had
dropped everything after about a third of the recording; it now keeps
about one frame in four across all of it. The Draws
table's `draws_collected` median was 72 (zeros of frames that rendered
nothing, and overlay-only counts); its rows now compare rendered frames.
`test_recording_scene_frames` checks the derived and recorded Scene frame
times, unsampled render-side zeros and the dropped derived series; the
full tester passed (873). Unavailable: a windowed run with the presenter
recording `frame.scene_interval` (headless renders a Scene every frame,
where the frame time stays `frame.wall`), and recordings made before this
change keep overlay-only world draw counts.

## Consequences

- A Tier 1 recording grows by the measurement values (396 on Bistro, about
  3 KB per frame), the allocator rows (64 B per named allocator per frame)
  and the census states (half a byte per candidate per frame: about 1.3 KB a
  frame on Bistro, 25 KB at 50,000 candidates), before compression; the
  Bistro walk's whole stream compressed to about 0.8 KB per frame.
- Tier 0 recordings and the overhead gate they passed do not change, save
  for `frame.scene_interval`, one gauge per Scene frame.
- On Metal with the presenter, `draw.world.*` hold samples only on frames
  whose GPU diagnostics read back, about one in ten rendered frames in the
  designer's recording; their means and medians come from those.
- The census pass costs GPU time only while a Tier 1 recording runs.
- Object boxes of moving objects go stale between world generations; their
  states stay exact.
- Blended draws, culled on the CPU, are not in the census.
- Zone recordings load every zone into memory once to reduce them per frame;
  a long zone recording costs up to about 100 MB while loading.
- The pipeline registry keeps up to 4,096 entries and 32,768 statistics on
  the heap for the renderer's lifetime (about 0.3 MB for Bistro's 478
  Metal pipelines); a rebuilt variant adds another entry.
- Turning Capture shader statistics on changes Vulkan pipeline cache keys:
  the first start compiles again. On the RX 6700 XT it added 0.2 % to a
  cold Bistro compile of about 4 minutes.
- The Apple driver's statistics are undocumented IORegistry keys; one that
  changes leaves its value unavailable, never wrong.
- A counter import costs about 47 s of `xctrace export` per traced second
  on an M1 Pro, on its own thread, and writes about 4 MB per traced second
  at 50 us buckets, so traces stay at 1 to 10 s (3 s by default). The
  export has no faster form: its XPath selects whole tables, not rows; the
  instrument offers no recording options (`--show-recording-options`); and
  the trace's `metal-gpu-counter-profile` and `gpu-aps-stream` tables are
  empty. Counters cover the whole GPU, other processes too; passes that
  overlap share samples, and a pass shorter than two buckets reads its
  neighbours' work: the samples come from each ring buffer about every
  39 us, so a finer bucket would not resolve shorter passes.
- xctrace writes a kernel trace of a few megabytes, `instruments*.ktrace`,
  to the user's temporary directory under a name the trace does not record,
  and ignores a child's `TMPDIR`. The trace call lists those files before
  and after its recording and removes the one that appeared; when two or
  more appear, another recording ran at the same time and none is removed.
- Per-pass counters come from the tiled pipeline's GPU-timed frames, one in
  8. Timing every frame while tracing would give 8 times the frames but
  splits the shared compute encoder (about 0.5 ms a Bistro frame), changing
  the work the counters measure; a 3 s trace's 90 timed frames per pass
  gave stable means, so the period stays.
- Signposts cost about 5 us a frame while on, so a recording with counters
  is not a timing reference.
- Sampled AMD frames carry GPUPerfAPI's commands around every pass, so
  their pass times are not a timing reference either. Each session reads
  one counter group, so a request covers every counter only when it spans
  as many sessions as there are groups; a short request leaves the later
  groups' counters unsampled.
- On AMD drivers before Adrenalin 26.7.1, a request that runs more than a
  few dozen sessions can still hang the GPU; at 30 frames a session, 10 s
  at 60 frames per second is 20 sessions.
- The editor and harness carry `GPUPerfAPIVK-x64.dll` (MIT) beside them on
  Windows; the configure step needs the network once per build tree.

## Alternatives considered

- **Describe the graph with `vkr_renderer_describe_graph` on a change.** It
  joins the render thread, a stall of up to a frame in the recording.
- **All new data only with all measurements.** Default recordings would show
  empty tabs; all-measurements recordings are not used for timing.
- **Frustum tests in the editor from recorded boxes and camera.** Exact for
  the frustum, but it cannot show occlusion, the reason most draws are culled
  indoors.
- **A rendered top-down backdrop.** Needs a second Scene render or a capture
  per level; the footprints carry the culling state, which an image cannot.
- **AMD streaming counters over time.** The closest match to the Instruments
  import, but GPUPerfAPI 4.4.1.3's Vulkan backend does not support them.
- **One session per frame with every counter group.** A session of several
  replays ends only after all of them ran, which a live frame cannot replay;
  one-frame sessions of one group hit the driver's hang within seconds.
- **AMD's Radeon Developer Panel or RGP captures.** Per-event detail, but a
  separate tool and capture per look, with nothing in the recording.

## Revisit when

The Vulkan census, counters, readback and pipeline statistics get their
native run; GPUPerfAPI's Vulkan backend offers streaming counters, or NVIDIA or
Intel counters are wanted; Instruments exports counters faster or another
platform gets a counter trace; a recording's size or the census pass's cost fails the
Tier 1 budget; or the map needs entity names, which needs picking ids joined
to entities in the recording.
