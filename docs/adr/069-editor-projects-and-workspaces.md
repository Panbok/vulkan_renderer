---
status: implemented
updated: 2026-09-12
authority: adr
---

# ADR-069: Editor Projects and workspaces

## Status

Accepted. The managed editor uses the workspace, project and scene owners below.
Windows empty-project creation, reopening and Back navigation have been checked;
broader Windows interaction and filesystem verification remain outstanding.
Selected native Metal workflows passed as recorded below;
this does not establish backend parity, long-session stability or performance.

## Context

The editor previously opened a fixed scene and persisted preferences outside a
project. Creating portable work requires an explicit owner for scene membership,
imported dependencies, preferences and generated artifacts. Compiled renderer
tables also need a different lifetime from content that can be loaded at runtime.

## Decision

The normal [editor application](../../editor/src/editor_application.c) starts
with a Projects chooser and no world scene. The user selects a workspace directory;
its `.vkreditor` child owns managed data. A machine-local locator remembers the
chosen directory. Explicit `--scene` remains the legacy scene entry point.
Creating or opening an empty project enters the editor without loading a scene
or automatically opening the Scenes modal. Creating project resources is a
project job, not scene loading. Default and cleared hierarchy search text remain
valid empty strings when project settings are saved.

[Project storage](../../editor/src/editor_project_store.c) owns a versioned
workspace manifest and UUID-named project directories. Each project has a
`project.json` with a display name, ordered scene references, asset inventory,
default font, editor settings and scene-specific editor recall. Each scene has
its own `scenes/<id>/scene.json` version 3, inventory and artifact directories.
Display names can change independently of IDs. Paths in manifests resolve within
the declared owner; imports copy their dependency closure into the workspace.
The editor holds an OS-backed workspace write lease. A second editor can inspect
the workspace without publishing changes or previews into it.

[Projects UI](../../editor/src/editor_projects.c) owns project selection, scene
creation/import, progress, Save/Discard/Cancel decisions and preference saves.
Preparation, validation and resolution use a compact centered Scene loader:
status text, a short progress bar and a small Cancel button. Its backdrop blurs
the retained Scene image and darkens it without blurring UI. With no retained
image, first startup has a dark backdrop. Cancelled or failed preparation offers a single centered Retry
button; diagnostic tooltips and Bakery retain details. These controls do not
capture the whole editor. The loader
disappears when the runtime accepts the asynchronous scene request, so streamed
scene content remains visible. Project/scene switching is disabled until the
active preparation or activation finishes.
Projects and Scenes use the shared navigation-button style on the left before Metrics,
with a distinct background highlight.
Graphics controls, runtime input/presentation preferences, layout, panel state,
Console filters, Bakery defaults and Content preferences belong to the project.
Viewport and hierarchy recall are keyed by scene within the project. Lights,
environment, probes, fog, authored cameras and edit overrides remain scene data.
Native window/display placement and the workspace locator remain machine-local.

The [job process](../../tools/editor_project_jobs.py) validates managed scene
version 3 and lowers typed asset references into explicit runtime scene and font
inputs. Runtime loaders do not discover workspaces. New source imports use
explicit cooker destinations. Cooked-only imports preserve `.vkb` geometry bytes
and use an adjacent versioned material-remap document; the
[cooked mesh decoder](../../runtime/src/assets/vkr_mesh_cooked_decode.c) requires
a complete exact-reference map when that document exists. Managed scene source
identity and cooked source-node identities preserve edit bindings across path
changes. Importing a legacy overlay validates its original source identity before
remapping it to managed content.

Jobs stage copied inputs and fresh build revisions, validate dependencies and
check the current manifest fingerprint before replacing its JSON. Artifact
publication precedes the manifest reference. Cancellation cleans uncommitted
staging; a revision transferred to publication ownership may remain unreferenced
rather than risk deleting committed data. Scene Save writes an immutable overlay
revision and atomically updates its manifest reference with conflict detection.
The editor stops and joins workspace writers before releasing the write lease.
Scene transitions pass explicit paths to the sample runtime, which unloads the
current world before loading another through existing resource retirement.

Editor font bundles and runtime bake outputs live in `.vkreditor`. Renderer
source tables that require recompilation retain their renderer/tool ownership.
Legacy `assets/` remains readable for migration and explicit legacy execution.
Runtime dependency resolution uses the owning file for `./` and `../` references;
bare legacy paths preserve their previous repository-relative meaning. No
process-wide filesystem root changes during project selection. Renderer graph
configuration remains a build/install resource with an explicit configured path.
Managed harness children locate renderer bootstrap files independently of their
workspace asset closure.

Settings serialization overlays known object members and preserves unknown nested
members; arrays remain serializer-owned values. The document parser enforces the
1 MiB byte limit and depth limit with temporary dynamically sized token storage,
so a large valid scene inventory does not fail a smaller fixed token ceiling.
Managed names retain UTF-8 even when a glyph is unavailable. The shipped managed
UI system face supplies its available Latin, Greek and Cyrillic glyphs through
U+052F; on-demand glyph rasterization and IME composition are separate work.

[Native dialogs](../../runtime/src/platform/vkr_file_dialog.h) return selected,
cancelled or error results with caller-released UTF-8 paths on the UI thread.
Managed editor native close requests remain pending until the dirty-state flow
resolves them. [Content](../../editor/src/editor_content.c) indexes manifests on
refresh and virtualizes filtered asset cards. Its layout follows the navigation,
sources, search and asset-view organization described in Epic's
[Content Browser interface](https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-interface-in-unreal-engine).
The left sources tree groups Scene, Project and Editor inventories into logical
asset-type folders; it does not expose build-revision directories. Breadcrumbs
navigate upward, the compact toolbar exposes Import, refresh and Back/Forward/Up
navigation, and cards show
type-color strips, names and textual state. A right Details panel contains
selection actions. Sources hide below 620 points; Details hide below 1040 points
or 240 points of height and can be toggled independently. Search, type/scope,
sort, card size and Details visibility persist in project preferences.
Sources and asset cards have draggable vertical scrollbars. When the asset grid
has keyboard focus, arrow keys select adjacent cards; Home/End and Page Up/Down
move through the list while keeping the selected card visible.
`Commands > Show Content` adds the panel to retained layouts, and Ctrl+Space
toggles it. New layouts place Content beside Console. Textures use bounded CPU previews;
materials use an isolated canonical-sphere
[preview job](../../tools/editor_material_preview.py). Meshes and fonts use vector
icons. Preview jobs and GPU texture requests have independent cancellation and
retirement owners; the browser retains at most 64 texture requests and prunes its
generated disk cache through a bounded worker operation.

## Verification and limits

Native window allocation initializes pending-close state before the first frame.
Mouse capture requests are idempotent: modal frames can request release repeatedly
without restoring or warping an already-free pointer. Startup regression checks
used `build_editor_run.sh` in Debug (ASan/UBSan) and Release, plus native chooser
text input and file-picker cancellation.

Normal Release Metal checks on 2026-09-12 established selected workflows:

- The copied Bistro workspace opened from `/tmp` through the native editor and
  rendered successfully. The bounded run exited 0; `projects-portable-ui.log`
  recorded `UPLOAD_WAIT_SUMMARY fence=57 queue_idle=0 device_idle=0 violation=false`.
  Native screenshots showed the Projects flow, project roundtrip and Cyrillic
  names. The later Save/reload check preserved the authored name
  `Sun Projects Check` through an immutable overlay revision whose SHA-256 was
  `ea3c589be3a774545f1df4260b118a29bc6b3af33c714db3466e4599584c358e`.
  Earlier failed diagnostic runs are not clean-run evidence.
- The managed `bake_scene` job captured all six Bistro reflection faces at 64×64
  through the unchanged `local-offscreen` profile. Every face report passed with
  exit 0 and one capture. The published RGBA16F cubemap SHA-256 was
  `634af72157d1cff138c46ad27c95c444ae77110e55046c027b291ff8b879ef5c`.
  The before/after scene comparison preserved entities, environment, overlay and
  scene identity; changes were confined to the probe recipe/artifact and bake flags.
- The isolated material-sphere job published `acceptance-material.png` with
  SHA-256 `6a764bad3f60c4cdfb9179f16db892ceff835fedebdf228a597aac41b504db00`.
  Its preview owner checked the harness report, image and dependency digests before
  publication. These are material-preview execution and output checks, not a
  timing comparison.
- `./build_test.sh` completed the CPU suite, including managed closure, dock tab
  geometry and gesture checks. CPU preview checks covered Unicode paths, bounded
  dimensions, alpha filtering, HDR mapping and failed-input publication. Read-only
  job tests checked unchanged workspace hashes for ready, stale and unbuilt inputs;
  the separate native read-only interaction check is recorded below.

The native launch used these explicit selectors from `/tmp`, with `repo` denoting
the absolute checkout directory and the copied Release bundle beneath it:

```sh
VKR_AUTOCLOSE_SECONDS=240 "$repo/.scratch/VKR Projects Check.app/Contents/MacOS/vkr_editor" \
  --workspace "$repo/.scratch/projects-portable-check" \
  --project 953af829-19db-4a85-b950-d551986224b4 \
  --scene-id bf7f5ecd-797c-466d-83e4-d52996acbece
```

The reflection job command, run from the checkout, was:

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  python3 tools/editor_project_jobs.py \
  --request .scratch/projects-portable-check/.vkreditor/jobs/acceptance-reflection/request.json \
  --result .scratch/projects-portable-check/.vkreditor/jobs/acceptance-reflection/result.json
```

Task-local evidence includes `.scratch/projects-reflection-evidence.json` with
all six report digests, `.scratch/projects-material-preview.log`,
`.scratch/projects-portable-ui.log`, `.scratch/projects-save-ui.log`, and
`.scratch/projects-jobs-check.log`. These observations do not cover native
Windows dialogs/filesystem operations or Vulkan visuals, the full DPI/keyboard
matrix, long-session load/unload retirement, or matched frame-budget impact.
The interactive Save run completed save, reload, project roundtrip and texture/
material browser navigation, then exited 0. It recorded
`UPLOAD_WAIT_SUMMARY fence=275 queue_idle=0 device_idle=0 violation=false`.
The native read-only run opened Bistro while another process held the workspace
write lease, then exited 0. SHA-256 snapshots of all 3,293 workspace files before
and after were identical; `.scratch/projects-readonly-evidence.json` records zero
changed files. Runtime projections and logs stayed in the OS-local jobs directory.
The navigation marks this state `[RO]`; Content disables asset-writing actions.

## Consequences

A copied workspace carries scene dependencies and preferences without preserving
absolute import-machine paths. Large source imports require additional disk space.
Atomic manifest replacement preserves the previous referenced revisions on a
failed publication; unreferenced revisions can remain until explicit cleanup.
Only one world scene is resident. A failed replacement leaves an error/selection
flow rather than promising that the previous GPU scene remains resident.

## Alternatives considered

A per-user project root would simplify discovery but make project transfer less
explicit. Referencing original external imports would avoid copies but break
workspace portability. Keeping preferences in scenes would make project-wide
layout and control choices disagree between scenes. Moving compiled renderer
tables into the workspace would require runtime table loading and is outside this
feature.

## Revisit when

Packaging introduces binary asset bundles, concurrent project writers require
finer leases, or cache/revision retention needs an explicit user-facing cleanup
policy. Remaining feature and acceptance scope stays in the
[Projects proposal](../proposals/editor-projects.md).
