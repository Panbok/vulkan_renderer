---
status: partial
updated: 2026-10-20
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
Drive, requests, annotations, Compare, harness recordings and perf re-runs,
and from R6 Re-simulate on the same build, with model joins and terrain
collision rebuilds at their recorded ticks and world partition streaming
replayed. A Tier 0 recording passed the
authoritative overhead gate on a GPU-bound Bistro orbit (2026-10-17, in the
proposal). On a CPU-bound offscreen Bistro view recording did not lengthen
frames either, but five of its eight runs failed the offscreen profile's
warmup rule, so that gate has no full authoritative A/B and harness runs
that record keep the authority reason `recording.enabled`.

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
  An open reads the bundle on a low-priority job worker into a staging
  playback, publishing the share read in the status
  (`opening`, `opening_progress`) and checking a cancel flag per chunk;
  frames keep running, and the frame that sees it done closes the old
  playback, moves the new one in (`vkr_recording_playback_move`) and answers
  the request. Play, Pause, Seek and settings sent meanwhile apply once it
  is open; Start and Drive are refused, and Close or another open cancels
  it. Watch shows the read on its card and in a panel where the playback
  bar will stand, with Cancel. Opening the 16,463-frame Level Design Test
  recording in the Bistro editor took 2.9 s with no editor frame over
  30 ms (Release, M1 Pro, 2026-10-20); it held one frame for 3.2 s before.
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
- **Entity poses (R3).** An editor recording adds stream `pose`: the pose
  of every entity whose world matrix changed that frame, as an f64
  translation plus the matrix's 3x3, written by the frame-loop thread after
  the update. The scene lists changed entities only while a recording tracks
  it (`vkr_scene_track_changes`, `SCENE_TRANSFORM_CHANGED`), so several
  transform passes in one frame lose none. Tracking starts by listing every
  entity once, at most 2048 a frame. An entity is named by its
  document-stable ref, a model node by its nearest ref'd ancestor and glTF
  node, and a document without refs by document index.
- **Parented poses (2026-10-19).** An entity with a parent is posed in its
  parent's space (`VKR_RECORDING_POSE_FLAG_PARENTED`): its local matrix
  when nothing overrides its world, else its world under the parent's
  inverse. The scene does not list an entity whose world changed only
  because its parent's did, so a moving lift, tram or rotor writes one pose
  a frame, not one per brush face it carries; on the Testbed level such
  children were about 1,400 of the 1,749 entities posed every frame. Three
  kinds keep world poses or every change, because Watch's hierarchy can
  differ for them: a root; a spawn's own pose (its stand-in is a root),
  whose changes the scene lists since it is transient; and an entity
  reparented after its world was first computed
  (`SCENE_TRANSFORM_REPARENTED`, such as by a script), which the scene lists
  for every change. An overridden child is listed when its parent moves,
  since its override does not follow. Recordings before the flag pose every
  entity in world space and play as before.
- **Watch of poses.** Watch groups the poses per entity, binds them to live
  entities once per scene generation, finds each entity's parent key
  through the live hierarchy, skips entities whose only pose matches the
  scene (its world pose, or for a parented one its local pose), and applies
  the pose at the playhead as an evaluated override before the frame's
  transform update. A parented pose equal to its entity's local pose needs
  no override: the scene's propagation carries it with its parent. Another
  parented pose becomes its parent's world pose at that frame times it, the
  parent's composed the same way. Closing removes the overrides, so
  authored transforms never change. The player's first pass counts poses per
  key and its second writes each pose in place, 72 bytes each; a 137 s
  Testbed recording of 22,109,504 poses that ran out of the player's 2 GB
  memory before opens within an 8 GB address reservation that commits only
  what the poses use. A
  pose carries `VKR_RECORDING_POSE_FLAG_HIDDEN` while its entity's own
  visibility is off, and a visibility change writes a pose, so Watch hides
  what the session hid, such as a first-person player's body around the
  camera; closing shows them again. Recordings before the flag show every
  body. A playback of a Play session also draws what Play drew: no clip or
  trigger brushes and no editor grid. The grid's axes, drawn from the
  recorded eye, ran across the view at eye height in Watch; Play now hides
  the grid too, as it hides the brushes.
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
  geometry, faded (30% of its colour) where collision hides a point from
  the camera: the collision ray Scene icons use, cast for up to 128 points
  a build from where the last build stopped while the view changes, so a
  moving camera costs a bounded number of rays and a still one none. A
  scene without collision, such as Bistro, fades nothing.
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
  the backend config's `pipeline_create` producer carries; Vulkan builds
  every pipeline at creation and publishes only the presentation pipelines
  a swapchain format change rebuilds, when HDR output turns on or off),
  shader library loads (`vkr.shader`, from `shader.load`: Metal's
  startup libraries and each project material library load, through the
  config's `shader_load` producer), asset loads (`vkr.texture`, `vkr.mesh`, `vkr.material`, `vkr.scene`) from
  the frame's published load durations, draws skipped while their pipeline
  compiles (`vkr.pipeline_late`, from `pipelines.late`; Metal only, since no
  Vulkan draw waits for a pipeline), world cells loaded
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
  `vkr.recording.annotations`, version 2) holds notes on frames (at most
  64), segments (at most 64) and a review state: unreviewed, reviewed or
  flagged, with who set it. A segment marks frames `first` to `last` with a
  title (64 bytes), a note of up to 1 KiB that says what happens there and
  why it matters, its author and when it was marked, so a person or an
  agent can be pointed at the parts that matter. A version 1 file's named
  ranges read as segments without notes; the file is rewritten whole on
  every change, so the stream and `summary.json` never change after the
  recording stops, and a bundle without it has none
  ([vkr_recording_annotations.h](../../runtime/src/assets/vkr_recording_annotations.h)).
  - Agents read the annotations with `rec.annotations` and change them with
    `rec.note`, `rec.segment` (add with `first`, `last`, `title` and
    `note`; with `segment`, an index or a title, edit the title or note or
    `remove` it) and `rec.review`, which take the open recording when they
    name none. `rec.segments` answers each segment with its start, length,
    frame-time p50, p95 and max, frames over the budget, spikes inside and
    worst frame (`vkr_recording_segment_stats` in
    [vkr_recording_analysis.h](../../runtime/src/assets/vkr_recording_analysis.h)).
    `rec.frames` and `rec.rerun` take a `segment` in place of frames.
    Offline, `vkr_rec segments` prints the same statistics, `vkr_rec frames`
    and `export` take `--segment NAME`, and `vkr_rec summary` and `rec.list`
    report the segments.
  - The playback bar adds a note at the playhead from a field above it,
    marks a segment with two presses of the ruler (the timeline's selection
    has a Segment button too), opens the Segments list, and toggles the
    review (Shift flags it). Notes show as squares and segments as outlined
    spans on the bar's track, and as spans on the timeline; hovering one
    shows its note. A new segment opens its panel with the title taking the
    keyboard.
  - The segment panel over the bar edits the title and a multi-line note,
    steps to the previous or next segment, and has Focus, Re-run, Delete and
    Save; Enter in the title or Cmd+Enter (Ctrl+Enter) in the note saves, and
    closing or opening another segment saves unsaved edits. Its footer gives
    the frames, the length, the author and the note's size.
  - The Segments list shows every segment by first frame, six to a page,
    each with its title, span and the first line of its note; a row opens
    the segment, and Focus views it alone: the timeline opens on its frames
    with a tenth of their span each side, playback loops them from the
    first frame, and the focused segment shows in the accent colour until
    Exit focus.
  - A card shows the note count and the review. Copy agent prompt puts the
    recording's facts, its segments with their notes first, its markers and
    notes, the review, the request it answers and the playhead on the
    clipboard
    ([editor_recording_annotations.c](../../editor/src/editor_recording_annotations.c)).
- **Harness recordings (R5).** `vkr_harness profile --record` gives each
  repetition a Tier 0 recording of its run in `runs/<i>/recording/` and lists
  its stream and `summary.json` among the report's artifacts; the run is not
  authoritative while the recorder's overhead gate is open (ADR-051). Two
  harness runs' recordings open in Compare like any others. A case camera of
  mode `recording` replays a recording's range under the case's fixed delta
  and harness rules (a perf re-run): the harness child seeks the runtime's
  playback each frame, so the run renders what Watch shows. `rec.rerun` and
  a range panel's Re-run write such a case under `tools/cases/rerun` (ignored
  by git, so authoritative profiles accept the tree) from the
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
  players' when both have player records, else the cameras'; every
  recording declares the player stream, so a declared but empty one does
  not count; where either path stands still, frames at that distance pair
  by the time since each stopped), or the frame number
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
- **Re-simulate (R6).** A Play session draws a session seed (the wall clock,
  or the recorded seed when re-simulating) and writes it to stream
  `session`; each running frame writes its clock (`now`, scene delta) and
  its streaming source (the camera position world partition and terrain
  streaming read) to `sim_frame`. SDK version 13 adds a seeded splitmix64 generator per module
  instance (`vkr_random_u32`, `vkr_random_f32`, `vkr_random_range`, seeded
  from the session seed, the module name and the container) and
  `vkr_sim_hash`, which feeds simulation values a module keeps outside
  components into its tick hash. After each fixed tick the script host
  writes `tick_hash`: CRC-32s of the transforms in 16 buckets, the
  physics bodies' poses and velocities, and per module its generators,
  `vkr_sim_hash` input, typed components and per-entity state
  ([vkr_script_hash.c](../../runtime/src/script/vkr_script_hash.c)); stream
  `hash_module` names the modules. Entities are named by a stable key
  (`vkr_scene_entity_stable_key`), never their ECS slot: the document-stable
  id an editor or cell object carries, else a document entity's index, else
  a node's glTF index or name chained to its parent's key; live entity ids
  inside script data hash as their keys, and per-entity hashes are summed
  into their buckets, so neither the slots nor the chunk order a load and
  unload history leaves changes a hash. The host caches each key by slot
  and generation; on the scoped Bistro case the first tick of a session
  hashes in 0.56 ms and later ones in 0.14 ms on average. Instance data is not hashed: it
  holds input bookkeeping and frame-time presentation that a replay does
  not reproduce. `rec.drive` with `resim`, or Re-simulate on a card, resets
  the session with the recorded seed, replays the recorded clock and
  actions from the scene start (without `vkr_drive_start`) and compares
  each tick's hashes; the first mismatch pauses the session and reports the
  recorded frame, the tick, the differing modules, bodies or transform
  buckets, and the bodies and characters in those buckets. Re-simulate
  needs a recording that began before Play started, and replays one without
  input too (an idle session); Drive needs actions and a player track. A recorded or
  re-simulated session starts from a rebuilt physics world, and a reset
  keeps the scene's body order: Jolt steps alike only from alike body order,
  and before, each reset reversed it and a first Play ran on the world as
  edits had built it, so twelve dynamic boxes parted at tick 1. Results
  that finish off the frame loop join at their recorded ticks: models the
  session requested and the collision bodies a streamed terrain builds on
  a physics worker as its window moves. The script host's scene hooks
  (`VkrSceneJoinHooks`) write stream `join` (the kind, the fixed ticks
  completed, the wrapper or terrain without its generation) while
  recording, and while re-simulating hold a finished result until the
  ticks its next recorded join followed; a frame whose due result still
  builds neither ticks nor runs modules nor takes a recorded frame. Such
  sessions also start a due terrain rebuild at once instead of within the
  frame's wall-clock budget (`deterministic_session`). Before, the FPS
  mannequin joined whenever its load finished and a re-simulation could
  part at tick 6. Joins name their entity by stable key too. World
  partition sessions also replay their streaming: stream `cells` keeps the
  cells loaded when the session starts, a re-simulation loads and unloads
  cells to match before it starts, each re-simulated frame streams around
  the recorded source, and cells load without the frame budget; the
  deterministic reset then orders bodies by stable key, whatever order
  loads left them in. Behaviors of such a session run, and `vkr_find`
  answers, in stable-key order too, since a module that draws from its
  generator per entity would otherwise draw in slot order.
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
  on the GPU at stop have none. With a render thread a frame is published
  during a later frame-loop iteration (ADR-082), which runs its update and
  waits for the frame before it closes the metrics frame. The Tier 0 list
  ([vkr_recording.c](../../runtime/src/recording/vkr_recording.c)) also
  carries the render thread's static candidate pack time
  (`cpu.packet_candidate_pack`), the Vulkan frame-begin waits for the
  command slot, image acquire, present fence and image reuse
  (`cpu.backend_frame_wait`, which unlike `cpu.drawable_acquire` includes a
  GPU-bound frame's wait for its slot), the interval presentation paces the
  frame to (`frame.present_interval`: the display's refresh interval when
  the frame presented with vertical sync, raised to the frame limit's,
  unsampled otherwise; Metal does not yet report its vertical sync, so it
  samples only a frame limit), and three bit sets: why a slot packed its static candidate
  rows again (`packet.static_repack_reasons`, `VkrStaticRepackReason`),
  which publications moved the candidate publication generation
  (`packet.publication_changes`, `VkrPublicationChange`, both in
  [vkr_candidate_residency.h](../../renderer/src/vkr_candidate_residency.h)),
  and whether the world waited on a publication
  (`packet.publication_pending`). That iteration's
  `cpu.update`, `cpu.simulation`, `cpu.physics` and `cpu.render_wait`, and
  its `cpu.world_payload_build` when it built a decoupled frame first, are
  written on the iteration's own recorded frame, again as a record whose
  mask holds them alone, so a slow update sits on the frame whose
  `frame.wall` it lengthened. Before, a 290 ms agent capture's update sat
  one row before its `frame.wall` spike and the spike named another cause. The
  recording's first iteration publishes a frame from before the recording,
  so only its loop work is written, on frame 0. A spike whose largest
  contributor is the frame loop's wait for the renderer also names the
  frame among the 4 before it whose `gpu.submission` grew most (at least a
  quarter of the wait's excess): GPU work in frame N stalls the frame loop
  only when the frames in flight run out. `rec.spikes` answers it as
  `gpu_wait` and the Performance window as "GPU frame N". The agent
  operation `debug.gpu_spike` runs pass `Debug.GpuSpike` (ADR-044) in the
  next rendered frame to check this join.
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
- **All measurements.** A recording with `all_measurements` (the Recordings
  window's All measurements checkbox, kept with the project's editor
  settings; `rec.start` `measurements` all; a request's `measurements`)
  is Tier 1 with GPU pass stamps on every frame, tiled included, and the
  registry's event subjects on. It adds streams `measurement_name` (an index,
  a name and a unit per catalog metric the Tier 0 list does not carry,
  written at start) and `measurements` (pages of 64 values with a valid
  mask; `vkr_recording_format.h`), so a frame's every published metric is
  kept. Physics publishes `physics.bodies`, `physics.awake_bodies`,
  `physics.contacts`, `physics.characters` and `physics.ticks` while the
  simulation runs. The analysis appends the measurements as series after
  the metrics fields; `summary.json` counts them under `measurements`, and
  the Performance window lists every series with a sample in a Series
  table beside Passes, filtered by name. On Bistro an all-measurements Play
  recording of 966 frames carried 474 measurements with no dropped records
  and GPU pass times on every frame with pass rows. On tiled the stamps
  split the shared compute encoder in every frame, so timing comparisons
  still use Tier 0. The renderer also publishes each shadow cascade's fit,
  `draw.shadow.cascadeN.split_near`, `split_far` and `texel_size` in metres,
  and `draw.shadow.cascades`; a Bistro all-measurements recording carried 499
  measurements, the cascades' fits among them. The metric catalog holds 1024
  slots and a recording 1024 measurements.
- **Debug views during playback.** Watch and Re-simulate render the Scene
  live, so the render menu's views apply to a playing recording: each
  pipeline's views, the shadow views (cascades, shadow factor, map depth) on
  both, and Pass output, which draws a pass's image, such as a shadow cascade,
  depth or bloom, over the Scene (ADR-044). F6 cycles the shadow views.
- **The recorded level.** Watch, Drive and Re-simulate of a recording made in
  another scene of the open project first open that scene, with a toast, and
  run once it has loaded; the scene comes from the id in `summary.json`'s
  scene path. A switch that does not finish within 3600 editor builds drops
  the request with a warning. On the Testbed project the switch from Bistro to
  Level Design Test took 4.9 to 7.5 s headless.
- **Thumbnails stay on the presenter.** The 2 Hz thumbnail captures read the
  `editor_scene` image, so they ride the next Scene frame the editor presenter
  renders (ADR-111). Taking them outside it rendered two Scenes back to back,
  a 25 to 27 ms frame on each thumbnail in a Level Design Test recording. A
  windowed Bistro Play recording of 725 frames afterwards wrote 12 thumbnails
  with a p99 frame of 9.15 ms and no frame over 12 ms at a thumbnail. The
  editor asks for thumbnails and the warm-up capture only while the Scene
  renders: during a project scene's opening the renderer refused three
  warm-up captures, cancelling their frames, and the warm-up never ran again
  in that session.
- **Analysis.** At stop the runtime reads the stream back and writes
  statistics, the time-based 1% low and spikes (over 1.2 times the median of
  19 neighbours and 4 ms above it, or over twice the budget) into
  `summary.json` within 8 KB
  ([vkr_recording_analysis.h](../../runtime/src/assets/vkr_recording_analysis.h)).
  The budget is the median `frame.present_interval`, 13.33 ms on a 75 Hz
  display with vertical sync, else 16.667 ms; `frame_time.budget_source`
  names it, and `vkr_rec summary` and `vkr_rec spikes` take `--budget MS`
  for recordings made before the series. Before 2026-10-18 every recording
  used 16.667 ms, so a 75 Hz session that dropped to 20 ms frames counted
  only the frames past 16.7 ms. `slow_frames` compares the frames over 1.2 times the
  budget (`threshold_ms`, so a vsync-paced frame a little past its interval
  does not count, and one that missed it does) with the others: their count, their runs of adjacent frames by
  length (1 to 7 and longer), the longest run, and the five `ms` series,
  GPU passes and flag bits (the three bit sets above, one name per bit)
  whose slow-frame mean or share exceeds the others' most. Each comparison
  also tries the row before every frame and keeps the larger difference as
  lag 1, since a render-thread frame's work and GPU time land one row before
  the frame-loop iteration that waits for it; a pass absent from a frame
  counts as zero, so a pass that runs only in slow frames shows. Spikes
  cannot find a run of slow frames that the neighbour median absorbs. On a
  user's 46 s, 75 Hz warehouse recording (`vkr_rec summary --budget
  13.333`) the section found 683 slow frames in 115 runs, 52 of exactly six
  frames, with `cpu.render_submit` at 18.5 against 8.8 ms (lag 1) and
  `Cull.Upload` and whole `Shadow.Cascade.N` passes only in slow frames,
  while spikes ranked single frames.
  Pass statistics rank by total time; a spike names the passes that grew
  most in its frame, or in the run's slowest frame with GPU times.
  `vkr_rec spikes`, `vkr_rec frames` and `vkr_rec passes` query the same
  analysis, and so do the editor's `rec.summary` (the bundle's
  `summary.json`), `rec.spikes` (at most 100, its object bounded to 32 KB),
  `rec.frames` (at most 500 rows) and `rec.passes`, which name the
  recording or default to the open one; each spike and the frames' range
  also name their thumbnails, and `rec.seek` takes a spike rank. Each spike
  carries a `cause`, the first of the causes `vkr_recording_explain` ranks
  for its frame ([vkr_recording_analysis.c](../../runtime/src/assets/vkr_recording_analysis.c)).
  `rec.explain {spike|frame}` and `vkr_rec explain (--spike N | --frame F)`
  answer one frame against its typical frame (the 19-frame window median
  for frame time, recording medians for stages, GPU time and passes): CPU
  stages and passes by excess, engine events, platform signals, and causes
  ranked by excess of at least 0.25 ms (stages, passes, the GPU frame a wait
  stalled on, timed events), then conditions (serious thermal level,
  critical memory pressure, major page faults, GPU or log errors, missed
  presents), else `unknown`. Pass rows borrowed from a frame within ±8 for
  context never count as a cause. The editor's
  Performance window draws it with `vkr_ui_plot`,
  including a GPU-by-pass chart over the frames with GPU times, and lists
  spikes and passes in column tables (`editor_table.c`: resizable, sortable
  columns, row selection, virtual rows) under the charts, the charts, the
  lists and the two tables divided by split bars the designer drags; a pass
  row isolates its pass. Every chart marks the selected frame and the frame
  under the pointer on any chart. A click selects a frame, and the frame
  inspector beside the charts shows its time, spike and cause, CPU stages,
  the slowest GPU passes of the nearest frame with GPU times, counters,
  memory and nearby markers, steps by frame or spike, and Watch seeks the
  playback there (opening it). A drag across a chart zooms every chart and
  the frame-time cards to its frames until Show all. The stage legend hides
  stages, and a Series row charts its series under the memory charts.
  Analyze reads and analyzes on a job worker as well
  (`vkr_editor_recording_perf_poll` adopts the result): the window shows
  the read with its elapsed time and Cancel, or a header chip while another
  recording stays shown, and `rec.analyze` and `rec.compare` answer once
  the read is done. A given-up read finishes on its worker and is freed
  without waiting for it.

## Consequences

- Recording costs two bounded copies per frame on the frame-loop thread; the
  writer ran 6.8 µs per frame in the Bistro A/B. Tier 1 adds about 35 pass
  rows (12 bytes each) per Bistro frame: a 244-frame Bistro recording took
  65 KB compressed. On tiled, every 8th frame also pays for the pass stamps
  (about 0.5 ms on Bistro), so timing comparisons use Tier 0.
- Recording did not raise `frame.wall` in A/B runs on Bistro: the GPU-bound
  orbit (23 ms), a 640×360 orbit paced by the display, and a CPU-bound
  offscreen still view (2.4 to 2.7 ms) with and without the render thread.
  On the CPU-bound view it made frames 0.12 to 0.23 ms shorter: the writer
  thread keeps the M1's cores clocked up, which hides its own small cost
  (docs/proposals/recordings.md, R1).
- A seek reproduces the recorded camera exactly: a scripted agent-channel
  check found 0 m and 0° error at four frames on Bistro.
- A recording made by another build reads only if its dictionary's frame and
  camera layouts match; playback refuses others with a reason.
- Thumbnails cost the main thread 0.24 ms per capture (sampling a
  1275×700 Bistro Scene to 256×141) and 0.04 ms to start the write thread.
  The editor polls and releases its captures through the runtime, which
  runs both at frame completion while the render thread renders a frame
  (ADR-082): before, each poll waited for that thread, which on Vulkan
  added about 3 ms per thumbnail (`cpu.update` 12 ms against 0.7 ms), and
  on Metal the frames around a capture ran 0.5 to 0.8 ms longer. After it,
  in a 4.6 s headless Bistro recording (M1 Pro, Metal, Release) with 9
  thumbnails, the 35 frames from one before to five after each capture had
  a median `frame.wall` of 5.46 ms and `cpu.update` of 0.17 ms, against
  5.49 and 0.17 ms on the other 445. The renderer grows its readback memory on the first
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

CPU zones, GPU pass starts, the Perfetto export and rolling recordings
(`rec.start` `rolling`, `rec.save_last`, Keep last 30 s) extend recordings;
[ADR-126](126-cpu-zones-and-gpu-pass-timeline.md) owns them.

## Revisit when

Play recording (R3) adds world state, or frame metrics (R2) add render-thread
records, which bring the recorder frame number join described in the proposal.
- Segments, 2026-10-19, M1 Pro, Release: `run_recording_tests` round-trips
  segments with notes holding quotes, line breaks and UTF-8, keeps them by
  first frame, edits a title or a note alone, refuses an empty title,
  reads a version 1 file's ranges as segments, and checks
  `vkr_recording_segment_stats` on a synthetic recording (cut to the
  recording, an invalid frame left out, budget misses, the worst frame and
  one spike inside). On a headless Bistro editor a Tier 1 recording of 1,233
  frames took three segments through `rec.segment`, which kept them by
  first frame, edited a note by title and refused an empty title and an
  unknown segment; `rec.segments` and `vkr_rec segments` gave the same
  medians (7.02, 7.11 and 7.10 ms), and `rec.frames` and `vkr_rec frames`
  with a segment answered its 201 and 241 rows. By clicks: the Segments
  list showed the three; a row opened its panel; typing in the note and
  Cmd+Enter saved it; Focus opened the timeline on frames 680 to 920 of a
  700 to 900 segment and looped them; two presses of the ruler at frames
  1,000 and 1,100 opened a new segment whose typed title Enter saved.
