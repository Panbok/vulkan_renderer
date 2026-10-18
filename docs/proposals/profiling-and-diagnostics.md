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
[ADR-124](../adr/124-renderer-health-and-diagnostic-reads.md). The first part
of P1, the lib telemetry API with its macOS sources, the `os.*` and
`cpu.process_time` metrics and the harness's system conditions, is implemented
in [ADR-125](../adr/125-platform-telemetry-metrics.md), with Metal present
latency, the recording timeline's System lane, `perf.snapshot` and
`perf.set`.

Remaining gaps:

- Pass rows store durations only; there are no CPU zones and no CPU/GPU clock
  link, so no thread timeline or flame chart exists.
- Windows platform signals, present latency and other processes' GPU use
  are not recorded.
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

## P1: platform signals (remaining)

- Windows sources behind `vkr_platform_get_telemetry`: `PrivateUsage`,
  `GetProcessTimes`, `GetSystemPowerStatus`; plus
  `IDXGIAdapter3::QueryVideoMemoryInfo` and PDH GPU engine use of other
  processes as new metrics. Thermal state stays unavailable.
- Vulkan present latency (`VK_KHR_present_id`, `present_wait`) behind the
  present timing's latency and missed counts.
- Vulkan object names (`vkSetDebugUtilsObjectNameEXT`) and optional pipeline
  statistics per pass, gated like pass timing.
- The harness authority reason `gpu.peer_load`.
- Acceptance: the Tier 0 overhead gate rerun with the added Tier 0 fields; a
  Windows Bistro run with the Windows sources valid; present latency on a
  visible window.

## P2: timeline

- A runtime-gated zone macro writing begin and end into per-thread recorder
  rings (the unused RENDER producer and one ring per job worker), also
  emitting `os_signpost` and PIX events.
- A start offset in each pass row, placed on the CPU clock through
  `sampleTimestamps:gpuTimestamp:` and `VK_KHR_calibrated_timestamps`;
  `vkr_rec export --perfetto`; a rolling in-memory buffer saved by hotkey or
  `perf.capture_last`.
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

- Memory pressure costs authority only at critical (ADR-125); confirm that
  warn, the 16 GB Mac's normal state under Bistro, stays report-only.

- `baseline propose` does not check `authoritative`, so a snapshot that lost
  authority to `validation.messages` can still be proposed.
