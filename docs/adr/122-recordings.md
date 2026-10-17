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
window, and from R3 the entity poses that Watch replays, Play recording,
Drive, requests, annotations, Compare, harness recordings and perf re-runs.
Re-simulation remains in the proposal. A Tier 0 recording passed the
authoritative overhead gate on a GPU-bound Bistro orbit (2026-10-17, in the
proposal); a CPU-bound workload has not been measured, so harness runs that
record keep the authority reason `recording.enabled`.

## Context

The owner asked for recordings of a session that users and agents can watch,
seek and analyze, without changing frame times (owner decisions D1 to D6 of
2026-10-17 in the proposal). Recording must not block, allocate, lock or do
I/O on frame threads, a recording cut off by a crash must stay readable, and
an agent must read one without the editor.

## Decision

- **Format.** A recording is a directory bundle `<name>.vkrrec/` holding
  `stream.vkrs`, `summary.json`, `thumb.png` and, from the editor,
  `thumbs/`. The stream is a header, typed
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
  advance and keeps it. A spawn with a primitive shape instead gets a shape
  stand-in of the same size and color. Closing destroys the stand-ins.
- **Route (R3).** While a playback is open the Scene draws its route: the
  player's track, holding its position between fixed ticks, or the camera's
  in a recording without a player, at most 4,096 points, each segment
  colored by its frames' worst frame time against the 16.7 ms budget and
  dimmed ahead of the playhead. It is a `vkr_ui_path` over the Scene image
  ([vkr_ui_system.h](../../runtime/src/renderer/systems/vkr_ui_system.h)),
  projected in the editor's late camera hook with the frame's final camera,
  as object icons are; a point behind the camera breaks it. It draws over
  geometry.
- **Requests (R4).** An agent asks the user for a recording with
  `rec.request.create`: a question the recording must answer, up to 8 steps,
  and optionally a scene, a start pose, a tier, a stop after some seconds and
  an expiry (60 minutes by default). Each request is one JSON file,
  `<recordings>/requests/<id>.json` (kind `vkr.recording.request`),
  rewritten whole on every change, so the agent's operations and the editor
  read the same state; ids are letters, digits, `-` and `_`. Pending requests
  show as cards at the top of the Recordings window and once in a toast. Start
  (or Cmd `request.start <id>`) needs the request's scene open and no
  recording or playback; it places the camera, records under the request's
  id and marks it accepted. While it records, the question and steps stay at
  the top right of the Scene and "Next request step" (Cmd+Alt+N) adds a
  `step` marker per step. When the recording stops, by the user or by the
  stop rule, the request becomes recorded and names its bundle; a recording
  that has not started 5 s after Start returns the request to pending.
  Decline (or `request.decline <id> [reason]`) keeps the user's reason.
  `rec.request.wait` holds until the request is recorded, declined,
  cancelled or expired, or a timeout passes
  ([editor_recording_requests.c](../../editor/src/editor_recording_requests.c)).
- **Timeline.** The playback bar's chevron opens a timeline over it: a
  frame-time lane of at most 512 columns (each the slowest frame of its run)
  against the 16.7 ms budget, with the playhead; an event lane with
  markers, notes and ranges; and a ruler. The wheel zooms around the
  pointer and Shift-wheel pans; a playing playhead pulls the view along.
  Dragging across the lane selects frames, which can loop, become a range,
  get a perf re-run or fill the view. A loop span is a playback setting
  (`loop_start`, `loop_end`) that every settings request carries; `rec.set`
  takes `range` [first, last] frames, [] for the whole recording. A
  recording with metrics adds a memory lane: resident memory (RAM) and the
  GPU driver's allocated memory, each column's highest, on separate axes
  scaled to the view so a few MB of growth shows, with each line's range in
  its legend; they are never added, since on Apple silicon GPU memory is
  part of the resident footprint
  ([editor_recording_timeline.c](../../editor/src/editor_recording_timeline.c)).
- **Engine events.** Stream `event` carries, beside script events,
  engine events named `vkr.*` with a count and milliseconds
  (`VkrRecordingEngineEvent`, [vkr_recording_format.h](../../runtime/src/assets/vkr_recording_format.h)):
  pipeline builds (`vkr.pipeline`, from `pipeline.create`, which Metal
  publishes for its background Custom graph and tiled variant builds and
  the backend config's `pipeline_create` producer carries; Vulkan publishes
  none yet), asset loads (`vkr.texture`, `vkr.mesh`, `vkr.material`, `vkr.scene`) from
  the frame's published load durations, draws skipped while their pipeline
  compiles (`vkr.pipeline_late`, from `pipelines.late`), world cells loaded
  and unloaded by streaming (`vkr.cell_load`, `vkr.cell_unload`) and script
  hot reloads (`vkr.script_reload`). Renderer-derived events need Tier 0;
  cell and reload events record at every tier. The analysis totals them per
  name in `summary.json`, lists up to four engine then script events within
  10 frames of each spike, and `vkr_rec frames` and `rec.frames` give a
  range's events. The timeline shows them in a lane under the frame-time
  lane, one tick per column coloured by its costliest kind, with each
  kind's count and load time in the tooltip.
- **Thumbnails (R5).** While the editor records, it captures the Scene twice
  a second through capture channel `editor_scene` (the editor's Scene image
  after its grid, outlines and gizmos, before the UI, so the REC chip and
  windows stay out). The main thread samples each capture to at most
  256×240 with nine texels per pixel as it arrives, and a thread of its own
  writes `thumbs/<frame>.jpg` (eight-digit recorder frame, quality 82, 8.6 KB
  on average on Bistro); the first is also `thumb.png`, the card's picture. One
  capture at a time: a thumbnail waits while the agent's capture renders or
  the last thumbnail writes, an agent capture waits for a rendering
  thumbnail (`capture_busy`), and three failed captures in a row stop a
  recording's thumbnails. Hovering the timeline's frame-time lane shows the
  thumbnail at or before the hovered frame with its number, frame time and
  time; four stay loaded, and the nearest loaded one stands in while the
  wanted one loads
  ([editor_recording_thumbs.c](../../editor/src/editor_recording_thumbs.c)).
- **Annotations (R4).** A bundle's `annotations.json` (kind
  `vkr.recording.annotations`) holds notes on frames (at most 64), named
  frame ranges (at most 32) and a review state: unreviewed, reviewed or
  flagged, with who set it. It is rewritten whole on every change, so the
  stream and `summary.json` never change after the recording stops; a bundle
  without it has none
  ([vkr_recording_annotations.h](../../runtime/src/assets/vkr_recording_annotations.h)).
  Agents read it with `rec.annotations` and change it with `rec.note`,
  `rec.range` and `rec.review`, which take the open recording when they name
  none; `rec.list` and `vkr_rec summary` report it. The playback bar adds a
  note at the playhead from a field above it, marks a range with two presses
  of Range, and toggles the review (Shift flags it); notes show as squares
  and ranges as outlined spans on its track. Clicking one seeks to it and
  opens a panel over the bar: a note's text, author and Delete, or a range's
  name to rename (Enter) and Delete (`rec.range` `rename` for agents). A
  card shows the note count and the review. Copy agent prompt puts the recording's facts, its first
  markers, notes and ranges, the review, the request it answers and the
  playhead on the clipboard
  ([editor_recording_annotations.c](../../editor/src/editor_recording_annotations.c)).
- **Harness recordings (R5).** `vkr_harness profile --record` gives each
  repetition a Tier 0 recording of its run in `runs/<i>/recording/` and lists
  its stream and `summary.json` among the report's artifacts; the run is not
  authoritative while the recorder's overhead gate is open (ADR-051). Two
  harness runs' recordings open in Compare like any others. A case camera of
  mode `recording` replays a recording's range under the case's fixed delta
  and harness rules (a perf re-run): the harness child seeks the runtime's
  playback each frame, so the run renders what Watch shows. `rec.rerun` and
  a range panel's Re-run write such a case under `tools/cases/local` from the
  windowed timing case `bistro_shadow_orbit` (its settings, the recording's
  scene, the range's frames, measured frames covering the range's seconds at
  its fixed delta, no captures or assertions), copy the recording into
  `build/_artifacts/rerun`, and give the harness command; the editor never
  runs it, which would share the GPU with the benchmark
  ([editor_recording_rerun.c](../../editor/src/editor_recording_rerun.c)).
- **Compare (R5).** `vkr_recording_compare` compares recording B with A,
  both analyzed
  ([vkr_recording_compare.h](../../runtime/src/assets/vkr_recording_compare.h)).
  Statistics (p50, p95, p99, max, 1% low, mean, frames over budget, RAM and
  VRAM peaks) and passes, matched by name with GPU means when both have GPU
  pass times, differ without alignment. The frame-by-frame difference maps
  each frame of A to the frame of B at the same key: seconds from the start,
  seconds from a marker label both carry, meters along the path (the
  players' when both recorded one, else the cameras'), or the frame number
  for re-runs; from it come the mean change per aligned frame and the 30
  aligned frames of A where B is slowest. The Performance window shows B
  beside the analyzed recording: each card's last line is B's value and the
  change, B's frame times run as a line over A's bars, the pass table lists
  A, B and the change, largest first, and the header switches the
  alignment. A card's Compare button, `rec.compare` and `vkr_rec compare`
  start it.
- **Gameplay records (R3).** SDK version 12 adds `vkr_rec_player`,
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
  measurement and the editor shows it. `vkr_drive_start` gives the recorded
  player a drive starts from, so a module places its player there and a
  recording that began mid-session drives from where it began;
  `vkr_drive_follow` and `vkr_drive_reference` let a module steer its player
  back toward the recorded one (`rec.drive` with `follow`). The FPS module
  does both, pulling at most at run speed.
- **Frame metrics (Tier 0).** A recording with metrics adds stream `metrics`:
  a `valid` bitmask and one float64 per registry metric found from a fixed
  Tier 0 list, durations in milliseconds, plus process resident memory
  sampled every 100 ms. The frame-loop thread writes the record when the
  frame's metrics frame is published, under the frame's own recording number;
  a dropped publication writes no values. An editor recording turns on
  whole-frame GPU timing and restores it at stop. `gpu.submission` is the
  exception: a published frame carries the newest completed frame's GPU time,
  so the renderer keeps the frame that ran it beside the pass table
  (`vkr_renderer_metrics_get_submission`), and the recording writes it on
  the recorded frame that rendered that frame, as a second `metrics` record
  whose mask holds that field alone; readers merge records by mask. A
  completion repeated across publications is written once, and frames still
  on the GPU at stop have none.
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
  analysis, and so do the editor's `rec.summary` (the bundle's
  `summary.json`), `rec.spikes` (at most 100, its object bounded to 32 KB),
  `rec.frames` (at most 500 rows) and `rec.passes`, which name the
  recording or default to the open one; each spike and the frames' range
  also name their thumbnails, and `rec.seek` takes a spike rank. The editor's
  Performance window draws it with `vkr_ui_plot`,
  including a GPU-by-pass chart over the frames with GPU times and a pass
  table that isolates one pass.

## Consequences

- Recording costs two bounded copies per frame on the frame-loop thread; the
  writer ran 6.8 µs per frame in the Bistro A/B. Tier 1 adds about 35 pass
  rows (12 bytes each) per Bistro frame: a 244-frame Bistro recording took
  65 KB compressed. On tiled, every 8th frame also pays for the pass stamps
  (about 0.5 ms on Bistro), so timing comparisons use Tier 0.
- Recording did not raise `frame.wall` in authoritative A/B runs on Bistro
  (the GPU-bound orbit, and a 640×360 orbit with and without the render
  thread), nor any CPU stage; Bistro stays GPU-bound on the M1 at low
  resolution, so a CPU-bound workload is still to be measured
  (docs/proposals/recordings.md, R1).
- A seek reproduces the recorded camera exactly: a scripted agent-channel
  check found 0 m and 0° error at four frames on Bistro.
- A recording made by another build reads only if its dictionary's frame and
  camera layouts match; playback refuses others with a reason.
- Thumbnails cost the main thread 0.24 ms per capture (sampling a
  1275×700 Bistro Scene to 256×141) and 0.04 ms to start the write thread.
  In a 15 s idle headless Bistro recording (M1 Pro, Metal, Release) the
  frame after each request and the frame that reads the capture ran about
  0.8 and 0.5 ms longer than the 5.65 ms median and the one after about
  1.0 ms shorter. The renderer grows its readback memory on the first
  capture that needs more and waits for the GPU (about 6 ms on Bistro), so
  the editor makes one capture of the Scene while nothing records once the
  Scene image has kept its size for 30 builds; without it every first
  recording of a session began with that spike.

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

- **`thumbs.vkrt`, one BC1 file per recording** (the proposal's plan) was
  not built: the BC encoder is unavailable on arm64 hosts, the texture
  loader takes image files, and a JPEG a thumbnail is small and readable by
  agents without a tool. The write thread keeps the file count off the
  frame.

## Revisit when

Play recording (R3) adds world state, or frame metrics (R2) add render-thread
records, which bring the recorder frame number join described in the proposal.
