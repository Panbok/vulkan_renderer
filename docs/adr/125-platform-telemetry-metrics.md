---
status: partial
updated: 2026-10-19
authority: adr
---

# ADR-125: Platform telemetry metrics

## Status

Accepted (partial). Phase P1 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented on the Mac: the lib telemetry API, its macOS sources, the registry
metrics, their Tier 0 recording fields, `health.status`, the harness's system
conditions, Metal present latency, the recording timeline's System lane and
the `perf.snapshot` and `perf.set` operations. The Windows sources, the
OS's video memory figures and other processes' GPU use are implemented and
have a native Bistro run. Vulkan reports no present latency.

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

**Windows sources** ([vkr_platform_windows.c](../../lib/src/platform/vkr_platform_windows.c)),
about 1.4 µs a call on a Ryzen 5 2600, so every call reads every source:

| Signal | Source |
|---|---|
| Footprint | `K32GetProcessMemoryInfo`, `PROCESS_MEMORY_COUNTERS_EX.PrivateUsage` |
| CPU time | `GetProcessTimes`, kernel and user |
| Memory pressure | `QueryMemoryResourceNotification` on the system's low and high memory resource notifications, created once: low is critical, high is normal, neither is warn |
| Power source, low-power mode | `GetSystemPowerStatus`: `ACLineStatus` (255 is unknown) and `SystemStatusFlag` (battery saver) |

Windows signals the low notification when available memory falls to about
32 MB per 4 GB of RAM (at most 64 MB) and the high one above three times that,
so warn means under about 192 MB available on a 16 GB machine. Thermal level
and major page faults stay unavailable: Windows has no public per-process
thermal level, and `PageFaultCount` counts soft faults too. `GetProcessTimes`
advances in clock ticks (15.625 ms on that machine), so one frame's
`cpu.process_time` reads 0 or whole ticks; only sums over many frames are
exact.

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

**The OS's view of the GPU** ([vkr_platform_gpu_windows.c](../../lib/src/platform/vkr_platform_gpu_windows.c)).
`vkr_platform_gpu_monitor_create` takes the renderer's adapter LUID, which
the Vulkan device reads from `VkPhysicalDeviceIDProperties` and
`vkr_renderer_get_device_information` passes on; the runtime creates the
monitor with rendering and destroys it at shutdown. On Windows:

| Metric | Source |
|---|---|
| `memory.gpu.os.budget_bytes`, `memory.gpu.os.usage_bytes` (gauges) | `IDXGIAdapter3::QueryVideoMemoryInfo`, local segment, read on the frame |
| `os.gpu.other_process_util` (gauge, percent) | PDH `\GPU Engine(*)\Utilization Percentage` through its English path: every other process's use summed per engine of the same adapter, the busiest engine's sum |

One PDH collection takes about 1 ms, so a sampler thread created by the first
frame's read collects once a second and publishes through an atomic; destroy
joins it. The frame's read costs about 1 µs. macOS has no source yet and
reports all three unsupported. The three are in Tier 0 recordings.

The editor shows the driver's figure, which every Vulkan device and Metal
report: the stats chip's detail reads `GPU budget (driver)` from
`VkrDeviceMemoryStats.driver_usage_bytes` and `driver_budget_bytes`, the same
values as `memory.gpu.driver.*`. The Memory window lists them under
`GPU budget` and, where the platform has a source, the OS's usage and budget
beside them as a cross-check.

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
On battery is reported only. `system_conditions` also carries the highest
`os.gpu.other_process_util`, and a run loses authority with `gpu.peer_load`
when other processes used more than 10 percent of an engine at any counted
frame (`VKR_HARNESS_GPU_PEER_LOAD_PERCENT`). `vkr_harness perf-compare` flags
that condition only when the two runs fall on opposite sides of it.

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
Vulkan needs `VK_KHR_present_id` and `VK_KHR_present_wait` for the same
measurement; the Windows RX 6700 XT driver (Adrenalin 26.6.3) exposes
neither, so Vulkan publishes no latency there.

**Live reads** ([editor_ops.c](../../editor/src/editor_ops.c)):
`perf.snapshot` answers the newest published metrics frame from the live
registry, filtered by name prefix, with `{ms, count, max_ms}` per duration,
`{unavailable: reason}` per empty slot and, with `passes`, each executed
render graph pass's CPU and GPU milliseconds. `perf.set` switches pass and
submission GPU timing as `VKR_RG_GPU_TIMING`, `VKR_GPU_SUBMISSION_TIMING`
and the F7 key do; a recording still controls them while it runs. The UI
frame borrows the registry and the pass table for the build.

**Pipeline statistics** ([vkr_vulkan_graph.c](../../renderer/src/vulkan/vkr_vulkan_graph.c)).
`perf.set` `pipeline_stats` sets `VkrMetricsConfig.pass_pipeline_statistics`,
off by default; then each Vulkan frame slot brackets every pass with a
`VK_QUERY_TYPE_PIPELINE_STATISTICS` query at its timestamps, outside the
pass's rendering scope, counting vertex, fragment and compute shader
invocations. The device enables `pipelineStatisticsQuery` when present and
the startup report names it. The counts ride the pass rows, and
`perf.snapshot` `passes` answers them as `invocations`. They are not in
recordings, and the harness never turns them on, so no run fingerprint
changes. Metal reports none.

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
- Charging authority at warn pressure: rejected for the reason above, and
  confirmed by the owner on 2026-10-19.

## Revisit when

- A Windows source for thermal level or disk-read faults becomes reliable, or
  per-frame CPU time needs better than clock-tick resolution on Windows.
- A matched timing run shows the per-frame sample or the power read matters.
- A Windows driver exposes `VK_KHR_present_wait`, or a macOS source for the
  OS's GPU memory view or other processes' GPU use is chosen.

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
- Windows, 2026-10-18, Ryzen 5 2600, RX 6700 XT, Adrenalin 26.6.3, at
  `ef733a9b` with these sources: `vkr_harness profile --case
  tools/cases/local/p21_vulkan_bistro_windowed_validation.case.json
  --profile tools/profiles/local-windowed.json` (Release, 2 repetitions of
  256 warmup and 60 measured frames), report
  `sha256:bab6be25200832ad1e9378ed893e8eff1c6d59e357c9d82259da9f5791a9104b`:
  footprint, memory pressure, on battery and low power valid in 120 of 120
  samples (footprint p50 8760 MiB, pressure 0, AC, low power off); thermal
  level and major faults unavailable in every sample; `cpu.process_time` p50
  0 and max 31.25 ms; provenance AC, low power off and normal pressure at
  start and end. `health.status` filled `system` in a headless Debug and a
  windowed Release Bistro editor. A test main linked against the Release
  `vkr_foundation` saw the footprint rise 64.1 MB after touching 64 MB and
  measured 1.42 µs a call over 20,000 calls.
- The OS's GPU view, Windows, at `96857df1` plus these changes, the same
  case and profile twice (reports
  `sha256:61d93071c3aec68bbe4ebd49e6cc3774abb962bd498a355b5c4263d23d83776b`
  and
  `sha256:3822cbdfa6994ed0463c8b62ef50dac0597186adb2a1a6a45843a18f5e48c4ac`):
  `memory.gpu.os.budget_bytes` 12,032,638,976 against the driver's
  `memory.gpu.driver.recommended_budget_bytes` 12,032,638,800, and
  `memory.gpu.os.usage_bytes` 7,134 MB against the driver's 7,051 MB, all
  120 samples valid; `os.gpu.other_process_util` valid in 120 of 120, max 2
  percent, so no `gpu.peer_load`. `vkr_harness perf-compare` on the pair read
  both, found 170 work-volume rows unchanged and answered `inconclusive`:
  frame wall p50 2.83 to 2.90 ms (+2.5 %) with non-overlapping repetition
  ranges, but neither run is authoritative. A standalone probe of the monitor
  found the adapter by LUID and measured one PDH collection at about 1 ms
  over 318 instances.
- Pipeline statistics, Windows, RX 6700 XT (`pipelineStatisticsQuery`
  present): a headless Bistro editor answered `perf.set` `pipeline_stats`,
  then `perf.snapshot` `passes` gave `invocations` for all 200 executed
  passes, alone and with pass timing: 133 raster passes with vertex and
  fragment counts (`VBuffer.Opaque` 2,534,142 vertex and 925,142 fragment,
  `Shadow.Cascade.0` 2,180,410 vertex), 55 compute passes with compute
  counts only, 10 passes with none (`Cull.Upload`, `IBL.Bake`). Turning it
  off removed the counts. The same sequence in a Debug editor with the
  Khronos layer and synchronization validation counted 0 validation errors
  and 0 warnings.
