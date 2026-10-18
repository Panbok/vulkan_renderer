---
status: partial
updated: 2026-10-18
authority: adr
---

# ADR-127: Crash reports, GPU capture and the render graph description

## Status

Accepted (partial). The first part of phase P3 of
[Profiling and diagnostics](../proposals/profiling-and-diagnostics.md) is
implemented on the Mac: crash reports, the `debug.gpu_capture` request and
the `debug.graph` description. The Windows crash handler and a Vulkan
capture (RenderDoc) are stubs. Metal programmatic capture is refused by
macOS 26 for Metal 4 compute encoders.

## Context

A crash left only the session log and the system's `.ips` report, which
knows neither the frame, the scene nor the agent operation that ran. No
agent could start a native GPU capture. The compiled render graph was
visible to no tool: `vkr_rg_export_dot` had no caller.

## Decision

**Crash reports** ([vkr_crash.h](../../lib/src/platform/vkr_crash.h)).
`vkr_crash_install(directory, program)` installs handlers for SIGSEGV,
SIGBUS, SIGILL, SIGFPE, SIGABRT and SIGTRAP on an alternate signal stack
([vkr_crash_posix.c](../../lib/src/platform/vkr_crash_posix.c)).

- The handler writes `crash-<program>-<pid>-<unix seconds>.txt` with the
  signal, its code and address, the breadcrumbs, the newest 64 log records
  and a backtrace. It then restores the default action and raises again, so
  the system report and exit status stay as before.
- Breadcrumbs are fixed buffers any thread sets without locks:
  - the frame number and frame stage, from the application host loop;
  - the scene path, from the runtime each frame;
  - the running agent operation, from the editor's op dispatch;
  - the pass slot, which nothing sets yet.
- Reports go into the editor's session log directory (`<workspace>/logs/`),
  or the player's settings folder; with no directory, to stderr. A second
  fault during a report goes straight to the default action.
- `vkr_crash_write_report(reason)` writes the same report without a signal.
  Both GPU completion timeouts call it before they exit (ADR-124).
- The log tail is read from the history ring without the logger lock
  (`log_history_emergency_write`), so a record being written can come out
  torn. `backtrace` is not on the POSIX async-signal-safe list. Everything
  before the backtrace is already written if it fails.
- Windows: `vkr_crash_windows.c` is a stub. Its comment states the contract:
  an unhandled-exception filter, a minidump with thread info, the same text
  report, then `EXCEPTION_CONTINUE_SEARCH`.

**GPU capture** ([vkr_renderer.h](../../renderer/src/vkr_renderer.h)).
`vkr_renderer_request_gpu_capture(path, frames)` and
`vkr_renderer_gpu_capture_status` drive a capture for a GPU debugger.

- On Metal the render thread starts an `MTLCaptureManager` capture of the
  device before the next frame encodes, and stops it after `frames`
  committed frames
  ([vkr_metal_packet_frame.inc](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)).
- Capture to a `.gputrace` document needs the capture layer, which a
  process outside Xcode gets only with `MTL_CAPTURE_ENABLED=1`. Without it,
  the request fails with that reason.
- The capture layer throws in the pipeline archive lookup (an unrecognized
  `supportsAIRNT` selector sent to a dispatch queue), as the shader
  validator does. So capture-enabled runs compile pipelines from source
  ([vkr_metal_packet_setup.inc](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc)).
- Vulkan reports that it has no capture.
- `debug.gpu_capture {name, frames}` (1 to 8) writes into the recording
  directory and answers the path, or the failure with its reason
  ([editor_ops.c](../../editor/src/editor_ops.c)).

**Render graph description** ([vkr_rg_debug.h](../../renderer/src/vkr_rg_debug.h)).
`vkr_rg_describe` visits the compiled graph's passes, in execution order
and then culled, with the resources each reads and writes. It then visits
the resources with kind, extent, format, bytes, lifetime, instance domain
and first and last use.

- `vkr_renderer_describe_graph` joins the render thread, adds the backend's
  realized memory and, given a path, writes `vkr_rg_export_dot` output.
- `debug.graph {passes, resources, pass, dot}` answers it with the newest
  CPU and GPU milliseconds per pass, at most 512 rows per list.
- The graph never aliases memory, and one resource's backend instance count
  is not exposed, so only the backend's live totals count instances.

## Consequences

- A crash or GPU timeout leaves a report that names the frame, the scene,
  the agent operation and the recent log.
- An agent can read the render graph without a debugger and hand a DOT file
  to Graphviz.
- A Metal GPU capture from inside the engine waits for Apple to support
  Metal 4 compute encoders in programmatic capture. Xcode's capture layer
  no longer crashes the editor at startup.

## Alternatives considered

- A separate crash-reporting process: catches more (other threads' stack
  overflows) but adds a process and IPC.
- Writing reports from the logger's file: needs the logger lock, which a
  crashing thread may hold.

## Revisit when

- macOS supports capturing Metal 4 compute encoders.
- The Windows crash handler and RenderDoc capture land.
- A crash on another thread's stack overflow needs a report (per-thread
  alternate stacks).

## Evidence

- `./build_release.sh`, `./build_editor.sh Release` and `./build_test.sh`
  (Debug, ASan) on the M1 Pro, 2026-10-18: 860 test cases passed, including
  `test_crash_write_report`. That test checks the reason, pid, frame and
  every breadcrumb, that a long breadcrumb is cut to fit, and that a
  backtrace follows.
- A scratch driver built against `libvkr_foundation.a` wrote reports for a
  null write (exit 139), a trap (exit 133), a main-thread stack overflow
  and a timeout-style report; macOS still wrote its `.ips` reports.
- A headless Bistro editor started with `MTL_CAPTURE_ENABLED=1` aborted
  before the archive change, and the editor's crash handler printed the
  SIGABRT report with its backtrace. After the change it started, and
  `debug.gpu_capture` answered "Capturing Metal 4 Compute Command Encoder
  is not supported." Without the variable it answered the
  `MTL_CAPTURE_ENABLED=1` reason.
- `debug.graph` on a headless Bistro editor answered the tiled pipeline's
  34 passes and 31 resources, declared image bytes 258 MB against 406 MB
  live, and wrote a 13 KB DOT file.
- Windows: stubs only.
