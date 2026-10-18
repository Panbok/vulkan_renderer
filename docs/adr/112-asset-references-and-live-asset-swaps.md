---
status: implemented
updated: 2026-10-18
authority: adr
---
# ADR-112: Asset references and live asset swaps

## Status

Accepted. Implemented in the editor and runtime on macOS and Windows, both
fed by the Bakery daemon's file watch ([ADR-077](077-asset-build-system.md)).
The Metal editor has run texture and mesh swaps. The Vulkan editor on Windows
has run automatic rebuilds and swaps of a project texture only.

## Context

A scene stored a dropped project material by its absolute artifact path, under
`builds/<revision>/`. A rebuild writes a new revision and retires the old one,
so the stored path broke. It also broke on another machine after a depot sync,
because the workspace path differs. A dropped material never reached the
scene: the viewport refused it, `material.assign` accepts only content-root
paths, and in a project the drop fell into the rebuild catch-all. A texture
could not be dropped at all.

A Rebuild or Reimport unloaded the open scene when the job started and loaded
it again when the job ended. A source edit queued that Rebuild only for an
asset of the open scene, and only while the scene had no unsaved edits. A
project-shared asset and an edited or removed build file only showed a
diagnostic.

## Decision

1. **Asset references.** A scene names a managed asset as
   `asset:<scope>/<id>` (`scene`, `project` or `editor`), never by revision.
   Lowering writes `asset_refs`, `[{ref, path}]` sorted by `ref`, into every
   runtime scene: the primary artifact (the artifact whose role is the record's
   kind) of every record of the three inventories. A portable preparation lists
   only the references its documents name
   ([vkr_project_lower.c](../../tools/bakery/project/vkr_project_lower.c)).
2. **Reference table.** `VkrRenderAssets` keeps one table from reference to
   current path in its own DMemory heap behind a mutex, because scene loads
   publish from loader workers
   ([vkr_render_assets.h](../../runtime/src/renderer/systems/vkr_render_assets.h)).
   Scene loads add only the references the table does not hold yet, so a
   container lowered earlier cannot move a reference back to an older
   revision. The Content browser publishes every listed asset after each
   listing read, and a live rebuild publishes the new revision. Entries stay until shutdown, so the table is bounded by the
   distinct references loaded.
3. **Resolution.** `vkr_scene_material_load`, which loads material overrides,
   brush face materials and decals, resolves a reference to its current path
   first, so caches key on revision paths. A `.mt` loads as a material. Any
   other artifact is a texture, drawn as an opaque PBR material named after the
   reference, with the texture as sRGB base colour
   ([vkr_scene_system.c](../../runtime/src/renderer/systems/vkr_scene_system.c)).
4. **Drops.** Content accepts built materials and managed textures on the
   viewport. A drop picks the object under the pointer and runs
   `material.assign` with the item's reference, or with the content-root path
   for a built-in material. `material.assign` accepts a reference that
   resolves to a `.mt` or a texture image and rejects any other. Every submesh of a mesh, or every face
   of a brush, takes it. The project's content-action handler routes material
   drops and opens, as it already routed scripts
   ([editor_content.c](../../editor/src/editor_content.c),
   [editor_projects.c](../../editor/src/editor_projects.c)).
5. **Cooked standalone textures.** Importing, rebuilding or reimporting a
   standalone image writes `builds/<rev>/textures/<sha>.<ext>` and cooks
   `<sha>.<ext>.vkt` beside it, sRGB colour, at the preview tier with a recipe.
   It counts in `preview_assets`, so finalize brings it to the final tier
   ([vkr_project_import.c](../../tools/bakery/project/vkr_project_import.c)).
6. **Live rebuild.** `rebuild_asset` and `reimport_asset` take `asset_scope`
   (`scene` or `project`). A project-scope job rebuilds the record against the
   project, returns the project inventory, which the editor saves, and lowers
   the open scene again. It retires nothing, because other scenes may name the
   old revision. Every result reports `changed_assets`,
   `[{scope, id, kind, old_path, new_path}]`, and `retired_assets`.
   While the job runs the scene stays loaded with its edits, and saves wait.
   When it ends, the scene adopts the new publication fingerprint and keeps its
   loaded runtime path, as finalize does. Then each changed asset swaps in:
   - its reference names the new revision;
   - a material replaces the live one of its name;
   - a texture moves the material it draws as;
   - a mesh moves its instances with `vkr_mesh_manager_swap_asset_path`.

   The swap moves instances from the path the reference named before, which
   is the revision the instances draw, and from the job's `old_path` when it
   differs. A failed project save, or a result the editor cannot adopt, swaps
   nothing and shows a warning. A mesh that has no live swap, or a kind such as
   a font, reopens a clean scene; with unsaved edits it shows at the next open.
   A failed job keeps the last build and shows a warning
   ([editor_projects.c](../../editor/src/editor_projects.c)).
7. **Mesh swaps.** The swap loads the new revision with each old asset's
   domain, shader override and source index. When every new asset has settled,
   it moves the instances at one `vkr_mesh_manager_pump_async`. A moved
   instance keeps its handle, transform, overrides, render id and lightmap
   slot. Its content revision rises and its bounds note a shadow topology
   change. Old assets go through the reference count and the backends'
   GPU-completion retirement. Skinned instances keep the old mesh
   ([vkr_mesh_manager.c](../../runtime/src/renderer/systems/vkr_mesh_manager.c)).
8. **Automatic rebuilds.** For an asset of the project or of the open scene,
   the Content browser queues a rebuild when any of these changes on disk:
   - its source;
   - its build file (edited or removed);
   - for a texture, the image, which is both its source and its build file.

   The editor watches the resolved project directory and resolves each
   report once. It compares the report with the paths the listing resolved
   by the host's path rules (`file_path_equals`), because the daemon reports
   `/` separators while Windows resolves to `\`
   ([editor_content.c](../../editor/src/editor_content.c)).
   A build file proves an edit by its SHA-256 against the record's
   `fingerprint`, so a report that arrives after the listing was read again
   still counts. Another source still counts only when its time is newer than
   the listing. Rebuilds start one at a time, and only while no project job,
   finalize or depot step is running. A rebuild queued by a build file starts
   only if the listing still names that file and the file is still gone or
   different, so files a job replaced or retired queue nothing. A source edit
   replaces a queued build-file report of the same asset. The queue clears
   when the open scene changes, and each start checks again that the asset
   belongs to the project or the open scene.
9. **Following other revisions.** Any listing read publishes every listed
   asset. When a mesh or texture path changed, the open scene moves to it in
   the same way, so finalize, depot syncs and imports keep instances on the
   inventory's revision. Activating a scene that has the same identity refreshes
   the listing, and so does a finished scene finalize.

## Consequences

- References survive rebuilds and syncs. A scene names revisions only in its
  lowered runtime file and `asset_refs`.
- Editing a source, or deleting or editing a build file, updates the open
  scene in place, unsaved edits included. Its cost is the cook, not a scene
  reload.
- Material identity remains the file stem, so a rebuilt `.mt` replaces the
  live material of the same stem, as finalize already did.
- Details shows a stored reference as `asset:<scope>/<id>` rather than the
  asset's name.
- A user edit to a mesh source during a job counts only if its report arrives
  before the next listing read in a later second, because mesh records keep no
  source hash.
- A texture material whose first texture load failed stays white until its
  reference moves to another revision.
- Skinned instances need a reload to show a rebuilt mesh. A kept lightmap slot
  shows the old bake on new geometry until the next bake. The first frame
  after a swap can show one frame of reprojection error.
- Opening a scene whose build file was deleted still fails. The automatic
  rebuild needs the scene open.

## Alternatives considered

- **Absolute paths in scene values.** Rejected: they break on rebuild and on
  another machine.
- **Copying dropped materials into the workspace overlay.** Rejected: texture
  references inside the copy still name revisions.
- **Writing a `.mt` file for every dropped texture.** Rejected: it needs an
  import job per drop, and the file would name a texture revision.
- **Reloading only the changed asset's entities.** Rejected: node structure
  can change, and entity identities and edits would not survive.
- **Ignoring build-file reports while busy.** Replaced: a short depot step
  dropped real deletions. Reports now queue and are checked when the job ends.

## Revisit when

- Details should show asset names for references.
- Mesh records keep a source hash.
- A rebuilt mesh should keep skinned instances or rebake lightmaps.

## Evidence

On 2026-10-12, an M1 Pro ran a headless Release editor (Metal) on an APFS clone
of the Testbed workspace, Bistro scene, driven through the agent channel:

- **Runtime file:** the lowered scene held 777 `asset_refs`.
- **Assign op:** `material.assign` with `asset:scene/<material>` and
  `asset:scene/<texture>` drew the material and the texture. The Model view
  showed the brick texture on an awning.
- **Material drop:** `ui.drag` from Content onto an awning, in a project with
  the World loaded, stored `asset:scene/c01037fc-…` and drew it.
- **Texture drops:** a scene texture and a project texture stored their
  references. A newly imported PNG drew at once from its preview `.vkt`.
- **Texture edit:** editing a project texture's image logged "rebuilt; 1 asset
  updated in place" with no second scene activation.
- **Mesh edits:** colour, then vertex scales ×1.6 and ×0.625 on two rebuilds in
  a row, then a finalize. Each logged "2 instances now draw" the new revision,
  the panel bounds changed live, and the scene activated once.
- **Deleted build file:** removing the current `mesh.vkb` rebuilt it from source
  and moved the instances, before and after finalize.
- **Checks:** `check_editor_project_jobs.py`, `check_editor_texture_tiers.py`,
  `check_path_contract.py` and `check_bakery_recook.py` passed with the Release
  `vkr_bakery`. The Debug tester suites `mesh`, `scene`, `editor` and
  `material` passed.

On 2026-10-18, a Windows 10 host (Ryzen 5 2600, RX 6700 XT) ran a headless
Release editor (Vulkan), driven through `vkr_mcp`. It opened a copy of the
Testbed workspace's Bistro scene, with JSON documents copied and build files
hard-linked:

- **Setup:** `content.import` of a 64×64 red PNG made a project texture, and
  its finalize moved it to the final tier. A `brush.box` beside a Bistro
  doorway then took `material.assign` with `asset:project/<id>` and drew red.
- **Texture edits:** rewriting the record's image green, then blue, rebuilt
  it 2.8 s and 3.0 s after each write. Each logged "swatch rebuilt; 1 asset
  updated in place", and the brush drew the new colour. The scene activated
  once, and no mesh moved.
- **Baseline:** the same run with the previous `editor_content.c` logged no
  change and left the record's revision in place for 180 s after each write.
- **Not run:** Vulkan mesh and material swaps, a deleted build file on
  Windows, and an edit of a scene-scope asset on Windows. Bistro's own
  textures have no source; its one mesh record rebuilds the whole import.
