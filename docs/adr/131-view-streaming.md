---
status: partial
updated: 2026-10-22
authority: adr
---

# ADR-131: View streaming between editors

## Status

Accepted (partial). The first slice of phase 7 of the
[network protocol proposal](../proposals/network-protocol.md#view-streaming-use-case-4a)
is implemented on macOS: one editor serves its Scene image, encoded with
VideoToolbox H.264, to one other editor over the transport's view service;
the client shows the decoded image in its Scene panel instead of rendering
a Scene, and its Scene camera steers the server's. The pose-to-display
latency was measured between two editors on one Mac over loopback. A run
between two computers, the Windows encoder, forward error correction,
intra refresh, raw input forwarding and more than one client stay in the
proposal.

## Context

Use case 4a of the proposal: a server renders a view and streams encoded
frames to a client, which sends input back, so a Mac editor can show the
desktop pipeline rendered on a Windows machine. The transport already had
the delivery classes the proposal asks for, including `DEADLINE`, and
reserved service ID 5 (`VKR_NET_SERVICE_VIEW`); it has no FEC. No code
touched VideoToolbox or IOSurface.

Owner decisions of 2026-10-22: a Mac-to-Mac first slice over loopback with
this ADR and a glass-to-glass latency measurement, Windows encode later; a
lib video module wraps the platform codec over IOSurface frames, and the
renderer only copies each finished Scene image into a surface and can draw
a decoded surface in place of the Scene; the client's camera pose steers
the server's view (raw input later); the server streams its own Scene
image and the client scales it.

## Decision

1. **Codec in lib.** [`vkr_video.h`](../../lib/src/platform/vkr_video.h)
   owns hardware video coding (ADR-118: platform services belong to
   `lib/`). A `VkrVideoSurface` is a reference-counted BGRA8 image the GPU
   and the codecs share: on macOS an IOSurface-backed, Metal-compatible
   `CVPixelBuffer` tagged BT.709. The encoder
   ([`vkr_video_macos.c`](../../lib/src/platform/vkr_video_macos.c)) is a
   hardware H.264 `VTCompressionSession` in real-time, low-latency rate
   control, Constrained High profile, without frame reordering, with a key
   frame only on request; each frame comes back on the encoder's thread as
   an Annex B stream whose key frames carry their parameter sets, with the
   caller's tag. The decoder is synchronous on the caller's thread, refuses
   frames before the first key frame and recreates its session when the
   parameter sets change. On other platforms every call reports the service
   unavailable ([`vkr_video_windows.c`](../../lib/src/platform/vkr_video_windows.c)).
2. **Renderer export and import.** `VkrEditorPassPayload` carries an
   optional `VkrViewExport` (surface, tag, `done` callback) and an optional
   `view_import` surface ([`vkr_frame_input.h`](../../renderer/src/vkr_frame_input.h)).
   `vkr_renderer_view_streaming_supported()` is true on Metal only; render
   validation refuses either field elsewhere, and an import with the
   presenter (ADR-111). Once a submit call accepts a frame, the renderer
   calls `done` exactly once: with `written` true after the GPU finished the
   export's copy (an `MTLSharedEvent` listener), or false when the frame
   made none — a present-only frame, a frame refused at validation or
   acquisition, a backend failure (the next frame or teardown reports it).
   The Metal backend ([`vkr_metal_packet_view.inc`](../../renderer/src/metal/internal/vkr_metal_packet_view.inc))
   wraps each surface's IOSurface as an untracked shared texture once and
   keeps up to 16 wrappings in a residency set, evicting the least recently
   used one the GPU finished. The export is one compute dispatch,
   `vkr_metal_packet_view_export` ([`view_export.metal`](../../renderer/src/shaders/metal/msl/editor/view_export.metal)),
   recorded in the frame's readback encoder after the barrier that waits for
   the Scene image's last writer: it resamples `editor_scene_image`
   bilinearly at each surface pixel's centre and stores sRGB-encoded BGRA8,
   because a headless Scene image is RGBA8 sRGB and an EDR one RGBA16F, so
   a blit cannot copy them into a BGRA surface. The import wraps as
   BGRA8Unorm_sRGB and the UI draws it through
   `VKR_UI_TEXTURE_REF_REMOTE_VIEW`.
3. **Runtime plumbing.** The standard runtime keeps the export waiting for
   the next editor Scene frame and the current import with a reference of
   its own (`vkr_standard_scene_runtime_set_view_export`/`_import`). A
   Scene frame takes the export last before its frame input is built, after
   every cancel path, and each runtime frame holds a reference to the import
   it draws until it is built again. A replaced or refused export is
   reported unwritten. While an import is set the frame renders no Scene of
   its own and the presenter stays off. The editor build asks through
   `VkrSampleUiFrame.view_stream_request`.
4. **Editor remote view and the view service.**
   [`editor_remote_view.c`](../../editor/src/editor_remote_view.c) serves
   or shows a Scene over service 5 on a host thread of its own, with
   three channels:

   | Channel | Delivery | Carries |
   |---|---|---|
   | 0 frames | `DEADLINE`, 33 ms (two frame intervals), 8 MiB messages | 4-bit type, key flag, width, height, frame number, the client pose's send time, the server's render and encode microseconds, then the Annex B frame |
   | 1 pose | `SEQUENCED`, `IMMEDIATE` | the client Scene camera's position, yaw, pitch and send time |
   | 2 control | reliable ordered | `REFRESH`: the next frame is a key frame |

   The server hands one of four surfaces sized to its Scene output (even
   extents) to each build while a client is open, applies the client's
   newest pose through `move_camera` in the same build, encodes a surface
   on the renderer's report and sends the frame from the encoder's
   callback. Frames are numbered only when they leave, so a gap means a
   frame the transport dropped at its deadline. The bitrate is 0.85 × the
   path's bandwidth estimate every 100 ms, between 2 Mbit/s and a cap
   (default 40 Mbit/s). A client open or `REFRESH` makes the next frame a
   key frame. The client queues up to 16 frames for a decoder thread,
   decodes in frame order, and on a gap, a late frame or a refused frame
   drops frames until the key frame it asks for; each build it shows the
   newest decoded frame and sends its pose. A server allows identity keys
   or, with none, anyone given its key; it takes one client. Agents drive
   it with `remote_view.serve`, `.connect`, `.stop` and `.status`; a
   client with no scene of its own can steer (`view.camera`) and capture
   its Scene panel (`view.capture`).
5. **Latency on the client's clock.** The server returns, with each frame,
   the send time of the client pose that frame shows. The client's sample
   is the time from that send to the build that hands the frame to its
   renderer, so both ends of it are on one clock on any network. The
   client's own frame render and present come on top.

## Consequences

- A thin client needs no project or scene: it decodes about 2 ms per
  1274×700 frame and renders only its UI.
- The renderer contract grows by one capability, two payload fields and a
  callback that may run on the render thread or the completion queue; the
  editor's ring is heap memory freed by its last reference, so reports that
  arrive after the view stopped are safe.
- One extra compute dispatch and one listener notification run per Scene
  frame only while a client views; frames without an export or import are
  unchanged.
- Frames in flight on the server are bounded by its four surfaces; the
  client holds at most 16 queued frames and the renderer at most 16 wrapped
  surfaces.
- The stream is lossy H.264: on Bistro's plaza the client's image is
  34.4 dB PSNR from the server's own capture, 43 dB on a plain wall.
- `DEADLINE` delivery is unordered; a frame that overtakes its predecessor
  costs a key frame. No run has seen one.
- A Windows editor reports view streaming unavailable until its encoder
  exists.

## Alternatives considered

- **A blit copy into the surface.** Exact and cheaper, but only for a BGRA8
  Scene image; headless (RGBA8 sRGB) and EDR (RGBA16F) Scene images need
  the conversion the compute pass does.
- **Encoding in the renderer.** The renderer would own a codec session and
  its thread; the lib module keeps platform services in `lib/` and leaves
  the renderer two plain fields.
- **Sending each frame from the editor's build.** Simpler threading, but a
  frame would wait up to one build interval after its GPU copy finished;
  the encoder's callback sends it at once.
- **Raw input forwarding first.** The server would need a second,
  synthetic input path; the camera pose reuses `move_camera` and already
  steers the view.
- **WebRTC.** Rejected in the proposal: SDP negotiation and a large
  dependency.

## Evidence

Release build on the M1 Pro (16 GB, macOS 26), both editors headless on
the one machine over loopback, the server on Bistro with the tiled
pipeline at a fixed scale (`gfx.dynamic = false`, light icons off):

```sh
python3 tools/checks/check_view_stream.py --editor build_release/editor/vkr_editor
```

| Run | Frames sent | Key frames | Latency p50 / p95 | Server render / encode p50 | Client decode / arrival-to-display p50 | Client vs server PSNR |
|---|---|---|---|---|---|---|
| Plaza view (eye 10, 4.2, 27.8) | 2230 | 1 | 43.3 / 44.6 ms | 30.0 / 8.6 ms | 2.2 / 4.0 ms | 34.4 dB, mean 2.72 of 255 |
| Wall view (eye −24, 5, −14) | 2607 | 2 | 29.4 / 30.9 ms | 17.2 / 8.1 ms | 2.1 / 2.4 ms | 43.1 dB, mean 1.23 of 255 |

The stream was 1274×700 at the 40 Mbit/s cap (the loopback estimate is far
above it). Latency covers the last 256 frames shown, while the camera held
still; poses stream every client build either way. "Server render" is the
time from the export's hand-out in a build to the GPU finishing its copy,
which includes the server's frame pipeline. Earlier runs each saw one gap
with one refresh, which cost about 19 frames once.

`--metal-validation server` and `--metal-validation client` each passed
with Metal API validation on that side and no report; MetalFX's temporal
scaler aborts under the debug layer, so those runs use the spatial
upscaler. `vulkan_renderer_tester --suite video` encodes 30 frames of
640×360 at 4 Mbit/s and decodes them, joining late at a key frame; its
worst PSNR is 41.1 dB.

Not measured: a run between two computers, 2560×1440 (the proposal's
target size), an EDR or windowed BGRA Scene source, and the client's own
present. The Vulkan backend reports the capability false, so it runs no
part of this.

## Revisit when

- The Windows encoder lands (Vulkan Video or Media Foundation), or a
  Windows server streams the desktop pipeline to a Mac.
- A run between two computers shows loss: FEC, intra refresh and a short
  reorder window become worth their cost.
- Raw input forwarding or a second client is needed.
