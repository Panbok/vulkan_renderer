---
status: partial
updated: 2026-10-20
authority: adr
---

# ADR-130: Performance window tabs and the Tier 1 inspection streams

## Status

Accepted (partial). The editor's Performance window has six tabs, and Tier 1
recordings carry the data the five new ones read. Metal evidence is on this
Mac; the Vulkan backend's census pass, counters, readback and pipeline
statistics have no native run yet (Windows machine).

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
   adds each executable's driver statistics, summarized into registers,
   spill bytes, instructions and shared memory by name fragment because the
   names are driver-defined. `VkrDeviceInformation` gains the GPU
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
   and a note that occupancy and limiter counters of Apple GPUs reach only
   Instruments and those of desktop GPUs only vendor profilers.
9. **UI primitives.** `vkr_ui_path` takes filled rectangles under its strokes,
   so a map draws up to 16,384 footprints per widget within the frame's
   widget budget. Editor tables take per-cell text colours, fills and share
   bars.

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

## Consequences

- A Tier 1 recording grows by the measurement values (396 on Bistro, about
  3 KB per frame), the allocator rows (64 B per named allocator per frame)
  and the census states (half a byte per candidate per frame: about 1.3 KB a
  frame on Bistro, 25 KB at 50,000 candidates), before compression; the
  Bistro walk's whole stream compressed to about 0.8 KB per frame.
- Tier 0 recordings and the overhead gate they passed do not change.
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
  the first start compiles again, and compile times with it on are not
  comparable with it off.
- The Apple driver's statistics are undocumented IORegistry keys; one that
  changes leaves its value unavailable, never wrong.

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

## Revisit when

The Vulkan census, counters, readback and pipeline statistics get their
native run; Instruments' GPU counters are imported into recordings
([profiling and diagnostics](../proposals/profiling-and-diagnostics.md)); a recording's
size or the census pass's cost fails the Tier 1 budget; or the map needs
entity names, which needs picking ids joined to entities in the recording.
