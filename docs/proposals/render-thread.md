---
status: proposed
updated: 2026-10-03
authority: proposal
---
# Asynchronous asset publication for the render thread

## Current baseline

[ADR-082](../adr/082-renderer-owned-render-thread.md)'s render thread acquires
and renders steady-state frames while the frame-loop thread builds the next.
Frames with pending asset finalization or active texture streams are still
acquired on the frame-loop thread, because ADR-045 finalizes into an acquired
frame and Vulkan records uploads into it. Asset systems call
`VkrAssetPublisher` synchronously and use its results at once: geometry
creation returns a handle only after publication succeeded, and texture
finalization handles out-of-memory and busy results as retryable. The
render thread is off by default.

## Proposed change

Publication becomes an ordered command stream from the frame-loop thread to
the render thread:

- publication, unpublication, bake and sampler commands carry their own
  payloads and run in submission order on the render thread inside its next
  acquired frame, before that frame's input is prepared, so every earlier
  frame has rendered before an unpublication and every later frame sees a
  publication;
- results return as completions the frame-loop thread drains each frame and
  dispatches to the geometry, texture, material and mesh systems; a resource
  is used only after its first publication is confirmed, so backends keep
  rejecting absent handles, and a texture's out-of-memory result keeps its
  retry path;
- loader finalization enqueues and keeps the request pending until its
  completions arrive; payloads are released after the render thread consumed
  them;
- values asset code reads from the renderer each frame (publication
  generation, upload capacity, SH slots, bake status) come from the last
  completion instead of a waiting call.

Then no frame needs the frame-loop thread to acquire it, and the render
thread can be considered for the default.

## Decision boundaries

- Whether synchronous creation entry points (`vkr_geometry_system_create`,
  synchronous scene loads) keep their signatures and wait for confirmation,
  or report pending handles to their callers.
- Payload ownership: copying small payloads into command storage and handing
  large texture and mesh payloads over until completion.
- The render-thread default, once a CPU-bound workload shows a measured
  benefit. The harness child's per-update resource pump waits for the
  in-flight frame, so harness profiles cannot show the overlap until it pumps
  through the runtime.

## Evidence needed

1. Bistro snapshots unchanged inline and threaded, including loading and
   texture-streaming frames, and ThreadSanitizer threaded runs clean.
2. `frame.decoupled` at 1 during Bistro loading and streaming.
3. Matched Release measurements of a CPU-bound Bistro workload inline and
   threaded.
4. A native Windows/Vulkan run of the threaded path.

## Code baseline

- [renderer worker and publisher forwarding](../../renderer/src/vkr_renderer.c)
- [asset publisher contract](../../renderer/src/vkr_asset_publisher.h)
- [resource finalization](../../runtime/src/renderer/systems/vkr_resource_system.c)
- [runtime frame path](../../runtime/src/application/vkr_standard_scene_runtime.c)
