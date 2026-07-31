---
status: partial
updated: 2026-07-31
authority: adr
---
# ADR-009: Per-Image Present Semaphores and Bounded Frames in Flight

**Status:** Accepted (partial)

## Context

Swapchain acquire, graphics submission, presentation, frame-slot reuse, and
swapchain-image reuse are different synchronization domains. In particular, a
frame fence does not prove that presentation has finished waiting on a binary
render-complete semaphore. Reusing that semaphore by frame slot can re-signal it
while presentation still owns the prior signal.

## Decision

Size synchronization objects by the lifetime they protect:

| Object | Count | Index |
|---|---|---|
| `image_available_semaphores` | max frames in flight | current frame slot |
| `in_flight_fences` | max frames in flight | current frame slot |
| `queue_complete_semaphores` | swapchain image count | acquired image |
| `images_in_flight` fence references | swapchain image count | acquired image |

`max_in_flight_frames = Min(swapchain.image_count, BUFFERING_FRAMES)`, with
`BUFFERING_FRAMES = 3`.

The submit path waits for prior use of the acquired image, associates it with
the current frame fence, submits while waiting on the frame-slot acquire
semaphore, signals the acquired-image render-complete semaphore, records a
monotonic submit serial, advances the frame slot, and presents waiting on that
image's semaphore.

This per-image render-complete policy is retained because it addresses binary
semaphore ownership by presentation. Resize recreates arrays for the new image
count and clamps the current frame slot.

### Incomplete surrounding invariants

The WSI sizing decision is sound, but the complete frame synchronization/error
contract is not yet correct:

- the three-entry instance and indirect stream rings are indexed by acquired
  image modulo three, so a four-image swapchain can alias stream storage for two
  images still in flight;
- several fence-wait, acquire, command-buffer, submit, and present failures log
  an error but return `VKR_RENDERER_ERROR_NONE`;
- some early returns can leave frontend/backend frame-active state difficult to
  reason about;
- deferred readback/upload paths include infinite fence waits;
- rejected packets end/present an acquired frame even though no valid graph pass
  may have established the assumed swapchain layout.

## Consequences

**Positive**

- Present wait semaphores are not prematurely reused by a different image.
- Bounded frames in flight cap latency and per-frame resource count.
- Per-image fence references prevent recording/submitting over an image still in
  use.
- Submit serials provide a useful retirement timebase.

**Negative / risks**

- Similar-looking objects intentionally use different counts and indices.
- Binary semaphore/fence bookkeeping is more complex than a timeline-based
  internal dependency model.
- Fixed triple buffering is not configurable.
- Other per-frame resources must use the same frame-slot/image lifetime model;
  current stream buffers do not.
- Error swallowing can report a frame as successful when synchronization,
  submission, or presentation failed.

## Alternatives Considered

- **Index both semaphore classes by frame slot.** Rejected because presentation
  can still own a render-complete semaphore after the frame fence signals.
- **Timeline semaphores for internal work.** Useful for uploads/compute, but WSI
  acquire/present still uses binary semaphores. Complementary, not a direct
  replacement.
- **Per-image sizing for every frame resource.** Correct if all such resources
  are recreated with the swapchain; potentially more memory than frame-slot
  sizing.
- **Frame-slot sizing for streams.** Preferred for mapped stream buffers if the
  backend exposes the actual slot used by the submission fence.

## Revisit When

- Fix all frame-resource index domains and add tests/validation runs with
  two-, three-, and four-image swapchains.
- Propagate every Vulkan frame failure and document state after each failure.
- Add async compute/upload dependencies, where timeline semaphores become more
  valuable.
- Add explicit frame pacing or present-wait support.
