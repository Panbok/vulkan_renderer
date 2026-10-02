---
status: implemented
updated: 2026-10-03
authority: adr
---
# ADR-082: Renderer-owned render thread

## Status

Accepted. Off by default; see Revisit when.

## Context

The frame-loop thread, which runs the application host loop on the main
thread, used to do all of a frame's work in order: update, extraction,
input validation, graph realization, native recording, submission and
presentation. Update could not overlap recording. A source audit found three
couplings that rule out a simple hand-off: shadow reuse needs the acquired
frame's retained-shadow tokens and image index, ADR-045 publishes assets only
after acquisition, and the frame input borrows data that the next update
changes or frees (animation palettes, bind poses, world text geometry and the
harness capture request). Asset systems also publish from update, for example
during synchronous scene loads.

## Decision

`VkrRendererBackendConfig.render_thread`, or `VKR_RENDER_THREAD=0|1`, gives
`VkrRenderer` its own worker thread with an 8 MiB stack, the stack the main
thread already provides; a default secondary-thread stack overflows during
Metal image realization. `VkrStandardSceneRuntimeConfig.render_thread`
forwards the choice; nothing enables it by default.

`vkr_renderer_submit_frame()` consumes the acquired `VkrFrame` and hands
input validation, frame preparation, graph realization, recording, submission
and presentation to the worker. `vkr_renderer_complete_frame()` waits for it
and returns a `VkrRendererFrameResult`: error, frame metrics, validation
error and the render CPU time. `vkr_renderer_render_frame()` is submit
followed by complete. Without a worker, submit renders before returning, so
inline rendering keeps the previous behavior.

At most one frame is with the worker. Every public renderer entry point,
capture poll and release, metrics collection and asset-publisher call waits
for that frame first, so renderer and native state have one user at a time.
With a worker, `asset_publisher` forwards to the backend's table after that
wait; inline rendering keeps the backend table. The worker never waits on
itself. Acquisition stays on the frame-loop thread: the command-slot wait,
the Metal drawable and `CAMetalLayer` size, and Vulkan image acquisition do
not move.

The frame input and every array it references stay borrowed and unmodified
until completion. The standard scene runtime keeps them in runtime-owned
frame storage (draw context, acquired frame, editor overlays, packet) instead
of the `draw_frame` stack. With a worker it:

- completes the previous frame after the update callback and before camera,
  lighting, shadow, asset pumping and extraction, so the update overlaps
  rendering and everything the packet borrows from those systems is stable;
- alternates two frame-scratch arenas; a submitted frame's scratch scope
  closes at its completion;
- copies skinning bind vertices, influences and palettes, world-text geometry
  and the capture request into frame scratch, because the update may change
  or free them;
- settles only the capture or picking request the completed frame carried,
  because the next update may already have issued a new one;
- closes a metrics frame at completion and opens the next, so a published
  frame's renderer results, pass table and `cpu_frame_index` describe one
  rendered frame. `cpu.render_submit` is then the worker's render time and
  `cpu.render_wait` the frame-loop thread's wait at completion.

## Consequences

Output does not change. On the M1 Pro (Metal, Release, 2026-10-03), five
captures of the deterministic `bistro_shadow_motion_snapshot` were
byte-identical between the unmodified build and this build, inline and
threaded. The text and mannequin Bistro cases vary between two runs of the
unmodified build; their threaded differences stayed within that variation.
ThreadSanitizer threaded runs of the Bistro text and mannequin cases and the
CPU test suite reported no races once the resource-system fix below was in.

The thread pays off only for CPU-bound frames. The Bistro orbit stays
GPU-bound: inline and threaded `frame.wall` medians were 45.8 and 45.5 ms
(local, non-authoritative). In the app's `--gameplay` Bistro run, sampling
showed the render work leaving the main thread (14 % of samples inline) while
update ran beside it and the main thread waited 11 % at completion.

Results of frame N apply after update N+1 when threaded: editor viewport
output size, Scene error recovery and picking readback transitions are one
update later. Shadow commits still precede the next shadow decision.

Update code that calls a renderer function or publishes waits for the frame
and loses the overlap, without losing correctness. The harness child queries
the renderer in every update, so a threaded harness profile counts the render
time under `cpu.update`; harness reports cannot yet show the overlap.

The ThreadSanitizer runs exposed a pre-existing race: the resource system
freed request keys and unload names from its shared allocator after releasing
the request mutex. Those frees now happen under the mutex.

Native Vulkan behavior is unverified; only Metal ran.

## Alternatives considered

A runtime-owned thread that lends the renderer to the frame-loop thread in a
handoff window, with a renderer status snapshot, needs every caller audited
and kept correct by convention. Waiting inside the renderer keeps existing
callers correct and lets overlap grow as update-time renderer calls move.

Acquiring on the render thread would move `CAMetalLayer` work off the main
thread and add no overlap, because extraction needs the acquired frame.

A fully decoupled game thread that never waits for acquisition would move
shadow reuse, asset pumping and texture streaming to the render thread and
replace ADR-045. It is not justified while measured frames are GPU-bound.

## Revisit when

A CPU-bound workload shows a measured benefit, which decides the default; the
harness update stops querying the renderer every frame; or a native Vulkan
run is available.

## Code evidence

- [worker, waits, submit and complete](../../renderer/src/vkr_renderer.c)
- [public API](../../renderer/src/vkr_renderer.h)
- [result and worker state](../../renderer/src/vkr_renderer_internal.h)
- [frame storage, completion and copies](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [thread stack size](../../lib/src/core/vkr_threads.h)
- [resource-system allocator frees](../../runtime/src/renderer/systems/vkr_resource_system.c)
