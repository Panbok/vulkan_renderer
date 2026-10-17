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
frame metrics, Tier 1 pass rows, the recording analysis, the Performance
window, and from R3 the entity poses that Watch replays. Play
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
- **Edits.** An edit while recording adds an `edit` marker, and stream
  `edit` records where the scene journals stand: the sequence of the newest
  edit an undo would revert across every journal, at the start and at each
  frame that changed it. When a playback opens and the recording's last
  position is still the journals' newest edit, Watch steps the journals with
  undo and redo to each frame's position, at most 64 steps a frame, and
  closing or opening another recording redoes them to where they stood. A
  scene edited after the recording, or a history that dropped its edits,
  shows the scene as it is now; `rec.status` reports `edits` as `replayed`,
  `unavailable` or `none`.
- **Entity poses (R3).** An editor recording adds stream `pose`: the world
  pose of every entity whose world matrix changed that frame, as an f64
  translation plus the matrix's 3x3, written by the frame-loop thread after
  the update. The scene lists changed entities only while a recording tracks
  it (`vkr_scene_track_changes`, `SCENE_TRANSFORM_CHANGED`), so several
  transform passes in one frame lose none. Tracking starts by listing every
  entity once, at most 2048 a frame. An entity is named by its
  document-stable ref, a model node by its nearest ref'd ancestor and glTF
  node, and a document without refs by document index. Watch groups the
  poses per entity, binds them to live entities once per scene generation,
  skips entities whose only pose matches the scene, and applies the pose at
  the playhead as an evaluated override before the frame's transform update;
  closing removes the overrides, so authored transforms never change.
- **Spawns (R3).** A transient entity scripts spawned, or a node of a model
  spawned under one, is named by a spawn id the recording gives it on first
  sight (pose kind SPAWN). Stream `spawn` holds each id's container and,
  once known, its model's mesh and animation paths; `despawn` marks when it
  went away. Watch creates a transient stand-in for every spawn with a
  model, loads that model, shows it only between its spawn and despawn
  frames, and poses it and its nodes from their recorded poses. An animated
  stand-in takes its nodes' recorded poses, relative to its own pose of the
  same frame, as its player's pose (`vkr_animation_player_override_globals`),
  so its skin moves as recorded; with the scene stopped the player does not
  advance and keeps it. Closing destroys the stand-ins. A spawn without a
  model, such as a bare shape, has no stand-in yet.
- **Gameplay records (R3).** SDK version 11 adds `vkr_rec_player`,
  `vkr_rec_tag`, `vkr_rec_event` and `vkr_rec_action`
  ([sdk.h](../../sdk/sdk.h)). They write
  streams `player` (one record per fixed tick: feet position with the origin
  offset, velocity, view, flags and module state), `tag` and `event` under
  the frame whose update ran them, through the recording the script host
  borrows each frame; without a recording they do nothing. The FPS module
  records its player after each tick's move. The analysis keeps each frame's
  last player, places spikes at it and offers `player.x`, `.y`, `.z` and
  `.speed` to `vkr_rec frames`. The scoped Play case
  `assets/scenes/fixtures/bistro_play_local.scene.json` is Bistro with a
  collision-only slab under the street and a Player Start. Stream `action`
  holds each input action a module admitted, by tick and sequence; the FPS
  module records every command it drains.
- **Drive (R3).** `rec.drive`, or Drive on a recording card, resets the
  session and plays it while `vkr_driving` tells modules to take their
  actions from `vkr_drive_actions` instead of live input
  ([vkr_recording_drive.h](../../runtime/src/recording/vkr_recording_drive.h)).
  Driven tick `base + n` replays recorded tick `first + n`; actions of a
  tick the run skipped are not replayed late. Each `vkr_rec_player` call
  during a drive is compared with the recorded player at the same tick,
  giving the distance, its maximum, the driven path length and the first
  recorded tick and frame past 0.25 m. The drive ends when its ticks run out
  (the session pauses) or the session stops; `rec.status` reports the
  measurement and the editor shows it.
- **Frame metrics (Tier 0).** A recording with metrics adds stream `metrics`:
  a `valid` bitmask and one float64 per registry metric found from a fixed
  Tier 0 list, durations in milliseconds, plus process resident memory
  sampled every 100 ms. The frame-loop thread writes the record when the
  frame's metrics frame is published, under the frame's own recording number;
  a dropped publication writes no values. An editor recording turns on
  whole-frame GPU timing and restores it at stop.
- **Pass rows (Tier 1).** A Tier 1 recording adds streams `pass_name` (an id
  and a name, written once before the id's first row) and `passes`: the
  executed passes of one rendered frame, each with CPU encode and GPU
  milliseconds (`vkr_recording_format.h`). The renderer's pass table
  describes a completed earlier frame; the recording maps its source frame
  index to the recorded frame through a 32-frame history and writes each
  source frame once. GPU pass stamps run every frame on desktop and in one
  frame of 8 on tiled, where they split the shared compute encoder; such
  frames carry `VKR_RECORDING_FRAME_PASS_TIMING`. The editor's Record makes
  Tier 1 recordings; `rec.start` takes `tier` none, 0 or 1. Stop restores the
  pass timing setting.
- **Analysis.** At stop the runtime reads the stream back and writes
  statistics, the time-based 1% low and spikes (over 1.2 times the median of
  19 neighbours and 4 ms above it, or over twice the budget) into
  `summary.json` within 8 KB
  ([vkr_recording_analysis.h](../../runtime/src/assets/vkr_recording_analysis.h)).
  Pass statistics rank by total time; a spike names the passes that grew
  most in its frame, or in the run's slowest frame with GPU times.
  `vkr_rec spikes`, `vkr_rec frames` and `vkr_rec passes` query the same
  analysis, and the editor's Performance window draws it with `vkr_ui_plot`,
  including a GPU-by-pass chart over the frames with GPU times and a pass
  table that isolates one pass.

## Consequences

- Recording costs two bounded copies per frame on the frame-loop thread; the
  writer ran 6.8 µs per frame in the Bistro A/B. Tier 1 adds about 35 pass
  rows (12 bytes each) per Bistro frame: a 244-frame Bistro recording took
  65 KB compressed. On tiled, every 8th frame also pays for the pass stamps
  (about 0.5 ms on Bistro), so timing comparisons use Tier 0.
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
- **A `start/` overlay plus encoded edits** (the proposal's plan) was not
  built. Restoring it reloads the scene, which drops the user's unsaved
  edits and undo history unless they are saved first, and the network edit
  encoding names entities by document id, which Bistro's document lacks.
  Stepping the journals reuses the editor's exact undo, needs no reload and
  leaves the user's state as it was; its limit is that the recording's
  edits must still head the history.

## Revisit when

Play recording (R3) adds world state, or frame metrics (R2) add render-thread
records, which bring the recorder frame number join described in the proposal.
