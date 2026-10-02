---
status: proposed
updated: 2026-10-03
authority: proposal
---
# Render thread by default

## Current baseline

[ADR-082](../adr/082-renderer-owned-render-thread.md) added an opt-in render
thread: `VKR_RENDER_THREAD=1` or `render_thread` in the renderer or standard
scene runtime config. The renderer's worker renders a submitted frame while the
frame-loop thread runs the next update; every other renderer call waits for it.
Nothing enables it by default.

The Bistro cases measured so far are GPU-bound on an Apple M1 Pro. A
non-authoritative local profile of `bistro_shadow_orbit` (Metal, Release,
2026-10-03, 2560×1440, immediate present) had inline and threaded
`frame.wall` medians of 45.8 and 45.5 ms, with about 41 ms of each frame
waiting for a command slot. Sampling the app's `--gameplay` Bistro run showed
the render work leaving the main thread while update overlapped it, without a
frame-rate change.

## Proposed change

Enable the render thread by default for the app, editor and player once a
CPU-bound workload shows a measured benefit, keeping inline rendering
available through `VKR_RENDER_THREAD=0`.

## Decision boundaries

- The harness child queries the renderer in every update, so its update waits
  for the frame being rendered and threaded profiles count the render time
  under `cpu.update`. Either the harness update moves those queries after
  frame completion, or timing evidence comes from an instrumented app run.
- Whether the app and editor expose the choice as a graphics setting or keep
  the environment variable.
- A fully decoupled game thread that never waits for acquisition would move
  shadow reuse, asset pumping and texture streaming onto the render thread and
  replace ADR-045. It needs its own measured justification.

## Evidence needed

1. A CPU-bound Bistro workload, such as editor UI or gameplay at reduced
   resolution, measured inline and threaded with matched capture-free Release
   runs that report `cpu.update`, `cpu.render_submit`, `cpu.render_wait` and
   `frame.wall` without the harness update waiting on the renderer.
2. A native Windows/Vulkan run of the threaded path: snapshots and a focused
   validation run.

## Code baseline

- [renderer worker](../../renderer/src/vkr_renderer.c)
- [runtime frame completion](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [harness child update](../../tools/harness/vkr_harness_child.c)
