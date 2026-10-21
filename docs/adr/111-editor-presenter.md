---
status: partial
updated: 2026-10-22
authority: adr
---
# ADR-111: Editor presenter

## Status

Accepted (partial). Implemented on Metal, where the editor uses it by default;
`VKR_EDITOR_PRESENTER=0` turns it off. Vulkan has no presenter, so its editor
presents once per Scene frame, as before.

## Context

The editor drew the Scene, the editor composite and the UI into one command
buffer on one queue, and presented that command buffer's drawable. The UI could
therefore present only as fast as the Scene finished on the GPU. On the M1 Pro's
120 Hz display, a Bistro view that takes 10 ms of GPU per Scene frame put the UI
at about 100 Hz, and a heavier view (Epic preset, fixed scale) put it at 63 Hz.
The render thread ([ADR-082](082-renderer-owned-render-thread.md)) overlaps
frame building with recording, but it cannot hide a Scene that takes longer than
the display interval.

A probe on the M1 Pro showed that a small command buffer on a second Metal queue
runs between the passes of a long command buffer on the first queue. With 20 to
40 ms of work split into passes on queue A, a small render on queue B completed
4 to 5 ms after commit on average (9 to 12 ms at worst), against 0.9 ms with the
GPU idle. On the same queue it waited for all of queue A's work.

## Decision

The Metal renderer owns a presenter: a second `MTL4CommandQueue` with its own
completion event, three command slots with shared upload buffers, and three
Scene images in their own residency set. A presenter frame clears the drawable,
composites a Scene image into the editor's Scene rectangle with the editor
composite's pipeline and flags, draws the UI, and presents.

The editor payload selects it per frame (`VkrEditorPassPayload.presenter`):

- A Scene frame records its graph on the frame queue as usual, except that
  `Editor.Composite`, `Editor.Clear` and `UI.Editor` are off
  (`editor_presenter`). The new `Editor.Present` pass (no-cull) copies
  `editor_scene_image` into a presenter image. The drawable is taken only after
  the Scene is committed, so the Scene never waits for the display. A
  presenter frame then presents.
- A presentation-only frame (`present_only`, acquired with
  `VkrFrameConfig.present_only`) has no Scene, graph or histories. It takes
  only the drawable and runs a presenter frame.

A presenter frame shows the newest Scene image the frame queue has finished. A
Scene frame with `present_wait_scene` shows its own Scene instead, and the
presenter queue waits for it on the GPU. A presenter frame never shows a Scene
older than an earlier one showed. An image is rewritten only once a newer image
is finished or shown, and the copy waits on the GPU for presenter frames still
sampling it.

Resource lifetime stays on the frame queue's values. A presenter frame tags the
textures its UI samples with the frame-queue value after its mark, and frame-
queue completion (`vkr_metal_packet_completed`) stays at that mark until the
presenter frame finishes; `vkr_metal_packet_wait` waits for such presenter
frames too. A presenter frame waits on the GPU for uploads of the textures it
samples that the frame queue has not finished.

The standard scene runtime chooses each editor frame
(`vkr_standard_scene_runtime_choose_presentation`). It counts editor Scene
frames the GPU finished (`vkr_renderer_scene_frames_completed`, lock-free),
smooths their GPU time from their completion times, and renders a Scene when the
GPU should be ready for it at this frame's commit, about one display interval
after it is built: at most one Scene may still be running then, and the queue
must free within one more interval. Other frames present only. A Scene expected
to finish within three quarters of an interval sets `present_wait_scene`, so
cheap Scenes appear in their own frame as before. The display interval comes from
`vkr_window_refresh_rate`.

A capture whose channels all read images the Scene renders, such as the
`editor_scene` thumbnails a recording takes twice a second, stays on the
presenter: presentation-only frames leave it pending, and the next Scene frame
the presenter renders carries it. Leaving the presenter for it rendered two
Scenes back to back, which stalled the editor for about 25 ms on each
thumbnail. Final-color captures, the animation preview, Render Stop, the
backdrop blur and a remote view ([ADR-131](131-view-streaming.md), whose
image the frame's own UI pass draws) keep one
presentation per frame, through the frame's own graph. A Scene presented that
way makes the presenter's images stale, so the next presenter frame renders a
Scene instead of only presenting: presenting with no image left the Scene
empty for a frame, which flickered twice a second while a recording captured
its thumbnails (`presenter_images_stale`). A frame that presents itself after presenter frames waits for them on
the GPU, so drawables present in order.

The HUD's FPS reports the Scene's rate while presentation-only frames run.
`stats.scene_frames` and `stats.present_frames` count both kinds of frame
([ADR-075](075-editor-cmd-bar-and-evaluator.md)).

## Consequences

UI interactions, hover and press animations and the Cmd bar present at the
display's rate while the Scene renders at its own rate. The Scene image can lag
the UI by up to one Scene frame. Gizmos, selection outlines and picking stay in
the Scene and follow its rate.

Measurements (M1 Pro, Release, macOS 26, built-in 120 Hz display, windowed
editor at 2560×1600 pixels, Bistro, `view.camera` glide, 4 s window after 14 s,
runs alternated with `VKR_EDITOR_PRESENTER=0|1` in one build; the screen was
locked, so frames were paced but not shown):

| Setting | Path | UI interval median / p95 / max | Scene frames/s |
|---|---|---|---|
| Default | presenter | 8.22–8.31 / 9.50–9.67 / 10.1–10.3 ms | 63.0–63.5 |
| Default | one per frame | 15.70–15.78 / 16.9–17.2 / 17.3–20.4 ms | 64.3 |
| Epic, fixed scale | presenter | 8.24–8.32 / 9.77–9.78 / 10.3–10.4 ms | 63.3–63.5 |
| Epic, fixed scale | one per frame | 15.75–15.80 / 16.7–17.0 / 17.9–18.0 ms | 63.8–64.3 |

The UI interval is the editor's `stats.frame_ms` (time between UI builds). The
Scene loses about 1.5 % of its rate to the copy and the presenter's GPU work.
With a static camera and a 9.7 ms Scene, the median UI interval went from 9.7 to
8.3 ms while the Scene kept about 105 frames/s.

Drawables read back from both paths at the same frame number differed in 2.9 %
of pixels, almost all by one level, in the cloud sky and on surfaces the clouds
light. Two frames of the old path captured 3.5 s apart differed in 2.5 % of
pixels in the same places; the UI and the static Scene matched.

A presenter frame is recorded on the render thread after its job's Scene, so a
Scene frame's presentation waits for that Scene's CPU recording, about 1 ms in
Bistro. Each Scene frame registers one completion block with the frame queue's
event.

Native Vulkan is unchanged and unverified. On-screen presentation timing
(`presentedTime`) could not be measured with the screen locked.

## Alternatives considered

Presenting the UI on the frame queue between Scene frames: the queue runs a
command buffer only after earlier ones finish, so a UI frame still waits for the
whole Scene.

Moving the Scene to its own CPU thread: the Scene's frame-loop cost was not the
bound; its GPU time was, and the renderer serves one user at a time (ADR-082).

Double-buffering `editor_scene_image` in the graph instead of copying: the graph
image is retained for Render Stop and read by later passes of the same frame;
the copy costs about 0.1 ms of GPU and keeps the graph unchanged.

Waiting on the CPU for each Scene before presenting: it couples presentation to
the Scene again.

## Revisit when

A Vulkan editor needs a presenter (a second graphics queue with a timeline
semaphore), on-screen timing is available, a Scene's CPU recording grows past a
display interval, or the copy shows up in a GPU budget.

## Code evidence

- [presenter](../../renderer/src/metal/internal/vkr_metal_packet_present.inc)
- [presenter state, completion helper](../../renderer/src/metal/vkr_metal_packet_renderer.m)
- [presenter-aware waits and use rollback](../../renderer/src/metal/internal/vkr_metal_packet_commands.inc)
- [deferred drawable and commit order](../../renderer/src/metal/internal/vkr_metal_packet_frame.inc)
- [graph passes](../../assets/render_graphs/tiled.rendergraph.json)
- [payload fields](../../renderer/src/vkr_frame_input.h)
- [acquisition and presentation-only frames](../../renderer/src/vkr_renderer.c)
- [frame choice](../../runtime/src/application/vkr_standard_scene_runtime.c)
- [refresh rate](../../runtime/src/platform/vkr_window_macos.m)
