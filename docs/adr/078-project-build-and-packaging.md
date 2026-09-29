---
status: implemented
updated: 2026-09-29
authority: adr
---

# ADR-078: Project build and packaging

## Status

Accepted. Phase 1 of the
[project packaging proposal](../proposals/project-packaging.md) is implemented
and verified on macOS/Metal. The Windows/Vulkan package gate has not run.
Incremental packages and shipping polish remain in the proposal.

## Context

[ADR-077](077-asset-build-system.md) packed repository recipes into bundles, but
a managed project could not ship. Lowered scenes named absolute workspace paths,
only the editor loaded the root World, and the sample app carried harness and
debug controls. The user chose these on 2026-09-29: game settings and build
profiles in `game.json`; overlays ship with rewritten references; a separate
`vkr_player` executable; and a Build menu, with Bakery moved to a developer
menu.

## Decision

**Game settings.** `game.json` beside `project.json` holds the `game` object
(name, version, company, executable name, startup scene, ordered included
scenes, window size and Graphics defaults) and a `profiles` array (name,
platform, `development` or `shipping`, output folder, project-relative extra
includes, `bake_lighting`, `run_after_build`). The
[project store](../../editor/src/editor_project_store.h) parses and writes it.
It saves through a staged file and merges members it does not own back under
its own. Without a file, a project packages every scene, starts in the first,
and has one development profile named after the project. The bundle command
applies the same rules and owns validation: the name, an executable name without
separator or extension, a window mode of `windowed`, `fullscreen` or
`borderless` with a window size of 320x240 to 16384x16384, and a startup scene
among the included scenes.

**Command.** `vkr_bakery bundle <project directory> [--profile <name>] [--out
<dir>] [--template <dir>]` ([package](../../tools/bakery/vkr_bakery_package.c))
runs seven stages. Each is a `start`/`done` event pair with coded diagnostics:

| Stage | Work |
|---|---|
| Validate | Settings, profile, host platform, player template, and an output outside the workspace directory and repository. An existing output must hold a `bundle.json` or be empty. Portable lowering of the World and every included scene. Warns when the startup scene has no player. |
| Finalize | `finalize_textures` for a scene with preview or deferred assets (the job publishes the scene), and `finalize_project_assets` when project assets are. The returned inventory is lowered against without publishing `project.json`, which the editor owns. Shipping finalizes with the final encoder, development with the fast one. Fast-encoded final textures are not re-encoded; the report counts them. |
| Bake | With `bake_lighting`, `bake_scene` with reflection and diffuse for each included scene. |
| Lower | Lowers again only when an earlier stage published. |
| Pack | Walks the closure over identity mounts: staged documents, `project/` to the project directory, `editor/` to the editor bundle, `assets/` to the template's engine resources. Rejects a document naming the workspace directory or the repository. Writes `content/game.vkpak` and `content/engine.vkpak` (`assets/...`). |
| Stage runtime | Copies the profile's player as `<executable>[.exe]` and only the host backend's shader catalog. Writes `bundle.json` version 2. |
| Verify and report | Validates and rehashes both archives, then renames `<out>.staging` over `<out>`. |

Project jobs run as child `vkr_bakery project` processes, so publication keeps
its owners. The package is assembled in `<out>.staging`; a failure or
cancellation removes it and keeps the previous package. Each run writes
`<workspace>/logs/builds/<UTC>-<n>.json` with stage durations, warnings, bytes
by loader kind and by first-reaching scene, the largest files, the archives and
any finalized inventory.

**Portable lowering.** A read-only `prepare_scene` request with `portable: true`
([lowering](../../tools/bakery/project/vkr_project_lower.c)) writes the same
runtime scene with content identities instead of absolute paths. Scene assets
map to `project/scenes/<id>/...`, project assets to `project/...` and editor
fonts to `editor/...`. Overlay collider assets change from workspace-relative
paths to identities. `project_assets` substitutes a finalized inventory. The new
`package_world` operation checks that every `./` and `../` string of
`world.scene.json` names a file inside the project and that no string is
absolute. It ships the document byte-identical, because without a
`source_identity` the World overlay binds entities by a fingerprint of those
bytes. The overlay is rewritten like a scene's. Identities mirror the project
directory, so owner-relative material and texture references keep resolving.
The closure resolves `./` and `../` against the owning document only and ships
a mesh's `.remap.json`.

**Package layout.** `bundle.json` version 2 keeps version 1's members, drops
`scene`, and adds `game`: `world`, `world_overlay`, `startup_scene`,
`startup_overlay`, `scenes[]`, `fonts[]` (`default-scene-font` falls back to the
engine's UbuntuMono configuration), `window`, `graphics`, and `startup_camera`,
the startup scene's editor viewport recall without selection. The
[vfs](../../lib/src/filesystem/vkr_vfs.h) reads only root members, through
`vkr_json_find_root_field`, and keeps the description for the player.

**Player.** `vkr_player` (development: INFO logging and the F6 overlay) and
`vkr_player_shipping` (errors only, no developer UI) are prebuilt in every tree
under `<build>/player`. So are `template.json` and `engine/assets`, which hold
the render graph and runtime fonts of the former Bistro recipe with the files
they name. The [player](../../player/src/main.c) mounts the package, then opens
the World and the startup scene with their overlays on its first frame through
the same requests the editor issues. It registers the package fonts and applies
the startup camera once the scene activates. A scene's player entity starts
gameplay; Escape opens Resume, a Fullscreen/Windowed switch and Quit. The
player enters `game.window.mode` once its window exists through
`vkr_window_set_mode` ([window](../../runtime/src/core/vkr_window.h)). On macOS,
`fullscreen` is the native fullscreen Space, requested from the event pump once
the application is active, and `borderless` is a frameless window over the whole
screen with the menu bar and Dock hidden. On Windows both are a borderless popup
over the monitor, since the renderer requests no exclusive fullscreen. Leaving
either mode restores the previous frame, and resize events carry the new extent
to the swapchain. Graphics preferences read `game.graphics`
as defaults and live in `%APPDATA%/<company>/<name>/settings.json` or
Application Support. The texture transcode cache moves to `%LOCALAPPDATA%` or
`~/Library/Caches` through `vkr_texture_transcode_cache_set_root`; otherwise the
first Bistro run wrote 2.9 GB into the install folder.

**Editor.** A Build menu (Build, Build and Run, Build Settings..., Open Last
Build, Build log) and a Develop menu (Bakery, Draws and render graph, Memory)
sit beside Scene; Scene gains Bake lighting. The Build settings window edits
profiles, game fields, included scenes with order and Startup, configuration,
output folder, extra includes, bake and run options and Graphics defaults.
Build saves `game.json`, then runs the preflight: the existing Save/Discard/
Cancel prompt for any container with unsaved edits, then a drained preference
writer. The package job runs as `EDITOR_BAKE_PACKAGE` on Bakery's single worker,
so it never overlaps a project job. A status strip shows the stage and Cancel.
The Build tab shows the stage checklist and durations, the diagnostics, the
log, Open Folder, Run and Copy Log. A diagnostic whose source is a package
identity or a host path has Reveal: the identity maps back to the project or
editor-bundle file, and `vkr_editor_content_reveal_path` selects the asset whose
artifact it is, else the asset whose build revision holds it, in its Content
folder. `content.reveal <path>` runs the same step. Build and Run forwards the game's output to the Console as
`[game]` lines. `build.game [profile]` and `build.run [profile]` hold the Cmd
queue and report the result
([ADR-075](075-editor-cmd-bar-and-evaluator.md)).
[`editor_build.c`](../../editor/src/editor_build.c) owns this workflow.

## Consequences

- A project ships without the editor, workspace or repository. Overlays and the
  World need no second application path, and the runtime has one boot path.
- Package bytes do not depend on the build path: the editor's `build.game` and
  the command line write identical archives for one profile.
- A project-asset finalize publishes revisions the manifest does not name yet.
  After an editor build, `vkr_editor_projects_adopt_inventory` publishes the
  report's inventory while the project inventory still matches its SHA-256 at
  the build's start, then points World models at the new revisions; otherwise
  workspace cleanup removes the revisions after 24 hours and the next build
  finalizes again from the cache. A command-line build never adopts, and the
  package's World keeps the revisions its document names.
- Every package carries about 24 MiB of engine resources, most of it the CJK
  fallback font.

## Evidence

- [`check_bakery_package.py`](../../tools/checks/check_bakery_package.py), part
  of `build_test.sh`, builds a synthetic project through the job runner. It reads
  the package with an independent `.vkpak` parser and asserts the two archives,
  products and hashes. It also asserts no absolute or workspace path in any
  document, a byte-identical World, a rewritten collider identity, the startup
  scene, the seven stage events, the report, and that the project directory is
  unchanged. A failed build keeps the earlier package, a non-package folder is
  refused, and a SIGINT once staging exists exits 3 with no package. A
  preview-tier project asset is finalized, the report returns a final-tier
  inventory, and `project.json` is unchanged. The CPU store test covers
  defaults, merge preservation and validation.
- Adoption: a fixture workspace with one preview-tier project import, headless
  editor `--exec 'build.game'`, settles in 0.46 s and logs the adoption;
  `project.json` then names the new mesh revision with final-tier ASTC
  textures and no preview-tier asset.
- Bistro, macOS/Metal, Release. From the repository Testbed workspace,
  `vkr_bakery bundle .vkreditor/projects/45a13e01-... --out
  /tmp/vkr-pkg/Testbed` exits 0 in about 21 s. It packs 781 files: `game.vkpak`
  3051.8 MiB (SHA-256 `0f80cf7b...`) and `engine.vkpak` 23.4 MiB
  (`8576f359...`). The player then ran under `sandbox-exec` with reads and
  writes below the repository denied, with `VKR_AUTOCLOSE_SECONDS=40` and
  `$VKR_VFS_RECORD`, and exited 0. All 779 distinct content reads are package
  products. Only the unread font source and the `.fnt` that its `.vkf` replaces
  go unread. The screenshot at the startup camera shows textured, lit Bistro.
- Editor, headless, on an APFS clone of that workspace: `--exec 'build.game
  "Mac Shipping"'` settles in 24.8 s. Both archives, the executable and
  `bundle.json` equal a command-line build of the same profile.
- Window modes, macOS: a fixture package with `borderless` opens a 1512x982
  point window covering the display, and with `fullscreen` a frameless
  1512x949 point window in its own Space below the notch; a windowed run is
  1280x752 with its title bar. The pause-menu switch was not exercised, since
  the run takes no input.
- Reveal: headless `content.reveal` on a fixture selects the texture that
  owns its `.vkt` artifact in Textures, and for the mesh revision's
  `mesh.vkb.remap.json`, which no record names, the mesh in Meshes; a path no
  asset owns is refused. Clicking a Build tab diagnostic was not exercised.
- Not run: Windows/Vulkan packaging and window modes (the Windows code is not
  compiled on this host), a copy to another volume, a package from a
  Debug tree (its players are Debug), and a timed build claim, which needs
  matched Release runs.

![Build settings and the Build tab after a build](../../assets/editor/build-settings.png)

![Packaged Bistro at its startup camera](../../assets/editor/packaged-bistro.png)

![Build settings with the Display modes](../../assets/editor/build-settings-display.png)

## Alternatives considered

- Flatten overlays into documents: a second overlay application; kept for a
  later optimization (`effective_bake_runtime` already applies one in C).
- Rewrite the World document: breaks its overlay's byte-fingerprint binding.
- Publish the finalized project inventory from the bakery: `project.json`
  belongs to the editor store and its stale-write protection.
- Ship `vulkan_renderer`: it carries harness, overlay and sample controls, and
  its `--gameplay` spawns the Bistro training platform.

## Revisit when

Packages need incremental or streamed archives, compression (`.vkpak` version 2)
or cross-platform output; script modules load at runtime; or a second
application boots from `bundle.json`.
