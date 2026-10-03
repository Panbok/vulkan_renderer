---
status: proposed
updated: 2026-10-03
authority: proposal
---
# Render thread default

## Current baseline

[ADR-082](../adr/082-renderer-owned-render-thread.md)'s render thread acquires
and renders every steady-state and loading frame while the frame-loop thread
builds the next. Asset publications travel with the frame as ordered commands,
and asset systems use a resource once its completion confirms it. Only frames
that resize or change the Scene output are acquired on the frame-loop thread.
The render thread is off by default.

## Proposed change

- Move the renderer calls that update code still makes into cached values or
  commands, starting with the harness child's per-update resource pump, which
  reads the renderer's submit serials and so waits for the in-flight frame.
  Editor and sample code that queries the renderer during update is audited
  the same way.
- Hand loaded texture payloads to the queue instead of copying them: the
  request already keeps its payload until confirmation, so the copy only adds
  the second batch's memory (peak loading memory 3.9 GiB threaded against
  3.3 GiB inline in Bistro).
- Enable the render thread by default once a CPU-bound workload shows a
  measured benefit.

## Decision boundaries

- The CPU-bound workload and acceptance threshold that decide the default.
- Whether borrowed texture payloads justify a second recording mode in the
  publisher contract.

## Evidence needed

1. Matched Release measurements of a CPU-bound Bistro workload inline and
   threaded.
2. Harness profiles that show the overlap once update no longer waits.
3. A native Windows/Vulkan run of the threaded path, including texture
   admission against the Vulkan upload capacity.

## Code baseline

- [renderer worker and waits](../../renderer/src/vkr_renderer.c)
- [publication queue](../../renderer/src/vkr_publication_queue.c)
- [harness child update](../../tools/harness/vkr_harness_child.c)
- [runtime frame path](../../runtime/src/application/vkr_standard_scene_runtime.c)
