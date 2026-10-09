---
status: partial
updated: 2026-10-09
authority: adr
---

# ADR-093: Material graphs and the Art workbench

## Status

Accepted (partial). Material graphs (`.mtg`) and graph instances (`.mt`
with `graph=`) lower to the `.mt` definition the material loader already
reads, in the Standard tier only: a Standard graph costs no shader and no
pipeline. The editor edits graphs on a node canvas in the Material panel of
the Art workbench, keeps material files in a document journal that undoes
in order with scene edits, and agents drive every step through `material.*`
operations. Surface themes ([ADR-094](094-surface-themes-and-art-pass.md))
and layered Standard graphs
([ADR-095](095-layered-standard-materials.md)) followed, and Custom graphs
with generated shaders on the Metal tiled pipeline
([ADR-096](096-custom-material-graphs.md)). Look volumes and the other later
parts of [the artist toolkit proposal](../proposals/artist-toolkit.md) are
not implemented.

## Context

Before this decision a material was one `.mt` file of factors, texture
paths and state, read by
[material_loader.c](../../runtime/src/renderer/resources/loaders/material_loader.c).
No material had its own shader: features are flag bits of a material row
that fixed shaders branch on, and every pipeline is created at startup
([ADR-087](087-gpu-class-graphics-pipelines.md)). The owner asked for a node
graph editor in a separate workbench that agents can drive, without the
shader permutations and compile stutter of engines whose material graphs
each compile to a shader (owner decisions of 2026-10-08 and 2026-10-09 in
the proposal). The `shader=` and `pipeline=` keys of `.mt` files had no
consumer that chose a shader or pipeline.

## Decision

1. **Graph documents.** A `.mtg` file is JSON: `version` 1, `settings`
   (alpha mode, alpha cutoff, double sided, subsurface profile, temporal
   reactivity, largest roughness) and `nodes`, each with an `id`
   (`[a-z0-9_]`, 1 to 31 bytes), a `type`, optional `parameter`, `value`,
   `path`, `color_space`, `position` and `inputs` that name `node.port`
   outputs. [vkr_material_graph.h](../../runtime/src/assets/vkr_material_graph.h)
   owns the node registry, reading, writing (one node a line), validation
   (ids, links, port types, cycles, one surface output), the edit
   primitives and lowering. It lives in `vkr_asset_formats`, so the runtime,
   the editor, Bakery and the lightmap baker share it.
2. **Node set.** Scalar, Colour and Texture inputs (any of them exposed as a
   named parameter), Multiply, Normal map and the Surface output, whose 23
   inputs are the material model's: base colour, opacity, metallic,
   roughness, occlusion, normal, emissive, dielectric specular, the
   clearcoat, sheen, anisotropy, transmission, thickness and attenuation
   inputs, IOR, subsurface and diffuse transmission. Layer and layer blend
   nodes (ADR-095) feed its `layers` input. Nodes outside the Standard tier
   belong to the Custom tier (ADR-096).
3. **Standard lowering.** `vkr_material_graph_lower` writes `.mt` lines: a
   surface input takes a constant, a texture output or a multiply of one
   constant and one texture output (two constants fold); a normal input
   takes a normal map, whose strength is the scale. Scalars read fixed
   channels (metallic b, roughness g, occlusion r, clearcoat r, clearcoat
   roughness g, sheen roughness a, anisotropy b, transmission r, thickness
   g), and inputs that share a texture slot share its file. Anything else
   fails with a reason and the node at fault, which the editor marks. A
   graph's texture paths resolve against the graph with `./` or `../`, else
   against the content root, and lower to content-root relative paths.
4. **Instances.** A `.mt` with `graph=` holds only `name`, `graph` and
   `param.<name>=` lines. `vkr_material_loader_parse_text` expands it to its
   graph's lowered definition with the overrides before it parses, so every
   load, batch and live replacement reads instances; an instance's own
   texture path resolves against the instance. The lightmap baker's parser
   (`vkr_bake_material.cpp`) expands instances the same way.
   `vkr_material_loader_parse_definition` parses a definition without
   loading textures.
5. **From a definition.** `vkr_material_graph_from_definition` builds the
   Standard graph of a PBR `.mt`, leaving out factors equal to the loader's
   defaults, so the graph lowers back to the same material. Legacy Phong
   keys have no graph form.
6. **Dead keys removed.** `VkrMaterial.shader_name`, `VkrMaterial.pipeline_id`
   and the parsing of `shader=` and `pipeline=` are gone; the importers no
   longer write them, and older files keep loading because unknown keys are
   ignored.
7. **Document journal.** The editor writes material documents at once,
   through a temporary file and a rename, and updates the live materials
   that read them: `vkr_material_loader_replace_live` for a `.mt`, and for a
   `.mtg` every instance an index of `assets/` names
   ([editor_material.h](../../editor/src/editor_material.h)). Each write is
   a step with the text before and after, numbered from the scene journals'
   counters (`vkr_scene_edit_take_sequence`, `vkr_scene_edit_take_group`):
   Undo and Redo take the newest step across scene and document journals,
   and an agent's `undo` takes only its own steps. A reviewed agent write is
   a pending change with a `document`; Reject reverts its group when no
   later step changed the same file.
8. **Editor.** The Art workbench (tab id `art`) puts the Tools palette left
   of the Scene, the Material panel, Content and Console under it, and the
   Outliner and Details on the right. The Material panel shows the open
   document's tier and cost, the graph on a node canvas
   ([editor_graph.h](../../editor/src/editor_graph.h)), which builds over the
   dock at the root because it places itself from the window origin, and an
   inspector for the selected node, the graph settings or an instance's
   parameters. A plain definition shows read-only with Convert to graph,
   which writes `<name>.mtg` and rewrites the definition as its instance in
   one step. Content opens materials in the panel. The palette edits the
   selected face's material, assigns the open material to the selected face
   or brush, resets faces to greybox and toggles the greybox view.
9. **Mesh material overrides.** A `material_override` component
   (`material_0` to `material_7`, content-root `.mt` paths) gives one placed
   mesh entity its own materials for its first eight submeshes; the shared
   mesh asset keeps its own. `VkrMeshInstance` holds the overrides in a
   bounded table with one material reference each
   (`vkr_mesh_manager_instance_set_materials`), and the frame's classify
   and emit passes and the mesh facts read them through
   `vkr_mesh_instance_submesh_material`, one compare per draw.
   [vkr_scene_material_override.c](../../runtime/src/renderer/systems/vkr_scene_material_override.c)
   applies the component once the instance has loaded and its materials
   have published, at most 32 entities an update, and releases the
   references when the component, the entity or the scene goes. A change
   records a static content change bounded by the instance for a static
   caster, so retained static draws repack; a dynamic caster's moves only
   the dynamic generation.
10. **Operations.** `material.list`, `material.describe` (nodes as text,
   parameters, lowering, instances), `material.create`, `material.patch`
   (`add`, `remove`, `connect`, `disconnect`, `set`, `settings`, all or
   nothing), `material.set_param`, `material.compile`, `material.open` and
   `material.assign` (a face, a brush's faces or a mesh's submeshes; an
   empty material clears). A material dropped from Content on the Scene is
   assigned to the brush or mesh under it, and the Art palette's Assign
   does the same for the selection. `face.set_material` assigns materials
   to brush faces
   ([ADR-084](084-agent-channel-and-level-design-toolkit.md)). The
   [vkr-art skill](../../.codex/skills/vkr-art/SKILL.md) teaches agents the
   order of work.

## Consequences

- Standard graphs and instances add no shader and no pipeline: lowering ends
  in the same material rows a hand-written `.mt` gives.
- A graph that neither tier lowers loads as no material (the loader reports
  the reason); a graph the Standard tier refuses lowers to the Custom tier
  (ADR-096).
- Graph edits reach every loaded instance by file index; materials outside
  `assets/` of the content root are not indexed.
- Material documents have no unsaved state: every committed edit is on disk
  and undoes through the journal for the editor session.

## Evidence

- `./build_release/tests/vulkan_renderer_tester --suite
  run_material_graph_tests` (2026-10-09): validation, documents, lowering
  against hand-written definitions, tiers, and a round trip of every `.mt`
  under `assets/materials`: 980 files, 670 raised and lowered to an equal
  parse, among them all 254 Bistro materials
  (`assets/materials/bistro-lights`); the 310 others are legacy Phong
  materials with no graph form.
- A headless Bistro editor (Release, Metal), driven through the agent
  channel with the operations above, raised a Bistro material
  to a graph, tinted it through `material.patch`, made an instance, assigned
  it to a brush wall and changed its parameter: the wall turned from red to
  blue without a reload. A failing edit changed nothing; `undo` by the agent
  restored the instance; a reviewed `material.set_param` listed with its
  `document` and `changes.reject` reverted it. Captures showed the Art
  workbench with the graph and an instance's parameters.

- `./build_release/tests/vulkan_renderer_tester --suite
  run_mesh_material_override_tests`: an override changes only its instance
  and submesh, a second instance of the asset keeps the asset's material,
  reference counts return after replacing, clearing and destroying, and a
  static caster's change advances the static generation inside its bounds
  only.
- In the same headless Bistro run, `material.assign` of a green instance to
  the ceiling fan node `subset_0` coloured only the fan's blades, and
  clearing it restored them.

## Alternatives considered

- **A shader per graph.** Rejected for the Standard tier: instances and most
  environment materials need no code, and per-graph shaders are the
  permutation growth the owner rejected. Custom graphs compile to
  shaders under a budget (ADR-096).
- **Interpreting graphs at runtime.** Rejected: lowering to the existing
  definition reuses one parser, one material row and every published
  contract.
- **Snapshot undo per document.** Rejected (owner, 2026-10-09): agents would
  lose review and rejection of their material edits.

## Revisit when

- Materials live outside the content root's `assets/` and need indexing.
