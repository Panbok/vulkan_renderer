---
status: proposed
updated: 2026-10-19
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

The Windows sources, the OS's GPU memory view, other processes' GPU use,
`gpu.peer_load` and per-pass pipeline statistics are in
[ADR-125](../adr/125-platform-telemetry-metrics.md);
Vulkan object names in [ADR-124](../adr/124-renderer-health-and-diagnostic-reads.md).
Remaining:

- Vulkan present latency (`VK_KHR_present_id`, `present_wait`) behind the
  present timing's latency and missed counts, on a driver that exposes them
  (the Windows RX 6700 XT driver does not).
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

## P3: deep tools (remaining)

Crash reports with the pass breadcrumb, the `debug.gpu_capture` request,
`debug.graph`, the Render graph window, debug drawing, freeze culling with
occlusion on Metal, and the LOD, light count and overdraw views are
implemented in
[ADR-127](../adr/127-crash-reports-gpu-capture-and-graph-description.md).
Remaining:

- Vulkan checkpoints or buffer markers (`VK_AMD_buffer_marker` and
  device-coherent memory exist on the Windows RX 6700 XT) naming the last
  pass before a device loss.
- A Capture GPU frame button once a backend can capture on its machine
  (Metal 4 compute encoders are not capturable programmatically on macOS
  26; RenderDoc needs `descriptorBufferCaptureReplay`, which the Windows AMD
  driver lacks).
- Graph declarations for the resources `IBL.Bake`, `DiffuseVolume.Compose`
  and `Clouds.SkyLight` write, which `debug.graph` shows without reads or
  writes on Vulkan. ADR-002 leaves their barriers to the executors; an audit
  on 2026-10-19 found two that missed readers (the IBL prefilter's compute
  readers, the cloud sky light's radiance chain), now fixed. Declaring them
  needs per-frame imports of published textures and native buffer imports,
  which the Vulkan graph lacks.
- Overdraw, LOD and light count views on the desktop pipeline, and
  occlusion under freeze culling on Vulkan (it keeps no frozen pyramid).
- Optional vendor counters and an `xctrace` table importer.
- Acceptance: native runs on each backend; Vulkan needs the Windows machine.

## Open items

- Memory pressure costs authority only at critical (ADR-125); confirm that
  warn, the 16 GB Mac's normal state under Bistro, stays report-only.

- `baseline propose` does not check `authoritative`, so a snapshot that lost
  authority to `validation.messages` can still be proposed.
