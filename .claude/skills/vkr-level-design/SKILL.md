---
name: vkr-level-design
description: Build, change or check VKR levels (brush blockouts, stairs and corridors, terrain, population, entity IO) through the editor's agent channel, alone or beside other agents, with plans run by script and proved by queries, floor maps, lint and capture sheets. Use for level-building work that goes through editor operations; use vkr-editor-cmd to start or drive the editor itself.
---

# VKR level design through the agent channel

[ADR-084](../../../docs/adr/084-agent-channel-and-level-design-toolkit.md)
defines every operation, and `ops.list` returns their schemas. This skill
sets the order of work and the checks that prove a level.

## Start

1. Connect to the user's running editor (default socket), or start a
   headless one with `--agent-socket` as `vkr-editor-cmd` describes. Never
   quit, reload or save the user's editor or scene unless asked.
2. Name yourself in every request: the plan's `agent`, `level_run.py
   --agent` or `vkr_mcp --agent`. Your changes then carry your name and
   `undo` takes only your batches.
3. Read before writing: `editor.status`, `claims.list`, `changes.feed` with
   `after` 0, and `scene.describe` with a `region` around your area.

## Plan, then apply

- Write the level as data first: spaces, their sizes and floor heights,
  links and openings, from the metrics below. Compute every coordinate with
  a script from that data, relative to your region's origin; do not type
  coordinate lists by hand.
- Claim your region with a margin (`claims.set`) before the first write.
  `VKR-AGENT-0010` means a write touched another agent's claim: build
  outside it or ask that agent. Release your claims when you finish.
- Prefer intent operations: `blockout.room`, `blockout.corridor`,
  `brush.stairs`, `blockout.doorway`, `brush.snap` and `entity.place`
  (`on`, `inside`, `against`). Use raw `brush.box` corners for simple
  pieces only. A turned piece takes `rotation`; a shape no primitive makes
  is one `brush.planes` or `brush.hull`, not `entity.create` faces.
- Send one `batch` with a `label` per unit, such as a room and its doors.
  A batch holds 2,048 edits and a box brush costs 7 (the brush and its six
  faces). Send a large batch with `dry_run` first.
- Later operations of a batch can edit, cut, place against or group what
  earlier ones made, by name or `$k`. Room parts carry the room's name:
  cut a door into `north/Store/Wall South +Z` in the room's own batch.
- Give names your prefix (`north/Store`) so name references stay unique.
- With `parent`, corners, points and planes are in the parent's space:
  subtract the parent's position from world coordinates first.
- Pick materials from the brush palette, `assets/materials/dev/dev_<name>.mt`
  (`ls` the folder); `dev_light` glows, for ceiling panels.
- Detail breaks routes: props in a walkway, a seat (0.45 m is above a step)
  or a railing across a ladder top. Keep walkways 1 m wide and rerun every
  route query after each detail pass.

## Gameplay pieces

- Doors, lifts and platforms: `mover.create` on their brushes; `hinge` makes
  a swinging door. Make a lift a timed loop (`loop` true, `wait` seconds at
  each end) unless the spec asks for a call button; a trigger brush under a
  mover moves with it.
- Buttons: `component.add` `button` on a solid or clip brush, or on a parent
  up to 8 levels above it, within 2 m of the player's eyes (1.6 m above its
  feet). Wire `io.connect` from `on_pressed` to the mover's `open`.
- Ladders: a trigger brush with `component.add` `fps_ladder`. The player
  climbs while a 0.6 m ray forward from its chest (1 m above its feet) hits
  the trigger, so make the trigger about 0.6 m deep in front of the climbed
  face and reach at least 1 m above the upper floor. Leave the railing open
  where the ladder meets that floor.
- Railings: a 1 m `clip` brush stops the player and the checks; draw the
  rails and posts as `visual` brushes.
- Hiding an object in the editor removes it from placement and picks only:
  collision, level checks and the game still have it.

## Verify, cheapest first

Put the checks in the same `level_run.py` plan as the writes, each with
`expect` entries from the spec, so one run reports pass or fail:

1. Numbers: `query.raycast` (its `collider` names the brush hit),
   `terrain.sample` and `query.bounds` against the spec.
2. Routes: `query.reachable` for every path the spec requires; it answers
   `crouch` when a route needs crouching and `ladders` when it climbs any
   (a ladder is a trigger brush with `fps_ladder`).
3. `level.map` for each storey, the region's top below its ceilings and at
   most 200 cells a side: `#` walls, `n` gaps too narrow, `,` out of reach,
   `c` passable crouched.
4. `level.lint` in tiles of 76 m or less (0.3 m cells); expect no issue of
   a kind the spec forbids.
5. One `view.capture` sheet (`views`, `max_width` 768) with labelled
   `marks` at doors, spawns and stairs. Each mark answers its pixel and
   `hidden` (collision between the camera and the point); judge look and
   scale from the picture, not positions. In the designer's windowed
   editor a capture waits until they stop working; capture-heavy work
   belongs in a headless editor.

6. Walk the routes with the player (`vkr-editor-cmd`, Drive the player):
   steer toward points from `query.reachable` paths, or your own where it
   answers false, and report where the player stops making progress or
   falls more than a step below its route. The walk sees what the grid
   checks miss: collision a brush lost, a prop in a doorway, a mover that
   did not open.
   - Ask `query.reachable` for `points` 512: the default 32 points cut
     corners the grid walked around, as past a stair's stringer. Drop the
     points that lie within 0.2 m of the line between their neighbours, and
     count a point reached within 0.3 m across and 0.5 m in height.
   - A rise between neighbouring points beyond a step is a ladder: walk to
     the lower point, then hold forward facing the upper one until the
     player stands there.
   - When the player stalls, plan again from where it stands before you
     report a failure. Wait for a mover by polling `query.bounds` of its
     moving part.

Reads wait for rebuilds; repeat a read that answers `settled` false. Only
collision counts in checks: Bistro's own meshes have none, so checks see
brushes, blockout shapes, terrain and colliders.

## Fix and coordinate

- To drop one of your earlier batches, `changes.reject` it; a refusal names
  the later change that blocks it. Never reject or edit another agent's
  work unless the user asks.
- Read `changes.feed` from your last `next` to see what others changed near
  you, instead of paging `scene.describe`. To wait for them, add `wait`
  (up to 60 s): the read answers when something changes and holds no one
  else's requests. In Claude Code, hear of them without asking instead:
  run the `vkr_mcp --watch` command from the vkr server's instructions with
  the Monitor tool (`timeout_ms` 1800000) and arm it again when it ends;
  each line is another author's batch, review or claim. On Windows Monitor
  needs Git Bash (`CLAUDE_CODE_GIT_BASH_PATH` when Claude Code cannot find
  it).
- The designer reviews every change: report each change id with what it
  built.

## Metrics

Defaults for the player capsule (radius 0.3 m, height 1.8 m, step 0.35 m,
slope 45°); prefer the spec's own values when it gives them.

| Element | Default | What lint flags |
|---|---|---|
| Door | 1.2 m wide, 2.2 m high | under 0.6 m wide or 1.8 m high |
| Corridor | at least 1.5 m wide, 2.5 m high | under 0.6 m wide |
| Stairs | steps of 0.1875 m (`brush.stairs` default) | a step over 0.35 m |
| Ramp | at most 30° | over 45° |
| Ceiling | at least 2.6 m above the floor | under 1.8 m |
| Drop | at most 4 m stays one walkable area | an edge with no floor within 4 m |
| Crawl space | 1.15 to 1.7 m clear (crouched capsule 1.08 m) | `crouch_only`, by design |

## level_run.py

`scripts/level_run.py [--socket PATH] [--agent NAME] plan.json` runs a plan's
steps through `vkr_mcp` (`build_release/tools/vkr_mcp`) and prints one line
per step, `level.map` rows in full and capture paths:

```json
{"agent": "north-wing",
 "steps": [
  {"op": "claims.set", "args": {"region": {"min": [0, -1, 0], "max": [30, 8, 20]}}},
  {"op": "batch", "name": "rooms", "args": {"label": "rooms", "ops": [...]}},
  {"op": "query.raycast", "name": "door open",
   "args": {"origin": [4, 1, 3], "direction": [1, 0, 0], "max_distance": 8},
   "expect": [{"path": "hit", "equals": false}]}]}
```

An expectation reads a dotted `path` (keys and list indices) from the
result and tests `equals`, `between`, `at_least`, `at_most`, `count` or
`contains`. A step that fails stops the run unless it sets `"continue":
true`. Exit 0 means every step passed, 1 that one failed, and 2 a plan,
usage or connection error.

## Keep the level

Save only when the task keeps the level. Run Cmd `scene.save`, then confirm
`editor.status` answers `scene.unsaved` false before your last client
disconnects: a headless editor quits once its `--exec` script has ended and
no client is connected, discarding unsaved edits. A scene file holds at most
65,536 created objects (a box brush is 7) and a save over that is refused;
build a larger level in world partition cells.

## Done when

The plan's report passes every expectation the spec implies (routes,
openings, heights and no forbidden lint kinds), you inspected one capture
sheet, your claims are released, and your report names each change id.
