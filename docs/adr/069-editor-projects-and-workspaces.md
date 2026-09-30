---
status: implemented
updated: 2026-09-30
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
The project creation form offers Blank, FPS Arena and RPG Grounds starter scenes,
with Blank selected initially, plus No starter scene. Selecting a starter imports
its source scene from `assets/templates` through the same managed scene job as
a user import, then opens the published scene. The source GLBs and their static
collision proxies are editor content;
the [distribution install](../../editor/CMakeLists.txt) carries this closure.
The import cooks scene geometry and copies the collision journal and its cooked
proxy into the project. An empty Player Spawn entity binds the movement client.
The templates author their own daytime
sky light and sun with clouds disabled; other unoverridden World components
retain the normal inheritance rules.
Blank and FPS Arena start in first person; RPG Grounds starts in third person
through its [player binding](073-native-gameplay-foundation.md).
No starter scene enters the editor without loading a scene or automatically
opening the Scenes modal. Creating project resources is a
project job, not scene loading. Creation publishes `project.json` with default
settings before that job starts; the job then imports the project font and the
optional first scene. A failed or cancelled first job therefore leaves a listed
project without that scene instead of an unlisted directory. Default and
cleared hierarchy search text remain valid empty strings when project settings
are saved.

[Project storage](../../editor/src/editor_project_store.c) owns a versioned
workspace manifest and UUID-named project directories. Each project has a
`project.json` with a display name, ordered scene references, asset inventory,
default font, editor settings and scene-specific editor recall. Each scene has
its own `scenes/<id>/scene.json` and artifact directories. Scene version 4 keeps
its asset records in an immutable `inventory/<id>.json` revision named by the
manifest, so a scene manifest stays within the 1 MiB scene-document limit while
an inventory may reach 16 MiB. A project manifest carries the project asset
inventory inline and shares the 16 MiB inventory bound, which the editor store,
job results and the bakery's project-document validation all apply; one large
imported model otherwise leaves no room for a second import. A job publishes a new inventory revision before atomically
replacing the manifest, as with edit overlays. Version 3 manifests with inline
records remain readable and become version 4 at their next job publication.
Display names can change independently of IDs. Paths in manifests resolve within
the declared owner; imports copy their dependency closure into the workspace.
A glTF snapshot keeps only the images a texture samples: images named only by
an optional extension (Bistro's `MSFT_texture_dds` alternates) are left out,
the kept images renumbered and the extension references removed.
A glTF image resolves beside its model; a model inside the repository's
`assets` tree also finds it where the mesh cooker looks, under `assets` and
`assets/textures`, without a legacy `objects/` prefix and by file name, so the
repository Bistro imports with its shipped texture layout. Models elsewhere
resolve only beside themselves. On APFS the job and the mesh cooker clone every
file they copy copy-on-write, so a closure from the same volume, such as
Bistro's 3.1 GiB of source textures, and a bundle's `dependencies` and
`textures` copies share blocks instead of duplicating them; other file systems
copy the bytes. The cooker writes derived textures (paired normal/roughness,
cutout, converted specular-glossiness and embedded images) once into
`.vkreditor/cache/generated`, named by source content and parameters, including
the texture tier and encoding ([ADR-077](077-asset-build-system.md)), so later
imports reuse them. Converted specular-glossiness images are packed from
memory, named by their pixels' content hash, and never written as image
files. Their material records a memo keyed by its source images' bytes,
samplers and factors that holds the converted images' extent and hashes, so a
later import names its packed textures without decoding and converting; only
a pack whose output is missing or stale converts again.
Job-packed material textures use `.vkreditor/cache/textures` keyed by source
hash, class, tier and encoding; after a packer rebuild the packer revalidates a
cached output and recooks only when its recorded recipe no longer matches.

Every successful write job then cleans the workspace on a best-effort basis;
read-only and scene-opening jobs do not, and a cleanup failure never fails the
published job. A cache entry survives while a listed scene's records or the
project's assets name its content digest, because bundles hold clones or copies
of cache files rather than paths into the cache. `cache/generated-index.json`
retains derived-file digests between runs; a derived file younger than the
grace period is not hashed, since no rule removes it yet. A derived file no
bundle holds,
such as a paired bake's intermediate, is removed once it is older
than 24 hours, so importing the model again within a day reuses it. If any scene or project manifest is
unreadable, no cache entry is removed. Build and inventory revisions a live
scene no longer names are removed after 24 hours, so an editor still streaming a
replaced revision keeps its files; staging leftovers also wait 24 hours.
Unlisted scene directories and project directories without `project.json` are
removed after one hour, and job directories after seven days.
The editor holds an OS-backed workspace write lease. A second editor can inspect
the workspace without publishing changes or previews into it.

The Scenes list offers confirmed permanent deletion in writable, idle projects.
Deleting the active scene discards its edits and waits for the runtime unload
before publication; deleting another scene preserves the active scene. The
project store atomically removes membership and scene recall with stale-write
rollback. A background `delete_scene` job then erases only the unreferenced
`scenes/<id>` directory, preserving project-shared assets. It refuses links and
Windows reparse points. File-removal failure or cancellation leaves membership
removed and offers Retry to finish cleanup; it does not claim the files survived
unchanged. An interrupted editor retains the job request in the workspace jobs
directory; a later write job removes the unlisted scene directory once it has
been unmodified for an hour, but deletion itself does not resume after restart.
Projects can be deleted from their chooser card or the Scenes view after
confirmation. Deleting the open project unloads its scene and discards unsaved
edits. The store then removes `project.json` under the project lock, which is
the commit point that drops it from discovery, and a background
`delete_project` job erases the unlisted directory. That job refuses a still
published manifest, links and Windows reparse points; if it stops, workspace
cleanup removes the directory after an hour. Its cleanup pass frees cache
entries that only the deleted project used.
CPU storage tests cover ordering, recall removal, conflicts and empty projects;
isolated filesystem checks cover deletion, retries and Windows junction refusal.
Native interaction and active-scene retirement through this dialog remain unverified.

[Projects UI](../../editor/src/editor_projects.c) owns project selection, scene
creation/import, progress, Save/Discard/Cancel decisions and preference saves.
Preparation, validation and resolution use a compact centered Scene loader:
status text, a short progress bar and a small Cancel button. Its backdrop blurs
the retained Scene image and darkens it without blurring UI. With no retained
image, first startup has a dark backdrop. Cancelled or failed preparation offers
Back and Retry; diagnostic tooltips and Bakery retain details. These controls do
not capture the whole editor. The loader
disappears when the runtime accepts the asynchronous scene request, so streamed
scene content remains visible. Project/scene switching is disabled while a job
is queued or running, or while activation is pending. Back, or Projects/Scenes
navigation, abandons a stopped request and keeps whatever the job committed;
a new project whose first job stopped then opens with its saved settings.
Projects and Scenes use the shared navigation-button style on the left before Metrics,
with a distinct background highlight.
Graphics controls, runtime input/presentation preferences, layout, panel state,
Console filters, Bakery defaults and Content preferences belong to the project.
Viewport and hierarchy recall are keyed by scene within the project. Lights,
environment, probes, fog, authored cameras and edit overrides remain scene data.
Native window/display placement and the workspace locator remain machine-local.

Hierarchy offers Add entity for the loaded writable managed scene. Its form
reuses the scene-creation light controls and native model picker, accepting
GLTF, GLB, OBJ and cooked VKB models plus directional, point, spot and rectangle
lights. The `add_entities` job appends source entities, preserving existing
indices and overlay bindings, and publishes through the asset import owner.
Unsaved edits resolve through Save/Discard/Cancel before the job starts. The
runtime reloads the published scene and the editor selects the first added
entity. Adding is durable publication, outside Inspector undo history. Failed
jobs return to the draft for correction; Cancel reloads the original scene.

The [job runner](../../tools/bakery/project/vkr_project_main.c),
`vkr_bakery project` ([ADR-077](077-asset-build-system.md)), validates managed
scene versions 3 to 5 and lowers typed asset references into explicit runtime scene and font
inputs. Runtime loaders do not discover workspaces. New source imports use
explicit cooker destinations. Cooked-only imports preserve `.vkb` geometry bytes
and use an adjacent versioned material-remap document; the
[cooked mesh decoder](../../runtime/src/assets/vkr_mesh_cooked_decode.c) requires
a complete exact-reference map when that document exists. Managed scene source
identity and cooked source-node identities preserve edit bindings across path
changes. Importing a legacy overlay validates its original source identity before
remapping it to managed content.
A requested diffuse bake whose inspection finds no closed-room cell publishes
the scene without a diffuse volume and returns a result warning. Any earlier
volume is dropped. Every other bake failure still fails the job.

Jobs stage copied inputs and fresh build revisions, validate dependencies and
check the current manifest fingerprint before replacing its JSON. Artifact
publication precedes the manifest reference. Cancellation cleans uncommitted
staging; a revision transferred to publication ownership may remain unreferenced
rather than risk deleting committed data. Scene Save writes an immutable overlay
revision and atomically updates its manifest reference with conflict detection.
Periodic preference and viewport recall saves use one background worker with an
immutable copy of the project and all borrowed JSON views. The UI thread retains
and reuses snapshot capacity, adopts the completed publication fingerprint, and
keeps changes made during the write pending for the next snapshot. Durable file
synchronization and conflict checks remain in the project store. Explicit saves,
project mutations and teardown drain the worker before changing its owner or
releasing the workspace write lease.
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
members; arrays remain serializer-owned values. An owner without persistable
state writes an empty object, which keeps its last saved members; the animation
editor does this until an animated source populates its document. The document
parser enforces the
16 MiB byte limit and depth limit with temporary dynamically sized token storage,
so a large valid scene inventory does not fail a smaller fixed token ceiling;
documents of about 1.2 KB, such as one inventory record, use inline token
storage instead of an arena. Duplicate-key checks and member lookups compare
key hashes before decoding, so an object with many members (a mesh record's
closure) parses in linear time. Content resolves each inventory's owner once
and its records' paths beneath it.
Managed names retain UTF-8 even when a glyph is unavailable. The shipped managed
UI system face supplies its available Latin, Greek and Cyrillic glyphs through
U+052F; on-demand glyph rasterization and IME composition are separate work.

[Native dialogs](../../runtime/src/platform/vkr_file_dialog.h) return selected,
cancelled or error results with caller-released UTF-8 paths on the UI thread.
Managed editor native close requests remain pending until the dirty-state flow
resolves them. [Content](../../editor/src/editor_content.c) indexes manifests on
refresh, reading a version 4 scene's inventory revision in place rather than
tokenizing up to 16 MiB, and virtualizes filtered asset cards. Its layout follows the navigation,
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
Sources and asset cards scroll in shared UI scroll areas, so their scrollbars
drag and page like every other panel's (ADR-027); the asset grid snaps to card
rows. When the asset grid
has keyboard focus, arrow keys select adjacent cards; Home/End and Page Up/Down
move through the list while keeping the selected card visible.
`Commands > Show Content` adds the panel to retained layouts, and Ctrl+Space
toggles it. New layouts place Content beside Console. Textures use bounded CPU previews;
materials use an isolated canonical-sphere
preview job, `vkr_bakery preview material`. Meshes and fonts use vector
icons. Preview jobs and GPU texture requests have independent cancellation and
retirement owners; the browser retains at most 64 texture requests and prunes its
generated disk cache through a bounded worker operation.

## Verification and limits

On 2026-09-30, `./build_editor.bat Release` compiled the starter selector and
`./build_test.bat` passed the Debug CPU suite and the 28 shared path cases.
The managed import check in [check_default_templates.py](../../tools/checks/check_default_templates.py)
passed for all three templates. Normal Release Vulkan on Windows (RX 6700 XT,
Vulkan 1.4.315, AMD 26.6.3, graphics validation variables unset) loaded Blank,
FPS Arena and RPG Grounds with 36, 427 and 472 entities, respectively, accepted
their collision journals and advanced `sim.play` to one second without errors.
Three native 1280 by 800 offscreen overview captures passed and were visually
inspected. Their report SHA-256 prefixes are `2c86c88581b8` (Blank),
`2df9ce52492a` (FPS Arena) and `ac62bda39dd0` (RPG Grounds). These captures are
local evidence with no baseline comparison or timing authority. Manual keyboard
traversal and launcher clicks were not tested; Metal was unavailable on Windows.

Publishing the manifest at creation passes the Release editor build and the CPU
project-store suite. A native click-through of a failed first scene job remains
unverified. `tools/checks/check_editor_project_jobs.py` with the Release
`vkr_bakery` covers the open-scene diffuse skip and an invalid recipe that still
fails.

On 2026-09-25 the Release editor opened a scratch copy of a new project's
manifest without loading a scene. It exited 0 and wrote graphics, runtime,
layout and panel settings; before the animation owner wrote an empty object,
every such save failed and shutdown exited 6. A scratch scene failing job
validation showed Back and Retry in a native screenshot. Clicking Back or the
navigation after a failure was not exercised natively.

Two production `create_scene` jobs imported the repository Bistro glTF into one
scratch workspace on the 16 GB M1 Pro, preparing assets without bakes. The first
took 772 s and consumed 3.11 GiB of disk, against about 14 GB for the earlier
layout. It wrote 306 paired files (119 shared normals), 280 converted
specular-glossiness and 18 cutout files to `cache/generated`, and 185 packed
material textures to `cache/textures`; the bundle kept no derived intermediates.
The second import packed no texture, took 332 s and consumed 0.30 GiB, including
swap-file noise on the shared volume. Materials reference 509 textures
(2.95 GiB) instead of 577 (3.38 GiB); all 187 earlier per-factor normals and
187 roughness outputs match the new files' KTX level bytes exactly. The v4
manifest is 887 bytes with a 930 KB inventory. A 90-second Release editor run
rendered the import at 5,093 MiB managed GPU memory without budget errors. A
scratch scene with a 1.89 MB, 1,504-record inventory listed 1,506 assets in
Content, including two editor fonts.
`tools/checks/check_editor_workspace_cleanup.py` builds a synthetic workspace
and checks each keep and remove rule, the grace periods, a second pass that
removes nothing, the unreadable-manifest guard, and cleanup after a real
`create_scene` job. It also checks that `delete_project` refuses a published
project and a link, erases an unpublished project, and frees its cache entries.
The CPU store suite checks unpublishing. Native clicks through the deletion
dialog were not exercised. Existing job checks pass with cleanup active.

Scene import runs a read-only `inspect_scene` preflight before creation. It
reports the detected version, entity, mesh and material counts, dependencies
that do not resolve (including glTF buffers and images), whether a saved-edits
sidecar exists, and what the import will drop. Create stays disabled while a
dependency is missing; **Locate folder** re-inspects against a chosen legacy
root, and **Include saved scene edits** controls the sidecar. The jobs check
covers counts, missing glTF and mesh files, a located root and an unchanged
workspace. On 2026-09-27 `scene.import` inspected Bistro natively (version 2,
three entities, one mesh, nothing missing) and a synthetic scene with a
missing glTF (Create disabled, Locate folder shown). Job warnings, such as an
OBJ channel the renderer does not use, now reach a toast and the Console
instead of being dropped; a native import of such an OBJ showed the toast.
When images are the only missing dependencies, **Use placeholders for missing
images** lets the import replace each with a gray placeholder and record a
warning; the jobs check covers failure without the choice and success with it.
Content details show an asset's build recipe, dependency count and texture
size, and a cooked-only import states that its source is unavailable for
reimport.

On 2026-09-29 a main-thread stack sampler with a window-response probe
(RelWithDebInfo editor, Windows 10, one `content.import` of
`assets/models/bistro-lights.gltf` into a scratch project) found the UI thread
blocked for 3.65 s after the background finalize and 0.59 s after the import,
both in the Content refresh re-parsing a 767 KB inventory whose mesh record
holds about a thousand closure keys; a user's Debug session showed a 7.4 s
gap. With the linear parser, inline record tokens and one owner resolve per
inventory, the refreshes after the finalize and after a new-scene placement
took 0.36 s and 0.27 s under the sampler, most of it the per-asset
containment resolves. Sampler timings include its own suspension
overhead.

Entity addition passes the Release editor build and
`tools/checks/check_editor_add_entities.py --mesh-cooker <built cooker>`.
The CPU publication check covers four light kinds, source/cooked model instances,
reopening, unchanged existing entities and overlay identity, and rejected-request
preservation. Native Add-form interaction and screenshots remain unverified:
the isolated full Bistro import exceeded the existing 1 MiB managed-document
limit before a scene could open. No asset or baseline changes were published.

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
  timing comparison. That digest belongs to recipe 1's HDR studio; recipe 2
  lights the sphere with a constant ambient and two rectangle-light softboxes
  under [ADR-058](058-revision-baked-sky-atmosphere.md), and a 256-pixel job
  published a ready thumbnail with the same report and digest checks.
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

The reflection job ran through the former Python job runner; the same request
now runs as:

```sh
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION -u VK_INSTANCE_LAYERS \
  ./build_release/tools/bakery/vkr_bakery project \
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
[ADR-076](076-project-object-model.md) supersedes the single active world
scene and the Hierarchy, Inspector and Settings ownership described here once
its container and browser phases ship.
