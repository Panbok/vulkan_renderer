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

- Vulkan records no pass starts.
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

The Windows sources, the OS's GPU memory view, other processes' GPU use and
`gpu.peer_load` are in [ADR-125](../adr/125-platform-telemetry-metrics.md);
Vulkan object names in [ADR-124](../adr/124-renderer-health-and-diagnostic-reads.md).
Remaining:

- Vulkan present latency (`VK_KHR_present_id`, `present_wait`) behind the
  present timing's latency and missed counts, on a driver that exposes them
  (the Windows RX 6700 XT driver does not).
- Optional pipeline statistics per pass, gated like pass timing.
- Acceptance: the Tier 0 overhead gate rerun with the added Tier 0 fields;
  present latency on a visible window.

## P2: timeline (remaining)

CPU zones per thread, Metal and Vulkan GPU pass starts on the CPU clock,
the Perfetto export, rolling recordings and the editor's Thread timeline are
implemented in
[ADR-126](../adr/126-cpu-zones-and-gpu-pass-timeline.md); `rec.explain` and
`vkr_rec explain` in [ADR-122](../adr/122-recordings.md); `vkr_harness
perf-compare` in [ADR-051](../adr/051-renderer-harness-and-evidence.md).
Remaining:

- PIX event output of the zone macro on Windows and `os_signpost` on macOS,
  so native profilers show the zones.
- More zones where cost hides: asset loads, physics steps inside jobs,
  script ticks, shadow updates.
- Counters and markers for recordings without `Frame` zones in the export.
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
