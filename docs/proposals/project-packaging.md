---
status: proposed
updated: 2026-09-29
authority: proposal
---
# Project build and packaging

This proposal turns a managed editor project into a standalone game directory:
an executable, its shader catalog and `.vkpak` content, runnable from any
location without the editor, workspace or repository. It extends the bundle
scope that [ADR-077](../adr/077-asset-build-system.md) leaves open ("a bundle of
a managed project") and section 11 of the
[asset build system proposal](asset-build-system.md). The editor gains one Build
action backed by the same `vkr_bakery` command a script or CI job runs.

## Current baseline

- `vkr_bakery bundle <recipe> --out <dir> [--app <exe>] [--shaders <dir>]`
  ([bundle](../../tools/bakery/vkr_bakery_bundle.c)) packs a repository
  recipe's closure into `content/<name>.vkpak`, copies an executable and a
  shader catalog beside it, and writes `bundle.json`. The closure walker has
  one root; identities must be root-relative, and absolute references are
  rejected.
- The runtime mounts that layout when `bundle.json` sits beside the executable
  ([mounts](../../lib/src/filesystem/vkr_vfs.h)), and a bundle's `scene`
  becomes the requested scene
  ([options](../../runtime/src/vkr_sample_runtime_config.c)).
- Managed scenes reach the runtime through
  [lowering](../../tools/bakery/project/vkr_project_lower.c)
  (`vkr_project_lower`), which resolves each typed asset reference with
  `vkr_project_contained` and writes the **absolute** workspace path into the
  runtime scene. Lowered scenes are therefore not relocatable.
- The root World (`world.scene.json` and its overlay, ADR-076) loads only when a
  UI client issues a `VkrSampleWorldRequest`
  ([runtime API](../../runtime/src/vkr_sample_runtime.h)); only the editor
  does. Editor-scope assets such as the default scene font live in
  `.vkreditor/editor/bundles/1`, outside the project directory. Textures may
  still be at the `preview` tier; `finalize_project_assets` promotes them.
- `vulkan_renderer` ([app](../../app/src/main.c)) is the sample host: debug
  overlay, one `--scene`, gameplay only with `--gameplay`, and graphics
  settings written to `<content root>.vkr-graphics-settings.json`, which in a
  bundle is the install directory.
- The editor's [Bakery panel](../../editor/src/editor_bakery.c) lists
  individual recipes (renderer tables, shaders, cooks, scene bakes). It has no
  project-level build.

## Engine survey

| | Unreal Engine 5 | Unity 6 | Godot 4 |
|---|---|---|---|
| Entry | Platforms > Package Project; Project Launcher profiles | File > Build Profiles > Build, Build And Run | Project > Export > Export Project |
| Game settings | Maps & Modes default map; Description name, version, company, icon | Player Settings: product, version, icon, resolution | `application/run/main_scene`, name, icon |
| Build settings | Packaging: configuration (Development, Shipping), maps to cook, always/never-cook directories, IoStore, compression | Per-platform profile: ordered scene list (index 0 starts), Development Build, compression | Per-platform preset (`export_presets.cfg`): resource selection, filters, texture formats, debug/release |
| Content selection | Listed maps and their references plus always-cook directories | Listed scenes and dependencies, `Resources/`, Addressables | Selected scenes and dependencies, or all resources |
| Executable | Game target compiled by UnrealBuildTool | Prebuilt player plus compiled scripts | Prebuilt export template, copied, renamed, icon and metadata set |
| Content format | IoStore `.utoc`/`.ucas` | `<Game>_Data/` serialized files | `.pck`, separate or embedded |
| Headless | `RunUAT BuildCookRun` (build, cook, stage, pak, archive) | `-batchmode -executeMethod` with `BuildPipeline.BuildPlayer` | `godot --headless --export-release "<preset>" <path>` |

Common structure: game settings are separate from build profiles; content is a
closure from listed scenes plus explicit includes; cooking reuses the editor's
cache; the output pairs packed content with a runtime; one button ends in a
report; the headless command and the button share one pipeline. VKR has no
per-project native code to compile (gameplay is runtime C and script modules
have no loader), so it follows Godot and Unity: a prebuilt **player template**
plus archives.

## Decisions taken

The user accepted these on 2026-09-29.

| Decision | Choice | Rationale | Tradeoff accepted |
|---|---|---|---|
| Game settings and profiles | `game.json` beside `project.json`, owned by the project store | Keeps jobs from rewriting the 16 MiB manifest for a title or profile edit; matches `presets.json` and `content.labels.json` | One more project file |
| Scene overlays in a package | Ship each overlay with its references rewritten to content identities | The runtime already validates and applies overlays; no second implementation of overlay application | Editing data ships; flattening stays a later optimization |
| Game executable | New `vkr_player` target, separate from `vulkan_renderer` | Keeps harness, debug overlay and sample controls out of shipped games | One more application target |
| Bakery panel | Leaves the main navigation for a developer menu; Build replaces it for users | Users see one build action; tables, shaders and ad hoc cooks stay available | Bakery is one menu deeper |

## Concepts

| Term | Definition |
|---|---|
| Game settings | Project-wide shipped identity and startup: name, version, company, executable name, startup scene, included scenes, window and default graphics. |
| Build profile | A named target: platform, configuration, output directory, extra includes and options. A project has one or more. |
| Player template | A prebuilt `vkr_player` executable per configuration plus its engine resources. Packaging copies it; it is never compiled per project. |
| Package | The output directory: executable, shader catalog for one backend, `content/*.vkpak`, `bundle.json`. |
| Content identity | A path below the package content root: `project/…` for project and scene owners, `editor/…` for editor-scope assets, `assets/…` for engine resources. Never absolute, never `..`. |

## Proposed change

### 1. `game.json`

Version 1, beside `project.json`, read and written by the
[project store](../../editor/src/editor_project_store.h) with the same bounds,
validation and atomic replacement as `presets.json`:

```json
{
  "version": 1,
  "game": {
    "name": "Testbed",
    "version": "0.1.0",
    "company": "",
    "executable": "Testbed",
    "startup_scene": "<scene id>",
    "scenes": ["<scene id>"],
    "window": {"mode": "windowed", "width": 1600, "height": 900},
    "graphics": {"version": 1}
  },
  "profiles": [
    {
      "name": "Windows Shipping",
      "platform": "host",
      "config": "shipping",
      "output": "D:/Builds/Testbed",
      "include": [],
      "bake_lighting": false
    }
  ]
}
```

The World is always included and never listed. `startup_scene` must be in
`scenes`; an empty `scenes` ships the World alone. `graphics` holds
`VkrGraphicsSettings` defaults through the existing reader. `executable` is a
file name without separators or extension. A missing `game.json` yields one
default profile for the host named after the project; the file is written on
the first edit. Unknown members are preserved.

### 2. `vkr_bakery bundle` for a project

```
vkr_bakery bundle <project directory> --profile <name> [--out <dir>] --json
```

A positional argument naming a directory with `project.json` selects project
mode; a recipe file keeps today's behavior. `--out` overrides the profile's
output. Stages, each a `progress` phase in the event stream with coded
diagnostics:

| Stage | Work | Owner |
|---|---|---|
| Validate | `game.json`, profile, startup scene membership, every referenced asset has its artifact, output outside the workspace and repository, player template present. Warn when the startup scene has no camera or player. | bundle command |
| Finalize | Promote `preview`-tier assets in the included closure to `final`; unchanged ones are cache hits. | existing `finalize_project_assets` |
| Bake (optional) | `bake_lighting` runs the scene's reflection and diffuse bakes. | existing `bake_scene` |
| Portable lowering | Lower the World and each included scene into staging with content identities instead of absolute paths; rewrite overlay dependencies the same way. | `vkr_project_lower` gains a portable output mode, not a second lowering |
| Pack | Walk the closure from the lowered documents over a mount table of identity prefixes (`project/`, `editor/`, `assets/`, staging) instead of one root. Write `content/game.vkpak` and `content/engine.vkpak`. | bundle command |
| Stage runtime | Copy the profile configuration's player template as `<executable>`, copy only the target backend's shader catalog, write `bundle.json` version 2. | bundle command |
| Verify and report | Validate each archive with the runtime reader and rehash chunks (existing); write `build_report.json` under `.vkreditor/logs/builds/`: bytes by loader kind and by scene, the largest assets, stage durations and warnings. | bundle command |

The package is written to `<out>.staging` and renamed over `<out>` only after
verification, so failure or cancellation keeps the previous package.
Cancellation uses the existing process-tree termination; staging is removed on
the next run.

Output:

```
<out>/
  <executable>[.exe]
  bundle.json
  shaders/<backend>/…
  content/engine.vkpak    render graph, runtime fonts
  content/game.vkpak      World, scenes, overlays, meshes, .vkt, materials,
                          collision, animation, volumes, probes
```

`bundle.json` version 2 adds a `game` object (`name`, `version`, `company`,
`world`, `world_overlay`, `startup_scene`, `startup_overlay`, `scenes[]`,
`window`, `graphics`) beside the version 1 members. The runtime keeps reading
version 1 recipe bundles.

### 3. `vkr_player`

- A `player/` executable linking `vkr_sample_runtime`, built by the repository
  wrappers in two variants, both with Release optimization: **development**
  (INFO logging, debug-overlay toggle) and **shipping** (errors only, no
  developer UI, `ASSERT_LOG=0`). A post-build step places engine resources (the
  render graph and runtime fonts) in a template directory beside each.
- The player is a UI client of the sample runtime. On its first frame it issues
  the `VkrSampleWorldRequest` and `VkrSampleSceneRequest` the editor issues,
  from the `bundle.json` game object, then starts simulation with gameplay on.
  Its only UI is a pause and quit menu. The runtime gains no second boot path.
- Graphics settings move to a per-user directory
  (`%APPDATA%/<company>/<name>/settings.json` on Windows, Application Support
  on macOS), with `game.graphics` as defaults.
- The editor finds templates like it finds `vkr_bakery` today, through a
  compile-time template directory; a distributed editor later ships them in
  `templates/` beside itself.

### 4. Editor

- **Build menu** in the top bar: Build, Build and Run, Build Settings…, Open
  Last Build. Cmd commands `build.game`, `build.run` and `build.settings`
  ([ADR-075](../adr/075-editor-cmd-bar-and-evaluator.md)) make builds
  scriptable with `--exec`.
- **Build Settings window**: profiles list (add, duplicate, delete); Game
  fields; Scenes with include checkboxes, drag order and a Startup marker, the
  World shown as always included; Target with platform limited to the host
  (others disabled with a reason), configuration and output folder; Options
  (bake lighting, run after build); footer with Build, Build and Run and the
  last package size.
- **Preflight**: the existing Save/Discard/Cancel prompt for every container
  with unsaved edits, then the preference writer drains. The build runs on
  Bakery's single worker, so it never overlaps an import or other write job.
- **Progress and report**: a non-blocking status strip with the current stage,
  fraction and Cancel, and a Build tab in the bottom dock with the stage
  checklist and durations, the log (a diagnostic with an asset reveals it in
  Content) and the final report with Open Folder, Run and Copy Log. Build and
  Run forwards the game's stdout to the Console tagged `[game]`.
- **Bakery** moves to a developer menu. Scene lighting bakes are reachable from
  the profile option and the scene's own menu.

A new `editor_build.c` owns the Build window, preflight and job lifetime: an
independent responsibility from Bakery's recipe list, sharing its worker
through `vkr_editor_bakery_project_start`-style calls rather than a second
worker.

## Handoff notes for the implementer

Facts found in code on 2026-09-29, before implementation started:

- Managed artifacts reference each other owner-relatively: a mesh names its
  materials `./materials/<name>.mt` (from `tool mesh --inspect`), a material
  names textures `./../textures/<name>.vkt?…`, and a sibling
  `mesh.vkb.remap.json` maps material references and must ship when present.
  The runtime resolves `./` and `../` against the owning file and bare paths
  against the content root, so identities that mirror the project directory
  under `project/` keep those references valid; lowered documents can name
  assets by bare identity.
- The current `vkr_bundle_walk_mesh` adds mesh materials root-relative and
  would fail on `./materials/…`; the walker needs owner-relative resolution
  there, as `vkr_bundle_reference` already does for JSON and `key=value`
  documents.
- `prepare_scene` with `read_only` already lowers a scene into a
  `runtime_directory` outside the workspace (`vkr_project_lower`,
  `vkr_project_lower_overlay`); portable lowering is that path with identities
  instead of `vkr_project_contained` absolute paths.
- `effective_bake_runtime` ([bakes](../../tools/bakery/project/vkr_project_bake.c))
  already applies an authored overlay to a runtime copy in C. It is the
  starting point if overlay flattening is revisited.
- The editor builds World requests from `<project>/world.scene.json` and
  `world.editor.json` (`project_world_prepare`,
  [Projects UI](../../editor/src/editor_projects.c)); the World document names
  meshes by `./builds/<revision>/mesh.vkb` plus a typed `asset` reference and
  is not lowered by a job today.
- Workspace imports may encode textures at the editor's fast speed
  (`bc-fast`, `astc-fast`); `finalize_project_assets` promotes preview and
  deferred tiers only. Decide whether the Finalize stage also re-encodes fast
  textures at the final speed for a shipping profile, and report the count
  either way.

## Phases

1. **Packaging on the host platform.** `game.json` and its store; portable
   lowering; project mode in `bundle` with the multi-root closure; `vkr_player`
   with per-user settings; `bundle.json` version 2; Build menu, Build Settings,
   progress, report and Cmd commands; Bakery demoted.
2. **Incremental packages.** Reuse unchanged chunks and archives, one archive
   per scene for streaming, per-chunk compression. Compression changes `.vkpak`
   to version 2 and needs approval first.
3. **Shipping polish.** Windows icon and version resources, macOS `.app`
   layout and signing, linking script module archives once the
   [entity behavior](entity-behavior-system.md) runtime loads them.

Cross-compiling a package for another platform is out of scope: Metal
libraries need the macOS toolchain and the Vulkan catalog is produced per
host; each platform packages on its own host.

## Evidence needed

- A CPU check beside
  [`check_bakery_bundle.py`](../../tools/checks/check_bakery_bundle.py) that
  builds a small synthetic managed project, packages it, and asserts: no
  absolute or workspace path in any archived document, the World and startup
  scene present, overlays rewritten, the archive validates, a failed stage
  leaves an earlier package unchanged, and a cancelled run leaves no package.
- Bistro: import `assets/models/bistro.gltf` into a scratch workspace with
  Bistro as the startup scene, build the Shipping profile, copy the package to
  another volume, and run it there in normal Release with graphics validation
  unset and `VKR_AUTOCLOSE_SECONDS` set. It exits 0 and `$VKR_VFS_RECORD`
  shows every content read inside the package. Record the package size,
  stage durations and report digest. Windows/Vulkan and macOS/Metal are
  separate gates, each on its own host.
- The editor path: `--exec` running `build.game` produces the same archive
  bytes as the command line for the same profile.
- A package build time claim needs matched Release runs with the event log
  retained, as ADR-077 requires.

Ask before: changing the `.vkpak` format, the ADR-069 workspace layout, or the
set of engine resources a template carries.

## Code baseline

- [bundle command](../../tools/bakery/vkr_bakery_bundle.c),
  [project lowering](../../tools/bakery/project/vkr_project_lower.c),
  [project job runner](../../tools/bakery/project/vkr_project_main.c)
- [content mounts](../../lib/src/filesystem/vkr_vfs.h)
- [sample runtime API](../../runtime/src/vkr_sample_runtime.h),
  [runtime options](../../runtime/src/vkr_sample_runtime_config.c),
  [sample app](../../app/src/main.c)
- [Bakery panel](../../editor/src/editor_bakery.c),
  [project store](../../editor/src/editor_project_store.h),
  [editor targets](../../editor/CMakeLists.txt)
