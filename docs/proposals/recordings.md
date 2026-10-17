---
status: proposed
updated: 2026-10-17
authority: proposal
---
# Recordings: capture, playback and performance review

## Outcome

A user or an agent records a session in the editor, the app or a harness run.
A recording holds the camera, the player, the input, the world changes and,
optionally, frame metrics. Every sample has one recorder frame number. Anyone
can then:

- watch the session again in the editor and scrub to any frame;
- find a frame-time, VRAM or RAM spike and jump to the place, the player state
  and the world state of that frame;
- drive the current build with the recorded player actions, to check that a
  fix removes a defect;
- run a recorded range again as a matched Release benchmark;
- re-simulate a session exactly on the same build and machine, with a report
  of the first tick that diverges;
- read a short summary and query frames by text, without the editor.

The user and agents exchange recordings in both directions. An agent requests a
recording with steps and a question. The user records it, and the agent
analyzes it. Agent benchmarks are recordings, so the user reviews them in the
same viewer.

Phases R0 and R1 are implemented; [ADR-122](../adr/122-recordings.md) records
their contract. The rest of this document is the design of the remaining
phases. Code, operations and the command use
the word "recording"; "replay" already names the harness capture passes
(`vkr_harness_replay.c`), and "session" names collaborative editing
([ADR-106](../adr/106-collaborative-editing-session.md)).

## Owner decisions

The owner accepted every recommendation below on 2026-10-17. Each choice changes
scope, ownership or an accepted budget.

| # | Choice | Recommendation | Tradeoff |
|---|---|---|---|
| D1 | Playback model | **State first.** Watch applies recorded state. Drive and Re-simulate run gameplay code from recorded actions. | Watch works on any machine and seeks freely, but runs no gameplay code. Input-only replay is about 100× smaller but breaks on every build, script or platform change (research table). |
| D2 | Metric cost during a live recording | **Tier 0 live on both backends. Per-pass GPU timing live on Vulkan; on Metal only in sampled frames (1 in N, labeled) or in a perf re-run.** | Per-pass stamps on Metal cost about 0.5 ms per Bistro frame. A spike from streaming or simulation may not repeat in a re-run, so sampled frames keep some live detail. |
| D3 | Owner | **`runtime/src/recording/`**, used by the editor, the app, the player and the harness. The editor owns only the UI. | Packaged builds can record. The runtime gains one module. |
| D4 | Storage | **Workspace `local/recordings/` by default.** Share moves a recording to `projects/<id>/recordings/`, which the depot syncs. Harness and app runs write to their output directory. | Large files stay out of the depot unless a user shares them. |
| D5 | Re-simulation scope | **Same build on the same machine.** | Cross-machine determinism needs `JPH_CROSS_PLATFORM_DETERMINISTIC` (about 8% physics cost), no `-march=native` and no FMA contraction in gameplay code. |
| D6 | Harness integration | **The harness writes recordings, and a case can play one.** | One viewer serves user sessions and agent benchmarks. The harness gains a reader and a camera mode. |

## Current baseline

These code facts decide what the design reuses and what it adds.

### Frame, threads and simulation

- `vkr_application_host_run` takes dt from the clock, clamped to 0.1 s
  (`runtime/src/application/vkr_application_host.c:135-147`). Only the harness
  sets `fixed_delta_seconds`.
- The scene simulation runs fixed 1/60 s ticks, at most 8 per update
  (`runtime/src/renderer/systems/vkr_scene_simulation.c:121-155`). Rendering
  interpolates with alpha = accumulator / dt.
- With the render thread, the update of frame N+1 runs before frame N
  completes (`runtime/src/application/vkr_standard_scene_runtime.c:2463`).
  The renderer frame number is assigned on the render thread at acquisition
  (`:2705`). A skipped frame gets no number (`:2797`).
- Metrics frames are published on the render thread
  (`vkr_metrics_end_frame`, `lib/src/core/vkr_metrics.c:215`). A pinned
  target buffer makes publication skip that frame.
- Play is a transport action (`vkr_standard_scene_runtime_apply_transport_action`,
  `runtime/src/vkr_sample_runtime.c:6762-6810`). When simulation stops, the
  scene dt is 0 and the fly camera moves the camera. Origin rebase runs only
  while simulation runs (`:4320-4328`). Reset restores the rebase and the
  authored state (`:6801`).
- The game camera replaces the editor camera only when a script publishes one
  through `vkr_set_camera` (`:3955-3975`). The engine has no player concept;
  the FPS module keeps its player in untyped `vkr_state_add` data.
- Partition cells follow the camera (`sample_stream_sources`, `:4375-4388`).
  Cell documents load synchronously on the main thread within a 2 ms
  wall-time budget (`:4118-4134`). Model loads and script tasks finish
  asynchronously.
- The scene keeps a per-frame list of entities whose transforms changed
  (`render_dirty_entities`, `runtime/src/renderer/systems/vkr_scene_system.h:1515`).

### Input

- `input_observe` allows one observer with no replacement
  (`runtime/src/core/input.h:434-436`). The script host holds it
  (`vkr_script_host.c:2267`). Transitions carry wall-clock time.
- The engine has no action layer. The FPS module defines a serializable
  command, `FpsCommand {tick, sequence, action, pressed, x, y}`
  (`scripts/fps/src/fps_input.h`). It is the model for a recorded action.

### Determinism hazards

These hazards rule out input-only playback as the primary model:

- Scripts read wall time through `VkrScriptFrame.now` and `sdk_time`. The FPS
  module sets its scene step from wall time.
- `vkr_rand_*` wraps the global, unseeded `rand()`
  (`lib/src/math/vkr_math.c:3-17`).
- The wall-time cell budget and asynchronous model and task completion change
  which tick an entity appears on. `VkrEntityId` values can differ between
  runs.
- Jolt multithreads above a body count. `JPH_CROSS_PLATFORM_DETERMINISTIC` is
  off, and gameplay C code builds with `-march=native`.
- Nothing saves or restores runtime ECS, physics or script state
  ([Game networking](game-networking.md), "Current baseline").
- Hot reload changes module code and tolerant type layouts during a session
  ([ADR-079](../adr/079-c-script-modules.md)).

### Metrics

- The `VkrMetrics` registry ([ADR-015](../adr/015-metrics-module.md)) has
  512 typed slots with lock-free writers. Its measured always-on overhead is a
  median of +0.24%.
- Frame scopes cover `frame.wall`, `cpu.update`, `cpu.render_prepare`,
  `cpu.render_submit`, `cpu.render_wait` and others
  (`vkr_standard_scene_runtime.c:176-218`). Physics, scripts, animation and
  culling sit inside `cpu.update` with no own scopes.
- Per-pass GPU timing has up to 416 rows per frame. Rows arrive 1–2 frames late
  with `gpu_source_frame_index` (`renderer/src/vkr_renderer_metrics.h:42-73`).
  The pass table is overwritten each frame.
- On Metal, per-pass stamps split the shared compute encoder: about 10 µs idle
  per pass, about 0.5 ms per Bistro frame (vkr-performance skill). Vulkan
  stamps do not split encoders.
- `gpu.submission` (whole-frame GPU time) is off by default
  (`VKR_GPU_SUBMISSION_TIMING`,
  `runtime/src/vkr_sample_runtime_config.c:254-255`). Metal reads command
  buffer feedback. Vulkan writes two timestamps in each frame's command
  buffer: the first waits for earlier queue work, the second precedes the
  present blit (`vkr_vulkan_renderer.c`).
- Metal reports device `currentAllocatedSize` and the recommended working set.
  Vulkan reports `VK_EXT_memory_budget` usage and budget summed over the
  device-local heaps, sampled at each submission; without the extension both
  metrics read unsupported. GPU owner rows exist. GPU peaks are lifetime
  high-water marks.
- `vkr_platform_get_process_resident_memory` reads RSS
  (`lib/src/platform/vkr_platform.h:150`). It feeds only the HUD text.
- Asset loads record events. Pipeline creation, shader loads, cell loads and
  hot reloads record none.

### Editor UI and agent channel

- The editor UI is the immediate-mode grid UI
  ([ADR-027](../adr/027-immediate-mode-grid-ui.md)). It has no plot, stacked
  bar, timeline, column table or in-panel splitter. A frame holds at most 2048
  widget nodes. Each world line costs one `vkr_ui_bezier` node, and world lines
  share `PHYSICS_LINE_MAX` (512) with physics shapes, lights and session lines.
- Peer cameras already draw as frustums with names (`vkr_editor_session_lines`).
- Agent operations are rows in `s_ops[]` (`editor/src/editor_ops.c`) over the
  socket, MCP and `--exec`
  ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md)). Reads run
  during Play. The batch edit path refuses edits while `simulation_running` is
  set (`editor_ops.c:5666`).
- The harness camera supports at most 64 keys
  (`tools/harness/vkr_harness.h:44`). `VkrHarnessCamera` is part of the
  persisted summary layout.

### Library and wire services

| Need | Present | Missing |
|---|---|---|
| Threads, atomics | `core/vkr_threads.h`, `core/vkr_atomic.h` | Thread QoS |
| Queue between threads | `containers/queue.h` (not lock-free) | A single-producer, single-consumer byte ring |
| Compression | zstd decode in the runtime (`vkr_ktx_read`); the full zstd in the editor, the depot tool and the tester | A codec interface shared by the depot and the recorder |
| File output | `file_open`, `file_write` (synchronous) | Nothing; the writer thread hides latency |
| Quantized encoding | `net/src/vkr_bitstream.h`, `net/src/vkr_wire.h` | Change masks and delta coding (also missing for networking) |

## What the research shows

| Lesson | Evidence | Rule here |
|---|---|---|
| Input-only replays break on version, mod and platform changes. | Doom, StarCraft II, Factorio, Halo campaign films | D1 |
| Systems with good scrubbing store state keyframes. | Unreal replay checkpoints, Braid, Halo 3 Theater | A keyframe every 2 s |
| A keyframe assembled over several frames mixes states. | Unreal `CheckpointSaveMaxMSPerFrame` | Copy one tick's state at once |
| Gameplay state and profiling on one timeline explain a spike. | Unreal Rewind Debugger with Insights | One recorder frame number for every stream |
| Per-thread buffers drained by one worker cost a few ns per event. | Tracy (2.25 ns per zone), Unreal Trace, Perfetto | One ring per producer; drops counted |
| Self-describing schemas keep old traces readable. | Unreal Trace | Field dictionary in the header and in update chunks |
| Per-subsystem hashes find the first divergent tick. | Factorio, Age of Empires, Jolt "Check Determinism" | Hash chunks for Re-simulate |
| A spike is relative to its neighbors, not only to a budget. | Nsight Systems stutter rule | Neighbor rule plus budget |
| A summary with links to the worst moments is the entry point. | PIX timing summary | `summary.json` and the Overview tab |
| Thumbnails make a frame recognizable without seeking. | Tracy FrameImage (320×180, BC1, 28.8 KB) | Thumbnails at a fixed rate |
| Agents need small precomputed answers and capped queries. | Tracy and Perfetto MCP servers | Capped summary and queries |

## Concepts

| Term | Meaning |
|---|---|
| Recording | A bundle on disk: stream, summary, annotations, thumbnails. |
| Recorder frame | The number the recorder gives each frame at update start. Every record carries it. |
| Capture tier | The metric set a recording collects: none, Tier 0 or Tier 1. |
| Watch | Playback that applies recorded state. Gameplay code does not run. |
| Drive | Playback that feeds recorded actions to the current build. Divergence is reported, not fatal. |
| Re-simulate | Playback that feeds recorded input on the same build and stops at the first hash mismatch. |
| Perf re-run | Watch or Drive of a range at a fixed delta, as a benchmark under harness rules. |
| Marker | A named point or range from a user, an agent or the engine. |
| Recording request | A request for a recording, with steps, a question, a stop rule and a lifecycle. |

## Playback modes

| Mode | Rendering cost | Simulation CPU | Finds | Seek | Runs on |
|---|---|---|---|---|---|
| Watch | Same views and poses | Not run | Visual defects; gameplay defects as recorded | Any frame | Any machine with the same content |
| Drive | Same views if the player follows the path | Yes, current build | Whether a fix removes a defect; perf of the current build | From the start or a recorded checkpoint pose | Any machine |
| Re-simulate | Same | Yes, same build | Logic defects, exactly | From the start | Same build and machine (D5) |
| Perf re-run | Matched, capture-free | Drive only | Before/after timing | A range | Release, harness rules |

A recording made with simulation stopped holds only the camera, the view state
and edits. Watch and perf re-runs reproduce it exactly.

## Architecture

### Frame identity

The recorder frame number is the join key for every stream. It solves the
overlap of update N+1 with render N:

```text
 main thread             render thread
 -----------             -------------
 update start:           acquisition:
  seq = next()            renderer no.
  tick records,           -> seq map
  frame part              render part
  (seq)                   (seq, no.)
     |                        |
     v                        v
 sim ring                 render ring
     \                       /
      v                     v
   writer: join by seq;
   GPU rows by renderer no.
   -> seq; skipped frames
   recorded as skipped
```

1. `seq` is assigned at update start and travels in the frame handoff,
   `VkrStandardSceneRuntimeFrame`.
2. The update part holds the ticks, input, actions, state changes, the render
   camera and the player record.
3. The render part comes from frame acquisition and completion. It holds the
   renderer frame number, `render_ns` and the Tier 0 values published by
   `vkr_metrics_end_frame`.
4. The GPU part arrives 1–2 frames later. The writer maps its renderer frame
   number back to `seq`.
5. A skipped frame keeps its `seq` with a skipped flag.

### Ownership

| Part | Owner | Lifetime |
|---|---|---|
| `VkrSpscRing` | `lib/src/containers/` | One per producer thread, preallocated at recorder start, released at stop. |
| `VkrCodec` | `lib/src/core/vkr_codec.h`, moved from the depot | The editor and the tester pass the zstd they carry; without a codec, chunks are stored raw. |
| Input observer slots | `runtime/src/core/input` | `input_observe` gains a fixed list of two observers; the script host keeps its slot. |
| Stream format writer and reader | `runtime/src/assets/vkr_recording_format.c` in `vkr_asset_formats`, shared with the tools | Writer per recording; reader per open file. |
| Recorder, Watch player, Drive driver | `runtime/src/recording/` (D3) | One recorder per runtime; one player per open recording. |
| Library, overlay, player UI, performance view | `editor/src/editor_recording*.c` | Editor UI state on `VkrEditorUi`. |
| Plot, timeline, table, splitter widgets | `runtime/src/renderer/systems/vkr_ui_system` | Shared UI primitives. |
| `vkr_rec` command | `tools/recording/` | Offline reader; no GPU and no editor. |
| Harness recording mode | `tools/harness/` (D6) | Per case run. |

Recorder memory uses the allocator tag `recording`, so RAM metrics report the
recorder's own bytes separately.

### Frame cost contract

The main and render threads only copy bounded records:

- **Per tick:** input transitions and actions from the observer slot; the
  player record; spawn and destroy events; the transforms of entities in
  `render_dirty_entities` that the recorder tracks. A catch-up frame with six
  ticks copies six times, so the gate measures spike frames separately.
- **Per frame (main):** the render camera, the transport state, display dt,
  simulation time and interpolation alpha.
- **Per frame (render):** a fixed list of about 40 Tier 0 metric ids, copied
  when the metrics frame is published. The recorder never pins snapshots.
- **On GPU completion:** the frame's GPU values with the renderer frame number;
  pass rows only in Tier 1.

The producers never compress, diff, allocate, lock, write files or wait on the
GPU. A full droppable lane (metrics, thumbnails) drops the record and counts
it. A full reserved lane (frame, input, state) writes a gap event; the
timeline shows the gap. The writer runs at utility QoS, appends without fsync
during the recording, and syncs at stop. A process crash keeps every chunk
already written.

Size estimates, to be measured in R1 and R3:

| Content | Raw | Compressed (estimate) |
|---|---|---|
| Frame part plus 40 Tier 0 values, 60 fps | about 0.4 KB per frame, 1.4 MB/min | 0.3–0.5 MB/min |
| 500 moving entities, changed only, quantized | up to 18 MB/min | 2–6 MB/min |
| Keyframe of 500 entities every 2 s | 20 KB each | 0.3 MB/min |
| Thumbnails at 2 Hz, 320×180 BC1 | 3.5 MB/min | 3.5 MB/min |

### File format

A recording is a directory bundle, `<name>.vkrrec/`. The Library shows one
item. An agent reads `summary.json` with no tool.

| File | Contents |
|---|---|
| `stream.vkrs` | Header, field dictionary, typed chunks, index footer. |
| `summary.json` | At most 8 KB: identity, statistics, top 20 spikes with context, markers, request id. Rewritten every 10 s and at stop. |
| `annotations.json` | Notes, named ranges, review state. Edited without rewriting the stream. |
| `thumbs.vkrt` | BC1 thumbnails by recorder frame. |
| `start/` | The scene edit overlay at record start, for recordings that contain edits. |
| `request.json` | The request that started the recording, if any. |

`stream.vkrs` is append-only:

- The header holds the format version, the field dictionary (name, type, unit,
  quantization) and the content identity.
- Each chunk has a type, a recorder frame range, a size and a CRC. A reader
  skips unknown chunk types.
- A dictionary-update chunk and an identity event record a hot reload or a
  settings change during the recording.
- Each 1 s metric chunk ends with per-second minimum, maximum and mean values.
  Plots of long recordings and agent queries read these levels first.
- The footer index maps frame ranges to offsets. After a crash, the reader
  rebuilds it from chunk headers.
- State chunks hold quantized changes. A keyframe every 2 s holds the full
  recorded state. Change masks and delta coding are new code beside `vkr_wire`.

The content identity holds the build id and commit, the backend and pipeline
class, the GPU and RAM, the scene id and start revision, asset content hashes,
script module hashes, graphics settings and the resolution. Playback lists
every difference before it starts.

### Recorded state

The recorder uses explicit hooks, not shared descriptors:

- **Player:** `vkr_rec_player(pose, velocity, state)` once per tick from the
  module that owns the player. Without it, the recorder records the render
  camera only.
- **Tags and events:** `vkr_rec_tag(key, value)` for state such as
  `weapon=rifle`; `vkr_rec_event(name, payload)` for gameplay events.
- **Module state:** `vkr_rec_state(...)` for visuals that Watch must show.
- **Engine state:** transforms of moving entities (authored by `VkrEntityRef`,
  runtime spawns by a recording-local id with their model or prefab), animation
  player state, script-changed light state, spawn and destroy, cell loads and
  unloads, origin rebase shifts, time of day, graphics settings changes.
- **Edits:** R1 records an `edit` marker at each frame where a loaded
  scene's journal changed, and playback shows the scene as it is now. R3
  replays edits with the world state stream: the `start/` overlay at record
  start plus edits encoded with `vkr_net_scene_edit`; an edit kind that
  encoding rejects is a gap event.
- **Place:** each spike context derives the room name from rooms and portals
  ([ADR-115](../adr/115-rooms-and-portals.md)) and the cell from the partition,
  so places do not depend on module tags.

Module state that a module does not record does not appear in Watch. The
thumbnails still show it. A later convergence with the replication descriptors
in [Game networking](game-networking.md) can replace the explicit hooks.

### Watch owns the world

Watch runs as a new transport state, **Playback**:

1. Entering Playback saves the editor camera, loads the bundle's `start/`
   overlay if the recording has edits, and stops scripts and physics with
   scene dt 0.
2. The live streamer is off. Recorded cell events load and unload cells. In
   free and follow camera modes, the recorded camera still drives streaming.
3. Recorded rebase shifts are applied through `sample_rebase_shift`.
4. Recorded state is applied at the playhead tick; runtime spawns become
   transient entities.
5. Exiting Playback uses the transport reset path, which restores the authored
   scene and the rebase.
6. A `playback_active` guard joins `simulation_running` in the batch edit path,
   and collaborative publishing is suppressed.

Seeking applies the nearest keyframe and then the changes up to the target.
Cell loads after a long seek show a "settling" indicator in the viewport.

### Drive and re-simulation

Drive feeds the recorded actions to the current build:

- It starts from the recording's start state or from a recorded checkpoint
  pose for the player.
- It does not stop on divergence. It reports the distance between the recorded
  and the driven player path and the first tick past a tolerance.
- An optional follow controller steers movement actions back toward the
  recorded path, so a long session stays comparable after a fix.

Re-simulate adds these gameplay changes:

- The script host gives modules a frame clock from the recording; wall time
  stays out of simulation.
- The SDK gets a seeded generator per module. `vkr_rand_*` stays out of
  simulation code.
- The FPS module takes its scene step from the host.
- Cell loads, model joins and task completions apply at their recorded ticks.
- Physics runs with a fixed worker count, with sorted callbacks.
- Each tick writes a hash per subsystem: transforms, bodies, script state,
  generators.

The first mismatch stops Re-simulate. The report names the frame, the tick, the
subsystem and the entities that differ.

### Metrics and capture tiers

| Tier | Contents | Cost rule |
|---|---|---|
| None | Camera, player, input, state, events | Overhead gate |
| Tier 0 | Frame wall time, CPU scopes, whole-frame GPU time, draw and visibility counts, RSS and `phys_footprint` with their peaks, VRAM driver bytes and budget, GPU owner rows and allocator tags when they change, recorder self-cost | Overhead gate, measured with whole-frame GPU timing on; its instrumentation fingerprint is recorded |
| Tier 1 | Tier 0 plus per-pass CPU and GPU times | Vulkan: live. Metal: sampled frames (1 in N, labeled) or a perf re-run (D2) |
| Thumbnails | 320×180 BC1 at 2 Hz, downscaled before the UI composite | Measured in R5 |

Whole-frame GPU time and `VK_EXT_memory_budget` on Vulkan are implemented.
Tier 0 still needs these additions:

1. RSS, `phys_footprint`, `resident_size_max` and `ledger_phys_footprint_peak`
   as metrics on the writer thread at 10 Hz. The peak counters catch short
   peaks between samples.
2. Scopes for physics, scripts, animation and culling.
3. Events for pipeline creation, shader loads, cell loads and hot reloads.

On Apple silicon, GPU memory is part of `phys_footprint`. The view shows RAM
and VRAM as separate, labeled lines and never adds them.

## User interface

Four surfaces share one recording state. The mockups published with this
proposal show each one.

### Record overlay

- A REC pill at the top left of the viewport shows the time, the size, the
  tier and the drop count. It turns amber when the recorder drops data.
- A Record button next to Play has a popup for the tier, thumbnails and the
  stop rule.
- During Play the mouse is captured and keys reach the game. A marker is one
  modifier keypress; its note is typed later in the timeline. "Flag bug" is
  another single keypress. The exact keys are checked against `s_keymap` and
  game input in R1.
- An active request shows its steps at the top right. A "next step" key marks
  the step as done and drops a step marker.
- The editor UI and the overlay are not in thumbnails.

### Library

A dock panel:

- A sortable table: thumbnail, name, scene, date, length, author (user or
  agent), tier, spikes, p95, status (recording, ready, divergent, content
  changed, requested).
- Filters by scene, author, tag and status; a search over names, tags and
  notes.
- A detail pane: statistics, worst spikes, notes, content check, and the
  actions Watch, Analyze, Drive, Re-simulate, Perf re-run, Compare, Share to
  project, Export, Reveal, Delete and Copy agent prompt.
- A Requests tab: open requests with their question and steps, and Start or
  Decline with a reason. Start asks before it closes a scene with unsaved
  edits.
- Empty states explain how to record, and link to the record popup.

### Player

- A timeline docks under the viewport: a ruler with zoom and pan, a playhead,
  and lanes for markers, engine events, tags, frame time and memory.
- Transport: play, pause, step frame, step tick, speed from 0.1× to 4×, loop a
  range, next spike, next marker.
- Camera modes: recorded camera, free camera, follow player.
- The viewport draws the recorded path colored by frame time, a ghost frustum
  at the playhead and pins at markers.
- Details shows the recorded state of the selected entity at the playhead.
- Edits are refused during Playback.

### Performance view

An Analyze workbench lays out the viewport, the timeline and these tabs:

- **Overview** opens first: frame time p50, p95, p99, max and the time-based
  1% low; budget misses; hitch count; VRAM and RAM peak and average; the worst
  spikes with thumbnails.
- **Frames:** frame-time bars colored against the target, budget lines at
  16.6 ms and 33.3 ms and an overview brush; stacked CPU subsystem bars with a
  "difference from the median frame" toggle; VRAM and RAM lines, each on its
  own axis, with peak markers.
- **Spikes:** frame, time, duration, ratio to neighbors, the subsystem or
  owner that grew most, cell loads and pipeline creations nearby, the room and
  the tags. Each row has Perf re-run with Tier 1.
- **Map:** a top-down path colored by frame time. A click selects the
  nearest frames.
- **Compare:** two recordings aligned by marker, by distance along the path or
  by time. Frame alignment applies only to re-runs of one recording.

The frame inspector shows the thumbnail, the pose, the player record, tags,
events within 10 frames and the largest differences from the median frame.
Its actions are Seek here, Perf re-run with Tier 1 and Copy agent reference.

Keys: N and P for the next and previous spike, M for the median frame, W for
the worst frame, `[` and `]` for the range, Enter to seek.

### UI primitives to add

[Editor UI extensions](editor-ui-extensions.md) asks for a named interaction
before a new component. These are the interactions:

| Primitive | Interaction |
|---|---|
| Plot canvas | Bars, lines and stacked areas in one node; hover, click and brush in data coordinates |
| Polyline node | Many segments in one node; also draws the world path within the line budget |
| Timeline | Ruler, zoom, pan, draggable playhead, lanes, markers, range selection |
| Column table | Resizable, sortable columns, row selection, virtual rows |
| Splitter in a panel | Resizable panes inside a dock panel |

## Agent workflow

### Operations

New `s_ops[]` rows under `rec.`. Reads and transport changes do not use the
batch edit path, so they run during Play and Playback.

| Operation | Purpose |
|---|---|
| `rec.start`, `rec.stop` | Start with name, tier, reason and request id; stop |
| `rec.mark` | Marker with a note |
| `rec.list` | Filter by scene, author, request and status |
| `rec.open`, `rec.play`, `rec.pause`, `rec.seek` | Seek by frame, time, marker or spike rank |
| `rec.summary`, `rec.spikes`, `rec.frames` | Capped JSON; truncation is stated |
| `rec.drive`, `rec.resim` | Drive or re-simulate; return the divergence report |
| `rec.rerun` | Perf re-run of a range with a tier |
| `rec.compare` | Statistics difference with an alignment mode |
| `rec.request.create`, `.list`, `.cancel`, `.wait` | Request lifecycle |
| `rec.annotations`, `rec.note`, `rec.range`, `rec.review` | Read and change notes, named ranges and the review state |
| `rec.export` | CSV or a harness case |

`view.capture` at a sought frame gives an agent the picture.

### Offline command

`vkr_rec` reads a bundle without the editor or the GPU. This matters on a
16 GB machine shared with other sessions.

```sh
vkr_rec summary <bundle>
vkr_rec spikes <bundle> --top 10
vkr_rec frames <bundle> --range 1200:1260 --fields frame_ms,gpu_ms,vram,pos
vkr_rec diff <a> <b> --align marker
vkr_rec verify <bundle>
```

### Recording requests

A request is a file in the recordings directory's `requests/` (the
workspace's `local/recordings/requests/` in a project):

| Field | Meaning |
|---|---|
| `id`, `created`, `expires` | Identity and lifetime |
| `owner` | The requesting agent and session |
| `status` | pending, accepted, declined, recorded, cancelled, expired |
| `scene`, `start_pose`, `steps`, `tier`, `stop_seconds` | What to record |
| `question` | What the recording must answer |
| `recording`, `decline_reason` | Result |

```text
 agent: rec.request.create
        |
        v
 user: Requests tab, toast;
       Start or Decline
        |
        v
 user: scene opens, camera
       placed, steps shown;
       plays, marks, stops
        |
        v
 agent: rec.request.wait
        returns the bundle;
        summary, spikes, seek,
        capture, Drive a fix
```

The agent answers from `summary.json` and capped queries. After a fix, the
agent drives the current build with the recording and compares the results.
The reverse direction: "Copy agent prompt" puts the bundle path, the selected
range, the markers and the notes on the clipboard.

### Agent benchmarks for the user

With D6, each harness repetition writes a recording. The user opens an agent's
before and after runs in Compare and sees the charts, spikes and paths the
agent reported.

## Phases

| Phase | Delivers | Main evidence |
|---|---|---|
| R0 (implemented 2026-10-17) | `VkrSpscRing` and `VkrCodec` in `lib/`; input observer slots; stream format, writer, reader; recorder; `vkr_rec summary` and `verify` | `run_recording_tests`: ring wrap and threads, format round-trip, truncated-file recovery, recorder drop and gap accounting |
| R1 (partial, 2026-10-17) | Simulation-stopped recording (camera, view state, edits with `start/`), tier None; Playback transport state; Watch; minimal timeline (ruler, playhead, markers); record overlay; Library with one thumbnail per recording. Implemented: camera and frame recording, markers, edit markers, Playback with exact seek, the playback bar with marker ticks, the REC indicator and Record button, the Recordings window with thumbnails, `rec.*` operations. Moved to R3: replaying edits with `start/`. Open: the authoritative overhead gate | Overhead gate with tier None; Bistro Watch matches recorded poses (passed: seek error 0 m and 0 degrees at four probe frames, `.scratch/recordings/r1_check.py`). Non-authoritative overhead A/B on 2026-10-17: `tiled_bistro_plaza_timing_sun_contact_on` with `local-offscreen-gpu-repeated`, 5 repetitions each, recording off then on through `VKR_RECORDING_DIRECTORY`: `frame.wall` per-repetition medians 17.744–17.860 ms off, 17.729–17.755 ms on (p50 difference −54 µs, inside the spread); writer thread 6.8 µs per frame; no dropped or lost records. Not authoritative: local profile, dirty tree, and the profile's transmission warmup metric reads 0 on the tiled pipeline |
| R2 (partial, 2026-10-17) | Recorder frame join; Tier 0 additions; plot, table and splitter primitives; performance view; `summary.json`; read operations and `vkr_rec` queries. Implemented: per-frame Tier 0 metrics joined to the recorded frame at publication on the frame-loop thread, whole-frame GPU time on both backends while an editor recording runs (Vulkan: two timestamps per frame), driver memory usage and budget on Vulkan through `VK_EXT_memory_budget`, sampled process memory, `cpu.simulation` and `cpu.physics`; the `vkr_ui_plot` primitive; the Performance window; the analysis in `summary.json`; `vkr_rec spikes` and `frames`; `rec.analyze`. Tier 1, moved here from R5: pass rows of every rendered frame, placed on the recorded frame that rendered them, with GPU pass times every frame on desktop and in one frame of 8 on tiled (labeled in the frame record and the view); per-pass statistics ranked by total time and spike pass causes in the analysis; `vkr_rec passes` and `pass.gpu:`/`pass.cpu:` frame columns; the GPU-by-pass chart and pass table in the Performance window; `rec.start` `tier`. `gpu.submission` is filed on the recorded frame that rendered it, not the one that published it (the published value lags by the frames in flight: 3 on Vulkan, 1–2 on Metal); on Metal, a Bistro Tier 1 recording of 302 frames correlated it with the same frame's 12 largest GPU pass times at r = 0.986 over 38 timed frames, against 0.79 to 0.90 one or two frames apart. Open: pipeline, shader, cell and hot-reload events, `rec.summary`, `rec.spikes` and `rec.frames`, column table and splitter primitives | Tier 0 overhead gate; spike-join gate; Vulkan rows on Windows (passed 2026-10-17, RX 6700 XT, Release: a headless editor Bistro Tier 1 recording of 242 frames had GPU pass times on all 238 frames with pass rows, the last 4 still in flight at stop; `gpu.submission` on 238 frames, mean 12.27 ms, 0.17 ms above the pass sum of the frame 3 back; driver usage 5.83–6.29 GB of an 11.21 GB budget; a 62-frame run under the Khronos validation layer reported nothing) |
| R3 (partial, 2026-10-17) | Play recording: player hook, tags, events, dirty-list state, actions; Watch of the world, with edits replayed from `start/`; Drive with path distance. Implemented (R3a): entity poses recorded from a per-scene changed-entity list and replayed in Watch as evaluated overrides; a Bistro check moved an entity over 60 frames while recording and Watch showed its recorded positions at four frames and restored it at close, with 5,985 of 5,986 keyframe entities found already in place. Implemented (R3b): `vkr_rec_player`, `vkr_rec_tag` and `vkr_rec_event` (SDK 10), the FPS player recorded per tick, player positions in the analysis, and the scoped Bistro Play case `bistro_play_local` (a clip slab and a Player Start); a recorded Play session there tracked the player along 7.75 m and a turn at up to 5.0 m/s. Implemented (R3c): `vkr_rec_action` and Drive (`rec.drive`, the card's Drive button); on the scoped case a recorded walk and turn drove back on the same build over 250 ticks with a maximum distance of 0.000 m, and with the player's move speed lowered to 4 m/s it left 0.25 m at recorded frame 193 (tick 77) and ended 2.41 m apart. Implemented (R3d): edit replay by stepping the edit journals with undo and redo instead of a `start/` overlay (ADR-122); on Bistro an entity created and another moved three times while recording were absent and at their start at frame 0, as recorded at the last frame, and back to the live state at close, and a later edit made the recording report its edits unavailable. Implemented (R3e): script spawns named by spawn id, with their model paths and lifetimes, and Watch stand-ins that load those models and take their recorded node poses as their skin's pose; on the scoped case the FPS mannequin, recorded in third person, showed in Watch mid-stride at its recorded place. Implemented (R3f): the route in the Scene during Watch, colored by frame time and dimmed ahead of the playhead, through a new `vkr_ui_path` widget. Implemented (R3g): shape stand-ins for spawns without a model, Drive starting from the recording's first player pose, and the follow controller; on the scoped case a recording begun after a 5 m walk drove back at 0.000 m over 170 ticks, and with the move speed lowered to 4 m/s the path parted by 1.95 m without follow and 0.12 m with it. Open: occlusion of the route by geometry | A scoped Bistro Play case; Drive after a code change |
| R4 (partial, 2026-10-17) | Recording requests with lifecycle; annotations; Copy agent prompt. Implemented (R4a): request files, `rec.request.create`, `.list`, `.cancel` and `.wait`, request cards in the Recordings window with Start and Decline, Cmd `request.start` and `request.decline`, the steps over the Scene with "Next request step", the stop rule and expiry. The bundle takes the request's id instead of holding `request.json`. On the scoped Bistro Play case (`.scratch/recordings/r4_check.py`) a request waited pending through a 1 s timeout, a second was declined with its reason, a third was refused for another scene and cancelled, an id with `../` was refused, and the first, started through Cmd, placed the camera 0.000 m from its start pose, took both step markers, stopped itself after 4 s and came back from `rec.request.wait` recorded with its bundle and `summary.json`. Implemented (R4b): `annotations.json` with notes, named ranges and a review state; `rec.annotations`, `rec.note`, `rec.range` and `rec.review`; notes, ranges and review in `rec.list`, `vkr_rec summary` and the cards; the playback bar's note field, Range marking and review toggle; Copy agent prompt with markers, notes, ranges, review, the request's question and the playhead. `run_recording_tests` round-trips notes and ranges with quotes, line breaks and UTF-8, cuts text on a UTF-8 boundary and refuses a foreign file; on the scoped Bistro Play case (`.scratch/recordings/r4b_check.py`) two notes, a reversed range and a flag came back in frame order and the five malformed calls were refused. Clicking a note or range on the bar opens its panel: the note's text and Delete, or the range's name to rename and Delete; `rec.range` takes `rename`. | An agent request recorded by the user and answered by text |
| R5 | Perf re-run; harness recording mode; Compare; thumbnails at 2 Hz. Tier 1 moved to R2 and is implemented | Matched Release A/B of a re-run on Bistro |
| R6 | Re-simulate: frame clock, per-module generators, recorded async completions, hashes, divergence report | Same-machine re-simulation without divergence |
| Later | Rolling buffer ("save last N minutes"), world snapshots for seeking in Re-simulate, map heat grid, Chrome trace export, convergence with networking descriptors | — |

## Acceptance evidence

Every gate uses Bistro, as `AGENTS.md` requires.

1. **Overhead.** Run the Bistro glide case in windowed Release with recording
   off and on, per tier. Report p50, p95, p99, max, the hitch count and the
   recorder's main-thread and render-thread time per frame in ns. All stay
   within the matched noise of the authority profile. Repeat in the scoped
   Bistro Play case with moving bodies, and on Vulkan before claiming the
   desktop pipeline. Thread QoS cannot be A/B tested under the harness on this
   Mac, so also measure one windowed run outside it.
2. **Fidelity.** Watch reproduces the recorded camera to 0.1 mm and 0.01°.
   Captures at three frames match the thumbnails within the snapshot
   tolerance.
3. **Seek.** Applying recorded state for any frame takes under 250 ms on the
   M1 Pro. Cell settling is shown and timed separately.
4. **Join.** With the render thread on, a forced GPU spike (a debug pass with a
   fixed cost) appears on the frame that submitted it.
5. **Crash.** Killing the process during a recording leaves a bundle that
   `vkr_rec verify` reads up to its last written chunk.
6. **Drive.** A recorded Play session driven after a change to movement code
   reports the path distance and the first tick past tolerance.
7. **Re-simulate.** A scripted session re-simulates without divergence on the
   same machine. An injected `rand()` call in a module produces a report that
   names that module's hash and tick.
8. **Agent loop.** An agent creates a request, the user records it, and the
   agent answers the question from `summary.json` and capped queries alone.

Bistro has no collision, so R3, R6 and gates 6 and 7 use the scoped Bistro
Play case `assets/scenes/fixtures/bistro_play_local.scene.json`: Bistro with
a collision-only slab under the street and a Player Start. Moving bodies for
those gates are added through the agent channel; do not substitute another
scene.

## Risks

| Risk | Mitigation |
|---|---|
| Catch-up frames copy several ticks on exactly the frames that already spike. | Gate measures spike frames; per-tick records stay small and fixed. |
| Many moving entities fill the state lane. | Changed entities only; gap events; lane size shown in the overlay. |
| Content changes make Watch show wrong objects. | Content check before playback; per-entity mismatch list. |
| Module visuals are missing in Watch. | `vkr_rec_state`; thumbnails show the original. |
| Recordings fill the disk. | Size shown while recording; a workspace quota that removes the oldest unshared, unannotated recordings first. |
| Just-written files are slow to open on this machine (15–30 ms each). | One stream file and one thumbnail file per recording; no file per chunk. |
| Per-pass GPU stamps change Metal timing. | D2: sampled frames are labeled; full detail comes from a re-run. |

## Sources

- Replays: [Doom demos](https://doomwiki.org/wiki/Demo),
  [Age of Empires networking](https://www.gamedeveloper.com/programming/1500-archers-on-a-28-8-network-programming-in-age-of-empires-and-beyond),
  [Factorio replays](https://www.factorio.com/blog/post/fff-36) and
  [desync reports](https://factorio.com/blog/post/fff-55),
  [Braid rewind](https://gdcvault.com/play/1012210/The-Implementation-of-Rewind-in),
  [Unreal replay streamers](https://dev.epicgames.com/documentation/en-us/unreal-engine/demonetdriver-and-streamers-in-unreal-engine),
  [Overwatch replay technology](https://www.gdcvault.com/play/1024053/Replay-Technology-in-Overwatch-Kill),
  [snapshot compression](https://gafferongames.com/post/snapshot_compression/),
  [floating-point determinism](https://gafferongames.com/post/floating_point_determinism/),
  [Jolt determinism](https://jrouwe.github.io/JoltPhysics/index.html).
- State and profiling on one timeline:
  [Rewind Debugger](https://dev.epicgames.com/documentation/unreal-engine/animation-rewind-debugger-in-unreal-engine),
  [Unreal Insights](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-insights-reference-in-unreal-engine-5),
  [Chaos Visual Debugger](https://dev.epicgames.com/documentation/en-us/unreal-engine/chaos-visual-debugger-user-guide?application_version=5.5),
  [Visual Logger](https://dev.epicgames.com/documentation/en-us/unreal-engine/visual-logger-in-unreal-engine).
- Profilers: [Tracy manual](https://raw.githubusercontent.com/wolfpld/tracy/master/manual/tracy.tex),
  [PIX spike tutorial](https://developer.microsoft.com/en-us/games/articles/2024/02/tutorial-using-pix-to-diagnose-spikes-in-cpu-frame-time/),
  [Nsight Systems stutter analysis](https://developer.nvidia.com/blog/using-nsight-systems-for-fixing-stutters-in-games/),
  [Unity Profile Analyzer](https://docs.unity3d.com/Packages/com.unity.performance.profile-analyzer@1.1/manual/frame-range-selection.html),
  [Perfetto shared-memory ABI](https://perfetto.dev/docs/design-docs/api-and-abi),
  [CapFrameX metrics](https://www.capframex.com/blog/post/Explanation%20of%20different%20performance%20metrics).
- GPU timing and memory: [Vulkan queries](https://docs.vulkan.org/spec/latest/chapters/queries.html),
  [calibrated timestamps](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_calibrated_timestamps.html),
  [VK_EXT_memory_budget](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html),
  [Metal GPU counters](https://developer.apple.com/videos/play/tech-talks/10001/).
