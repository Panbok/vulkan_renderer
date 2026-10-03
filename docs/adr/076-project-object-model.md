---
status: implemented
updated: 2026-10-03
authority: adr
---
# ADR-076: Project object model: descriptors, containers, entities and components

## Status

Accepted. The editor and runtime implement typed descriptors,
entities as an ID plus components, the root World beside a primary scene and
additive scenes with Set primary, one shared physics world, World-only
physics and animation settings, generated Details for every component
including shape, text, animation and a read-only mesh view, structural undo,
presets, the Content folder browser, the Outliner above Details, a
registration boundary for module-owned component types, document-stable
entity IDs, version 5 scene documents and prefab instances.

## Context

The editor exposed three unrelated ways to reach data. Scene entities appeared
in Hierarchy and edited through Inspector. Scene-level rendering state such as
the environment, atmosphere, clouds and fog lived inside `VkrScene` and was
authored through scene JSON and bake tools. Everything else was a Settings
window whose graphics preferences mixed machine quality gates with display
grading. Each owner hand-wrote its Inspector layout, JSON reader and writer,
undo payload and [Cmd evaluator](../../editor/src/editor_cmd_eval.c) member
table.

The runtime held exactly one active scene, and that scene was both the entity
container and the owner of world-level singletons, so an empty project had no
world at all. Entity IDs already reserved a 16-bit world field
([`VkrEntityId`](../../runtime/src/core/vkr_entity.h)) that no second
container used.

## Decision

### Everything the editor shows is an object with a type descriptor

An **object** is anything the editor can select, inspect and search. Component
types carry a static **type descriptor**
([`vkr_type_desc.h`](../../runtime/src/core/vkr_type_desc.h)) with a property
table. The table is the single source for the Details rows
([`editor_details.c`](../../editor/src/editor_details.c)), JSON reading and
writing, validation, defaults, edit-journal payloads and Cmd evaluator paths.
The scene descriptors live in
[`vkr_scene_types.c`](../../runtime/src/renderer/systems/vkr_scene_types.c):
transform, visibility, the three light types and the world component types
(environment, atmosphere, clouds, analytic and froxel fog, fog density box,
post process, reflection probe, diffuse volume, subsurface, physics settings,
animation settings), shape, text and animation.
[`vkr_scene_physics.c`](../../runtime/src/renderer/systems/vkr_scene_physics.c)
describes the physics body settings and one collider; Details generates their
rows, the snapshot validation applies their rules, and Cmd paths and presets
reach the body through the edit values. Machine graphics preferences use the
same descriptor path in the Preferences window.

Shape, text and animation components hold authored values, so Details, Cmd
and undo edit them like any component. Setting a shape rebuilds its generated
mesh, setting text replaces its text slot, and removing either releases them.
Text holds its content, font, size as the em height in meters, color,
per-line alignment, line spacing and letter spacing in ems; a scene's
`text3d` block lowers to it with size `font_size / texture_width` and left
alignment, so existing text renders unchanged. The font names an engine
configuration `assets/fonts/<font>.fontcfg` (Ubuntu Mono, the default; Ubuntu
Mono Bold, Inter and Inter SemiBold); the scene loads another on first use
and holds it until shutdown, and a package carries it only when text uses it
([ADR-078](078-project-build-and-packaging.md)). Details edits the content in
a multi-line text area that applies each keystroke within one undo gesture,
picks an enum of more than three values from a dropdown, and the viewport
marks every text with an icon just above its glyphs, because empty or distant
text is otherwise hard to find.

Every color property shows a swatch that opens the color picker
([`editor_color.c`](../../editor/src/editor_color.c)): `COLOR` values (linear
RGB, possibly past 1) and `VEC4` values flagged `VKR_PROPERTY_FLAG_COLOR`
(linear RGBA, or sRGB RGB with linear alpha under `VKR_PROPERTY_FLAG_SRGB`,
as text color is). The picker edits the display color as hue, saturation,
value and alpha, with a hex field, a palette and the session's recent
colors; a linear color past 1 keeps its intensity. Each drag applies live as
one undo step, and Escape restores the color it opened with.
The animation component (clip, rate, loop, playing) belongs to an animated
mesh's binding: setting it reconciles the player, restarting only when the
clip or loop changes, and a clip outside the bound bank is rejected. It
cannot be added, removed or deleted with its entity, because undo cannot
restore the binding. Details also shows read-only Mesh rows (asset, first
material, submeshes, vertices, triangles, bounds, load state, shadow
mobility) generated from `vkr_scene_mesh_info_type`, which describes a view
of the mesh instance, not a component.

Modules outside the renderer, such as script modules
([ADR-079](079-c-script-modules.md)), register their own component descriptors
with `vkr_scene_register_world_type` before any scene initializes. A registered type joins the world types, so
documents, overlays, Details, Add component, presets and Cmd paths accept it
and scenes store it generically; its behavior belongs to its module.

| Category | Lives in | Identity | Edited through |
|---|---|---|---|
| Asset | Project, scene or editor inventory on disk | UUID or bundle name | Project jobs (ADR-069), Content folders and tags |
| Entity | A loaded container's ECS world | `VkrEntityId`; the world field names the container | Edit journal of its container |

### Entities are UE5-style actors: an ID plus components

An entity is an ID plus component instances in archetype chunks (ADR-010).
Components are plain data with bounded fields. A world component type may be
a **singleton**: at most one instance takes effect per frame. Details shows a
losing instance as inactive and still edits it.

Structural edits go through the journal
([`vkr_scene_edit.h`](../../runtime/src/renderer/systems/vkr_scene_edit.h)):
create, delete, reparent, add component and remove component. Undo and redo
recreate deleted entities and remap their IDs. The edit overlay (version 4)
records authoritative `components`, created entities, deleted records and
parents, and per-scene settings. New objects come from built-in **object
kinds** (empty, four light kinds, one per live world type and the Player Start),
grouped under headings in menus; Cmd `create` also accepts each registered
script component type. A new object lands by the Scene's Snapping settings
(below) at the drop or right-click point in the Scene, else at the viewport's
centre (Outliner and Content menus, Cmd `create`); the runtime places it in
front of the camera only without a valid view.

### Containers: the root World, a primary scene and additive scenes

**World-only types** (`VKR_TYPE_FLAG_WORLD_ONLY`), physics settings and
animation settings, exist only in the root World. Scene documents, overlays,
Add component and Cmd reject them in a scene, and creating one targets the
World. Every scene resolves the World's instance whether or not it inherits
the World. Physics settings set gravity; their Details section opens the
collision layers window, where the World's collision layers, matrix and
presets are edited. Animation settings scale the clock of every animation in
the active scene.

A **container** is one `VkrScene`: one ECS world, allocator, document, overlay
and journal. The **root World** uses world id `VKR_SCENE_WORLD_ROOT_ID` and
loads from the project's `world.scene.json` with `world.editor.json` as its
overlay. It stays loaded while the project is open. It loads asynchronously
through the resource system, like an added scene, so models in it stream
instead of blocking the frame; its picking range follows the additive ranges
(`VKR_SCENE_WORLD_RENDER_ID_BASE`). A reload after its document changed keeps
the added scenes and the view, and the Outliner shows the World loading until
it activates. A new project's World is a
blank level: directional light, sky atmosphere, volumetric clouds, height fog
and post process. Project creation optionally imports Blank, FPS Arena or RPG
Grounds as its first scene; No starter scene creates only the World.
[ADR-069](069-editor-projects-and-workspaces.md) owns these template imports.

The **primary scene** is world 0: the scene of the active viewport document.
`scene.add` loads up to `VKR_SCENE_ADDITIVE_MAX` additive scenes as worlds 1
to 6. Entity references never cross containers. Render ids are partitioned per
container (`world id × VKR_SCENE_RENDER_ID_RANGE`), so picking resolves a
container without a shared allocator.

Singleton resolution picks the primary scene's enabled, visible instance with
the lowest entity index. The World's instance is the fallback only while the
scene **inherits the World**. That per-scene setting defaults on, is undoable
and is saved in the overlay. Turned off, the scene uses only its own objects,
except World-only types.
Additive scenes' singletons have no effect. Hidden entities never render,
light or resolve. A scene with no sun light uses the World's directional light
on the same terms ([ADR-058](058-revision-baked-sky-atmosphere.md)).
Disabling, hiding or removing the resolved atmosphere turns the sky off.

All loaded containers share one Jolt world through
[`VkrScenePhysicsSet`](../../runtime/src/renderer/systems/vkr_scene_physics.h).
The primary scene drives the fixed-step clock, and every member steps with it.
The World's `physics_settings`, resolved by the driver, set the gravity of
every body and of the player character; without them, gravity is 9.81 m/s²
down.
The World owns the collision layers when present, otherwise the driver does.
Reset rebuilds every member.

**Set primary** makes an added project scene the primary scene: the editor
removes it, opens it as the primary and adds the previous primary back beside
it. The Outliner names added project scenes by their project name.

Saving writes every loaded container. Before a scene switch or close, the save
prompt names each container with unsaved edits, and Save or Discard resumes
the interrupted action: opening a scene, placing a mesh or closing to the
World. Unloading an additive scene clears its selection and journal.

### Settings split by ownership

- **Authored rendering intent** (exposure, white balance, grading, bloom, fog,
  environment, atmosphere) is a world component. A new project's World post
  process takes the grading values older editors kept in machine preferences.
- **Machine preferences** (vsync, HDR, frame limit, render scale, upscaling,
  shadow quality, feature gates) stay machine-local `VkrGraphicsSettings`,
  edited in the Preferences window through descriptors.
- **Editor UI state** (layout, panels, Content view and folder) is persisted
  project state, not an object.

### Presets, not per-type profile assets

Any live world component or light can be saved as a **preset**: a named,
typed value in the project's `presets.json` (version 1). The
[project store](../../editor/src/editor_project_store.h) reads and writes it
through the type's descriptor (a type flagged `VKR_TYPE_FLAG_TOLERANT`, as
script components are, also skips members it no longer has and keeps the
default for one whose kind changed) and skips records of unknown types or invalid
values. Details offers Save as preset and the presets of that type from each
component header; applying one is an ordinary undoable component edit.
Content lists presets in its Presets folder for rename and delete.

### Content is the World; System holds what the editor ships

[Content](../../editor/src/editor_content.c) is a virtual file system with two
roots:

- **Content is the World.** Its root lists the World's objects, a folder per
  project scene, the project's assets, its folders and Presets. A scene's
  folder lists that scene's objects and its own assets.
- **Assets group by type.** Project and scene assets list in type folders,
  Textures, Materials, Meshes, Fonts, Environments, Probes, Animations and
  Other, under their home: the Content root or their scene's folder. A type
  folder appears once it holds an asset and cannot be renamed, moved or
  deleted.
- **System** holds what the editor ships. Assets has the editor bundle's
  defaults, such as the default scene font. Objects has one item per
  creatable object kind. Editor has the rest of the editor bundle.

Objects list for loaded containers only: the authored entities of the World,
the primary scene and added scenes, not the nodes a model imports. Content
rebuilds them when a container's structure or names change.

The project's `content.labels.json` (version 2) stores the project folders
and each project asset's folder and tags. Files never move, because cooker and
import-closure rules own their layout. Project assets and project folders live
under the Content root; an asset without a folder label lists in its type
folder. A scene's assets stay in its folder, and System, Presets, scene and
type folders cannot be renamed, moved or deleted as folders.
Labels that name a folder of the earlier layout (Objects, Editor, Scene
assets) fall back to their item's default.

Content has a folder tree with tags, back, forward and up, a one-row toolbar
with the search and sort order, tile and list views, and a list header that sorts by name,
type, location or tags. Each tile or row is one hit target for select,
double-click, drag and right-click, and hovering shows a description. Search
and tag filters list matching items below the current folder (choosing the
chosen tag again clears its filter); a search from
Content leaves out System. Dragging a project asset or folder onto a folder
moves it. While an item is dragged, a translucent card with its icon and a
name chip follows the pointer, highlighted where a drop takes effect. The
card is a root overlay in the root's single cell.

A right-click on empty space opens the shown folder's menu: New folder, New
script, Import, a Create submenu of the object kinds grouped under headings,
and Refresh. An object created from Content, by that submenu, a drop or a
double-click, is then revealed: Content opens its folder and selects it. A
right-click on an item opens the item's menu; a loaded object's menu adds the
Outliner's Script and Add component submenus. A scene folder adds Load scene,
which loads the scene into the viewport, before the four common commands:

| Item | Open | Put into viewport | Rename | Delete |
|---|---|---|---|---|
| Scene folder | Show its folder (Open scene) | Add it beside the open scene | Rename the scene | Delete the scene after confirmation |
| Object | Select it | Frame it | Rename it (undoable) | Delete it (undoable) |
| Scene asset | Show details | Place a mesh at the viewport centre | Rename it | Delete it when nothing uses it |
| Object kind | — | Add it to the scene | — | — |
| Project folder | Open | — | Rename | Delete when empty |
| Content root | Open | Show the World in a document | — | — |

Rename edits the name in place: Enter or leaving the field commits and Escape
cancels. Double-click opens a folder or scene folder, selects an object and
opens its script when it has one, opens a Script asset's source, or adds an
object kind. A Script asset dragged onto an object in the viewport or the
Outliner attaches its script to it ([ADR-079](079-c-script-modules.md)). Dropping on the viewport loads a scene folder, adds an
object kind or places a built mesh under the pointer by the Scene's Snapping
settings: on the first collision surface (Surface, the default, falling back
to the ground plane), on the nearest grid crossing or cell center (Grid), or
where the pointer meets the ground plane (Free, else 8 m along its ray). An
offset moves the object along the snap normal, a 15-degree-step yaw turns it
about that normal, and surface snaps can turn its up axis to the surface
normal. A shape rests its base on the snap point; a placed mesh rests its
authored origin there, because its bounds are unknown until it loads. Surface
rays skip sensors. The settings persist with the project's editor settings.
**Snap selected** (End, the object's context menu, Cmd `snap`) rests an
existing object by the same settings
([`vkr_editor_viewport_snap`](../../editor/src/editor_viewport.c)): the box
around its loaded meshes and shapes
([`vkr_scene_entity_local_bounds`](../../runtime/src/renderer/systems/vkr_scene_system.c))
drops straight down onto the first collision surface that is not its own,
else the ground plane (Surface), onto the nearest grid point (Grid) or onto
the ground plane (Free), as one undoable transform edit. Aligning tilts the
object's up onto the surface normal and keeps its heading. The gizmo does not
snap while dragging. Loading
the scene that is already loading or open does nothing, from Content, the
Scenes view or `scene.open`.
The placed entity references the existing asset by scope and ID; nothing is
copied into the scene. Files dropped from the OS (macOS drag destination,
Windows `WM_DROPFILES`), and models chosen with Import, open the import step
of the Create or import window. Its assets are filed in the project folder
under the pointer. When every file is a model, the step places it too:

- **New scene** runs `create_scene` with the models and opens the scene; its
  assets belong to that scene.
- **World** imports the models into the project, then the
  [project store](../../editor/src/editor_project_store.c) appends one root
  entity per mesh to `world.scene.json`, naming the project asset
  (`mesh.asset`) and the artifact path it resolves to (`mesh.path`, from the
  document's directory), and the World reloads. A finalize that rebuilds the
  asset under a new revision, and each project open, rewrite that path.
- **Scene** runs `add_entities` with the models on a chosen project scene
  and opens it; a scene loaded beside the open one is refused.
- **Content only** imports without placing.

Edits the placement reloads ask to be saved or discarded first. A failed
placement reopens the scene that was open.

While a background finalize encodes a model's final textures (ADR-077), a
viewport chip names the model and the materials already applied. Its Content
items (the mesh and the materials sharing its build revision) show a lock and
"Cooking" and refuse rename, delete, move, rebuild and reimport. Outliner
rows whose mesh loaded from that revision, and their ancestors, show a
pulsing spinner and a lock and refuse delete and detach.

While the World or a scene streams in after an import, a project open or a
World reload, the chip names the loading model first ("Loading"), and the
[project store](../../editor/src/editor_project_store.c) lists the World's
mesh asset IDs so Content can lock those mesh cards with "Loading". A folder
card containing a locked item, and a World object card whose mesh loaded from
a cooking revision, carry the same lock and status. Details shows mesh
information for World and added-scene objects through the entity's own
container.

The Outliner lists the World's entities with each loaded scene nested below
it, as a tree table under a column header: toggle columns for Scene
visibility (undoable), viewport icons and pinning, then Item Label and, when
the panel is wide enough, Type. Turning an object's viewport icons off hides
its own and its descendants' icons; pinned objects and their expanded subtrees
stay in a block above the tree. Pins and hidden icons are editor state saved
with the primary scene's expansion. Each scene row has an inherit toggle, and
double-clicking a row frames its object. Details below it is generated from descriptors. The viewport panel
carries document tabs, each showing the World or one project scene. Only the
active tab renders, and switching tabs loads its scene.

While a project is open, its dialogs (the unsaved-edits prompt, rename,
delete, the scene list and the create forms) are compact floating windows over
the live editor. They take the pointer only where they are and the keyboard
until a click lands outside, and their title bar drags them. Only the launcher,
before a project opens, fills the window.

### Documents and compatibility

Managed scenes are version 5 documents, lowered by the
[project jobs](../../tools/bakery/project/vkr_project_lower.c) to the runtime scene
format:

- Every entity has a document-stable UUID `id`, unique within its document.
  `parent` names the parent's id.
- An entity's `components` map holds its transform, mesh, shape and text
  blocks beside its descriptor components.
- Jobs read version 3 and 4 documents and write version 5 at the next
  publication. Internally they edit blocks and parent indices; the
  conversion happens only at the read and write boundary.

The runtime format keeps blocks and parent indices, and carries each
entity's id. A document gives every entity an id or none, and the loader
rejects a partial or duplicated set. The World document uses the runtime
format directly; a new World gives its entities ids.

An entity's runtime `components` map carries any live world type, registered
type, light type or visibility by type name, read through the descriptors. A
light may not appear both as a block and as a component. Loading lowers shape
and text blocks into their typed components.

World components and authored entities persist through the edit overlay.
Overlay version 5 lists the document's ids in order, so loading maps each
saved entity index to the index of the entity with that id. A document can
be reordered without detaching its edits, and an edit whose entity left the
document fails the load. Older overlays and documents without ids bind by
index. Saving authored edits accepts version 3 to 5 manifests with a scene id
and replaces only their `edit_overlay` member
([`vkr_editor_project_save_scene_overlay`](../../editor/src/editor_project_store.c)).

A created entity's physics body is saved with its record. Loading builds the
body after every created entity exists and is parented, outside the load's
all-or-nothing preparation: a body that cannot be built leaves its entity
loaded without it and logs a warning
([`edit_structure_load_commit`](../../runtime/src/renderer/systems/vkr_scene_edit.c)).

A **prefab instance** copies another scene of the project into the open
scene under one new root entity (`scene.instantiate`, jobs `prefabs`).
Copies get new ids, and their parents point inside the copy. Scene-scoped
assets they use are copied into the scene; project and editor assets stay
shared. An instance keeps no link to its source, and placing one reloads the
scene, like placing a mesh. Linked prefabs with overrides and lifecycle
belong to the [behavior proposal](../proposals/entity-behavior-system.md).

The legacy `--scene` entry and every harness case load one scene with no
World, so Bistro cases and baselines are unchanged.

## Consequences

- A new component type needs one descriptor to get Details rows, JSON, undo,
  presets and Cmd paths; hand-written panels for migrated types are gone.
- The frame gathers the World, the primary scene and additive scenes; a
  single-scene harness run has no World and keeps its previous work.
- Save, dirty state and journals are per container. ADR-069's single active
  world scene is superseded by the World plus primary scene.
- A reload of the World drops it until its replacement resolves: the
  resource cache holds one load per document path, so the old World cannot
  stay visible while the new one streams (about 3 s for bistro-lights).
- On 2026-09-29 the Release editor (Windows 10, RX 6700 XT, Vulkan) placed
  `assets/models/bistro-lights.gltf` in the World, a new scene and an
  existing scene (the second add used the `clearcoat_furnace` fixture)
  through `content.import <path> world|new|scene` in a windowed run on a
  scratch copy of a project. Screenshots showed each model in the viewport,
  Outliner and Content, the cooking chip, locks and spinners during
  finalize, and their removal afterwards; a reopened project loaded the
  World and its model asynchronously. Windows now accepts
  `WM_*BUTTONDBLCLK` as a press, so double clicks open Content folders; a
  synthetic double click on the Materials folder card opened it.
- A later run of the same build showed the "Loading" chip and locked mesh,
  folder and World object cards while the World streamed, and their removal
  once it resolved. Toggling HDR and FSR in Preferences no longer freezes the
  window: the "Restart required" notice had filled the section's one-cell
  grid, so the auto-placed scroll area failed placement and every later frame
  failed to prepare; the scroll area now claims the cell explicitly. The run
  logged no frame preparation failure.
- On 2026-10-03 a headless macOS Release editor (Apple M1 Pro, Metal) on
  Bistro rested a cube on a second cube given a static physics body
  (Surface: y 2.0 on a top at 1.5) and on the grid (Grid: y 0.5). In a
  managed project, a created cube saved by `scene.save` to the scene's
  version 5 `edits/` document under `created`, and a new session restored
  it at its saved position.

## Alternatives considered

| Alternative | Why not |
|---|---|
| World Settings as a struct on the World, components only for placed things | A second parameter mechanism and a second Details path. |
| Components attached to a container directly | Every consumer would need two lookups; a World entity costs one ID. |
| Last-loaded singleton wins | Order-dependent and silent. The primary scene plus an explicit inherit toggle is deterministic and visible. |
| Per-type profile assets | Multiplies asset kinds; one preset mechanism covers every component. |
| Tags or filters instead of folders | The user rejected filtered views; UE-style folders are the structure and tags are metadata. |
| Moving physical files into folders | Breaks cooker and closure layouts; virtual folders keep files in place. |
| Physics world per container | Bodies in different scenes could not collide. |
| Collision matrix as a descriptor component | Its layer names and 16-bit matrix need array and 16-bit property kinds. It belongs to the World's physics settings, whose Details open its journaled window. |
| A separate container set type | The runtime already owns the World, primary and additive slots with serialized loads and unloads; a wrapper would add a forwarding layer without new behavior. |
| Copying a dropped mesh into the scene | Duplicates project assets; a scoped reference keeps one owner. |
| New 32-bit document entity IDs | Managed documents already gave every entity a UUID. A second identity would need a counter per document and a migration; the runtime stores 16 bytes per document entity. |
| Linked prefab instances now | Overrides, propagation on save and lifecycle need the behavior proposal's semantics; copying gives instancing without a stale link. |

## Revisit when

A shipped runtime needs streamed loading beyond the editor's loaded set, the
additive cap or 16-bit world field constrains a project, dynamic-length
component fields become necessary, or prefab lifecycle requires references
across containers.
