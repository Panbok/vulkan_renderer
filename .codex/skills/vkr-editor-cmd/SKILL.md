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
3. Start the script with `wait.scene` and end it with `quit discard`. Also set
   `VKR_AUTOCLOSE_SECONDS` (for example `180`) as a backstop: a refused or
   failed `quit` otherwise leaves the editor running.

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  HOME="$PWD/.scratch/cmd/home" \
  VKR_EDITOR_LAYOUT_PATH="$PWD/.scratch/cmd/layout.json" \
  VKR_GRAPHICS_SETTINGS_PATH="$PWD/.scratch/cmd/graphics.json" \
  VKR_AUTOCLOSE_SECONDS=180 \
  ./build_release/editor/vkr_editor --scene assets/scenes/bistro.scene.json \
  --exec 'wait.scene; select Sun; sel.light.intensity; quit discard' \
  > .scratch/cmd/run.log 2>&1
grep -o '\[cmd\][^[]*' .scratch/cmd/run.log
```

Other process output can share a line with a result, so extract records with
`grep -o` as above rather than anchoring at line start.

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
entities or edit physics bodies. It does not capture images; use the harness
(`vkr-harness`) for pixel evidence. Release builds without
`VKR_EDITOR_LOGGING` do not show these results in the Console; stdout is the
record.
