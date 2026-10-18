---
status: partial
updated: 2026-10-18
authority: adr
---

# ADR-126: CPU zones and the GPU pass timeline

## Status

Accepted (partial). The first part of phase P2 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented: CPU zones recorded per thread, GPU pass starts on the CPU clock
on Metal, the Perfetto export and rolling recordings. The Vulkan pass starts
and the editor's Timeline window are not built.

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

**Instrumented zones.**

| Zone | Thread |
|---|---|
| `Frame`, `Frame.Limiter` | Frame loop |
| `Update`, `Simulation`, `World.Payload`, `Render.Prepare`, `Render.Wait` | Frame loop |
| `Render` (the frame loop's when rendering inline) | Render thread |
| `Metal.Graph`, `Metal.Encode` | Render thread |
| `Job` (each job run) | Job workers |

**Export** ([vkr_rec_main.c](../../tools/recording/vkr_rec_main.c)).
`vkr_rec export <bundle> --perfetto <out.json> [--range A:B]` writes the
Chrome trace-event JSON that ui.perfetto.dev and chrome://tracing open:

- process CPU with one thread per slot and every zone;
- process GPU with a Queue thread of timed passes and a Frames thread of
  GPU frame spans;
- Tier 0 counters at each frame's `Frame` zone;
- markers as instant events.

The export caps output at 4,000,000 events.

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
- A rolling recording records Tier 1 with zones, but no poses and no
  all-measurements streams.
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

- Vulkan fills `gpu_start_ms` and the host GPU times (calibrated timestamps).
- The editor gains its Timeline window, or the rolling buffer lands.
- A timing run shows the zones' cost matters.

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
- Vulkan: compile only; no pass starts.
