---
status: proposed
updated: 2026-10-02
authority: proposal
---
# Render thread

## Current baseline

The app, editor, player and harness all run frames through
`vkr_standard_scene_runtime_run`. One thread runs the host loop: window and
input pumping, the runtime `update` callback, camera, lighting, atmosphere
preparation and shadow fitting, then `vkr_standard_scene_runtime_draw_frame`.
That function applies the resize mailbox, acquires the frame (including the
command-slot wait), pumps asset publication, extracts world, shadow, picking,
editor, UI, text and lighting payloads, and calls `vkr_renderer_render_frame`
to validate, realize the graph, record, submit and present. Only the event
worker, resource-preparation workers and editor tool workers run on other
threads; none of them records or submits GPU work.

A non-authoritative phase 0 observation on 2026-10-02 found the Bistro orbit
GPU-bound on an Apple M1 Pro (Metal, Release at `91f22a1d`, hidden window
rendering 2560×1440, immediate present, three target images, two children of
300 measured frames, warmup unstable, local profile, documentation-only dirty
tree; report `sha256:9bf1a7ada126d4f76dda8face6bf71c78ee8f93ad856d60800d6dd8454ed91e4`):

```sh
./build_release/tools/vkr_harness profile \
  --case tools/cases/performance/bistro_shadow_orbit.case.json \
  --profile tools/profiles/local-windowed.json
```

| Metric | p50 ms | Mean ms |
|---|---:|---:|
| `frame.wall` | 47.17 | 48.23 |
| `cpu.render_prepare` (one command-slot wait in every frame) | 43.01 | 44.20 |
| `cpu.render_submit` (would move to the render thread) | 2.48 | 2.41 |
| `cpu.update` | 0.24 | 0.30 |
| `cpu.world_payload_build` | 0.41 | 0.38 |

Without the slot wait, the frame spends about 4.0 ms of CPU time (mean
`cpu.frame_work` minus `cpu.render_prepare`). About 2.4 ms of that is
`cpu.render_submit`; the remaining 1.6 ms is update, shadow fitting,
extraction and the UI, text and packet build. Overlapping these could
shorten only a CPU-bound frame, by at most `cpu.render_submit`. This case
waits on the GPU for about 43 ms per frame, so a render thread would not
change its frame rate. Gameplay, editor UI and native Windows/Vulkan CPU
splits are unmeasured: the harness has no scripts mode, and its `editor`
flag composes the paneled viewport without building the editor UI.

## Proposed change

Run acquisition, rendering, submission and presentation on a dedicated
render thread while the main thread keeps platform events, input, scripts,
simulation, editor UI and camera and shadow fitting. The two threads overlap
game update of frame N+1 with recording and presenting of frame N.

The renderer and asset publication keep exactly one owner at any time:

- The render thread owns them from packet handoff until a frame is acquired.
- After acquisition the render thread parks holding the acquired `VkrFrame`
  and lends ownership to the main thread for a short handoff window. The
  window runs today's middle of `draw_frame` unchanged: the previous frame's
  result, asset pumping, world extraction, shadow reuse against the frame's
  retained tokens, and the UI, text and packet build. Handing the packet back
  returns ownership.
- A quiesce request parks the render thread between frames without an
  acquired frame, giving the main thread exclusive ownership for scene
  load and unload, graphics settings, synchronous publishing loads and
  shutdown.

This keeps the ADR-004 frame protocol order and the ADR-045 rule that
finalization runs after acquisition on the renderer owner. Debug builds would
assert the current owner in renderer entry points and `VkrAssetPublisher`
calls. Running the same sequence inline, without a thread, stays available for
deterministic harness captures and A/B timing.

## Required changes

- Packet storage: two packet slots, each with its own frame arena, the draw
  context now on the `draw_frame` stack, and owned copies of live data the
  packet borrows today: skinning palettes and the UI system's cached draw
  list. A slot is reused only after its render call returned.
- Result handling: frame N's submit result, validation error, metrics,
  shadow commit and picking state are applied at the start of handoff N+1,
  before the next shadow reuse decision.
- Renderer status: values game code reads during update (shadow depth range,
  display exposure, render and output extents, frame metrics, device memory)
  are copied during the handoff window. Atmosphere preparation and polling
  move into that window because they publish.
- Platform: macOS window code stops reading `CAMetalLayer.drawableSize`,
  which the Metal backend writes, and render-thread layer property writes use
  an explicit `CATransaction`. The Win32 message pump stays on the main thread.
- Metrics: frame-domain metrics assert their creating thread; render-side
  timings are recorded per packet slot and published during the handoff.

## Decision boundaries

- Phase 0 decides whether the work proceeds: it needs a CPU-bound case in
  which render-side CPU time is a material share of the frame.
- A fully decoupled alternative, in which the main thread never waits for
  acquisition, moves shadow reuse, asset pumping and texture streaming onto
  the render thread and gives asset systems command queues. It replaces
  ADR-045 and is not proposed until the handoff design is measured.
- Whether threaded rendering becomes the default for the harness, or the
  harness keeps inline rendering for captures.

## Evidence needed

1. Phase 0: a CPU-bound Bistro workload in which `cpu.render_submit` is a
   material share of `frame.wall`. The renderer-only orbit above is not one.
   Remaining candidates need a harness scripts mode for FPS gameplay, an
   editor UI measurement, or a native Windows/Vulkan run.
2. Inline refactor: Bistro snapshots unchanged on Metal and Vulkan before
   any thread exists.
3. Threaded opt-in: a ThreadSanitizer CPU run, one focused Metal validation
   run and a Bistro smoke run.
4. Default on: compatible before and after Release reports for app and
   editor cases with equivalent output. Native Windows/Vulkan evidence is
   reported unavailable until it is run there.

## Code baseline

- [host loop](../../runtime/src/application/vkr_application_host.c)
- [frame callback and draw_frame](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [world extraction and skinning rows](../../runtime/src/renderer/systems/vkr_scene_frame.c)
- [UI draw list cache](../../runtime/src/renderer/systems/vkr_ui_system.c)
- [metrics thread ownership](../../lib/src/core/vkr_metrics.c)
- [macOS window layer reads](../../runtime/src/platform/vkr_window_macos.m)
- [Metal drawable acquisition](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)
- [ADR-004](../adr/004-stateless-render-packet.md),
  [ADR-045](../adr/045-resource-prepare-and-render-thread-finalize.md),
  [ADR-047](../adr/047-event-payload-and-resize-mailbox-lifetimes.md)
