---
status: proposed
updated: 2026-10-18
authority: proposal
---

# Profiling and diagnostics

## Baseline

Every timing consumer reads the metrics registry (`lib/src/core/vkr_metrics.h`):
the recorder ([ADR-122](../adr/122-recordings.md)), harness reports
([ADR-051](../adr/051-renderer-harness-and-evidence.md)), the editor's stats
chip and `stats.*`, and Metal dynamic resolution. A metric added there reaches
all of them. Phase P0, renderer health and diagnostic reads, is implemented in
[ADR-124](../adr/124-renderer-health-and-diagnostic-reads.md).

Remaining gaps:

- Pass rows store durations only; there are no CPU zones and no CPU/GPU clock
  link, so no thread timeline or flame chart exists.
- No platform signal is recorded: thermal state, power, `phys_footprint`,
  memory pressure, CPU time, present latency or other processes' GPU use.
  The harness report already has `thermal_state_start`, `thermal_state_end`
  and `power_mode` fields, all `unknown`.
- The Performance window opens only on a recording; there is no live view.
- No op or button triggers a native GPU capture; there is no crash handler.

## Owner decisions (2026-10-18)

- Timeline: our own CPU zones in `.vkrrec` plus a Perfetto export, not Tracy.
- Naming: a shared name where both platforms measure the same quantity
  (`os.thermal.level` normalized 0–3, `os.memory.footprint_bytes`); a
  `gpu.metal.*`, `gpu.vk.*`, `os.mac.*` or `os.win.*` prefix only without a
  counterpart. An absent signal reports an unavailable reason, never zero.
- Native capture triggers ship in editor builds only, never in `vkr_player`.
- Third-party headers (`renderdoc_app.h`, the PIX event runtime, NVML, ADLX)
  are optional and loaded only when present.

## P1: platform signals

- A lib telemetry module (ADR-118) sampled at 10 Hz on the recorder's writer
  thread, outside the frame loop:
  - macOS: `NSProcessInfo.thermalState`, `isLowPowerModeEnabled`,
    `task_info(TASK_VM_INFO).phys_footprint`, memory pressure events
    (`DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`), thread and process CPU time,
    page faults.
  - Windows: `IDXGIAdapter3::QueryVideoMemoryInfo`, PDH GPU engine use of
    other processes, `GetProcessTimes`, `PrivateUsage`,
    `GetSystemPowerStatus`. Thermal state stays unavailable.
- Present latency: `MTLDrawable.addPresentedHandler`, `VK_KHR_present_id` and
  `present_wait`. CPU/GPU clock link: `sampleTimestamps:gpuTimestamp:`,
  `VK_KHR_calibrated_timestamps`.
- Vulkan object names (`vkSetDebugUtilsObjectNameEXT`) and optional pipeline
  statistics per pass, gated like pass timing.
- Harness authority reasons `thermal.throttled`, `memory.pressure` and
  `gpu.peer_load`, and the existing provenance fields filled.
- A System lane under the Performance window's frame-time lane; agent ops
  `perf.snapshot {prefix}` and `perf.set {pass_timing, pipeline_stats}`.
- Acceptance: the Tier 0 overhead gate rerun; a throttle or pressure event
  visible in a Bistro recording.

## P2: timeline

- A runtime-gated zone macro writing begin and end into per-thread recorder
  rings (the unused RENDER producer and one ring per job worker), also
  emitting `os_signpost` and PIX events.
- A start offset in each pass row; `vkr_rec export --perfetto`; a rolling
  in-memory buffer saved by hotkey or `perf.capture_last`.
- A Timeline window with thread tracks, a GPU track and a flame chart;
  `rec.explain {spike}`; `vkr_harness perf-compare` for before/after verdicts.
- Acceptance: the overhead gate with zones on; zone sums match stage metrics.

## P3: deep tools

- `debug.gpu_capture` (MTLCaptureManager `.gputrace`, RenderDoc in-app API)
  and a Capture GPU frame button.
- A lib crash handler writing a breadcrumb file (frame, active pass, last log
  records, backtrace) and a Windows minidump; Vulkan checkpoints or buffer
  markers.
- A render graph window and `debug.graph` built on `vkr_rg_export_dot`.
- Debug line and box drawing, freeze culling, overdraw, LOD and light-count
  views; optional vendor counters and an `xctrace` table importer.
- Acceptance: native runs on each backend; Vulkan needs the Windows machine.

## Open items

- `baseline propose` does not check `authoritative`, so a snapshot that lost
  authority to `validation.messages` can still be proposed.
