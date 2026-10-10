---
status: partial
updated: 2026-10-10
authority: adr
---

# ADR-106: Collaborative editing session

## Status

Accepted (partial). Phase 4 of the
[network protocol proposal](../proposals/network-protocol.md) has its core:
one editor hosts a session over the transport of
[ADR-105](105-network-transport-and-asset-depot.md), other editors join it,
and every edit that changes a scene journal applies on every editor in the
host's order, terrain strokes as the samples they made on the host. A
Session window hosts and joins, and the Scene draws other editors' cameras
with their names, selections and gizmo drags in progress. Agents of every
editor share the host's claims, task board (with capabilities and slots)
and change feed, and their batches wait for review on every editor. Edits
that do not travel yet (listed under [Decision](#what-travels)), joining
from a depot commit and published files other than new terrains stay in
the proposal. Every check ran on one Windows host; macOS and cross-machine
runs are unverified.

## Context

The editor's journal ([vkr_scene_edit.h](../../runtime/src/renderer/systems/vkr_scene_edit.h))
records each edit with values in the editor's local `VkrEntityId`s, which
differ between editors. Edits reach the runtime through request slots that
the UI fills during its build: one `VkrSceneEditRequest` and one
`VkrSampleEditBatchRequest` per build
([vkr_sample_runtime.h](../../runtime/src/vkr_sample_runtime.h)). Gizmo drags
are the exception: the runtime moves the entity and records the journal
entry itself. Undo picks the most recent entry across every loaded
container's journal.

## Decision

### Topology and service

- [editor_session.h](../../editor/src/editor_session.h) is the editor's
  `collab` service (ID 4, version 1). The host binds a UDP port (7330 by
  default) with a fresh key pair; a participant connects with the host's
  public key and opens the service. Anyone with the address and key may
  join.
- Channel 0 is reliable and ordered: HELLO (name and scene digest),
  WELCOME, EDIT from a participant, RESULT for a refusal, APPLIED in
  session order and PEER_LEFT. Channel 1 is sequenced: PRESENCE at 10 Hz
  with the camera position, yaw and pitch and the selection's document id.
  The messages are hand-packed on the bit stream; the service's schema hash
  is a constant that names this layout.

### Edits on the wire

- [vkr_net_scene_edit.h](../../runtime/src/net/vkr_net_scene_edit.h) writes
  one edit: its action, gesture, the entities by document id
  (`VkrEntityRef` with the container's world id) or by the index of the
  batch edit that creates them, and its values through
  [vkr_net_type.h](../../runtime/src/net/vkr_net_type.h), which encodes a
  value by its type descriptor (exact float bits, varints, strings,
  entity refs; transient fields skipped; validated on read). A component
  type travels by its registered name.
- An edit body travels as aligned bytes after its header (single edit,
  batch, undo, redo, or a revert naming the batch by session sequence), so
  the host relays a participant's body unchanged.
- A creation gets its document id before it leaves the editor, so every
  editor gives the new entity the same id. A duplicate gets a seed instead:
  `vkr_scene_edit_duplicate` derives each copy's id from the seed and its
  original's id (SHA-256, as a version 4 UUID), so every editor makes the
  same copies under the same ids and names.

### What travels

APPLY of name, transform, visibility, the three lights and one world
component; CREATE, DUPLICATE, DELETE, REPARENT, component add, remove and
replace, and terrain ops; batches of those; undo, redo and batch reverts;
gizmo moves. Physics, collision layers, scene settings, partition edits and
scene or World loads are refused while a session runs, with a Console
warning or a batch result saying so.

### Terrain

A terrain op's result is floating-point work that can differ between
machines, so it travels as a request but applies everywhere as its result.
The op travels with its parameters, a stamp's image and a road's points
(raw arrays, 16-byte aligned so the reader borrows them). The host notes
the samples each terrain op of an edit may change
(`vkr_scene_edit_terrain_rect`), applies the op, then reads those samples'
heights and paint weights and appends them to APPLIED. Every other editor
writes them through `VKR_HEIGHTFIELD_OP_SAMPLES` in place of the op, in the
same journal path, so a stroke folds into one undo step there as on the
host. A small terrain an agent creates exists only in its editor's memory
until a save (`vkr_scene_terrain_stage`); the batch names it, and every
editor stages the same flat field before the batch applies unless the file
exists.

### Order and verification

- The session runs last in each UI build. It takes the build's journal edit
  requests out of the slots: the host queues its own and the participants'
  edits in arrival order; a participant sends its own to the host and
  queues the host's APPLIED edits. It then injects the queue's head into
  the free slots, one journal change per build, and none while a gizmo drag
  is pending, the simulation runs or a scene loads.
- The next build checks the change against the journals: a single edit by
  their revision, undo and redo by their cursors, a batch or revert by its
  result token. The host gives each applied edit the next session sequence,
  appends the APPLIED message to its history and sends the history to each
  participant; a refused participant edit gets a RESULT. A participant that
  cannot apply an APPLIED edit leaves the session with an error.
- Because every editor applies the same edits in the same order through the
  same journal calls, every journal mirrors the host's, and a shared undo
  undoes the same step everywhere. The host refuses an undo or redo past
  the steps of the session. A gesture keeps its identity per author, so a
  slider scrub still folds into one undo entry on every editor.
- A batch keeps its requester's token on the requesting editor, so an agent
  operation or Cmd statement receives its result after the round trip.
  Journal group IDs differ between editors; each editor maps session
  sequence to its own group, so author-scoped undo (`VKR-AGENT-0009`) and
  reverts name the same batch everywhere.
- Another author's single creation or duplicate applies as a batch of
  one, which leaves the local selection alone.

### Gizmo drags

The runtime reports each drag it records (`VkrSampleGizmoEdit`: the dragged
entity, its companions, the fields and the journal group) to the next build,
and `gizmo_edit_pending` while a drag has not recorded yet. The host orders
the drag as an APPLY or a batch of APPLY edits of the values the entities
now hold. A participant keeps its drag applied and forwards it; if the host
orders it next, the participant adopts it, and otherwise it undoes its
drags before applying the host's edits, which then include the drag in its
place. A drag whose fields cannot travel (physics) is undone.

### Joining

The joining editor must hold the host's session base: the same scenes as
the host had when it started hosting. HELLO carries a SHA-256 digest over
every entity with a document id in the loaded containers (container, id,
name, pose bits and parent id, sorted). On a match the host replays its
history from the first edit. The history keeps 64 MiB; past that, edits
every participant received leave and new joins are refused.

### Agent federation

Agents keep talking JSON to their own editor's socket
([ADR-084](084-agent-channel-and-level-design-toolkit.md)); the session
carries what they share.

- **Authors.** While a session runs, an agent's name becomes
  `<agent>@<editor>` (agent names cannot hold `@` and keep 31 bytes), so
  claims, tasks, the feed and author-scoped undo tell agents of different
  machines apart. EDIT and APPLIED carry a batch's author and label.
- **Claims and tasks belong to the host.** On a participant, `claims.set`,
  `claims.release`, `task.add`, `task.next` and `task.done` travel as ASK to
  the host, which runs them on its own tables and returns ANSWER; the
  operation waits for it (at most 1,800 builds). The host sends its claims
  and tasks as SHARED when they change and to each editor that joins.
  Participants show that copy in place of their own and keep it out of
  their claims file; leaving the session brings their own claims back.
  Each editor checks claims against its copy before and after a batch, as
  without a session.
- **Change feed.** Each editor records the agent batches other editors
  applied, with author, label and entities, beside its own events, and
  feeds claims that appear, move or leave in the host's copy. A batch its
  author reverts because it touched a claim still reads as applied, with
  no entities, in other editors' feeds.
- **Task board** ([editor_ops.c](../../editor/src/editor_ops.c), also
  without a session). `task.add` opens a task of a kind with a title, an
  optional region and the capabilities it `requires`; `task.next` gives an
  agent its assigned task or the oldest open task of the kinds it names
  whose requirements its capabilities cover: its editor's platform
  (`windows`, `macos`) and pipeline class (`desktop`, `tiled`;
  [ADR-087](087-gpu-class-graphics-pipelines.md)) and any it lists itself;
  `task.done` finishes the assignee's task as done or failed with a note;
  `task.list` reads the board. `task.slots` sets how many tasks an
  editor's agents may hold at once; the host gives an editor that holds
  that many no new task, so the rest go to editors with room. A full board
  (256) drops its oldest finished task.
- **Reviews.** EDIT and APPLIED carry whether a batch waits for review.
  Every editor lists every editor's review batches in its Agent changes
  window, by its own journal group. Reject reverts the batch, which the
  session carries to every editor; Accept sends REVIEWED with the batch's
  session sequence, and every editor drops it from review.

### Session window and Scene overlay

The Collaborative session window (View menu, Cmd `window session`) hosts
with a name and an address to listen on, or joins with an address and the
host's key, and while a session runs shows the address, the key with a
Copy button, and each peer in its colour with its camera position and a
Go to view button that moves the Scene camera to that peer's view. The
Scene draws each peer's camera as a small frustum with its name above it,
and its selection's bounds as a box, in the peer's colour. While an editor
drags a gizmo, its PRESENCE (every 33 ms instead of 100 ms) carries the
dragged entity and its world matrix, and other editors draw the entity's
box where the drag holds it, joined to where it rests, until the drag
records and travels as an edit. Slider scrubs and terrain strokes need no
preview: each step travels as an edit.

### Operations

`session.host`, `session.join`, `session.leave` and `session.status` join
the agent table ([editor_ops.c](../../editor/src/editor_ops.c)); the status
reports the mode, address, key, sequence, pending edits, last error, this
editor's digest and each peer's name, camera and selection. `task.add`,
`task.next`, `task.done` and `task.list` run the task board.

## Consequences

- Collaboration reuses the editor's own edit paths; the runtime gained only
  the gizmo report and `forced_result`, which answers a forwarded batch the
  host refused.
- A participant's structure and Details edits wait one round trip and one
  build on the host. Its gizmo drags show at once.
- The digest check makes joining strict: any unsaved host edit before
  hosting, or a different scene file, refuses the join.
- One injected edit per build bounds replay speed to the frame rate: a
  history of 10,000 edits takes about three minutes at 60 Hz.

## Alternatives considered

- **Journal results instead of requests** (the proposal's `JOURNAL_GROUP`):
  exact for floating-point operations such as terrain, but it needs the
  journal's payloads on the wire. Requests reuse the existing validation;
  terrain and brush geometry wait for results.
- **Per-author undo and optimistic local application**: edits would apply
  at once, but journals would diverge in order and a shared undo would
  differ between editors.
- **CRDT documents**: the scene is not a CRDT and the journal already gives
  a total order on one host.

## Evidence

Windows 10, Ryzen 5 2600, clang, `build_debug`, 2026-10-10.

- `vulkan_renderer_tester --suite net` passes, including
  `test_net_type_round_trip` (41 registered world types) and
  `test_net_scene_edit_replication` (edits written against one scene and
  applied to another whose local IDs differ; refusals; malformed input).
- `python tools/checks/check_editor_session.py --editor
  build_debug/editor/vkr_editor.exe`
  ([check_editor_session.py](../../tools/checks/check_editor_session.py))
  runs two headless editors on Bistro over loopback: the host creates,
  names and moves a cube before anyone joins; the guest joins (digest
  match), replays three edits, sees the cube at (1, 2, 3), creates, names
  and moves its own cube twice, undoes once, duplicates it (5,991 to 5,992
  entities) and has a physics edit refused. Both end at session sequence
  10 with equal digests (random document ids change them between runs);
  the host reads the guest's cube and its copy `GuestCube (1)` at
  (4, 5, 6). Passes.
- `python tools/checks/check_agent_federation.py --editor
  build_debug/editor/vkr_editor.exe --mcp build_debug/tools/vkr_mcp.exe`
  ([check_agent_federation.py](../../tools/checks/check_agent_federation.py))
  joins two headless Bistro editors and drives `mason` on the host and
  `painter` on the guest through `vkr_mcp`: the guest's agent adds a task
  that the host's agent takes (`mason@alpha`) and finishes; the host's
  agent cannot claim over `painter@beta`'s claim (`VKR-AGENT-0010`); each
  agent's creation inside the other's claim is reverted with
  `VKR-AGENT-0010`; each feed holds the other editor's applied batch with
  its entity; the guest's batch waits for review on the host until the
  guest accepts it; of two tasks that require the `tiled` and `desktop`
  classes, a third agent on the Windows host gets the `desktop` one; both
  editors end at sequence 7 with equal digests. Passes.
- A headless window capture of the host while the guest looks at the
  scene from (2, 2, 10) shows the Session window with the guest `beta`, its
  colour, position and Go to view button, and the guest's camera frustum
  in the Scene. The guest's selection box lay behind the window in that
  view and is not confirmed by a capture.
- `vulkan_renderer_tester --suite scene_edit` passes with the seeded
  duplicate, and `test_net_scene_edit_replication` duplicates a subtree on
  one scene and checks the other holds the same ids, names and parents.
- The federation check also creates a 64 m terrain on the host, raises it
  from the guest and smooths it from the host; `terrain.sample` over a
  21 x 21 grid reads the same heights on both editors (peak 2.61 m). With
  one slot, the guest editor's second agent gets no task while its first
  holds one, and a host agent takes the next. `vulkan_renderer_tester
  --suite heightfield` checks that SAMPLES copies a brush's heights and
  paint exactly; the codec test round-trips a terrain op with its image and
  road points.
- A second capture shows the guest's name above its camera.
- Not exercised: gizmo drags and their preview (no headless drag script),
  batch reverts across editors, history trimming, macOS, two machines.

## Revisit when

Physics or settings edits must travel, a session needs edits faster than
one per build, or joining must tolerate a host with unsaved edits
(snapshot transfer).
