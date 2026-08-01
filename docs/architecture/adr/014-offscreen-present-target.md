---
status: proposed
updated: 2026-08-01
authority: adr
---
# ADR-014: Present Target Seam for Offscreen Rendering

**Status:** Proposed

## Context

The frame path is coupled to the swapchain at both ends.
`vkr_renderer_prepare_frame()` acquires a swapchain image and returns its index
in `VkrFrameSetup`; `vkr_renderer_end_frame()` submits and presents. The render
graph imports the per-image `swapchain` color and the currently shared
`swapchain_depth` handle as external resources, resolved by name during
compilation. Frame-stream indexing, sizing of synchronization objects, and
resize recovery are all expressed in swapchain terms.

That coupling is correct for an interactive application and is a hard blocker for
three things the automation harness needs
([tooling spec](../../tooling/renderer-harness-and-metrics-spec.md)):

- running where no display or window system is available;
- capturing a frame without a presentation round-trip, and without the present
  mode dictating the frame cadence.

Offscreen output also permits functional cases to run without window focus.
Authoritative performance cases still run one at a time per GPU: two offscreen
processes contending for the same device do not produce comparable evidence.

A window is also not free. It costs surface creation, swapchain creation,
window-system event pumping, and — under FIFO — a hard cap at the refresh rate
that makes every CPU-side improvement measure as zero.

The existing `scene_color` offscreen graph resource proves that ordinary passes
can render to a non-WSI image. It does **not** prove that the graph has no
swapchain assumptions: import resolution calls `vkr_renderer_window_*`, frame
setup reports `swapchain_*` formats, the backend always creates a surface before
device selection, and device scoring requires a present queue plus swapchain
support. The seam must start at backend configuration and device selection, not
only wrap the last two calls in the frame loop.

### What makes this non-trivial

The specification's §7.4 sizing table is deliberate and easy to break:

| Object | Count / index |
|---|---|
| Acquire semaphores | max frames in flight / current frame slot |
| Submit fences | max frames in flight / current frame slot |
| Render-complete semaphores | swapchain image count / acquired image |
| Image-in-flight fence references | swapchain image count / acquired image |

P0 items 1–2 fixed defects in exactly this invariant family: a double-advanced
frame slot pinned indexing at 0 on a two-image swapchain; fence references
survived destruction across recreation; swapchain-sized arrays used the
requested minimum rather than the actual image count; and failure/recreate
paths could continue with invalid WSI state. An abstraction that blurs "image
count" and "frames in flight" or weakens the acquire failure contract
reintroduces them.

## Decision

Introduce a target-neutral output configuration at the frontend/backend boundary
and a private `VulkanPresentTarget` deep module inside `renderer/vulkan/`. The
harness selects `WINDOWED` or `OFFSCREEN` at initialization; after creation the
frontend frame path queries only target-neutral extent, format, image count, and
attachment handles.

```c
typedef enum VkrPresentTargetKind {
  VKR_PRESENT_TARGET_WINDOWED = 0,
  VKR_PRESENT_TARGET_OFFSCREEN,
} VkrPresentTargetKind;

typedef struct VkrPresentTargetConfig {
  VkrPresentTargetKind kind;
  uint32_t width;       /**< Required for offscreen. */
  uint32_t height;      /**< Required for offscreen. */
  uint32_t image_count; /**< Offscreen request; clamped to a supported bound. */
} VkrPresentTargetConfig;
```

The private Vulkan target owns lifecycle, imported color/depth wrappers,
acquisition, terminal layout, and optional WSI completion. Its acquisition
record contains the facts the submit path actually needs: image index, optional
acquire-wait semaphore, optional render-complete semaphore, and whether
presentation is required. `VK_NULL_HANDLE` is valid for both semaphores on the
offscreen path. The target preserves `VULKAN_SWAPCHAIN_RESULT_OK`, `SKIP`, and
`FAILED`; a resize/recreate skip must not collapse into success or device loss.

Two implementations provide real variation for this seam:

**`swapchain`** — the current code, behaviourally unchanged. `acquire` wraps
`vulkan_swapchain_acquire_next_image()` and keeps returning the tri-state
`VulkanSwapchainResult` distinction between recreate-and-skip and device loss.
It owns the surface, required instance/device extensions, present-queue
requirement, acquire/render-complete semaphores, recreation, and
`PRESENT_SRC_KHR` terminal transition.

**`offscreen`** — owns N color/depth image pairs with no `VkSurfaceKHR`, present
queue, or swapchain requirement. `acquire` selects an image only after the
common `images_in_flight[image]` fence proves its last submit complete. Submit
uses the frame-slot fence but no WSI semaphores, and completion has no present
call. Each image retains an explicit layout/access state for the next graph
import. There is no resize event or `VK_ERROR_OUT_OF_DATE_KHR`; changing extent
is an explicit target recreation outside an active frame.

Capture is **not** a target operation. `Capture.Readback` declares its image
reads in the graph and records copies before target completion. Putting an
optional copy in `present()` would hide GPU work from the graph and recreate the
undeclared-access defect this architecture is intended to remove.

Constraints on the seam:

1. **Image count and frames-in-flight stay distinct concepts.** The interface
   reports `image_count` separately, and per-image arrays remain sized by it. The
   offscreen target chooses its own image count; it is not required to equal
   `max_in_flight_frames`.
2. **Imports become target-neutral.** Add role-and-index based
   `present_attachment_get`, `present_attachment_state_get`,
   `present_attachment_count`, `present_image_index`, `present_extent`, and
   present color/depth format queries to `VkrRendererBackendInterface`; migrate
   graph import resolution and frame setup to them. Existing `window_*` and
   `swapchain_*` frontend wrappers may remain temporarily as deprecated
   windowed-only compatibility calls, but the graph may not use them. The
   windowed depth role may return its existing shared handle for every valid
   image index; the offscreen role returns the corresponding per-image depth.
   Import initial access/layout comes from the state query, never a hard-coded
   `UNDEFINED` assumption.
3. **The graph topology is shared.** The JSON import names remain `swapchain`
   and `swapchain_depth` for compatibility, but their resolver is the selected
   target. The graph compiler also asks the target for output terminal
   access/layout: `PRESENT`/`PRESENT_SRC_KHR` for a windowed image and the
   offscreen target's retained state otherwise. The capture pass, when present,
   precedes that terminal edge. Completion does not inject a hidden barrier.
   Both implementations exercise the same pass declarations. A later schema
   revision may rename the imports; that cosmetic migration is not part of this
   decision.
4. **Selection happens once.** The application or harness chooses a target in
   configuration. Renderer orchestration does not branch on "headless" per
   frame; target-specific WSI behavior remains in the private deep module.
5. **Target choice reaches device creation.** Offscreen initialization must not
   create a surface, request `VK_KHR_swapchain`, query swapchain formats, or
   require a present queue. Windowed initialization retains all four. Because a
   logical target cannot exist before its instance/device, construction is
   staged: derive target requirements from configuration, create only the
   required instance/surface, filter/select the physical device and queues, then
   create the target attachments and synchronization after the logical device.
   This avoids a factory that needs the device it is supposed to help select.
6. **Requested and actual configuration are distinct.** Reports record target
   kind, actual image count, extent, color/depth formats and color space, plus
   actual present mode for windowed targets. A requested IMMEDIATE mode that
   falls back is not an authoritative IMMEDIATE run.
7. `VkrCamera` currently holds a `VkrWindow *` for input and aspect ratio.
   Camera projection receives explicit target extent/aspect, while interactive
   input remains an optional window concern. No camera used by the harness may
   read window size implicitly.

## Consequences

**Positive**

- Automation runs without a display, which is what makes CI and remote agent runs
  possible at all.
- Present mode stops being a confound. An offscreen target has no vsync, so
  CPU-side improvements are visible instead of being clamped to the refresh rate.
- Capture and presentation become independent declared operations; snapshots do
  not need a presentation round-trip.
- Faster boot for automation: no surface, no swapchain, no window-system event
  pump.
- Target-neutral graph imports remove swapchain/window naming from the active
  frame contract while preserving the existing JSON topology.

**Negative / risks**

- This is the frame path. The invariants at risk — frame-slot advancement,
  per-image versus per-frame object sizing, fence lifetime across recreate — are
  the same family P0 items 1–2 had to repair once already.
- Two acquire/present implementations mean two paths where there is currently
  one. Only the swapchain path is exercised by interactive use, so the offscreen
  path needs its own coverage or it will rot.
- An offscreen run is not a fully faithful reproduction of a presented run:
  no presentation engine back-pressure, and no swapchain image-count effects.
  Results from the two must not be compared as though they were the same
  configuration.
- [Upstream MoltenVK guidance](https://github.com/KhronosGroup/MoltenVK/issues/2049)
  supports rendering to ordinary images without creating a swapchain; the
  unverified part is VKR's surface-free instance/device selection, queue
  requirements, formats, validation behavior, and performance on the pinned
  MoltenVK development stack and on a native Vulkan target.
- One GPU still provides one authoritative performance lane. Offscreen output
  removes window contention, not GPU/driver scheduling contention.

## Alternatives Considered

- **Hidden or offscreen window.** Cheap, works today, requires no seam. It is
  adopted as the phase-3 fast-boot profile precisely because it is cheap. It does
  not remove the display requirement, so it cannot be the end state.
- **`VK_EXT_headless_surface`.** MoltenVK has exposed this optional extension
  [since 1.2.7](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/Whats_New.md#moltenvk-127),
  so it can keep a WSI-like path on the primary development stack. It still
  retains swapchain/present semantics the harness does not require and is not a
  portable substitute for regular-image rendering. It may be a platform adapter
  later; it is not the baseline selected here.
- **Render to `scene_color` and skip the world→swapchain composite.** Reuses
  existing offscreen machinery with no new seam, but still acquires and presents
  a swapchain image every frame, so it removes neither the display requirement
  nor the vsync confound.
- **A second, automation-only frame path.** Simplest to write and worthless as
  evidence: measurements from a path the application never runs do not describe
  the application.

## Revisit When

Promote to Accepted once the offscreen target passes
`tools/validate_multithreaded_backend_matrix.sh` and a validation-layer run, and
once an offscreen and a swapchain run of the same case are shown to produce
identical work-volume metrics and compatible captures after canonicalization.
Differing work volume means the seam changed what is rendered, not merely where
it lands. The gate must cover at least two offscreen image counts and the
windowed image count reported by the device.

Revisit the target operations if a third output consumer appears or if target
recreation needs to become asynchronous. Timeline semaphores may simplify
internal offscreen completion, but they do not replace the binary semaphores
required by ordinary WSI acquire/present and are not a reason to merge the two
lifetimes.
