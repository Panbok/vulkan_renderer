---
status: implemented
updated: 2026-10-06
authority: adr
---
# ADR-082: Renderer-owned render thread

## Status

Accepted. On by default; `inline_rendering` or `VKR_RENDER_THREAD=0`
renders inline.

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

`VkrRenderer` gets its own worker thread with an 8 MiB stack, the stack the
main thread already provides; a default secondary-thread stack overflows
during Metal image realization. `VkrRendererBackendConfig.inline_rendering`,
forwarded from `VkrStandardSceneRuntimeConfig.inline_rendering`, renders on
the calling thread instead; `VKR_RENDER_THREAD=0|1` overrides either.

`vkr_renderer_submit_frame()` consumes the acquired `VkrFrame` and hands
input validation, frame preparation, graph realization, recording, submission
and presentation to the worker. `vkr_renderer_complete_frame()` waits for it
and returns a `VkrRendererFrameResult`: error, frame metrics, validation
error and the render CPU time. `vkr_renderer_render_frame()` is submit
followed by complete. Without a worker, submit renders before returning, so
inline rendering keeps the previous behavior.

At most one frame is with the worker. Every public renderer entry point,
capture poll and release and metrics collection waits for that frame first,
so renderer and native state have one user at a time. The worker never waits
on itself.

Asset publication does not wait. `VkrRenderer.asset_publisher` is the
renderer's own table, threaded or not: each publish, unpublish, sampler
update or bake call records an ordered command with copies of its payload
(`vkr_publication_queue.c`) and returns whether it was recorded. Texture
bytes have two exceptions (`VkrTexturePreparedLoad.upload_ownership`). A
loaded texture's request keeps the decoded payload until the publication
settles, even when canceled meanwhile, so the command borrows them
(`RETAINED`). A payload too large to hold twice, such as a lightmap set
(ADR-088), moves to the batch instead (`TRANSFERRED`): the command takes the
caller's buffers without copying, the texture system clears the caller's
pointers, and the batch frees them after it has run, once the native upload
has copied them into backend staging. A publication the queue refuses, or
one that finds the texture already present, leaves them with the caller.
Submitting a
frame hands the recorded batch to the thread that renders it, which runs the
commands through the backend's table in order before preparing that frame,
inside the frame once acquired, so Vulkan uploads still record into an
acquired frame (ADR-045). A frame that is not acquired, `wait_idle` and
destruction run the batch at once. Every command yields a completion naming
its resource and the native result; completing the frame delivers them with a
snapshot of what asset code queries without waiting (publication idle and
generation, texture upload capacity, bake status and SH slots). Completions
the asset systems have not polled yet are kept, and the queue is idle only
once no batch is recorded or in flight and every completion was polled.

A publication that succeeds is resolvable in the frame it was recorded for;
only its completion says whether it did. The geometry, texture and material
systems count each command they record and each completion against the
resource (`VkrPublicationState`), keeping the first failure since the
resource last settled. Frames name a resource only once it is confirmed:

- a mesh asset or mesh stays PENDING until every submesh geometry and material
  is confirmed, then becomes LOADED, or FAILED when one failed;
- a loaded texture's request keeps its payload and becomes READY only once
  the loader confirms the publication; a busy or out-of-memory result releases
  the texture and finalizes the retained payload again, and texture admission
  uses the capacity the last frame observed less the bytes recorded since;
- the environment and reflection probes are used once their prefilter's
  publication and IBL bake are confirmed, and gizmo draws once their shapes
  are;
- an atmosphere bake the backend rejects reports FAILED through its status.

Backends still omit world draws whose geometry or material is unresolved,
which covers a material republished with a replaced texture that fails.

`vkr_renderer_submit_unacquired_frame()` hands the worker a frame it acquires
itself with the caller's `VkrFrameConfig`, so the caller never waits for
frame-slot reuse; the result reports the acquisition and its time. A frame
whose acquired extent differs from its input is cancelled with
`VKR_RENDERER_ERROR_FRAME_SKIPPED`. `VkrFrameHooks` let the caller finish the
input after acquisition (`prepare`) and react to submission or cancellation
(`finish`), on whichever thread renders the frame. Metal then acquires the
drawable on the render thread: it changes `CAMetalLayer.drawableSize` only
when the size changes, inside an explicit `CATransaction`, and the window
publishes its pixel size from the main thread (`vkr_window_publish_pixel_size`)
so acquisition never calls AppKit.

The frame input and every array it references stay borrowed and unmodified
until completion. The standard scene runtime keeps two frames of storage
(draw context, acquired frame, editor overlays, packet, shadow inputs) instead
of the `draw_frame` stack. With a worker, a frame is either:

- decoupled: built from the target extent and render scale the last frame
  used, then acquired by the render thread, while the frame-loop thread
  builds the next; the previous frame completes just before this one is
  submitted. Every steady-state frame is decoupled.
- coupled: the frame-loop thread completes the previous frame and acquires
  this one itself, as inline rendering does. Frames that resize or change the
  Scene output extent are coupled, because the build needs the new extent.
  Loading and texture streaming do not couple frames; their publications
  travel with the frame.

Work that needs the acquisition runs in the `prepare` hook: the renderer's
publication generation and pending state, probe SH slots, the shadow fit,
cascade and local-shadow resolution, and picking scale. The render thread
therefore owns the shadow system; the frame-loop thread chooses its
configuration and fit invalidation (`shadow_config`,
`vkr_standard_scene_runtime_invalidate_shadow_fit`) and captures the camera,
light, caster bounds and SDSM sample into the frame. `finish` commits or
discards shadow reuse. The runtime also:

- alternates two frame-scratch arenas; a submitted frame's scratch scope
  closes at its completion;
- copies skinning bind vertices, influences and palettes, world-text geometry,
  lights, the UI draw list and the capture request into frame scratch,
  because the next frame's update and build change or free them;
- reads target extents, render extents, display exposure and the SDSM sample
  from values cached when the last frame completed, not from the renderer;
- carries a capture request only once, and settles only the capture or
  picking request the completed frame carried;
- closes a metrics frame at completion and opens the next, so a published
  frame's renderer results, pass table and `cpu_frame_index` describe one
  rendered frame. `cpu.render_submit` is then the worker's render time,
  `cpu.render_prepare` its acquisition of a decoupled frame,
  `cpu.render_wait` the frame-loop thread's wait at completion and
  `frame.decoupled` whether the render thread acquired the frame.

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

Every measured frame of the threaded Bistro orbit and of ThreadSanitizer
runs was decoupled (`frame.decoupled` 1). In the app's `--gameplay` Bistro
run the render thread spent 83 % of its samples acquiring, that is, waiting
for a frame slot, and the main thread waited at completion instead of in
acquisition. The frame rate stays GPU-bound.

Results of frame N apply after frame N+1 is built when threaded: editor
viewport output size, Scene error recovery and picking readback transitions
are one frame later. Shadow commits still precede the next shadow decision.

Loading frames are decoupled too: in the threaded Bistro text case the render
thread acquired 782 of 783 submitted frames, the remaining one being the
first. A newly published resource is confirmed when the frame that ran its
command completes, so meshes, loaded textures and baked environments appear
one frame later inline and two frames later threaded than they did when
publication was synchronous. While loaded texture bytes were still copied
into the queue, peak resident memory while loading Bistro reached 3.9 GiB
threaded against 3.3 GiB inline. Borrowing them, single samples of the
`bistro_shadow_motion_snapshot` child's peak were 3.62 GiB threaded and
3.77 GiB inline, against 3.84 and 3.59 GiB for the copying build; these
peaks vary by about 0.2 GiB between runs. With confirmation in place the five `bistro_shadow_motion_snapshot` captures
stayed byte-identical to the unmodified build, inline and threaded, and a
ThreadSanitizer threaded run of the Bistro text case and the CPU test suite
reported no races.

Update code that calls a renderer function waits for the frame and loses
the overlap, without losing correctness. Logging every such wait in the app's
`--gameplay` run and a headless editor session on Bistro found only frame
completion, scene unload and an explicit device-memory log line; the
sample's memory overlay reads the device memory cached at completion. The
harness child pumps resources with the serials cached at completion and
reads the renderer's frame number, which needs the wait, only while it
aligns the measured phase; during the phase it waits only to poll captures
and, in submission-timing profiles, GPU submission timings. `cpu.update`
then measures update alone: 0.29 ms mean threaded against 0.31 ms inline in
the case below.

The thread is on by default at the owner's direction (2026-10-03), without a
measured speed-up. In matched local profiles on the Metal desktop
implementation, removed on 2026-10-06, of `bistro_metal_production_040`, a
Metal desktop case since removed (`local-windowed`, Release, M1 Pro, 1,500
samples each, same build), `frame.wall` mean and p95 were 13.44 and 20.19 ms
threaded against 13.48 and 20.50 ms inline; acquisition (`cpu.render_prepare`,
about 10 ms) dominates both, so the frame is bound by the GPU and presentation,
not by the frame-loop thread. Every frame of the threaded profile was
decoupled. The deterministic `bistro_shadow_motion_snapshot` captures stayed
byte-identical to the previous build in both modes; the texture-streaming
`bistro_metal_text_snapshot` captures, from a Metal desktop case since removed,
varied between runs of the previous build by up to 1.7 % of pixels, and both
modes stayed in that range.

The ThreadSanitizer runs exposed a pre-existing race: the resource system
freed request keys and unload names from its shared allocator after releasing
the request mutex. Those frees now happen under the mutex.

Native Vulkan behavior is unverified; only the Metal desktop implementation
ran.

## Alternatives considered

A runtime-owned thread that lends the renderer to the frame-loop thread in a
handoff window, with a renderer status snapshot, needs every caller audited
and kept correct by convention. Waiting inside the renderer keeps existing
callers correct and lets overlap grow as update-time renderer calls move.

Keeping acquisition on the frame-loop thread leaves its frame-slot wait on
the main thread. Moving the shadow decision into the `prepare` hook instead
lets every steady-state frame acquire on the render thread.

Coupling the frames that finalize or stream assets kept loading on the
frame-loop thread's acquisition; the publication queue replaced it. A
render-side asset lock was rejected: it would cover every asset-system access
on the frame-loop thread. Waiting for each publication's result inside its
call would keep synchronous creation unchanged but serialize loading with
rendering again.

## Revisit when

A workload shows the thread costing frame time or memory; a CPU-bound
workload is available to measure its benefit; or a native Vulkan run is
available, including texture admission against the Vulkan upload capacity.

## Code evidence

- [worker, waits, submit and complete](../../renderer/src/vkr_renderer.c)
- [publication commands, completions and snapshot](../../renderer/src/vkr_publication_queue.c)
- [publication contract and state](../../renderer/src/vkr_asset_publisher.h)
- [completion dispatch](../../runtime/src/renderer/systems/vkr_render_assets.c)
- [mesh confirmation](../../runtime/src/renderer/systems/vkr_mesh_manager.c)
- [texture request confirmation and retry](../../runtime/src/renderer/resources/loaders/texture_loader.c)
- [public API](../../renderer/src/vkr_renderer.h)
- [result and worker state](../../renderer/src/vkr_renderer_internal.h)
- [frame storage, completion and copies](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [window pixel size for the render surface](../../runtime/src/core/vkr_window.c)
- [Metal drawable acquisition](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)
- [harness child update without waits](../../tools/harness/vkr_harness_child.c)
- [thread stack size](../../lib/src/core/vkr_threads.h)
- [resource-system allocator frees](../../runtime/src/renderer/systems/vkr_resource_system.c)
