---
status: partial
updated: 2026-10-17
authority: adr
---

# ADR-122: Recordings

## Status

Accepted (partial). Phases R0, R1 and most of R2 of
[Recordings](../proposals/recordings.md) are implemented: the stream format,
the recorder, camera and frame recording with simulation stopped, Watch
playback with exact seeking, the editor's Record control, REC indicator,
playback bar and Recordings window, the `rec.*` agent operations, Tier 0
frame metrics, the recording analysis and the Performance window. Play
recording, Drive, perf re-runs and re-simulation remain in the proposal.
The authoritative overhead gate has not run; a non-authoritative
Bistro A/B is recorded in the proposal.

## Context

The owner asked for recordings of a session that users and agents can watch,
seek and analyze, without changing frame times (owner decisions D1 to D6 of
2026-10-17 in the proposal). Recording must not block, allocate, lock or do
I/O on frame threads, a recording cut off by a crash must stay readable, and
an agent must read one without the editor.

## Decision

- **Format.** A recording is a directory bundle `<name>.vkrrec/` holding
  `stream.vkrs`, `summary.json` and `thumb.png`. The stream is a header, typed
  chunks with CRC-32 on header and payload, an index chunk and a trailer.
  Records carry a stream id and a frame number; a DICTIONARY chunk names each
  stream's fields, types and units. A reader keeps every whole chunk before a
  cut or a damaged chunk
  ([vkr_recording_format.h](../../runtime/src/assets/vkr_recording_format.h)).
- **Codec.** `VkrCodec` ([vkr_codec.h](../../lib/src/core/vkr_codec.h)), shared
  with the depot, is passed in by the executable. The editor and the tester
  pass the zstd they carry; the runtime decodes with
  `vkr_content_codec_zstd_decoder` and writes raw chunks when no compressor is
  given.
- **Recorder.** Each producer thread writes into its own preallocated
  `VkrSpscRing` ([vkr_spsc_ring.h](../../lib/src/containers/vkr_spsc_ring.h)).
  A full ring drops a droppable record or loses another one, and the writer
  records a gap; producers never block. One writer thread at utility priority
  joins, compresses and appends chunks of about one second and writes END,
  the index and the trailer at stop
  ([vkr_recorder.h](../../runtime/src/recording/vkr_recorder.h)).
- **Runtime records.** `VkrStandardSceneRuntime` writes a frame record
  (display dt, simulation time, hour, flags) and a camera record (world
  position in float64, yaw, pitch, lens) after the camera update; markers are
  kind plus label. Frame 0 is at time zero and each later frame follows its
  display dt ([vkr_recording.h](../../runtime/src/recording/vkr_recording.h),
  [vkr_standard_scene_recording.c](../../runtime/src/application/vkr_standard_scene_recording.c)).
  `$VKR_RECORDING_DIRECTORY` records a whole run.
- **Playback.** An open `VkrRecordingPlayback` drives the camera and the
  level's hour before the camera controller, interpolating between frames and
  exact at a frame's time
  ([vkr_recording_playback.h](../../runtime/src/recording/vkr_recording_playback.h)).
  Playback is a transport state: Play and Step are refused, scene edits and
  edit batches are refused, and closing restores the editor camera and hour
  ([vkr_sample_recording.h](../../runtime/src/vkr_sample_recording.h)).
- **Editor and agents.** One request per frame travels in
  `VkrSampleUiFrame.recording_request`; the status, live camera and open
  playback come back in `VkrSampleUiFrame.recording`. Recordings go to the
  workspace's `local/recordings`, or the agent directory outside a project
  ([editor_recording.h](../../editor/src/editor_recording.h)). The `rec.*`
  operations answer with the runtime's outcome
  ([editor_ops.c](../../editor/src/editor_ops.c)); `vkr_rec summary` and
  `vkr_rec verify` read bundles offline
  ([vkr_rec_main.c](../../tools/recording/vkr_rec_main.c)).
- **Edits.** An edit while recording adds an `edit` marker; playback shows the
  scene as it is now.
- **Frame metrics (Tier 0).** A recording with metrics adds stream `metrics`:
  a `valid` bitmask and one float64 per registry metric found from a fixed
  Tier 0 list, durations in milliseconds, plus process resident memory
  sampled every 100 ms. The frame-loop thread writes the record when the
  frame's metrics frame is published, under the frame's own recording number;
  a dropped publication writes no values. An editor recording turns on
  whole-frame GPU timing and restores it at stop.
- **Analysis.** At stop the runtime reads the stream back and writes
  statistics, the time-based 1% low and spikes (over 1.2 times the median of
  19 neighbours and 4 ms above it, or over twice the budget) into
  `summary.json` within 8 KB
  ([vkr_recording_analysis.h](../../runtime/src/assets/vkr_recording_analysis.h)).
  `vkr_rec spikes` and `vkr_rec frames` query the same analysis, and the
  editor's Performance window draws it with `vkr_ui_plot`.

## Consequences

- Recording costs two bounded copies per frame on the frame-loop thread; the
  writer ran 6.8 µs per frame in the Bistro A/B.
- A seek reproduces the recorded camera exactly: a scripted agent-channel
  check found 0 m and 0° error at four frames on Bistro.
- A recording made by another build reads only if its dictionary's frame and
  camera layouts match; playback refuses others with a reason.
- Thumbnails come from a Scene capture shortly after start and compete with
  agent captures; a lost capture retries up to three times.

## Alternatives considered

- **Input-only replay** was rejected as the primary model (D1): it breaks on
  any build, script or platform change.
- **A zstd encoder in `lib/`** was not needed: executables that write already
  carry one, so the format takes a codec instead.
- **Replaying edits in R1** moved to R3, which brings the world state stream
  that edit replay needs.

## Revisit when

Play recording (R3) adds world state, or frame metrics (R2) add render-thread
records, which bring the recorder frame number join described in the proposal.
