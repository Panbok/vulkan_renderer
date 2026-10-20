---
status: partial
updated: 2026-10-20
authority: adr
---

# ADR-126: CPU zones and the GPU pass timeline

## Status

Accepted (partial). The first part of phase P2 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented: CPU zones recorded per thread, GPU pass starts on the CPU clock
on Metal and Vulkan, the Perfetto export, rolling recordings, the
editor's Thread timeline window, and zones as os_signpost intervals for
Instruments, and `vkr_xctrace.py` to read zones and Metal GPU work back
from traces.

## Context

Pass rows held durations only and nothing recorded CPU work by thread, so a
recording could not show which thread or pass made a frame slow, or how CPU
and GPU work overlapped. The owner chose zones stored in `.vkrrec` with a
Perfetto export over Tracy (2026-10-18), so one bundle serves the editor,
agents and an external timeline viewer.

## Decision

**Zones** ([vkr_zone.h](../../lib/src/core/vkr_zone.h)). A zone is a named
span of one thread's time, written with `VKR_ZONE_BEGIN(scope, "Name")` and
`VKR_ZONE_END(scope)` on every path out of the scope; there is no hidden loop
for a `break` or `return` to skip.

- While no sink is installed a zone costs one relaxed atomic load. With a
  sink it reads `vkr_platform_get_monotonic_ns` twice and hands one 24-byte
  `VkrZoneRecord` (zone, thread slot, depth, begin and end in ns) to the
  sink on the thread that ran it.
- A thread records only after it names its slot with `vkr_zone_thread_set`:
  0 the frame loop (`vkr_application_host_run`), 1 the render thread, 2 + k
  job worker k, up to 16 workers.
- Sites register on first use into a table of 1,024 names.
- Stopping the sink waits until no thread is inside a write. The writer
  count and the active flag use sequentially consistent order, because each
  side writes one and reads the other.

**Native profiler intervals.** `vkr_zone_signposts_set(true)`
(`VKR_SIGNPOSTS=1` at start, or `perf.set {signposts}`) makes every zone on
a slotted thread also open and close a native interval, with or without a
sink, through `vkr_platform_signpost_begin` and `_end`
([vkr_platform.h](../../lib/src/platform/vkr_platform.h)).

- On macOS each is an os_signpost interval named `Zone` in subsystem
  `dev.vkr`, category `Zones`, whose message is the zone's name:
  os_signpost takes its interval name only as a literal. Windows emits
  nothing; PIX output was dropped on 2026-10-19 because PIX fits only a
  Direct3D 12 backend.
- With signposts on, the frame loop also emits an os_signpost event
  `Clock` per frame whose message is `vkr_platform_get_monotonic_ns`
  (`vkr_platform_signpost_clock`), so a trace's times map to the clock
  recordings use; GPU counter imports place their samples on recorded
  frames by it ([ADR-130](130-performance-window-tabs.md)).
- An interval's id packs the thread slot, the depth and the site's
  address, which is unique among the zones open at once, so begin and end
  pair without a stored id.
- `vkr_thread_set_name` names the render thread `VKR Render` and job
  workers `VKR Job N` (pthread_setname_np; SetThreadDescription on
  Windows), so Instruments, debuggers and crash reports label them.
- [vkr_xctrace.py](../../tools/recording/vkr_xctrace.py) reads traces
  through `xctrace export`. `zones` pairs a trace's `dev.vkr` os-signpost
  intervals and prints count, median, p95, max and total ms per zone, as
  text or `--json`. `gpu` reads a Metal System Trace's
  `metal-gpu-intervals` of one process (default `vkr_editor`): Metal labels
  each encoder `Command Buffer N:<pass>`, so GPU time sums per pass label
  (a batched compute encoder under its first pass) and per channel. Both
  write `--perfetto` trace-event JSON, and `timeline` writes zones and GPU
  intervals into one file. Record zones with only the os_signpost
  instrument (`xcrun xctrace record --instrument os_signpost --attach <pid>
  --time-limit 5s`), or both with `--template "Metal System Trace"
  --instrument os_signpost`: the tables share the trace clock, and the
  os_signpost window starts and ends later than the Metal tables'. A 3 s
  os_signpost trace is about 10 MB plus a 15 MB ktrace in `$TMPDIR`; a 3 s
  Metal System Trace of Bistro was 43 MB plus a 195 MB ktrace. Its GPU
  counter tables stay empty without the GPU counters instrument; the
  Metal GPU Counters instrument's samples import into a recording through
  `vkr_rec counters` or the editor (ADR-130).

**Recording** ([vkr_recording.c](../../runtime/src/recording/vkr_recording.c)).
`VkrRecordingConfig.zones` installs the sink while the recording runs.

- Each thread slot writes into its own recorder producer: MAIN, RENDER, or
  one of 16 worker producers with 1 MiB rings created only when zones are
  recorded ([vkr_recorder.h](../../runtime/src/recording/vkr_recorder.h)).
- New streams:
  - `zone_name` (23): a u32 id, then the name, written on the frame loop as
    sites appear.
  - `zone` (24): droppable 24-byte records.
  - `pass_starts` (25), with Tier 1 pass rows: a u32 row count, u32 flags,
    the submission's GPU start and end on the CPU clock, then each row's
    GPU start in ms after that start, in the order of the frame's `passes`
    record.
- Old recordings and readers are unaffected: the 12-byte pass rows keep
  their layout.
- A zone record files under a recorded frame. The frame loop's and job
  workers' zones take the frame their loop iteration records:
  `vkr_recording_loop_begin` sets it at the start of the iteration, before
  `Update`, so zones that end before the iteration's frame record agree
  with ADR-082's loop metrics. Render-thread zones take the frame the
  render thread renders: `vkr_recording_render_frame` sets it before each
  frame goes to the render thread. Recordings made before 2026-10-19 file
  zones that end before the frame record (`Update`, `Simulation`) one
  frame early and render-thread zones under the frame loop's frame.
- `rec.start` takes `zones` (default: on with `measurements` all), and the
  Recordings window's All measurements includes zones.

**GPU placement.** On Metal, a pass's start counts from the first timed
pass's begin stamp ([vkr_metal_packet_commands.inc](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc)).
The commit feedback's `GPUStartTime` and `GPUEndTime`, host seconds of the
same `mach_absolute_time` clock, place the frame on the CPU clock. So no
`sampleTimestamps:gpuTimestamp:` call is needed. Untimed work before the
first timed pass shifts the passes early by that work's length.

On Vulkan, pass starts count from the first timed pass's begin the same way
([vkr_vulkan_renderer.c](../../renderer/src/vulkan/vkr_vulkan_renderer.c)).
The frame's begin and end stamps reach the CPU clock through
`VK_KHR_calibrated_timestamps` (or its EXT origin) when the device pairs its
timestamps with `QueryPerformanceCounter`, the counter behind
`vkr_platform_get_monotonic_ns` on Windows:

- The device enables the extension only with both time domains, and the
  startup capability report names `calibratedTimestamps`
  ([vkr_vulkan_device.c](../../renderer/src/vulkan/vkr_vulkan_device.c)).
- The render thread keeps one calibration pair, a device stamp and the host
  time read with it, and refreshes it once it is a second old, which bounds
  the drift between the clocks.
- A stamp's distance from the pair is taken signed within the queue's
  `timestampValidBits`, then scaled by `timestampPeriod`;
  `vkr_platform_monotonic_ns_from_counter` converts the pair's raw counter.
- Without the extension or the domain, `gpu_host_valid` stays false and
  passes keep their device-relative starts.

**Instrumented zones.**

| Zone | Thread |
|---|---|
| `Frame`, `Frame.Limiter` | Frame loop |
| `Update`, `Simulation`, `World.Payload`, `Render.Prepare`, `Render.Wait` | Frame loop |
| `Scripts.Frame` (the script host's frame), `Physics.Step` (one fixed physics step) | Frame loop |
| `Render` (the frame loop's when rendering inline) | Render thread |
| `Metal.Graph`, `Metal.Encode`, `Shadows.Update` (shadow payload planning) | Render thread |
| `Vulkan.Graph` (graph realization), `Vulkan.Record` (command recording) | Render thread |
| `Job` (each job run), `Assets.Load` (a resource loader's decode inside it) | Job workers |

**Export** ([vkr_rec_main.c](../../tools/recording/vkr_rec_main.c)).
`vkr_rec export <bundle> --perfetto <out.json> [--range A:B]` writes the
Chrome trace-event JSON that ui.perfetto.dev and chrome://tracing open:

- process CPU with one thread per slot and every zone;
- process GPU with a Queue thread of timed passes and a Frames thread of
  GPU frame spans;
- Tier 0 counters at each frame's `Frame` zone;
- markers as instant events.

The export caps output at 4,000,000 events.

**Playback timeline** ([vkr_recording_playback.c](../../runtime/src/recording/vkr_recording_playback.c)).
Opening a recording loads its zones, zone and pass names, and GPU spans:

- Each frame's pass rows are joined with its pass starts in row order; a
  frame whose counts disagree keeps only its GPU frame span.
- Zones and spans are sorted by begin, and each frame maps to its `Frame`
  zone's begin.
- `vkr_recording_playback_zones_in` and `vkr_recording_playback_gpu_spans_in`
  find the candidates for a time range by binary search, widened by the
  longest span.

**Thread timeline window** ([editor_recording_flame.c](../../editor/src/editor_recording_flame.c)).
The window (`window timeline`, or the playback bar's layers button) shows
the playhead frame and one frame either side:

- one flame track per thread slot that recorded zones, one row per depth;
- GPU frames and GPU passes, after the Main and Render tracks and before
  the job workers' tracks, so they stay in view however many workers
  recorded zones;
- a ruler with frame starts.

It is drawn as one path widget with up to 160 labels. Spans narrower than
a pixel merge, and at most 2,400 draw per build. Hover names a span, a click
selects it and seeks to its frame, the wheel zooms about the pointer, a drag
or Shift+wheel pans, and the header steps frames, zooms and fits. GPU spans
belong to the frame that submitted them, so on the tiled pipeline they show
about three frames after their CPU frame.

**Rolling recordings** ([vkr_recorder.c](../../runtime/src/recording/vkr_recorder.c)).
`VkrRecordingConfig.rolling_seconds` keeps a recording in memory:

- The recorder writes no file. Its writer thread closes chunks every
  0.25 s, keeps them in a ring of up to 256, and drops any chunk that closed
  more than the window ago.
- `vkr_recording_save_window` asks the writer thread to write identity,
  dictionary, a preamble of every pass and zone name, and the kept chunks.
  Frames are renumbered from zero, so playback, analysis and the export
  read the bundle as any other. The caller waits for the save, and the
  recording keeps running.
- The save writes a summary with its analysis, using its own memory while
  the writer thread still owns the recording's.
- Stopping discards the window.
- A rolling recording records Tier 1 with zones, but no poses, no
  all-measurements streams and no thumbnails.
- `rec.start {"rolling": seconds}` (1 to 300) starts one and `rec.save_last
  {name}` saves it. In the Recordings window, Keep last 30 s starts one;
  Record then reads Save last 30 s, and the Scene's LAST chip and the record
  shortcut save too.

## Consequences

- One recording answers which thread and pass made a frame slow, and how
  CPU and GPU work overlap, in an external viewer today.
- A zone costs 1.3 ns while nothing records, about 37 ns with a sink that
  copies its record into a ring as the recorder does, and 350 to 460 ns
  with signposts on even with no tool attached, so signposts stay off by
  default. A Bistro frame records about a dozen zones plus one per job:
  under 1 us of a 7 ms frame while recording, about 5 us with signposts.
- A recording with neither zones nor GPU pass starts, such as a Tier 0
  harness recording, exports its counters and markers at the recorder's
  frame times (`frame_clock` `recorder` in the status line); one with zones
  but without a frame's `Frame` zone leaves that frame's counters out.

## Alternatives considered

- Tracy: a mature viewer on day one, but a second capture format, a network
  server and a dependency.
- A `for`-loop zone macro: shorter at call sites, but a `break` or `return`
  inside it skips the zone's end.
- `sampleTimestamps:gpuTimestamp:` each frame: unnecessary on Metal, where
  commit feedback already reports host time.

## Revisit when

- A Vulkan host without the `QueryPerformanceCounter` domain needs another
  host clock (`CLOCK_MONOTONIC` on Linux).
- The Thread timeline needs more than 2,400 spans in one view, or keyboard
  navigation.
- A timing run shows the zones' cost matters.
- Instruments accepts dynamic signpost names, or another platform gets a
  native profiler worth feeding.

## Evidence

- `./build_release.sh`, `./build_editor.sh Release` and `./build_test.sh`
  (Debug, ASan) on the M1 Pro, 2026-10-18: 856 test cases passed, including
  `run_zone_tests`. Those tests cover four worker threads recording nested
  zones into their own slots with correct depths and names, a thread without
  a slot recording nothing, and a stop that does not return while a write
  sleeps inside the sink.
- A headless Bistro editor recorded 3 s at Tier 1 with zones (587 frames).
  `vkr_rec export --perfetto` wrote 10,909 events: 4,693 zones on the Main
  and Render threads, 583 GPU frames and 2,116 GPU pass spans, 3,507
  counters, and no bad records. In all 73 frames with pass timing (tiled
  samples 1 frame in 8), every pass lay within its frame's GPU span. No job
  ran in that idle scene, so worker zones have only the CPU test as
  evidence.
- `test_recorder_rolling_save`: chunks older than the window go, frames
  are renumbered from zero, the preamble record comes first, the recording
  continues after a save, and no file of its own is written.
- A headless Bistro editor kept 4 s with `rec.start {"rolling": 4}` and
  ran 7 s. `rec.save_last` wrote 825 frames (4.2 s). `vkr_rec verify` read
  frames 0 to 824, indexed and complete, with no bad chunks. The Perfetto
  export resolved every pass and zone name (6,557 zones, 2,956 passes). A
  `vkr_rec explain --frame 400` answered `unknown` for a normal frame.
  `rec.stop` wrote nothing.
- Vulkan on Windows, 2026-10-18, Ryzen 5 2600, RX 6700 XT, Adrenalin
  26.6.3 (both calibrated-timestamps extensions present; the KHR one is
  used): the startup log read `Vulkan capability feature
  calibratedTimestamps: present`. A headless Release Bistro editor, driven
  through `vkr_mcp`, recorded 460 frames at Tier 1 with zones while the
  camera orbited. `vkr_rec export --perfetto` wrote 91,832 events: 3,863
  zones including 460 `Vulkan.Graph` and 460 `Vulkan.Record`, `Job` zones on
  11 workers, 456 GPU frames and 85,194 GPU pass spans, every pass inside its
  frame's span, no unmatched frame and no bad record. No GPU frame began
  before a `Vulkan.Record` ended; the next GPU frame began at least 14 µs
  after it (p50 5.7 ms, p95 11.3 ms), the queue still running the previous
  frame.
- On Vulkan, `rec.start {"rolling": 30}`, a camera jump and `rec.save_last`
  wrote 697 frames. `vkr_rec explain --spike 1` named frame 64,
  `gpu.submission` 15.7 ms against a 7.2 ms median; the trace shows that GPU
  frame at 15.75 ms with 182 timed passes summing 14.0 ms. Spike 2, frame
  107, followed a 42 ms GPU frame. The rolling start left a
  `.rolling.vkrrec` directory holding only a thumbnail.
- `test_recording_playback_timeline`: zones written out of order load
  sorted, names written after their zones resolve, pass rows join their
  starts, a frame whose counts disagree keeps only its GPU frame, `Frame`
  begins map to frames, and range queries find a zone that covers their
  start.
- A headless Bistro editor recorded 3 s at Tier 1 with zones and opened the
  Thread timeline. It showed Main and Render tracks, GPU frames and 29 GPU
  passes around frames 298 to 300, with a selected `Tiled.Opaque` pass
  (2.259 ms, frame 296). The empty states, fit, frame steps, zoom, click and
  drag were checked on screenshots.
- Signposts, 2026-10-19, `./build_release.sh` and `./build_editor.sh
  Release` on the M1 Pro: a headless Bistro editor with `perf.set
  {signposts: true}`, recorded 3 s with only the os_signpost instrument
  (9.6 MB trace), held 6,444 matched `dev.vkr` intervals of Frame, Render,
  Render.Wait, Metal.Encode, Metal.Graph, Update, World.Payload, Simulation
  and Shadows.Update. Started with `VKR_SIGNPOSTS=1` and traced through the
  scene load, `vkr_xctrace.py zones` read 20,884 intervals over 15.7 s, 4 left
  open at the cut: 599 `Assets.Load` (median 2.2 ms, total 2.35 s) inside
  600 `Job` zones on `VKR Job 0` to `6`, a 875 ms load frame, and the
  `Main Thread` and `VKR Render` tracks. Bistro has no physics bodies or
  scripts, so neither ran there.
- `Physics.Step`: a dynamic body added to a headless Bistro (`entity.create`,
  then Cmd `component.add physics_body` and `physics.motion dynamic`, not
  saved) and `sim.play` gave 213 `Physics.Step` zones in 3.55 s, the 60 Hz
  fixed step, median 19 us, and `Simulation` rose from 0.03 to 0.14 ms.
  `Scripts.Frame` needs a script instance from a project's compiled script
  library; Bistro opened with `--scene` has none, so it is compiled only.
- Overhead, 2026-10-19: a Release benchmark against `libvkr_foundation.a`
  (2,000,000 zones a run, three runs) took 1.2 to 1.3 ns per zone without a
  slot or a sink, 36.8 to 37.9 ns with a sink that copies the record into a
  ring with a release store, and 347 to 462 ns with signposts on and no
  tool. The sink stands in for `vkr_recorder_write` (reserve, copy,
  commit); no matched harness timing run was made.
- Zone sums against stage metrics: a headless Bistro editor recorded 20 s
  at Tier 0 with zones and all measurements (2,687 frames). Over frames
  1,500 to 1,999 the zones of each frame match its Tier 0 metrics on the
  same frame, median absolute difference: `Frame` and `frame.wall` 0.21
  us, `Render.Wait` and `cpu.render_wait` 0.16 us, `World.Payload` and
  `cpu.world_payload_build` 0.04 us, `Update` and `cpu.update` 0.04 us,
  `Simulation` and `cpu.simulation` 0.25 us. Before the frame labelling
  change `Update` and `Simulation` matched only one frame later. Every GPU
  frame began after its frame's `Metal.Encode` zone ended (at least 4.7
  ms later, the GPU queue's depth); filed one frame later, all but one
  frame would have started before its encode ended.
- `vkr_rec export --perfetto` of two zone-free Tier 0 editor recordings
  wrote 12,516 and 13,871 counter events at the recorder clock, before
  none; the frame.wall counters' median spacing (8.35 ms) equals the
  median frame.wall.
- Matched harness runs, 2026-10-19: Bistro `performance.bistro.shadow_orbit`
  (1280×720, windowed hidden, immediate, 120 warmup and 300 measured frames)
  under a dirty-tree copy of `performance-windowed`, five children each, in
  the order `--record`, `--record-zones`, `--record` (reports
  `sha256:ca80ec16…`, `sha256:5ea0bec2…`, `sha256:f91fa3de…`). Environment,
  workload and policy fingerprints matched, thermal level 0 and memory
  pressure warn in all three. All failed the warmup gate on
  `cpu.render_submit` and are non-authoritative (recording, dirty tree,
  local profile), so `perf-compare` refused them; the per-child means
  compare. Zones run against the second `--record` run: `frame.wall`
  23,116.6 against 23,124.5 us (child spread 22.8 and 19.3),
  `cpu.render_submit` 525.9 against 575.5, `cpu.update` 46.0 against 52.6,
  `cpu.world_payload_build` 139.0 against 147.4. Zones cost nothing
  measurable at about ±20 us a frame; the first `--record` run was 300 us
  slower than both, the drift of run order. Each zones repetition recorded
  about 7,650 zones (18 a frame); the plain runs none.
- Metal System Trace with signposts, 3 s of headless Bistro: `vkr_xctrace.py
  gpu` read 12,491 of the editor's GPU intervals in 501 frames (552 rows of
  other processes), `Tiled.Opaque` 2.66 ms median a command buffer,
  `Tiled.Blend` 0.46 ms, the compute batch under `IBL.Bake` 0.42 ms; all
  6,634 CPU encoder intervals (`metal-application-encoders-list`) inside the
  zones' window lay inside a `Metal.Encode` zone at zero offset, so the two
  tables share one clock.
