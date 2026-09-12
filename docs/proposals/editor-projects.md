---
status: proposed
updated: 2026-09-12
authority: proposal
---
# Editor Projects

## Outcome and decision status

Replace the editor's fixed scene target with a workspace-backed project chooser,
project and scene creation, project-owned editor preferences, managed imports and
bakes, native file selection, and an asset content browser. This document specifies
the requested scope and remaining acceptance work. Implemented ownership and
publication contracts are recorded in [ADR-069](../adr/069-editor-projects-and-workspaces.md)
and [Architecture](../ARCHITECTURE.md). Retained requirements here are not a claim
that every interaction and platform gate has passed.

The requested features are in scope. The user confirmed the following product
choices during analysis on 2026-09-12. These decisions define the proposed
implementation scope; current production integration is documented separately:

| Choice | Confirmed specification | Tradeoff / alternative |
|---|---|---|
| Workspace location | User chooses a directory containing `.vkreditor`; imports copy their dependencies. | Portable and independent of the repository; requires first-run folder selection. A home-directory store is easier to discover but couples content to one machine. |
| Settings ownership | Project owns editor preferences; scene owns authored content; local workspace state owns machine observations. | Preserves scene semantics and portable intent. Shared preferences with project overrides would introduce another precedence layer. |
| “No meshes” in previews | List mesh assets with vector icons; render texture thumbnails and material spheres only. | Meshes remain discoverable without loading arbitrary geometry for previews. Hiding mesh cards is possible if that was intended. |
| Global compiled tables | Keep renderer source tables and shaders in the build/install tree. | Moving them into `.vkreditor` requires a separate runtime table-loading contract and is not included by default. |

Other recommendations are specified concretely below so implementation has a
reviewable target. Binary asset packaging, cloud collaboration, external linked
imports, and a general material editor are later work.

## Implemented foundation and remaining acceptance

The managed editor now starts at a workspace/project chooser, creates and imports
versioned projects/scenes, copies dependency closures, persists project-owned
preferences, publishes scoped jobs and opens explicit runtime scene inputs.
Native file selection, deferred-close dirty prompts, immutable scene edit
revisions and Content are integrated. [ADR-069](../adr/069-editor-projects-and-workspaces.md)
owns the current contract and selected Metal evidence; the historical fixed-scene
baseline is no longer the production managed-editor behavior.

The remaining requirements below retain the original acceptance target where
coverage or implementation detail remains incomplete. They do not supersede the
current architecture. Native Windows/Vulkan execution, broad DPI/keyboard/modal
coverage, prolonged scene/preview retirement checks and matched Release budget
measurements have not been accepted by the Metal workflow checks alone. Read-only
preparation has CPU checks for unchanged workspace contents, including stale and
unbuilt inputs; its native interaction check remains separate.

## Scope and corrected requirements

1. A workspace contains zero or more projects. A project contains zero or more
   scenes. Exactly one project and at most one world scene are active per editor.
2. Project creation offers an initial scene but permits **Create empty project**.
   An empty project's Scene area explains how to create or import its first scene.
   It must not repeatedly force a wizard that the user cannot dismiss.
3. “All editor settings” means all durable, user-editable preferences, including
   ones currently not persisted. Authored environments, lights, transforms,
   physical camera/lens values and bake regions remain scene content.
4. “Bake once” means reuse an artifact whose complete input fingerprint and
   format/tool versions match. Existence alone is not freshness.
5. Runtime-required conversion cannot be unchecked when no compatible artifact
   exists. Expensive optional lighting bakes can be deferred; raw models do not
   become runtime-loadable by skipping cooking.
6. New imports and generated assets belong under `.vkreditor`. Repository
   `assets/` remains an explicit legacy input and existing app/harness resource
   root during migration. Renderer shaders and compiled integration tables remain
   build/install resources.
7. The chooser needs readable fonts before any project or font bake exists.
   Ship a small versioned bootstrap bundle; do not require first-run cooking to
   display the interface that starts cooking.
8. Model import collects referenced materials, images and buffers. It does not
   promise support for every glTF extension, animation, skinning or OBJ material
   interpretation. Unsupported data must produce an import report.

## User journeys

### Startup and project chooser

Start the application host, renderer and editor UI with a neutral background and
no world scene. Present one focus-trapping Projects modal. Covered panels, camera
capture, transport, picking and shortcuts must not receive input. The normal dock
is not visible until a project is selected.

On first run, offer **Choose workspace** with a native directory picker and explain
that `.vkreditor` will be created there when project creation starts writing data.
Remember the chosen workspace location in a small OS-local editor preference
record. This record is a locator, not a second project registry. If unavailable,
show the chooser with **Locate workspace**; do not silently start a new one.
A command-line workspace argument can provide the location without a dialog.

Scan project manifests in the chosen workspace asynchronously. Rows show project
name, scene count, last successful open time and status. Include search, **New
project**, **Open**, **Refresh**, **Change workspace**, and **Quit**. Corrupt or
newer-version projects remain visible with diagnostics but cannot open for write.
Do not load scene meshes or generate thumbnails during registry enumeration.

The project list is derived from manifests. Last-open metadata belongs in local
state so merely browsing/opening does not rewrite project content. No independent
mutable database duplicates the authoritative project list.

Opening an existing project restores its preferences and selects its last active
scene when valid, otherwise its first scene, otherwise the empty-project view.
Every ordinary application launch still starts at the project chooser. An explicit
automation launch can bypass it as defined under migration and CLI behavior.

### Create project and add scene

Use a two-column dialog. The left column holds project name, project default font,
and the initial scene list. The right column initially offers **Import scene JSON**,
**Create scene**, and a short explanation of empty projects. Selecting Create scene
replaces the right half with the scene form without discarding the project draft.
At narrow widths, use sequential steps with Back, preserving identical draft state.

Validate the name after trimming surrounding whitespace: nonempty UTF-8, at most
128 Unicode scalar values, no control characters. Names are labels, not directory
names. Duplicate display names are allowed and disambiguated with location/ID in
selection UI. Generate stable IDs independently. Validate again on submission.

Allow multiple scene drafts before project submission. Each may be edited or
removed. Cancelling a draft performs no project publication. Submission displays
a review of sources, warnings, required conversions, optional bakes and planned
storage. **Create project** starts the transaction. For an existing project,
**Add scene** reuses the same scene form and publication pipeline without asking
for the project name again.

### Scene form

| Section | Controls and behavior |
|---|---|
| Identity | Required scene name; stable ID generated internally. |
| Environment | None or HDR environment. Native `.hdr` picker, source dimensions/preview, enabled, overall/diffuse/specular intensity and supported SH deringing. Use loader defaults, displayed as actual numbers. Import existing cubemap/atmosphere settings losslessly; procedural-sky authoring UI is a later extension. |
| Reflection probes | Scene-level enable/disable for the draft's probes; list with add/remove, center, positive extents, blend distance, intensity and diffuse/specular contribution. Reuse existing limits and validators. |
| Models | Zero or more `.gltf`, `.glb`, `.obj` files, selected through native multi-select; per-import root transform, dependency summary and diagnostics. Import is allowed without any model. |
| Font | Inherit project default, or choose a scene font. Select source TTF/OTF or an existing managed font. This affects scene text; editor UI fonts stay editor-owned. |
| Entities | Add, edit and remove directional, point, spot and rectangle lights. Show supported controls and physical units; validate finite values, positive ranges/sizes and spotlight angle ordering with existing owners. |
| Baking | Editor, project and scene sections with per-recipe state, dependencies and Reuse/Rebuild/Skip controls. Show why a control is required or unavailable. |

The light list uses the existing component semantics. Imported model lights are
preserved and shown separately from newly authored lights so the wizard cannot
accidentally duplicate them. Do not add a default sun when the imported content
already supplies one. **Add default light** is explicit. Other entity types in an
imported scene must survive even though the initial creation UI only adds lights.

HDR skybox lighting and local reflection probes are distinct. Disabling local
probes does not disable global environment lighting. Project graphics settings can
suppress probe rendering without deleting scene probe data.

When probes are enabled, offer **Add probe from model bounds** after import bounds
are available. Show/edit the result before baking; do not infer useful probes from
empty or degenerate bounds. Default probe values and bake quality reuse existing
validated recipe defaults, materialized into the saved recipe so upgrades do not
silently change old projects. A broad automatic probe-placement algorithm is out
of scope.

### Import scene JSON

Accept supported legacy scene JSON or the managed scene version below. Parse and
validate before changing project state. Display the detected version, entity and
asset counts, missing dependencies, sidecar conflicts and unsupported features.

Import the dependency closure: meshes, material files, textures, environment and
probe faces, diffuse volumes, font config/artifact/source when available, and
source buffers/images where present. Legacy paths first use the explicit legacy
root; relative authored paths use the import's documented origin. If both could
resolve to different files, require a mapping choice instead of guessing. Provide
**Locate missing file/folder** with the native picker and show the remapped set.

Copy inputs and rewrite managed references; never mutate source files. A cooked
legacy mesh can be imported without source geometry, but mark it **Source
unavailable: reimport disabled**. Validate every referenced cooked dependency.
A legacy scene sidecar is content: offer **Include saved scene edits**, enabled
by default when a valid matching sidecar exists. Preserve source identity and
fingerprint conflict checks. Never silently discard conflicting edits.

A scene imported from another managed project receives a new scene ID and remapped
owned asset IDs. Project-scoped dependencies are copied into the destination
project; editor defaults resolve by their bootstrap bundle key/version. Imported
scene settings do not overwrite the destination project's editor preferences.

### Creation progress and recovery

Replace the form with a loading view that shows current phase, current asset,
completed/total known items, elapsed time, warnings and expandable logs. Phases are:
validate, collect/copy, convert required assets, bake optional outputs, validate
outputs, publish project/scene, load resources, ready. Show indeterminate progress
where tools cannot report a denominator; do not fake a smooth percentage or an ETA.

The first phase fixes the plan and dependency counts. Later per-job progress is
monotonic within its phase. A job protocol supplies structured stage/status events;
plain cooker output remains available for diagnosis. Job completion is distinct
from scene activation and optional texture-streaming progress.

Cancel terminates the active child process tree, stops dependent jobs and waits
for writer shutdown before cleanup. Publication is a short noncancellable boundary;
show **Finishing save** there. Failure keeps the draft and diagnostics with
**Retry failed work**, **Back to setup**, and **Discard draft**. Successfully
validated work can be reused only after its fingerprint is rechecked.

If creation saved successfully but runtime loading fails, report **Project saved;
scene could not open**, retain the saved project and offer Retry or Return to
project. Never report the entire creation as lost or delete published content.

### Project and scene switching

The editor header contains separate project and scene dropdowns plus **Add scene**.
The project menu includes **Manage projects**, returning to the chooser. Lists show
loading/failed/unbuilt scenes with textual status. Selecting an unbuilt scene opens
the same build review instead of passing source content to the runtime loader.

Resolve unsaved scene edits with Save / Discard / Cancel before replacement. Flush
project preference saves before switching; a failed save blocks departure unless
the user explicitly chooses to discard those preference changes. End gizmo drags
and text edits before calculating dirty state. Ctrl/Cmd+S saves dirty project and
active scene documents and reports any partial failure accurately.

Only one world scene is resident in v1. Preflight the target on the CPU, then stop
world submission, drain existing resource/GPU use and unload, then load the target.
The loading UI remains active. This avoids doubling Bistro residency. After a
failure, stay in the chosen project's empty/error scene view and offer Retry or
Reopen previous scene; no promise is made that the previous GPU scene survives.
Commit last-active-scene metadata only after successful activation.

Invalidate selection, undo bindings, pending picking results, camera capture and
all scene-dependent renderer histories at replacement. Every asynchronous result
carries a project/scene generation and must be discarded if its generation is old.
Switching is disabled during publication; other pending jobs are cancelled and
joined through the same safe lifecycle before their project owner is released.

## Persistence and file contracts

### Workspace layout

IDs below are illustrative stable IDs, not display names. A project is a JSON
manifest with supporting shared assets; each scene owns a separate folder.

```text
<chosen-directory>/
  .vkreditor/
    workspace.json
    editor/
      bundles/<bundle-version>/
        manifest.json
        fonts/                  # shipped editor UI and fallback fonts
        textures/               # bootstrap atlases and preview environment
      builds/<recipe-key>/      # optional rebuilt editor font artifacts
    projects/<project-id>/
      project.json
      sources/<import-id>/      # project font and other shared sources
      builds/<recipe-key>/      # immutable project artifact bundles
      scenes/<scene-id>/
        scene.json
        edits/<revision>.json   # immutable authored edit overlays
        sources/<import-id>/    # model/HDR/scene inputs and dependency closure
        imports/<import-id>.json
        builds/<recipe-key>/    # meshes, materials, textures, fonts, probes, DVOL
    cache/thumbnails/<preview-key>.png
    local/state.json
    staging/<transaction-id>/
    jobs/<transaction-id>/       # bounded logs and recovery metadata
```

`workspace.json` contains format version and workspace ID, not duplicate project
records. `.vkreditor` is created only when first project creation begins writing
its staging data. Until then the chooser uses installed bootstrap resources.
Install/copy the verified editor bundle on that first transaction. Cancel may leave
an initialized empty workspace and reusable bundle, but no published project.

Source copies are authoritative rebuild inputs, not a cache. Builds are immutable
artifacts referenced by manifests. Thumbnail caches and local state are disposable.
Logs have a retention limit; cleanup never removes a referenced source/build or
another process's active staging directory. No automatic broad garbage collection
ships in v1; explicit orphan cleanup must prove reachability and show its scope.

### IDs, versions and references

Use generated 128-bit IDs in canonical lowercase textual form for workspaces,
projects, scenes, imports, assets and newly authored entity identities. IDs remain
stable across rename and move within a workspace; duplication creates new IDs.
Existing imported node identities retain source-node indices and fingerprints.
Do not substitute display names, array positions or truncated filenames for IDs.

All persisted documents have an integer `version`. Reject unsupported newer
versions for editing without rewriting them. Migration validates and stages a
complete upgraded document before atomic replacement; keep the original backup.
Reject duplicate JSON keys, duplicate IDs, invalid UTF-8, nonfinite numbers,
invalid references and conflicting scene membership. Bound document size, nesting,
counts and decoded image dimensions before allocation; use existing owner limits
where defined and specify any new limits as named constants with diagnostics.

Managed asset references are typed `{ "scope": "scene|project|editor", "id": "…" }`
records. They resolve through the owning scene/project asset list or selected
editor bundle manifest. A scene may use its own assets, its project's assets and
editor defaults; it may not reference a sibling scene's private assets. Promote
shared inputs into project ownership or copy them when needed.

Stored artifact/source paths are relative to the document's declared owner root,
use `/`, and must remain inside that root after canonicalization. No absolute
paths, `..` escape, symlink/reparse escape or network URI is permitted in published
managed references. Native absolute source paths may exist in machine-local
provenance while selecting/importing; runtime documents never require them.
Validate containment with path-component boundaries, not a raw text prefix.

Introduce an explicit resource resolution context at load/import boundaries.
Resolve managed references into canonical absolute loader identities once, then
reuse them. Update transitive material/texture/font/cubemap resolution, cache keys
and sidecar paths together. Do not `chdir`, concatenate `PROJECT_SOURCE_DIR`, or
switch a global mutable filesystem root to implement project switching. Legacy
app/harness loads retain their existing context.

### Project manifest

The following is a structural example; empty settings objects request versioned
defaults on initial creation. A saved project materializes its complete durable
settings record. Existing validators remain authoritative for graphics values.

```json
{
  "version": 1,
  "id": "016fb5a2-8843-433a-a91e-8d987ef266a1",
  "name": "Bistro Lighting",
  "default_font": { "scope": "editor", "id": "default-scene-font" },
  "editor_settings": {
    "version": 1,
    "graphics": {},
    "layout": {},
    "viewport": {},
    "input": {},
    "panels": {},
    "bakery": {}
  },
  "scene_editor_state": {},
  "assets": [],
  "scenes": [
    {
      "id": "bf27578b-406d-4cb3-9ed7-8f54fb8dcba5",
      "name": "Bistro Day",
      "path": "scenes/bf27578b-406d-4cb3-9ed7-8f54fb8dcba5/scene.json"
    }
  ]
}
```

Editor bundle IDs such as `default-scene-font` are reserved stable symbolic keys,
resolved within an explicitly versioned bundle; generated user assets use UUIDs.
The project scene entry owns the display name. Scene JSON owns the same scene ID
but does not duplicate a mutable name; rename updates the project entry atomically.
Scene order is the project array order. `scene_editor_state` is keyed by scene ID
and stores editor camera pose, hierarchy expansion and other per-scene UI recall.
Removing a scene also removes those preference entries.

Project `assets` entries contain ID, kind, display name, managed source path when
available, import ID, recipe/configuration, dependency IDs, fingerprint, artifact
paths and format version. `default_font` must resolve to a validated font record.
Project fonts expose their source/config and cooked VKFA references through this
list; there is no separate unversioned font-path setting.

### Managed scene and imports

Add scene format version 3 as an evolution of the current scene schema.
Keep existing environment, lights, entities, fog, atmosphere, subsurface and
probe semantics. Version 3 adds `id`, `assets`, `default_font` (null means inherit),
`bake_recipes`, `edit_overlay` (null or an owned revision path), and stable IDs for
newly authored entity wrappers. Replace managed
resource string fields with typed asset references in v3; v1/v2 paths remain
supported only through the legacy loading/import context.

The v3 resource-field mapping is:

| Existing field / dependency | Managed representation |
|---|---|
| Entity `mesh.path` | `mesh.asset` reference with mesh artifact role; keep other mesh fields. |
| Shape `material.path` / material lookup name | `material.asset` reference with material role; display names never resolve identity. |
| Text `text3d.font` lookup name | `text3d.font` reference, or null for scene/project inheritance. |
| Environment `equirect`, `cubemap` / face base path | `environment.asset` reference with environment role and source kind in its asset record; scalar environment fields stay in the scene. |
| Probe `cubemap` / face base path | Probe `asset` reference with probe-cube role; capture and influence parameters remain scene-owned. |
| Diffuse volume `path` | Diffuse volume `asset` reference with volume role. |
| Material texture paths, font config source/artifact paths, cooked mesh material dependencies | Bundle-relative paths for newly cooked bundles; explicit immutable remap records for cooked-only legacy bundles. |

Omitted and null inherited font values normalize to the same saved representation.
Reject legacy path and managed asset fields appearing together on a v3 component.
Assets with absent required artifact roles are unbuilt; this state is derived from
their records, not a second writable scene-ready boolean.

A scene-owned asset entry has the same contract as a project-owned entry. Runtime
references select a validated artifact role (mesh, font, environment, etc.), not
an arbitrary path from the manifest. A mesh asset's bundle includes its material
and texture closure; file references inside a bundle are relative to that bundle
and are validated transitively. Renderers continue consuming existing prepared
handles and packet structures, not project JSON or asset IDs.

For cooked-only legacy inputs, retain validated immutable blobs and record a
complete old-reference-to-managed-reference map in the import manifest. The load
context resolves those exact references without legacy filesystem fallback. Scope
legacy material/resource names by import identity in runtime caches so separate
imports cannot collide. This is a compatibility path for copied cooked artifacts;
new cooker outputs must use the managed bundle contract. Reject an unmapped
dependency; source geometry is not required merely to relocate a validated mesh.

An import manifest records the copied root source, all dependencies with relative
paths and content hashes, importer/version/options, source-to-managed identity
mapping, generated artifact inventory, unsupported features and reimport status.
It is immutable per import revision and referenced by the owning asset record.
Reimport produces a candidate revision; it never overwrites published bundles.

Retain the existing authored override journal semantics in managed
`edits/<revision>.json` files, selected by `scene.json.edit_overlay`. Each overlay
must include/validate scene identity and source fingerprints and retain original
source entity ordering where legacy identities rely on indices. Expand the edit
owner to serialize newly authored lights and scene-level environment/probe changes
if those are edited after creation; the existing limited overlay is insufficient.
Keep one authority for each field: base imported/authored scene plus explicit
scene-content overrides. No layout, quality settings or browser state belongs in
this overlay. Flattening overlays into scene JSON is a later export/reimport
choice; it must not silently weaken fingerprint conflict protection.

### Complete settings ownership

| Persistent state | Owner | Application policy |
|---|---|---|
| All `VkrGraphicsSettings` fields | Project `editor_settings.graphics` | Retain validation; apply live-supported values and expose restart-required ones. |
| Dock layout, visible panels, floating overlay positions | Project settings | Serialize currently transient supported UI geometry; clamp on smaller displays. |
| Viewport presentation, grid/gizmo/label/debug preferences, camera speed and input bindings | Project settings | Persist exposed user choices; this does not require building a new shortcut editor. |
| Editor camera pose, hierarchy expansion, selection recall, scene-specific UI tab state | Project `scene_editor_state[scene-id]` | Restore only valid identities; no ECS pointers or runtime handles. |
| Console filters/follow mode, browser sort/filter/size, Bakery recipe defaults | Project settings | Persist preferences, not console contents, process handles or running-job state. |
| Lights, environment, probes, fog, authored camera/lens, entity transforms/material overrides and scene bake configuration | Scene content | Existing component/renderer validation and scene save/undo semantics. |
| Workspace locator, last opened project/scene, file-dialog directories, actual window/display placement and device capabilities | OS-local locator / workspace local state | Never changes portable project intent. |
| GPU handles, selection readbacks, undo storage, job progress, temporary errors | Memory or recovery job record | Never serialized as preferences. |

Before implementation, inventory every editor setting and default by owning field,
including [graphics UI](../../editor/src/editor_graphics.c),
[windows](../../editor/src/editor_windows.c), [dock](../../editor/src/editor_dock.c),
[labels](../../editor/src/editor_labels.c) and [scene panels](../../editor/src/editor_scene_panels.c).
The inventory is a completion checklist in the implementation work, not permission
to leave currently unsaved settings behind.

Preserve the requested graphics settings when a backend/display cannot support
them; derive separate effective settings and show the reason. Current startup-owned
values (vsync, HDR, temporal upscaling, dynamic resolution and render scale) remain
restart-required in v1. Switching projects applies live values and shows requested
versus running values for startup settings. **Restart editor** flushes state and
uses a one-use project resume token; normal launches still show the chooser.
Hot renderer reinitialization is not an implicit prerequisite for Projects.

Project preferences debounce through one project writer and flush on switch/exit.
No editor code writes the old standalone preference files in managed mode. Scene
content uses explicit Save and the existing dirty/undo behavior. A failed write
keeps the corresponding dirty state and presents an actionable error.

## Import and bake implementation contract

### Source dependencies and conversion

Resolve glTF buffers and image URIs relative to the glTF document. Extract GLB
bufferView and data-URI images into bounded managed source files, retaining enough
provenance to reimport from the original source. Resolve OBJ `mtllib` relative to
the OBJ and MTL image references relative to the owning MTL. Preserve subdirectory
structure or generate explicit unique mappings; never collapse to basenames.
Detect case-only collisions before publication so a workspace remains portable
between case-sensitive and case-insensitive filesystems.

Collect all referenced image bytes, including channels the renderer cannot use.
The report distinguishes **Imported and used**, **Imported but unsupported**, and
**Missing**. Missing required geometry/buffer/image dependencies block creation by
default. An explicit **Use placeholder** choice may accept a missing material
image; record that decision and display it in the browser. Never silently find a
same-named texture in legacy `assets/`. Unsupported geometry/compression cannot be
accepted as a successful visible model without a supported conversion path.

Retain current material conversion semantics. Texture recipe identity includes
color space, channel role, normal-map interpretation, cutout handling and paired
normal/roughness or specular/glossiness transformations. Equal source bytes with
different semantic use may need distinct cooked outputs. Do not deduplicate by
filename alone. Preserve imported mesh instancing, hierarchy and node fingerprints.

Durable cooker namespaces and recipe identities use the managed import ID plus
logical source-relative path/content, never physical staging or absolute workspace
paths. In particular, replace the managed path through the current glTF source-path
hashing helper. Moving staging into place or relocating the workspace must not
change material IDs or invalidate equivalent source fingerprints. Canonical
absolute paths are runtime cache lookup identities only; legacy import hashing
remains unchanged until converted through its explicit identity mapping.

Give mesh, material, image, font and generated-texture cookers explicit source and
output contexts. A mesh `--output` flag alone is insufficient: every generated
material and derived texture must honor the destination. Cooked `.vkb`, `.vkfa`,
KTX2, HDR probe and diffuse-volume formats retain their existing validators and
version rules. Input provenance changes do not by themselves justify a new GPU ABI.

Reimport is an explicit asset action in v1. Compare source/import fingerprints,
show changed dependencies, create a candidate bundle and check editor overrides
against the new source identities before committing. A conflict leaves the old
bundle active with diagnostics. Automatic file watching is optional later work.

**Rebuild** consumes the managed source snapshot. **Reimport** selects or locates
an external root and captures a new dependency closure. Missing original external
files do not prevent rebuilding a complete managed snapshot; they require Locate
before reimport. Cooked-only assets without source snapshots cannot be rebuilt.

Imported probe/volume artifacts retain their original provenance report. Verify
that provenance and derive a managed record only when the dependency mapping
proves equivalent content and scene semantics. Path rewrites alone are not proof.
Otherwise mark the bake Stale or Source unavailable and offer rebuild/disable;
valid payload checksums alone do not establish Current.

### Ownership and available work

| Scope | Products / work | Default |
|---|---|---|
| Editor | Installed UI/fallback font bundle, light label resources, canonical preview environment; optionally recooked editor fonts | Reuse verified shipped bundle; Rebuild only where source and tool are available. |
| Project | Project default font and other explicitly shared imports | Reuse if current; prepare required missing/stale products. |
| Scene required | Mesh/material/texture conversion, chosen scene font override, source validation and runtime environment preparation | Prepare required artifacts or save as unbuilt. |
| Scene optional | Local reflection-probe captures and supported diffuse-volume bake | User-selected; default expensive scene lighting bakes off. |
| Renderer build | GGX DFG, Charlie, anisotropy integration tables, production shaders | Outside project creation; retain explicit developer/build workflows. |

Environment conversion, SH projection and specular prefilter currently include
runtime GPU preparation. v1 reports this as **Preparing environment** during scene
loading; it does not claim a portable persistent offline cache exists. A future
persistent environment artifact requires its own versioned format and backend
acceptance. Source HDR and existing cooked probe cubes are managed scene assets.

Font precedence is scene override → project default → installed default scene font.
An absent override means inheritance, not a copied project font path. An invalid
explicit font blocks opening until repaired or the user selects fallback; do not
silently replace it. Existing scene text with explicitly named legacy fonts must
be mapped on import and remain explicit. Register project/scene fonts before text
entities instantiate, using scoped IDs to prevent name collisions. Editor UI fonts
outlive world scenes. Rebuilding an in-use editor font retains the current bundle
until restart; project/scene font replacement occurs at a safe scene reload boundary.

Diffuse-volume baking is available only when current geometry/bounds pass the
existing enclosed-volume proof and resource limits. Show the exact failure for
empty, open or degenerate scenes. Do not weaken bounds/probe/photon budgets to make
**Bake all** succeed. Reflection recipes expose probe position, extents, capture
resolution and existing supported quality parameters. Snapshot the current authored
scene plus validated overrides for lighting bakes; never bake a different on-disk
revision from the one shown in the review.

### Freshness and selection

Each recipe's observed state is Missing, Current, Stale, Unsupported, Queued,
Running, Failed or Cancelled. Selection is a separate action: Reuse, Rebuild or
Skip optional. **Bake all** chooses Rebuild for supported optional recipes and
prepares required work; it never enables unavailable recipes silently.

Fingerprint canonical recipe parameters, tool/format versions, dependency content
hashes, material semantics, relevant scene transforms/lights/environment and the
producer configuration needed for reproducibility. Include backend/capability
identity for backend-dependent captures. Verify output integrity before Current.
Changes in unrelated editor layout or filters do not invalidate lighting bakes.
A matching recipe hash with a corrupt/missing output is Missing, not Current.

When a requested scene feature requires missing or stale output, offer Rebuild,
Disable feature for this scene, or Save unbuilt. A disabled feature may retain a
previous artifact without using it. Stale output never becomes Current merely
because rebuilding was unchecked. **Save unbuilt** publishes authoring/source
content with missing artifact references explicitly marked; it is a valid project
entry but cannot activate until required products are prepared. There is no runtime
fallback to raw OBJ/glTF or an implicit expensive bake on every open.

Order jobs by dependencies: source collection → material/texture conversions and
font work → completed mesh bundle → temporary loadable scene snapshot → selected
lighting bakes → output validation → publication → runtime resource preparation.
Thumbnail generation is noncritical and starts after publication; a failed preview
must not fail a valid scene creation. Serialize cooker children initially, reusing
Bakery's existing queue. CPU-only parallelism can be added after ownership and
memory budgets are measured.

Reflection capture currently uses a repository-scoped harness wrapper. Adapt it
to accept a job-owned scene snapshot, explicit asset roots and job-owned case and
output paths without writing into `assets/` or tracked cases. Keep one GPU bake
process active and pause editor world/preview rendering during GPU captures.
The UI may continue ordinary UI-only presentation. Preserve capture process
completion and failure diagnostics; do not put offline cooking on the UI thread.

Pass executable and argument arrays to subprocesses; never interpolate selected
paths into shell command strings. Resolve installed tools independently of CWD.
Job records own copied inputs and progress strings. Bound in-memory log tails and
write full logs into the job directory with a documented retention limit.

### Publication, cancellation and concurrent access

Use one project writer plus a workspace coordinator for editor-bundle installation
and project discovery changes. Acquire an exclusive workspace write lease for v1;
a second editor offers read-only browsing/opening and never writes preferences,
imports or caches into that workspace. Use an OS-backed lock so crashes release
ownership; distinguish a live lock from stale recovery metadata.

Create/cook in a transaction directory on the destination filesystem. Build outputs
are immutable bundles. Validate the entire reference closure, flush completed
files, publish bundles, and atomically replace the authoritative manifest last.
A set of file renames is not one transaction: project creation is visible only
when its complete project manifest has been published. An added scene is visible
only when the project manifest links its completed scene document.

An update to existing scene content publishes its immutable overlay revision and
candidate bundles first, then atomically replaces `scene.json`, including its
`edit_overlay` pointer. Readers observe a matching base/overlay pair through that
one commit point. Never overwrite an overlay already referenced by a manifest.
Independent preference and scene saves may report separate results. The
transaction journal records intended publication, old/new revisions and completion
so recovery can distinguish a published commit from abandoned staging. Re-read the
expected manifest revision/hash before replacement to detect external modification.
Never overwrite externally edited data on a stale in-memory save.

Cancel/error leaves existing manifests and referenced artifacts intact. Remove
only transaction-owned unpublished files after all child writers stop. On restart,
scan incomplete journals and offer resume/retry or discard after verifying the
recorded paths remain within their owner. A crash after manifest publication but
before journal completion is a successful commit, identified by revision/hash,
not a reason to undo the new project. Disk full and permission failures preserve
drafts, dirty markers and the previous readable project.

Rehash or otherwise verify snapshotted source bytes against the plan before
publication. If input changes while being collected, fail with **Source changed;
retry import** rather than publishing a mixed revision. Once a complete source
snapshot exists, a later external source change does not mutate that snapshot.

## Native file selection

Expose a typed platform/window-owned dialog request for Open file(s), Open folder,
and Save file where export needs it. Support filters, initial directory and
multi-select. Return Selected with an owned UTF-8 path list, Cancelled, or Error
with a diagnostic. Cancellation is not an import error and does not clear the
previous valid field. Paths are released by the request owner after copying into
its draft/job. Do not retain native dialog memory or frame-scratch views.

Use native macOS and Windows dialogs on their required UI/platform thread. The
editor UI borrows window ownership through the application/platform boundary;
portable UI logic contains no platform object pointers. Restore focus and input
capture after dismissal, preventing a closing click or Enter from activating the
covered form. Modal ownership must survive resize and display-scale changes.

A filter is convenience, not validation. After selection, verify a readable regular
file (or selected directory), supported extension and actual parseable format;
check again during import. Reject malformed/oversized decoded content and report
the exact source/reference that failed. Handle Unicode, spaces, long paths,
symlinks/reparse points, deleted files, permission changes and case collisions.
Source trees may reference files outside their immediate folder: show these in
the dependency review and copy them through an explicit mapping; published managed
paths must still stay contained. Do not follow network URI dependencies implicitly.

Every source-path field has **Browse**; manual paste may remain an advanced option
using the same validation. Output directories are generated by the workspace owner,
not typed by users. Bakery's general file selectors should use the same facility.

## Content browser, thumbnails and icons

The implemented dockable **Content** panel follows the sources/navigation/search/
asset-view organization in Epic's [Content Browser interface](https://dev.epicgames.com/documentation/en-us/unreal-engine/content-browser-interface-in-unreal-engine).
It has a left Scene / Project / Editor sources tree with logical type folders,
upward breadcrumbs, compact Import/refresh controls, search/type/sort controls,
virtualized cards with type-color strips, and optional right Details actions.
Side panels hide at narrow sizes. Ctrl+Space toggles Content; the command palette
can add it to older layouts. Presentation preferences persist in the project.
Cards come from managed inventories refreshed explicitly, with immediate icons
while previews load. Arbitrary filesystem/build UUID trees are not content folders.

The remaining inspection and interaction targets are:

Cards show type, display name and Current/Stale/Missing/Building/Error status in
text and icon form. Selection shows source provenance, artifact role, dimensions
where relevant, dependencies, recipe and diagnostics. Initial actions are Import,
Inspect, Reveal in file manager, Reimport, Rebuild and Retry preview. Project/scene
rename is supported independently of filesystem names. Deletion, drag-and-drop
placement, arbitrary material authoring and dependency promotion UI are later
extensions; the storage contract already supports project-owned shared assets.

| Asset | Preview |
|---|---|
| LDR texture | Image thumbnail, correct color-space interpretation, checkerboard for alpha. Data maps can expose channel/normal views in inspection. |
| HDR environment | Fixed documented exposure/tonemap, independent of the active scene's exposure. |
| Material | Canonical sphere, fixed camera, neutral HDR environment and fixed rendering settings; show fallback plus diagnostic for unavailable dependencies. |
| Mesh | Vector mesh/type icon only, never a rendered model preview. |
| Font | Font icon and metadata in v1; optional sample text in Inspector using the existing font system. |
| Scene, source, probe, volume, unknown supported artifact | Distinct type icon and metadata; no arbitrary scene/mesh render for a thumbnail. |

Material previews reuse the existing portable material path and
[offscreen target contract](../adr/014-offscreen-present-target.md). They require a
separately owned preview scene/camera/history and a canonical sphere from existing
geometry primitives. They must not temporarily replace or mutate the active scene,
its materials, camera, graphics preferences or temporal histories. Keep the preview
owner small and lazy; suspend it during scene loading, switching and GPU bakes.
If a shared-renderer multi-view lifetime cannot be established, use a serial
isolated preview process rather than introducing a second interactive renderer
without an explicit memory/lifecycle decision. The chosen mechanism must pass the
acceptance gates before material spheres are considered delivered.

Cache previews by asset/dependency fingerprints, preview recipe version,
environment identity, output size and backend-relevant renderer version. Store
atomic completed images in `cache/thumbnails`; decode failures fall back to icons.
Texture previews must not load the full world scene. Generate only visible items
plus a bounded prefetch margin and cancel obsolete requests on navigation.

Initial implementation limits: one material preview job at a time, default 128-pixel
previews with supported sizes capped at 256 pixels, at most 64 queued preview jobs,
64 MiB decoded CPU preview cache and 64 MiB published GPU thumbnail cache. These are
proposed configurable upper bounds, not measured frame-budget claims. Decode through
an LRU/freeable owner; account separately for unavoidable full-image decoder scratch
and reject or defer inputs beyond its explicit import/preview cap. Do not hide such
scratch outside memory accounting. Evict disk thumbnails under a configurable
512 MiB workspace limit. Visible thumbnails may reload after eviction.

CPU work runs on owned workers; GPU upload/finalization stays at the render-thread
publication boundary. Completion-retire textures before reusing their resources.
UI holds frame-borrowed texture references, not pointers into a growable cache.
Project/scene generation checks discard late results. Instrument preview work and
verify it yields to interactive world rendering; lower scheduling frequency if
matched measurements show a frame-budget regression.

Extend `VkrUiIcon` and the existing DPI-aware vector renderer. Required icons:
project, folder, scene, new/add, import, browse, mesh, material, texture, environment,
font, directional/point/spot/rectangle light, probe, bake, refresh/rebuild, cancel,
warning, error, current and inherited. Reuse existing equivalents. Maintain a
consistent visual weight and alignment at supported DPI scales; do not introduce
an icon font or unrelated bitmap set. Buttons have text or tooltips, keyboard focus
and meaningful names. Status must remain understandable without color alone.

Projects owns the specific modal, wizard, asset grid and progress interactions.
The broader [Editor UI extensions](editor-ui-extensions.md) proposal still owns
native detachable windows and general accessibility work. Projects requires
keyboard traversal, focus trapping, Escape/Back behavior, scrollable validation
messages and mouse/keyboard parity without waiting for those broader features.

## Code ownership and integration boundaries

| Owner | Responsibility |
|---|---|
| `editor/src` project state | Workspace discovery, project serialization/writer, drafts, chooser/wizard/browser UI, preference ownership and typed lifecycle requests. Add a project module because this lifetime is independent of the active scene. |
| Existing sample/standard runtime | UI-only startup, explicit scene selection/context, graphics effective/requested values, resource loading, safe unload, history invalidation and font registration. Keep app behavior configurable separately from editor policy. |
| Resource/scene/edit owners | Managed v3 parsing/lowering, transitive resolution, scene-content overrides, dependency validation and source identity preservation. Runtime gets explicit scene/font/settings inputs; it does not discover editor workspaces. |
| Existing Bakery/tool owners | Immutable job inputs, explicit destinations, dependency collection, cook execution, progress and publication results. Extend existing recipes; do not create a competing bake implementation. |
| Platform/window layer | Native picker lifecycle and path result ownership; filesystem layer continues owning canonicalization, atomic rename and file errors. |
| Thumbnail owner | Bounded CPU/disk/GPU cache, canonical preview state and cancellation independent of scene entities. |
| Renderer | Existing frame/offscreen/material/publication contracts; no project JSON, project registry, filesystem browsing or cooker orchestration. |

Choose retained project/draft arenas for data destroyed together, freeable storage
for independently evicted thumbnail/index entries, per-job scratch for import
stages and frame scratch only for borrowed UI draw construction. Project shutdown
cancels and joins jobs before releasing their storage; resource loaders and the
native publisher survive through final GPU-safe release. Review
[ADR-006](../adr/006-cpu-memory-allocators.md),
[ADR-009](../adr/009-frame-synchronization.md) and ADR-045 at implementation time.
Any production shader or shader-visible preview contract change must inspect both
native backends and use the shader workflow; a thumbnail does not waive parity.

## Legacy migration and launch behavior

Keep legacy assets readable and label **Import legacy scene** in the import flow.
Do not physically rename or delete repository `assets/`, rewrite its manifests,
or silently copy all of it at startup. Import Bistro as the migration witness,
including its material/texture/environment/probe dependencies and optional valid
sidecar. New editor jobs cannot publish into legacy asset directories.

On first project creation offer **Use current editor preferences** to import the
standalone graphics/layout records once, validating them through their existing
owners. Preserve the old files. Project preferences become authoritative after
publication; malformed legacy preferences report their skipped sections. Do not
apply the first imported scene's preferences over later scenes in the project.

The editor's managed mode accepts proposed `--workspace <directory>`,
`--project <id-or-project-json>` and `--scene-id <id>` arguments. Explicit project
launch skips the chooser after validating membership and references; specifying
only a workspace keeps the chooser. Conflicting project and legacy scene inputs
are errors. Resolve a project manifest path to its containing workspace rather
than silently importing it.

Keep explicit `--scene <legacy-scene.json>` as a developer/harness compatibility
mode with clear legacy status and no managed project writes. `VKR_SCENE_PATH` and
`VKR_AUTOLOAD_SCENE` must not bypass the normal managed chooser accidentally;
restrict their editor behavior to explicit legacy/automation mode and document
wrapper changes. `--scene-only` controls presentation after explicit selection,
not selection policy. Existing app/harness paths remain compatible.
`VKR_GRAPHICS_SETTINGS_PATH` and `VKR_EDITOR_LAYOUT_PATH` retain their legacy/harness
meaning; managed mode uses project JSON and diagnoses conflicting write overrides.

Install editor bootstrap assets from a versioned application bundle independently
of repository location. During implementation the existing pinned assets can
supply that bundle, but shipping Projects requires a launch test outside the source
tree with no runtime reads from legacy `assets/`. Source shaders and compiler table
includes remain build inputs, and compiled runtime binaries remain installation
inputs; neither is user-project content.

## Acceptance inventory

Selected successful Metal workflows are recorded in ADR-069. The following
inventory defines regression coverage; ADR-069 records completed checks and
unavailable platform or measurement gates. A build or a different native backend
does not implicitly pass an entry. Build wrappers compile tools/shaders;
Bakery and explicit cooker commands own artifact generation. Serialize GPU runs.

### Required failure and regression checks

Use the cheapest independent oracle for each invariant. Small synthetic fixtures
are only for isolated parser/import/path/lifecycle defects. All scene-based native,
visual, smoke and performance checks use Bistro, never another substitute scene.
For switching, use distinct managed copies/configurations of Bistro plus an empty
project where appropriate.

| Invariant | Evidence / failure case |
|---|---|
| Project schema is durable | Save/reopen preserves every preference and scene link; malformed/newer versions, duplicate IDs/keys and wrong membership fail without overwriting originals. |
| Rename is safe | Rename projects/scenes/assets with Unicode and duplicate display names; IDs, source identities and artifact links remain valid. |
| Workspace is portable | Import Bistro, move `.vkreditor`, launch from another CWD/outside the repository, remove original source access and reopen successfully. |
| Asset references are contained | Traversal, sibling-prefix traps, symlink/reparse escapes, case collisions and absolute-path injection cannot escape managed output roots. |
| Import really collects dependencies | GLB bufferView images, glTF data URIs/external buffers, percent-encoded paths, duplicate image basenames and nested OBJ/MTL texture paths resolve to the correct bytes. |
| Unsupported import is honest | Unsupported material channels remain reported; malformed models and missing required buffers cannot produce success cards. |
| Settings have one owner | Change every inventoried setting, switch Bistro scenes/projects, restart and check the intended scope; no managed-mode writes to old preference files or scene UI sidecars. |
| Authored content survives | Valid existing Bistro edits import; changed source fingerprints reject conflicting overlays; new light/probe settings and font inheritance survive explicit Save/reopen. |
| Bake freshness is correct | Change each relevant dependency/recipe/tool version; invalidate only affected products. Corrupt outputs cannot be reused. |
| Publication is recoverable | Cancel or kill during copy/cook/publish, simulate permission/disk failure and restart; previous manifests still reference complete validated bundles. |
| First run is usable | No workspace/cooker outputs exist; chooser and progress text render using the shipped bundle. Missing/corrupt bundle gives an actionable startup diagnostic. |
| Switching is safe | Switch/cancel during CPU prepare, GPU finalization, picking and preview completion; stale generations never change the active project/scene. |
| Memory is reclaimed | Repeated Bistro load/unload and preview navigation return live bytes/handles after GPU retirement; separate bounded cache capacity from live leaks and OS RSS. |
| Native dialogs work | macOS and Windows select/cancel/error, multi-file, Unicode/space/long paths, missing-after-selection and focus/DPI transitions. |
| Previews are isolated | Material sphere/texture thumbnails do not alter Bistro camera, exposure, settings or histories; failed previews show icons and diagnostics. |
| UI remains usable | Chooser focus trap, keyboard navigation, narrow wizard, loading cancellation, scrolling grid, tooltips and all icon states have screenshots/clips at supported DPI scales. |
| Frame budget is preserved | Matched capture-free Release Bistro measurements with browser hidden, visible/current, and generating previews; inspect timing spread and bound/yield background work. No unmeasured speed claim. |
| Existing consumers still work | Repository wrapper builds and explicit legacy app/harness paths remain valid; no automatic asset cooking in normal builds. |

Run native Metal and Windows/Vulkan evidence separately. CPU tests or source audit
do not establish GPU parity. Record exact commands, configurations, reports and
unavailable gates in the implementation evidence; do not claim a platform passed
because the other did. Rendering/UI delivery includes screenshots or clips and
identifies installed bootstrap, imported and generated asset changes. This proposal
itself requires documentation/link checks only, not a renderer build.

## Alternatives and later extensions

- A single JSON containing project and scene payloads simplifies one write but
  makes scene reuse, independent bakes and large imports harder. Keep linked scenes
  in separate folders as requested.
- External linked sources save disk space but make relocation and reproducibility
  dependent on host paths. Copy on import for v1; later linked imports require a
  visible missing-source/relink policy.
- A global content-addressed asset store could deduplicate between projects but
  adds reference accounting and garbage collection. Use immutable recipe-keyed
  bundles within explicit ownership scopes first.
- Scene templates (empty, HDR-lit, imported model) can prefill the same wizard.
  Templates must not become a second creation or baking implementation.
- Later additions may include source watching, dependency promotion/cleanup UI,
  project duplication/export, mesh previews, material editing, drag-and-drop asset
  placement and procedural-sky creation controls. None blocks the requested v1.
- Future binary packaging can consume the manifest dependency graph and immutable
  artifacts. Keep logical IDs separate from storage paths now; do not design or
  implement blob formats, compression or runtime package mounting in this feature.
