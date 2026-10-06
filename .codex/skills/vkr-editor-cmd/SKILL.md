---
name: vkr-editor-cmd
description: Drive the VKR editor by text through its Cmd bar and expression evaluator, including scripted --exec runs whose results are read from [cmd] stdout lines. Use to inspect or change editor/scene state or to verify editor behavior without pixel input.
---

# VKR editor Cmd

The vocabulary, expression grammar and data members are defined in
[ADR-075](../../../docs/adr/075-editor-cmd-bar-and-evaluator.md). Read it before
writing a script; do not guess member names.

## Run a script

1. Build with `./build_editor.sh Release`. Use Bistro
   (`--scene assets/scenes/bistro.scene.json`) for every scene-based run.
2. Isolate editor state so the run cannot change the user's layout, settings
   or workspace: set `HOME`, `VKR_EDITOR_LAYOUT_PATH` and
   `VKR_GRAPHICS_SETTINGS_PATH` to paths under `.scratch/`, and unset
   `MTL_DEBUG_LAYER`, `MTL_SHADER_VALIDATION` and `VK_INSTANCE_LAYERS`.
3. Pass `--headless` unless the task needs the visible window: the editor
   then opens no window, takes no focus, and quits when the script ends,
   discarding unsaved edits. Start the script with `wait.scene`. Set
   `VKR_AUTOCLOSE_SECONDS` (for example `180`) as a tighter backstop than the
   600 s headless default. A windowed run ends with `quit discard` and needs
   `VKR_AUTOCLOSE_SECONDS`, because a refused `quit` leaves it running.

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  HOME="$PWD/.scratch/cmd/home" \
  VKR_EDITOR_LAYOUT_PATH="$PWD/.scratch/cmd/layout.json" \
  VKR_GRAPHICS_SETTINGS_PATH="$PWD/.scratch/cmd/graphics.json" \
  VKR_AUTOCLOSE_SECONDS=180 \
  ./build_release/editor/vkr_editor --headless \
  --scene assets/scenes/bistro.scene.json \
  --exec 'wait.scene; select Sun; sel.light.intensity' \
  > .scratch/cmd/run.log 2>&1
grep -o '\[cmd\][^[]*' .scratch/cmd/run.log
```

A project run replaces `--scene` with `--project <uuid>`, plus
`--workspace <dir>` and `--scene-id <uuid>` when needed; without `--scene` or
`--project`, the project launcher shows and no statement runs. Headless output proves editor
state, not window resize, DPI, input or presentation.

Other process output can share a line with a result, so extract records with
`grep -o` as above rather than anchoring at line start.

## Agent channel

[ADR-084](../../../docs/adr/084-agent-channel-and-level-design-toolkit.md)
defines the socket, its operations and the `vkr_mcp` adapter. Use it when a
task needs batches, entity creation, structured results or captures. Start the
headless editor as above with `--agent-socket "$PWD/.scratch/<task>/editor.sock"`
and an `--exec` of `wait.scene; wait 3`; it stays open while a client is
connected. Send one JSON request per line
(`{"v":1,"id":1,"op":"ops.list"}`) and read one response line per request;
the first request should be `{"op":"cmd","args":{"line":"wait.scene"}}`.
Through MCP, run `build_release/tools/vkr_mcp --socket <path>` and send MCP
2026-07-28 requests with the version in `_meta`. Captures land in
`$TMPDIR/vkr/captures/` (`%TEMP%\vkr\captures` on Windows); read the PNG to
see the result. Windows uses the same AF_UNIX socket, but Python's `socket`
module there has no `AF_UNIX`, so script it through `vkr_mcp`.

## Read results

Each statement prints `[cmd] > <statement>` and then `[cmd] <result>` or
`[cmd] error: <message>`. A statement with no message (for example `undo`)
prints only its echo. Treat any `error:` as a failed step and stop relying on
later state that depended on it.

Statements run one per frame and apply after that frame's UI build, so a read
reflects every earlier statement. An expression that assigns scene data goes
through the edit journal: it validates, can be undone with `undo`, and makes
the scene unsaved, so the closing `quit` needs `discard` unless you ran
`scene.save` deliberately. Do not save edits to tracked scene files unless the
task asks for it.

## Limits

The evaluator has no loops, user functions or file access, and cannot create
entities; `create`, `delete` and `component.*` make structural edits. Vector
literals have three components: a four-component color keeps its alpha.
Commands that start a Bakery job or a scene load (`scene.open`, `scene.add`,
`scene.primary`, `scene.instantiate`, `content.import`, and the others ADR-075
lists) hold the queue until the work settles and print
`[cmd] Settled after <s> s`; write the next statement directly, without a
timed `wait`. Save or discard edits before such a command, because an
unsaved-edits prompt does not hold the queue.
Mouse gestures such as dragging or right-clicking a Content item have no
statement: use `content.place`, `content.drop`, `content.move` and
`content.command`, which run the same actions, and report the gesture itself
as unverified. It does not capture images; use the harness (`vkr-harness`)
for pixel evidence. Results also reach the Console and the session log in
`<workspace>/logs/`; stdout is the record for scripts.
