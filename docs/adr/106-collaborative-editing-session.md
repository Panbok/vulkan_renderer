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
host's order. Agents of every editor share the host's claims, task board
and change feed. Presence travels but no editor draws it yet. Live gesture
previews, edits that do not travel yet (listed under
[Decision](#what-travels)), capability-based task assignment, a Session
window and published assets stay in the proposal. Every check ran on one
Windows host; macOS and cross-machine runs are unverified.

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
  editor gives the new entity the same id.

### What travels

APPLY of name, transform, visibility, the three lights and one world
component; CREATE, DELETE, REPARENT and component add, remove and replace;
batches of those; undo, redo and batch reverts; gizmo moves. Duplicate (each
editor would draw fresh ids), terrain strokes, physics, collision layers,
scene settings, partition edits and scene or World loads are refused while a
session runs, with a Console warning or a batch result saying so.

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
- Another author's single creation applies as a batch of one, which leaves
  the local selection alone.

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
  without a session). `task.add` opens a task of a kind with a title and an
  optional region; `task.next` gives an agent its assigned task or the
  oldest open task of the kinds it names; `task.done` finishes the
  assignee's task as done or failed with a note; `task.list` reads the
  board. A full board (256) drops its oldest finished task.

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
  and moves its own cube twice, undoes once and has a duplicate refused.
  Both end at session sequence 9 with digest `c992abb1…` (random document
  ids change it between runs); the host reads the guest's cube at
  (4, 5, 6); the entity count stays 5,991 after the refused duplicate.
  Passes.
- `python tools/checks/check_agent_federation.py --editor
  build_debug/editor/vkr_editor.exe --mcp build_debug/tools/vkr_mcp.exe`
  ([check_agent_federation.py](../../tools/checks/check_agent_federation.py))
  joins two headless Bistro editors and drives `mason` on the host and
  `painter` on the guest through `vkr_mcp`: the guest's agent adds a task
  that the host's agent takes (`mason@alpha`) and finishes; the host's
  agent cannot claim over `painter@beta`'s claim (`VKR-AGENT-0010`); each
  agent's creation inside the other's claim is reverted with
  `VKR-AGENT-0010`; each feed holds the other editor's applied batch with
  its entity; both editors end at sequence 7 with equal digests. Passes.
- Not exercised: gizmo drags (no headless drag script), batch reverts across
  editors, history trimming, macOS, two machines.

## Revisit when

Terrain, brush or duplicate edits must travel (journal results), a session
needs edits faster than one per build, or joining must tolerate a host with
unsaved edits (snapshot transfer).
