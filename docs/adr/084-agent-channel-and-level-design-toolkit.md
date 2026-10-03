---
status: partial
updated: 2026-10-04
authority: adr
---
# ADR-084: Agent channel and level design toolkit

## Status

Accepted (partial). The agent channel (phase 0 of the
[level design toolkit](../proposals/level-design-toolkit.md)) is implemented.
Brushes, level checks, IO, terrain and population remain in that proposal
until they ship.

## Context

Level designers want LLM agents to work in the editor beside them. The Cmd bar
(ADR-075) gives agents text control, but a line holds 255 bytes, one statement
runs per frame, results are text lines, expressions cannot create entities,
and nothing returns an image. An agent that builds part of a level needs typed
operations, batches that undo as one step, structured results, a picture of
the result and a way for the designer to review its work.

## Decision

### Socket and messages

The editor listens on a per-user local socket and speaks newline-delimited
JSON ([editor_agent.c](../../editor/src/editor_agent.c)). On macOS and Linux it
creates `$TMPDIR/vkr` (`/tmp/vkr` without `TMPDIR`) with mode 0700, refuses a
directory another user owns or others can open, and binds
`editor-<uid>.sock` under a 0177 umask. When a live editor already answers
there, the next editor binds `editor-<uid>-<pid>.sock` and logs the path. A
socket file nobody answers is stale and is replaced. `--agent-socket <path>`
and `VKR_EDITOR_AGENT_SOCKET` choose the path; `--no-agent-socket` turns the
listener off. Windows has no listener yet and reports the channel as off.

The editor polls the nonblocking listener once per UI build, serves at most 4
clients, closes a client whose unfinished line passes 1 MiB, and queues at
most 64 requests. A request is
`{"v":1,"id":<number|string>,"op":"<name>","args":{...}}`; a response is
`{"v":1,"id":<same>,"ok":true,"result":{...}}` or
`{"v":1,"id":<same>,"ok":false,"error":{"code":"VKR-AGENT-NNNN","message":"..."}}`.
Requests from every client run one at a time in arrival order; a request whose
edits apply after the UI build, or whose capture renders later, answers in a
later build. A headless editor (ADR-075) stays open while a client is
connected or a request waits or runs, so a script only needs to outlast the
client's connection.

| Code | Meaning |
|---|---|
| `VKR-AGENT-0001` | Malformed JSON or message shape |
| `VKR-AGENT-0002` | Unknown operation |
| `VKR-AGENT-0003` | Invalid or missing argument |
| `VKR-AGENT-0004` | Entity or component not found, or an ambiguous name |
| `VKR-AGENT-0005` | The scene rejected an edit; the batch rolled back |
| `VKR-AGENT-0006` | No scene is loaded, the simulation runs, or another batch is in flight |
| `VKR-AGENT-0007` | Capture failed |
| `VKR-AGENT-0008` | A limit was exceeded |

### Operations

One table in [editor_ops.c](../../editor/src/editor_ops.c) defines each
operation's name, description, JSON Schema and handler; `ops.list` returns it,
and the MCP adapter builds its tools from that list. Operations run on the UI
thread during the editor's build.

| Operation | Purpose |
|---|---|
| `ops.list`, `editor.status` | The table; loaded containers, selection, simulation, view and pending changes |
| `scene.describe`, `entity.get`, `query.bounds` | Entities with ID, name, parent, component types, local pose and world bounds; one entity's components as descriptor JSON |
| `entity.create`, `entity.set`, `entity.delete`, `entity.parent` | Structure and pose; delete with `recursive` deletes descendants first |
| `component.add`, `component.set`, `component.remove` | Component values by descriptor property name, partial for `set` |
| `batch` | Several write operations as one journal group |
| `changes.list`, `changes.accept`, `changes.reject` | Review of agent edits |
| `undo`, `redo`, `cmd` | Cmd statements through the Cmd queue, returning the `[cmd]` lines they printed |
| `query.raycast` | First physics surface along a ray |
| `view.capture` | A PNG of the Scene or the whole window, optionally from another view, framed on an entity or box, with grid labels |

An entity argument is `"<world>:<index>:<generation>"`, a unique exact name,
or `"$k"` for the entity operation `k` of the same batch created. Component
values read and write through `vkr_type_read_json_document` and
`vkr_type_write_json`, so they use the same names, units and validation as
scene documents; `set` keeps every property it does not name. Rotations are
degrees XYZ. Lights are set through `component.set` and created through
`entity.create`; physics bodies are added through `cmd`.

### Batches and journal groups

Every write runs as a batch: one `VkrSampleEditBatchRequest` of at most 256
`VkrSampleEditBatchItem` edits, all in one container, that the runtime applies
after the UI build ([vkr_sample_runtime.c](../../runtime/src/vkr_sample_runtime.c)).
The editor validates every argument and component value before submission.
The runtime opens a journal group, applies the edits in order and resolves
`$k` references to the entities earlier creations returned. The first failed
edit undoes the group's applied entries, drops them and reports the failing
index with the journal status. `dry_run` validates without submitting. Edits
wait while the simulation runs, as Details edits do.

A journal group ([vkr_scene_edit.c](../../runtime/src/renderer/systems/vkr_scene_edit.c))
tags each entry appended between `vkr_scene_edit_group_begin` and
`vkr_scene_edit_group_end`. Undo and redo move over a whole group; a failure
inside one stops there, as between two entries. The journal holds 1,024
entries; eviction removes the oldest whole group, and a group past 512 entries
fails. `vkr_scene_edit_group_rollback` undoes and drops the open group.

### Review

A batch with `review` (the default) becomes a pending change: its group,
container, label and the entities it created or edited, held in memory by the
operation table. The Agent changes window (View menu, Cmd `window changes`)
lists pending changes with Focus, Reject and Accept, the Scene outlines their
entities' local bounds in orange through the editor's line overlay, and a
toast announces each new change. Accept removes the mark only. Reject calls
`vkr_scene_edit_group_revert`:

- an undone group only loses its redo entries;
- otherwise no later applied entry may name an entity the group created,
  deleted, edited or reparented, a current descendant of one, or a parent of
  one as its own parent; a later physics batch or collision-layer edit also
  refuses. The group's entries then revert in reverse order and leave the
  journal, and the redo entries above the cursor are dropped.

A refused reject names the conflicting entity. A change disappears when its
group leaves the journal, as after a scene reload or an undo followed by a new
edit.

### Captures

`view.capture` optionally switches the camera view and grid labels and frames
an entity's world bounds or a box (`VkrSampleViewRequest.frame_box`). After
four builds it asks the runtime for one `final_color` capture
(`VkrSampleCaptureRequest`). The runtime marks editor captures with the high
bit of the request id, lends the poll result to the next build and releases it
after that build; the harness owns the capture slot when it runs, and the
request then fails. The editor converts RGBA8, BGRA8 or half-float color to an
RGBA8 PNG of the Scene image rectangle, or of the whole window with `area`
`window`, writes it to `$TMPDIR/vkr/captures/`, keeps the newest 32, and
restores the previous view, grid labels and camera.

### MCP adapter

[vkr_mcp](../../tools/agent/vkr_mcp.c) is an MCP server over stdio that the
editor build produces and the editor distribution installs. It implements only
the [2026-07-28 revision](https://modelcontextprotocol.io/specification/2026-07-28/changelog):

- every request must carry `io.modelcontextprotocol/protocolVersion`
  `2026-07-28` in `_meta`; another version, a missing one or an `initialize`
  request returns `UnsupportedProtocolVersion` (-32022) with the supported
  versions;
- `server/discover` returns the version, the `tools` capability, usage
  instructions and the server identity;
- `tools/list` maps each operation to `vkr_<operation with dots as
  underscores>` in table order with `ttlMs` 60000 and `cacheScope` `private`;
- `tools/call` forwards the arguments to the editor and returns the result as
  `structuredContent` and as JSON text, an editor error with `isError`, and a
  capture's PNG also as image content;
- every result carries `resultType` `complete` and `serverInfo` in `_meta`;
  notifications are ignored and logs go to stderr.

The adapter reconnects once per call, so it survives an editor restart. It
links Bakery's JSON tree, which the `vkr_bakery_json` library now shares with
Bakery and the editor.

## Consequences

Agents and scripts reach every editor feature through typed operations, with
the Cmd vocabulary still available through `cmd`. One journal group per
request makes agent work undoable and reviewable as the unit the agent chose.
Reverting a group out of order is refused instead of guessed whenever a later
edit could depend on it. A capture needs a rendered frame, so it reports the
Scene as rendered, not what the editor shows while rendering is stopped.

## Alternatives considered

- Extending only the Cmd bar keeps text results, one statement per frame and
  no images.
- A chat panel inside the editor would tie it to one model provider and its
  keys; the socket lets any MCP client connect.
- Applying agent edits to a preview scene would need a second copy of every
  container; real edits with a review mark reuse the journal and the
  renderer.
- Supporting the 2025-11-25 handshake as well was declined by the owner.

## Revisit when

A Windows listener is needed, an agent needs notifications pushed to it, or
reviews must survive a scene reload.

## Evidence

- `./build_test.sh` suite `scene_edit` covers grouped undo and redo, rollback,
  out-of-order revert, its refusal for a dependent later entry, the group
  limit and whole-group eviction (2026-10-04, macOS Debug).
- Headless macOS Release runs on Bistro through the socket and through
  `vkr_mcp` (2026-10-04): status, describe, create, a batch with `$k`
  references, validation and rollback failures, out-of-order reject, undo and
  redo of a batch, `cmd`, a top orthographic and a perspective capture, the
  Agent changes window with Focus, Reject and Accept and the orange outlines
  in a whole-window capture, the -32022 answers to `initialize` and to an older
  version, and 21 tools from `tools/list`.
- Windows and native Vulkan are unverified.
