---
status: partial
updated: 2026-10-23
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
latency was measured between two editors on one Mac over loopback. A
Windows editor serves too: its Vulkan backend encodes with Vulkan Video
H.264 (decision 6), and its own recording decodes on Windows; a Windows
editor shows no remote view. On 2026-10-23 a Windows server streamed
Bistro to a Mac client between two computers over the Mac's Wi-Fi. That
run lost frames at the sender's deadline, which led to the sender rules in
decision 4; those rules have not run between two computers yet. 2560×1440,
forward error correction, intra refresh, raw input forwarding and more
than one client stay in the proposal.

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
image and the client scales it. Owner decision of 2026-10-23: Windows
encodes with Vulkan Video, as the network proposal recommends, rather
than Media Foundation; Windows serves and the Mac shows, because the Mac's
endpoint security drops unsolicited inbound traffic.

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
   unavailable ([`vkr_video_windows.c`](../../lib/src/platform/vkr_video_windows.c)):
   a Windows server's codec is the renderer's own (decision 6), because
   Vulkan Video needs the renderer's device, queues and timeline.
2. **Renderer export and import.** `VkrEditorPassPayload` carries an
   optional `VkrViewExport` (surface, tag, `done` callback) and an optional
   `view_import` surface ([`vkr_frame_input.h`](../../renderer/src/vkr_frame_input.h)).
   `vkr_renderer_view_streaming_supported()` is true on Metal and on a
   Vulkan device that encodes H.264; `vkr_renderer_view_encoding_supported()`
   is true on the latter, whose exports carry an encode request in place of
   a surface (`width`, `height`, `bitrate_bps`, `frame_rate`, `key_frame`,
   `on_packet`). Render validation refuses an export without view
   streaming, an encode request without both callbacks, an even extent of
   at least 16, a bitrate and a frame rate, and an import with the
   presenter (ADR-111) or on an encoding renderer. Once a submit call
   accepts a frame, the renderer calls `done` exactly once: with `written`
   true after the GPU finished the export's copy (an `MTLSharedEvent`
   listener) or, encoding, after `on_packet` took the frame's packet, or
   false when the frame made none — a present-only frame, a frame refused
   at validation or acquisition, a backend failure (the next frame or
   teardown reports it).
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
   reported unwritten; an export is one with a `done` callback, so an
   encode request without a surface counts. While an import is set the
   frame renders no Scene of its own and the presenter stays off. The
   editor build asks through `VkrSampleUiFrame.view_stream_request`, and
   `view_encoding` tells it the renderer encodes.
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
   frame the transport dropped at its deadline. The sender's rules sit
   beside the client's frame order in
   [`editor_remote_order.c`](../../editor/src/editor_remote_order.c), with
   no clock or I/O, so `vkr_net_bench sim --mode video` runs them over the
   simulator. A frame's deadline is twice the time its bytes and the bytes
   queued ahead of it on the channel take at the stream's bitrate, at
   least two frame intervals and at most 1 s: a key frame on a slow link
   still arrives, and the frames queued behind it do not expire, each gap
   asking for another key frame. The server exports no frame while the
   channel's unsent bytes (`vkr_net_session_unsent_bytes`, without those in
   flight) would take more than one frame interval at the bitrate; a
   skipped frame is never encoded, so the numbers stay contiguous and the
   encoder references only frames that leave. The bitrate is 0.85 × the
   path's bandwidth estimate every 100 ms, between 2 Mbit/s and a cap
   (default 40 Mbit/s). The encoder is asked for that bitrate over its
   overshoot: a moving average, over about fifty frames, of each sent
   frame's size against its share of the asked rate, from 1 to 4. Key
   frames count, so the frames after one pay for it. A server's
   `remote_view.status` reports `frames_skipped`, `overshoot`,
   `encode_bitrate` and, while a client is connected, `link`: round trip,
   bandwidth estimate, packets sent and lost, and frames that expired
   before they left or while waiting to resend; a client reports its own
   `link`. A client open or `REFRESH` makes the next frame a key frame. The client's frame order
   ([`editor_remote_order.c`](../../editor/src/editor_remote_order.c))
   hands frames to a decoder thread in stream order. A frame that overtakes
   its predecessor waits up to 33 ms for it; a gap that window does not
   close, or a frame the decoder refuses, drops frames until a key frame,
   which the client asks for at once and again every 500 ms until one
   arrives. The order holds up to 64 frames, one second, which covers the
   decoder's start on the first key frame. Each build the client shows the
   newest decoded frame and sends its pose. A server allows identity keys
   or, with none, anyone given its key; it takes one client. Agents drive
   it with `remote_view.serve`, `.connect`, `.stop` and `.status`; a
   client with no scene of its own can steer (`view.camera`) and capture
   its Scene panel (`view.capture`). On an encoding renderer the server's
   ring holds no surfaces or encoder: each of its four slots is an encode
   request that asks for the key frame the view wants, the renderer's
   packet sends under the ring's lock (so no send outlives a stop), and the
   slot frees on the report, which asks for the key frame again when the
   request made none. The frame message and the client are the same on
   both servers. A server needs `vkr_video.h` encoding or an encoding
   renderer; a client needs `vkr_video.h` decoding and a renderer that
   takes imports. `remote_view.serve` with `"record":"<name>"` also writes
   every encoded frame to `<agent directory>/view/<name>.h264` and encodes
   with no client, so a computer that cannot be a client checks its own
   stream.
5. **Latency on the client's clock.** The server returns, with each frame,
   the send time of the client pose that frame shows. The client's sample
   is the time from that send to the build that hands the frame to its
   renderer, so both ends of it are on one clock on any network. The
   client's own frame render and present come on top.
6. **Vulkan Video encoder in the desktop backend.** Device selection
   ([`vkr_vulkan_device.c`](../../renderer/src/vulkan/vkr_vulkan_device.c))
   enables `VK_KHR_video_queue`, `VK_KHR_video_encode_queue` and
   `VK_KHR_video_encode_h264` with one queue of the first other family that
   encodes H.264, when the device has them; the capability report names
   it as optional. [`vkr_vulkan_view.c`](../../renderer/src/vulkan/vkr_vulkan_view.c)
   checks the High-profile encode capabilities, NV12 pictures the export
   can write through plane views and NV12 reference pictures, and reports
   encoding supported. The first export of an extent and frame rate makes
   a session on the rendering thread: a video session with its memory,
   one SPS and one PPS, a two-layer DPB, four slots (an NV12 picture both
   queue families share, its luma and chroma storage views, a mapped
   bitstream buffer, a feedback query, an encode command buffer), a
   timeline and an encoder thread. Video images and buffers take memory
   outside the renderer's pools, because each chains the session's profile
   list. A session with no export for 2 s, or of another extent, is
   destroyed once its slots came back.
   After the graph, one dispatch of `vk_view_export`
   ([`view_export.slang`](../../renderer/src/shaders/vulkan/slang/editor/view_export.slang))
   resamples `editor_scene_image` as the Metal kernel does and converts to
   BT.709 video-range 4:2:0, one thread per 2×2 block, out to the picture's
   16-pixel coded extent by repeating the last row and column. After the
   frame's submission the slot's encode goes to the encode queue, waiting
   for the frame's timeline value at the encode stage; it signals the
   session's timeline. The encoder thread waits for both values in
   submission order, reads the feedback (offset and bytes) and calls
   `on_packet` with the Annex B slice, after the encoded SPS and PPS on a
   key frame, then `done`. The stream is Constrained High (High with
   `constraint_set4/5`), 4:2:0 8-bit, CABAC with the 8×8 transform, POC
   type 2, one reference frame: each P picture references the one before
   it and an IDR picture comes at a session's start, on request or after a
   failed submission. Rate control is CBR (VBR or the default where CBR is
   absent) with one frame interval of buffering, the device's whole QP
   range and ultra-low-latency tuning; the bitrate follows each request.
   Without an explicit QP range the AMD driver encodes at one QP whatever
   the bitrate. The VUI names BT.709
   primaries, transfer and matrix, video range, and no reordering
   (`max_num_reorder_frames = 0`). Every slot busy drops the frame's
   picture; a failed encode reports its export unwritten.

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
  client holds at most 64 frames and the renderer at most 16 wrapped
  surfaces.
- The stream is lossy H.264: on Bistro's plaza the client's image is
  34.4 dB PSNR from the server's own capture, 43 dB on a plain wall.
- `DEADLINE` delivery is unordered; a frame that overtakes its predecessor
  waits up to 33 ms, and only a frame lost past that window costs a key
  frame. Neither the loopback runs nor the Windows-to-Mac run saw a frame
  overtake another.
- A link narrower than the stream skips frames at the server instead of
  losing them in flight: the client sees fewer frames and asks for no key
  frame. A frame behind a key frame arrives late instead of expiring.
- An encoder that runs over its rate is asked for less, so its frames are
  smaller than the rate allows; an encoder that stays under it is asked
  for the full rate.
- A Windows editor serves but cannot show a remote view: `vkr_video.h` has
  no Windows decoder and the Vulkan backend takes no import.
- A Vulkan device with video encode creates one more queue and enables
  three more extensions whether or not anything streams; the session's
  memory (four NV12 pictures, a two-layer DPB and four bitstream buffers,
  about 13 MB at 1280×704, plus the driver's session memory) exists only while
  exports arrive, outside the renderer's pools and their accounting.
- The Vulkan export costs one compute dispatch and one encode submission
  per Scene frame, only while a client views or a recording runs.

## Alternatives considered

- **A blit copy into the surface.** Exact and cheaper, but only for a BGRA8
  Scene image; headless (RGBA8 sRGB) and EDR (RGBA16F) Scene images need
  the conversion the compute pass does.
- **Encoding in the renderer.** The renderer would own a codec session and
  its thread; the lib module keeps platform services in `lib/` and leaves
  the renderer two plain fields. Chosen for Vulkan Video only (decision 6):
  a Vulkan encode session needs the renderer's device, queues and
  timeline, which `lib/` cannot reach.
- **Media Foundation on Windows.** Mature hardware encoders through one
  OS API, kept in `lib/` as on macOS, but the Vulkan Scene image would
  cross to Direct3D 11 through a shared texture and keyed-mutex or fence
  interop, or a CPU copy. Vulkan Video keeps the picture on the device and
  is one API across vendors; the owner chose it.
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
which includes the server's frame pipeline.

Before the frame order, runs saw one gap as the stream started: the
decoder's first key frame took long enough to overflow a 16-frame queue,
which cost about 19 frames and a key frame. With the order, a run on
2026-10-23 had no gap and dropped no frame (2127 frames sent, latency p50
43.7 / p95 44.9 ms, 34.3 dB). The client still asks once, 500 ms after the
stream opens, because the server's first frame takes that long while its
encoder starts; the request merges with the open's key frame.
`vulkan_renderer_tester --suite remote_order` checks the order: a frame
that overtakes another inside the window decodes in order with no key
frame asked, a gap past it asks one at once, a lost key frame is asked
again every 500 ms, and late, duplicate and refused frames drop.

The split check ran over loopback on 2026-10-23. A server with `--allow`
the Mac's identity key and a client with `--identity` that key passed:
2201 frames, latency p50 43.7 / p95 44.9 ms. A client with a fresh key
stopped with "The server does not allow this editor's key.":

```sh
python3 tools/checks/check_view_stream.py --editor <editor> --role server \
    --bind 0.0.0.0:7340 --allow <client identity key>
python3 tools/checks/check_view_stream.py --editor <editor> --role client \
    --address <server>:7340 --key <server key> --identity <identity file>
```

`--metal-validation server` and `--metal-validation client` each passed
with Metal API validation on that side and no report; MetalFX's temporal
scaler aborts under the debug layer, so those runs use the spatial
upscaler. `vulkan_renderer_tester --suite video` encodes 30 frames of
640×360 at 4 Mbit/s and decodes them, joining late at a key frame; its
worst PSNR is 41.1 dB.

Windows Release build on the RX 6700 XT (Adrenalin 26.10.1), 2026-10-23:
one headless server editor on Bistro with the desktop pipeline at the
plaza view (`gfx.dynamic = false`, light icons off) records its stream
with no client, and PyAV decodes it on the same machine:

```sh
python tools/checks/check_view_stream.py --role record \
    --editor build_release/editor/vkr_editor.exe
```

It recorded 382 frames of 1274×700 (coded 1280×704) at the 20 Mbit/s
starting rate, 4.1 MB: 10.7 KB a frame on the still view, under the
41.7 KB that 20 Mbit/s at 60 Hz allows. PyAV decoded 387 frames, the first
an IDR picture, and read BT.709 and video range from the VUI. The frame
nearest the server's capture, compared with the capture resampled to the
stream as the kernel resamples the Scene image, was 41.5 dB PSNR from it;
the last frame, 33.9 dB, shows a Scene that kept settling (mean signed
difference about −1.4 of 255). Server render p50 58.4 ms, encode p50 4.4 /
p95 4.6 ms. With `--vulkan-validation` (the Khronos layer) the check passed
with no message, and a run with `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`
added reported none either. A check run's fresh home starts the driver's
shader cache cold, which takes about 250 s before the scene loads.

The QP range is what makes the bitrate count. At `c47c7484`, without it,
the same view encoded about 51 KB a frame at 10 and at 20 Mbit/s, with a
one-frame, 100 ms or 1 s buffer and with VBR alike (2.4 times the rate at
10 Mbit/s), at 42.8 dB. With it, the still view takes 7.1 KB a frame at
10 Mbit/s and 10.2 KB at 20 Mbit/s (41.0 and 41.5 dB), and flying the
client check's path across the square takes 13.6 KB (0.65 of the share)
and 21.5 KB (0.52), with single frames up to about 2.5 times the share.

Between two computers, 2026-10-23, with the rules before the sender's
deadline, skip and overshoot rules: the Windows server above at
`c47c7484` on Bistro, `--role server --allow <Mac key> --hold 900`, and
the Mac client at `cdda7e8c` over the Mac's Wi-Fi, `--role client
--address 192.168.3.2:7340 --key <server key> --identity <identity file>`.
The check passed: the server sent 2778 frames (34 key frames, 33
refreshes, no send failure, the bitrate at its 40 Mbit/s cap); the client
received 2525, decoded 2162 and showed 2151. It dropped 362 frames over 33
gaps, with no late or reordered frame, so the 253 missing frames expired at
the sender. Latency p50 49.0 / p95 57.4 ms over 256 frames; client
arrival-to-display p50 3.9 ms, decode 3.8 ms; server render 26.6 ms,
encode 4.8 ms at the last frame. The client's image showed the server's
plaza; no PSNR was taken, because the captures stayed on two computers.

`vkr_net_bench sim --mode video` reproduces the gaps without loss. It
streams 40 s over the simulator (3 ms each way, 4 ms jitter) with the view
service's channels, rate rule and client order; `--overshoot` scales every
frame and `--key-ratio` key frames on top:

```sh
build_release/tools/vkr_net_bench sim --mode video --bandwidth-mb 1.375 \
    --latency-ms 3 --jitter-ms 4 --seconds 40 --overshoot 1.25 --key-ratio 5
```

| Link, loss | Overshoot × key size | Earlier rules: gaps, frames decoded of 2400 | Current rules: gaps, skipped, decoded, send-to-decode p95 |
|---|---|---|---|
| 11 Mbit/s, 0 | 1.0 × 1 | 14, 2244 | 0, 23, 2348, 38.9 ms |
| 11 Mbit/s, 0 | 1.0 × 5 | 208, 208 | 2, 34, 2353, 37.4 ms |
| 11 Mbit/s, 0 | 1.25 × 5 | 99, 100 | 3, 51, 2332, 39.0 ms |
| 11 Mbit/s, 1 % | 1.25 × 5 | 107, 107 | 3, 57, 2320, 48.0 ms |
| 50 Mbit/s, 0 | 1.25 × 5 | 176, 176 | 1, 17, 2346, 18.6 ms |
| 50 Mbit/s, 1 % | 1.25 × 5 | 164, 165 | 2, 29, 2326, 28.5 ms |
| 1 Gbit/s, 0 | 1.0 × 5 | 0, 2400 | 0, 0, 2400, 0.8 ms |

The earlier rules ran in this change's development build with the
bandwidth filter fix of ADR-105; before that fix, a sender below the link
lost its estimate. On the lossless links every expiry under them was a
frame that never left (`expired_unsent`): a key frame delayed the frames
behind it past their 33 ms deadline and each gap asked for the next key
frame, and an overshooting encoder kept a backlog that expired most
frames. The 1 Gbit/s row
matches the loopback runs. `vulkan_renderer_tester --suite net` checks
that a lossless link the sender outruns counts only `expired_unsent` and a
lossy link with no time to resend counts `expired_resend`.

With the current rules the Mac loopback check passed on 2026-10-23: 2161
frames sent, 0 skipped, measured overshoot 1.00 (VideoToolbox keeps to its
rate), 0 gaps, latency p50 43.5 / p95 48.5 ms, 34.5 dB.

Not measured: the current sender rules between two computers, 2560×1440
(the proposal's target size), an EDR or windowed BGRA Scene source, and
the client's own present.

## Revisit when

- The sender rules run between two computers and show gaps or skipped
  frames the simulator did not predict, or a Windows client is needed (a
  decoder and a Vulkan import).
- A run between two computers shows loss the reorder window does not
  absorb: FEC and intra refresh become worth their cost.
- Raw input forwarding or a second client is needed.
