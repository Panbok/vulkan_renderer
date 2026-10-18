---
status: partial
updated: 2026-10-18
authority: adr
---

# ADR-124: Renderer health and diagnostic reads

## Status

Accepted (partial). Phase P0 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented on both backends, and each has a native Bistro run. No Vulkan
GPU wait in those runs came near the 5 s completion bound.

## Context

Before this decision four failures were silent. Metal reduced a command
error to a flag that only dropped a timing sample. Vulkan waited forever on
a GPU hang. `cpu.backend_present` was registered but never written. Agents
could read no log record, validation count or GPU error through the agent
channel, and the harness accepted runs that had validation errors as
evidence.

## Decision

**Health counts.** `vkr_renderer_get_health`
([vkr_renderer.h](../../renderer/src/vkr_renderer.h)) returns
`VkrRendererHealth`: GPU errors, validation errors and warnings, whether
validation is counted, and device loss. Counts only grow. Any thread may read
them while the render thread runs.

- Metal attaches commit feedback to every frame commit
  ([vkr_metal_packet_frame.inc](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)).
  The callback retains the `NSError`. The render thread counts it and logs its
  domain, code and description, thinned after eight occurrences to powers of
  two ([vkr_metal_packet_commands.inc](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc)).
  A commit that did not ask for submission timing publishes its result at
  completion as before; its record only reports errors, and slot reuse waits
  for feedback only when pass timing is on.
- Metal validation layers write to stderr outside the engine, so
  `validation_counted` is false on Metal.
- The Vulkan debug messenger counts ERROR and WARNING validation messages in
  atomics on `VkrVulkanDevice`, its user data
  ([vkr_vulkan_device.c](../../renderer/src/vulkan/vkr_vulkan_device.c)).
  The first `VK_ERROR_DEVICE_LOST` from any call marks the device lost, sets
  the renderer's terminal failure and logs the `VK_EXT_device_fault` report
  when the device supports that extension and its `deviceFault` feature.
  The startup capability report names the `deviceFault` feature, so a log
  shows whether a lost device will explain itself.

**Bounded Vulkan completion waits.** Frame-slot reuse, swapchain image reuse,
idle, FSR slot and graph resource waits wait at most 5 s
(`VKR_VULKAN_COMPLETION_TIMEOUT_NS`), the Metal watchdog's limit. On timeout
the backend logs the device fault report, writes a FATAL line naming the wait
and its values, and exits without teardown, because completion is unproven
([vkr_vulkan_renderer.c](../../renderer/src/vulkan/vkr_vulkan_renderer.c)).
`vkAcquireNextImageKHR` and present-fence waits stay unbounded: the
presentation engine may hold images while a window is minimized or occluded.

**Metrics** ([vkr_renderer_metrics.c](../../renderer/src/vkr_renderer_metrics.c),
[vkr_standard_scene_runtime.c](../../runtime/src/application/vkr_standard_scene_runtime.c)):

| Metric | Kind | Meaning |
|---|---|---|
| `cpu.backend_present` | duration | CPU time in the present calls since the previous frame; not sampled for a frame that presented nothing |
| `cpu.drawable_acquire` | duration | CPU time waiting for the next presentable image (`nextDrawable`, `vkAcquireNextImageKHR`) |
| `gpu.errors` | counter | Submissions the GPU finished with an error; on Vulkan, device loss |
| `gpu.validation.errors`, `gpu.validation.warnings` | counter | Validation messages; unsupported on Metal |
| `log.errors`, `log.warnings` | counter | ERROR and FATAL, and WARN records the logger took (`log_counts_get`, [logger.h](../../lib/src/core/logger.h)) |

The backends accumulate present and acquire time; the metrics collector is
the only reader and reset owner. Because they are registry metrics, they
reach recordings, harness reports and `rec.frames` without new plumbing.

**Agent reads** ([editor_ops.c](../../editor/src/editor_ops.c)):
`log.read` answers records from the log history with sequence, UTC time,
level, source and message, filtered by `level` and a case-sensitive `grep`;
without `after` it answers the newest `limit` matches, with `after` it follows
the log. `health.status` answers the health counts, log totals by severity,
the Scene's renderer error, the newest error records and an `ok` verdict.

**Harness** ([vkr_harness_report.c](../../tools/harness/vkr_harness_report.c)):
each run, auxiliary run and report carries `diagnostic_counts`. They are the
five counters summed over warmup and measured frames, plus a scan of the
child's `stdout.log` and `stderr.log` for engine level prefixes and Vulkan
and Metal validation text. A counter without samples is `null` with a reason.
A run loses authority with `gpu.errors` when a GPU error occurred and with
`validation.messages` when a validation error was counted or printed.

## Consequences

- An agent detects a broken run from `health.status` or a report instead of
  reading stdout.
- A Vulkan hang ends the process after 5 s with a named wait instead of
  freezing it; unsaved edits are lost, as on Metal.
- Each Metal frame now dispatches one feedback callback even with timing off.
  Its cost was not measured separately.
- Errors logged before warmup reach the report only through the output scan.

## Alternatives considered

- Counting Metal validation by redirecting stderr in process: rejected; the
  harness scans the child's output files instead.
- Bounding present-fence waits too: rejected, because a hidden window would
  end the editor.
- A separate health channel beside the metrics registry: rejected; the
  registry already feeds every consumer.

## Revisit when

- Metal shader logging (`MTLLogState`) routes validation through the engine.
- The crash handler and GPU breadcrumbs of the proposal's P3 land.
- A healthy Vulkan GPU job that takes seconds runs against the bounded
  waits, or a device loss exercises the fault report.

## Evidence

- `./build_release.sh`, `./build_editor.sh Release` and `./build_test.sh`
  (Debug, ASan) passed on the M1 Pro, 2026-10-18; 851 test cases passed.
- A temporary hidden-window Bistro case at 1280×720 (2 repetitions, 60 warmup
  and 120 measured frames) under `tools/profiles/local-windowed.json` passed
  on Metal. `cpu.backend_present` was valid in all 240 samples, p50 19 µs;
  `cpu.drawable_acquire` p50 29 µs. `gpu.errors` and `log.errors` were 0 over
  360 counted frames; the validation counters were unavailable. The output
  scan read 8 files and found nothing. The run is local and not authoritative.
- A headless Bistro editor answered `health.status` (`ok` true, no GPU errors,
  validation not counted) and `log.read` in tail, `grep`, `after` and invalid
  `level` forms.
- Vulkan on Windows, 2026-10-18, Ryzen 5 2600, RX 6700 XT, Adrenalin
  26.6.3, API 1.4.315, at `ef733a9b` plus the capability-report row:
  `build_release.bat`, `build_editor.bat Release` and `build_test.bat`
  (Debug) passed, 841 test cases passed, and no changed Vulkan file warned.
- `vkr_harness profile --case
  tools/cases/local/p21_vulkan_bistro_windowed_validation.case.json
  --profile tools/profiles/local-windowed.json` (hidden window, immediate
  present, 2 repetitions of 256 warmup and 60 measured frames). Release,
  report `sha256:bab6be25200832ad1e9378ed893e8eff1c6d59e357c9d82259da9f5791a9104b`:
  `cpu.backend_present` valid in 120 of 120 samples, p50 150 µs and p95
  225 µs; `cpu.drawable_acquire` valid in 120 of 120, p50 124 µs and p95
  221 µs; `gpu.errors` and `log.errors` 0 over 632 counted frames; the
  validation counters unavailable. Debug, with the Khronos layer and
  synchronization validation, report
  `sha256:9be63deddeacc4970c658784f0f4bdf91d278d1ab2cfd0a079dcbdcb59f1024a`:
  `gpu.validation.errors` 0 over 632 counted frames, equal to the
  `Validation Error` lines in each child's output. No validation message
  occurred, so a nonzero count and its authority reason are unexercised.
  Both runs are local and not authoritative.
- A headless Debug Bistro editor answered `health.status` with validation
  counted and 0 errors, and `log.read` at `warn` returned the same 2 records
  as the session log's `[WARN]` lines. The startup log read `Vulkan
  capability feature deviceFault: present (optional)`.
- A windowed Release Bistro editor stayed minimized for 40 s, recreated its
  swapchain on restore, answered `health.status` throughout and logged no
  completion timeout.
- `vkr_bakery bake probe --scene assets/scenes/bistro.scene.json --position
  -28.5 4.6 2.9 --size 256 --supersample 4` captured six 1024×1024 Bistro
  faces through a Release Vulkan harness child in 45 frames and 40 s, report
  `sha256:bd91d1cfe1b35f2786a3c359699e9120932742c6610f50fa61004fc620540e8d`:
  `gpu.errors` and `log.errors` 0, and the output scan found no ERROR or
  FATAL line, so no completion wait timed out. The editor's Bake lighting
  job could not run beside an open Bistro editor on the 16 GB machine: its
  diffuse-volume Bakery process left under 1.5 GB free.
