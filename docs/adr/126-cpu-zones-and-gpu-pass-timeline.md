---
status: partial
updated: 2026-10-19
authority: adr
---

# ADR-126: CPU zones and the GPU pass timeline

## Status

Accepted (partial). The first part of phase P2 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented: CPU zones recorded per thread, GPU pass starts on the CPU clock
on Metal and Vulkan, the Perfetto export, rolling recordings, the
editor's Thread timeline window, and zones as os_signpost intervals for
Instruments with `xctrace_zones.py` to read them back.

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
- An interval's id packs the thread slot, the depth and the site's
  address, which is unique among the zones open at once, so begin and end
  pair without a stored id.
- `vkr_thread_set_name` names the render thread `VKR Render` and job
  workers `VKR Job N` (pthread_setname_np; SetThreadDescription on
  Windows), so Instruments, debuggers and crash reports label them.
- [xctrace_zones.py](../../tools/recording/xctrace_zones.py) exports a
  trace's os-signpost table through `xctrace export`, pairs the `dev.vkr`
  intervals and prints count, median, p95, max and total ms per zone, as
  text or `--json`, and writes `--perfetto` trace-event JSON with a track
  per named thread. Record with only the os_signpost instrument:
  `xcrun xctrace record --instrument os_signpost --attach <pid>
  --time-limit 5s --output zones.trace`. xctrace also leaves a `ktrace`
  file of about 15 MB per run in `$TMPDIR`.

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
- GPU frames and GPU passes;
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
- Zones cost about two clock reads and one ring copy each while recording;
  the instrumented set is a few per frame plus one per job. No timing run
  measured their cost.
- A recording without `Frame` zones exports no counters or markers.

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
  scene load, `xctrace_zones.py` read 20,884 intervals over 15.7 s, 4 left
  open at the cut: 599 `Assets.Load` (median 2.2 ms, total 2.35 s) inside
  600 `Job` zones on `VKR Job 0` to `6`, a 875 ms load frame, and the
  `Main Thread` and `VKR Render` tracks. Bistro has no physics bodies or
  scripts, so `Physics.Step` and `Scripts.Frame` did not run (Play steps no
  physics there); they are compiled only.
