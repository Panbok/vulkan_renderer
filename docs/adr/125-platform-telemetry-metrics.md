---
status: partial
updated: 2026-10-18
authority: adr
---

# ADR-125: Platform telemetry metrics

## Status

Accepted (partial). Phase P1 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented on the Mac: the lib telemetry API, its macOS sources, the registry
metrics, their Tier 0 recording fields, `health.status`, the harness's system
conditions, Metal present latency, the recording timeline's System lane and
the `perf.snapshot` and `perf.set` operations. The Windows sources are a stub
that reports every signal unavailable, and Vulkan reports no present latency.

## Context

Throttling, memory pressure, low-power mode and the process's CPU time change
frame time without any engine change. Before this decision none of them was
recorded, so a timing run on the shared 16 GB Mac could not show why it was
slow, and the harness report's `thermal_state_start` and `thermal_state_end`
were always `unknown`.

## Decision

**API.** `vkr_platform_get_telemetry`
([vkr_platform.h](../../lib/src/platform/vkr_platform.h)) fills
`VkrPlatformTelemetry`: the footprint the OS charges the process, process CPU
time, major page faults, a thermal level normalized to nominal, fair, serious
and critical, memory pressure normalized to normal, warn and critical, the
power source and low-power mode. Each value has a validity flag; a platform
without a source leaves it false, and consumers report it unavailable.

**macOS sources** ([vkr_platform_macos.c](../../lib/src/platform/vkr_platform_macos.c)):

| Signal | Source | Cost on M1 Pro |
|---|---|---|
| Footprint | `task_info(TASK_VM_INFO).phys_footprint` | 0.65 µs |
| CPU time, major faults | `getrusage(RUSAGE_SELF)` | 0.22 µs |
| Memory pressure | `sysctl kern.memorystatus_vm_pressure_level` (1, 2, 4) | 1.4 µs |
| Thermal level | `notify(3)` key `kOSThermalNotificationPressureLevelName`; `notify_get_state` (13 µs) only after `notify_check` (0.04 µs) reports a change | 0.04 µs |
| Power source, low-power mode | `IOPSGetProvidingPowerSourceType`; `NSProcessInfo.isLowPowerModeEnabled` through the Objective-C runtime; both cached for 1 s | 90 µs once a second |

The costs come from a standalone loop of 20,000 calls (200 for the power
source) built with `clang -O2` on 2026-10-18. `vkr_foundation` links IOKit,
CoreFoundation and `libobjc`; a process without Foundation finds no
`NSProcessInfo` and reports low-power mode unavailable.

**Metrics.** The standard scene runtime samples once per published frame
([vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c)),
on the thread that publishes frame metrics, about 2.5 µs a frame:

| Metric | Kind |
|---|---|
| `os.memory.footprint_bytes` | gauge, bytes |
| `os.memory.pressure` (0 to 2), `os.thermal.level` (0 to 3) | gauge |
| `os.power.on_battery`, `os.power.low_power` (0 or 1) | gauge |
| `cpu.process_time` | duration since the previous frame, every thread |
| `os.page_faults.major` | counter |

Names are shared across platforms where both measure the same quantity; the
first frame's interval values read not sampled. Tier 0 recordings
([vkr_recording.c](../../runtime/src/recording/vkr_recording.c)) add these
signals with `cpu.backend_present`, `cpu.drawable_acquire`, `gpu.errors` and
`log.errors`. `health.status` reports a `system` object with the thermal
level, memory pressure, footprint, power source and low-power mode.

**Harness** ([vkr_harness_report.c](../../tools/harness/vkr_harness_report.c)):
the parent samples the system when a profile, snapshot or autotest starts and
ends and writes `thermal_state_*`, `power_source_*`, `low_power_*` and
`memory_pressure_*` into the report's provenance. These values are not in any
fingerprint; `power_mode` is unchanged, so baselines keep matching. Each run
and report carries `system_conditions`: the highest thermal level and memory
pressure and the frames in low-power mode or on battery over warmup and
measured frames. A run loses authority with `thermal.throttled` at a serious
or critical thermal level, `memory.pressure` at critical pressure and
`power.low_power` in low-power mode, at any counted frame or at start or end.
On battery is reported only.

**Present latency** ([vkr_metal_packet_present.inc](../../renderer/src/metal/internal/vkr_metal_packet_present.inc)):
each window present writes the drawable's ID and the CPU time of the call
into one of 8 marks and adds a presented handler created once at setup. The
handler, on a Core Animation thread, adds `presentedTime` minus that time to
atomics, or counts a miss when `presentedTime` is 0. Both clocks are
`mach_absolute_time`. A drawable on its way to the display keeps the handler
after the renderer is destroyed, so the marks and totals live in a separate
allocation that the renderer and each pending present reference; the last
reference frees it. The collector publishes `gpu.present.latency` (one mean
sample per frame) and `gpu.present.missed` (counter), both in Tier 0.

**Live reads** ([editor_ops.c](../../editor/src/editor_ops.c)):
`perf.snapshot` answers the newest published metrics frame from the live
registry, filtered by name prefix, with `{ms, count, max_ms}` per duration,
`{unavailable: reason}` per empty slot and, with `passes`, each executed
render graph pass's CPU and GPU milliseconds. `perf.set` switches pass and
submission GPU timing as `VKR_RG_GPU_TIMING`, `VKR_GPU_SUBMISSION_TIMING`
and the F7 key do; a recording still controls them while it runs. The UI
frame borrows the registry and the pass table for the build.

**System lane** ([editor_recording_timeline.c](../../editor/src/editor_recording_timeline.c)):
a recording playback loads the thermal level, memory pressure, low-power
mode, footprint, GPU errors and log errors of each frame
([vkr_recording_playback.c](../../runtime/src/recording/vkr_recording_playback.c)),
and the timeline shows them under the memory lane for recordings that carry
them. Each column keeps its worst value: fair thermal and warn pressure are
faint warning bands, serious thermal and low power warning bands, critical
levels error bands, with error ticks for frames that counted GPU or log
errors. The legend names the view's worst signals or says that all were
nominal; hovering names a column's states and footprint, and a click seeks.

## Consequences

- A slow frame in a recording or report can be checked against throttling,
  memory pressure and process CPU time.
- Warn-level memory pressure does not cost authority: a Bistro profile reached
  warn once its footprint passed about 6 GB, and a headless Bistro editor at a
  4 GB footprint read warn, so warn is the 16 GB Mac's normal state.
- Each frame does about 2.5 µs more work on the publishing thread, and a frame
  once a second about 90 µs more for the power source. Neither was measured
  in a timing run.
- A hidden harness window never reaches the display: every present there is
  a miss and no latency is sampled.

## Alternatives considered

- Sampling on the recorder's writer thread: rejected; that thread exists only
  while recording, and harness and editor runs need the signals too.
- `NSProcessInfo.thermalState` for thermal state: rejected; the `notify(3)`
  key works in processes without Foundation, such as the harness parent.
- Charging authority at warn pressure: rejected for the reason above.

## Revisit when

- The Windows sources land (proposal P1; Windows session).
- A matched timing run shows the per-frame sample or the power read matters.

## Evidence

- `./build_release.sh`, `./build_editor.sh Release` and `./build_test.sh`
  (Debug, ASan, 851 passed) on the M1 Pro, 2026-10-18.
- `vkr_harness profile --record` of a temporary hidden-window Bistro case at
  1280×720 (2 repetitions, 60 warmup and 120 measured frames) under
  `tools/profiles/local-windowed.json`, report
  `sha256:baf740f225610995b546a63cb0af0436ee53753570c56c0b442e4ee2c1adabd3`:
  every new metric valid in all 240 samples; thermal 0, low power 0, on
  battery 0, footprint p50 5.05 GB and max 6.06 GB, memory pressure 0 for the
  first frames and 1 (warn) after the footprint passed 6 GB; provenance
  nominal, AC, low power off and normal pressure at start and end. The
  recording's `vkr_rec frames` rows carried the new fields. The run predates
  the critical threshold and lost authority to `memory.pressure` at warn.
- A headless Bistro editor's `health.status` returned thermal nominal,
  pressure warn, footprint 4066 MiB, AC power and low power off.
- Present feedback: a hidden-window Bistro run counted 240 of 240 presents
  as missed (`presentedTime` 0) and sampled no latency; latency on a visible
  window has no run yet.
- A headless Bistro editor: `perf.snapshot` with `prefix` `os.` answered the
  six platform gauges; after `perf.set` `pass_timing` true, `passes` answered
  GPU milliseconds per pass and `unsupported_timestamp_scope` for transfer
  passes. A 4 s Tier 0 recording opened in Watch showed the System lane
  without Analyze, all nominal, and a click on frame 416 showed its states
  and a 3.9 GiB footprint.
- Windows: stub only.
