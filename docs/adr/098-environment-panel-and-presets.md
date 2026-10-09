---
status: implemented
updated: 2026-10-09
authority: adr
---

# ADR-098: The Environment panel and environment presets

## Status

Implemented. The Environment window shows a scene's environment in one
place: each part, where it comes from (the scene or the World) and its
values. An environment preset (`.environment`) captures every part.
Applying a preset copies its values into the scene, as component presets
do.

## Context

Every environment setting is a scene singleton, and a scene can inherit the
World's instance ([ADR-076](076-project-object-model.md)). Before this
decision an artist edited each singleton through its own entity in Details,
and could not see at once which values came from the World. Component
presets hold one component type each. [The artist toolkit
proposal](../proposals/artist-toolkit.md) (Part 7) asks for one panel and a
preset of the whole environment. The owner settled on 2026-10-09 that
presets apply by copy, and that World inheritance shares values across
levels.

## Decision

1. **Parts.** [editor_environment.c](../../editor/src/editor_environment.c)
   names nine parts, keyed as in documents and operations:
   - the components `environment` (the sky light), `atmosphere`,
     `clouds`, `fog` (height fog), `volumetric_fog`, `post_process` and
     `time_of_day`;
   - `sun` and `moon`: the first enabled directional light that drives the
     atmosphere's sun, or its moon.
2. **Resolution.** A component part takes the entity world resolution chose
   for the container, which may be the World's. The sun and moon come from
   the container's lights, else the World's. A part without one shows its
   defaults as unset.
3. **Documents.** A preset is a content-root document, by default under
   `assets/environments/`:
   `{"version": 1, "parts": {"<key>": {<values>}, ...}}`.
   - Values are written through each part's type descriptor, as scene
     documents write components.
   - Reading rejects an unknown part or an invalid value with its key.
   - Saving captures every part the container resolves, World values
     included.
   - Writes go through the document journal
     ([ADR-093](093-material-graphs-and-art-workbench.md)), so they undo in
     order with scene edits and agents' writes are reviewed.
4. **Applying.** Applying a preset is one batch of scene edits, one journal
   group:
   - Each part goes onto the container's own entity that holds it, or onto
     a new entity named for the part when only the World or nothing sets
     it.
   - The time of day applies to the World only.
   - A sky light, which loads with its scene, applies only where one
     exists.
   - A sun's or moon's preset holds its light's values, not its entity's
     rotation.
5. **Window.** The Lighting palette's Environment button and Cmd `window
   environment` open the window. It shows the primary scene's environment,
   else the World's. It has:
   - **Save preset**, which saves `assets/environments/<scene>.environment`;
   - the presets under `assets/environments/`, each with **Apply**;
   - a section per part, titled with its source (Scene, World or not set),
     whose rows edit the value on the entity that holds it.
     - A World part says that edits change every scene inheriting it, and
       offers **Override in scene**, which copies it into the scene.
     - An unset part offers **Add**.
6. **Operations.**
   - `env.describe`: per part, its key, label, source (`container`,
     `world` or `unset`), entity and values.
   - `env.preset.save`: `path`, `container` and `overwrite`.
   - `env.preset.apply`: `path` and `container`; a batch of edits that
     agents' review and `undo` cover.

## Consequences

- One action moves a level to a look, and one undo takes it back.
- A preset is a copy: editing it later does not change the scenes it was
  applied to. Shared values belong in the World.
- Presets are content documents. Agents read them, version control tracks
  them, and projects can share them.

## Evidence

On a headless Bistro editor (Release, Metal),
`.scratch/artist/phase5b_check.py` in the working tree ran these steps
through the agent channel (2026-10-09):
- `env.describe` reported the sky light, atmosphere, clouds and sun as the
  scene's own. It reported fog, volumetric fog, post process, time of day
  and moon as unset.
- `env.preset.save` wrote those four parts.
- `env.preset.apply` of a dusk preset changed three parts:
  - it created a Height fog entity with the preset's colour and density;
  - it created a Post process entity with saturation 0.35 and contrast 1.2;
  - it left the time of day alone in the primary scene.

  The capture turned orange and hazy.
- A missing preset and one with an unknown part (`weather`) failed with
  their reasons.
- The agent's `undo` returned every part to its previous state in one step.
- `window environment on` showed the window with the presets, their Apply
  buttons and the parts' sections.
- No CPU test was added. The document format goes through the type
  descriptors, which the scene document tests cover.

## Alternatives considered

- **Presets by reference.** Rejected (owner, 2026-10-09): a level would
  change when someone edits a shared preset; the World shares values on
  purpose instead.
- **Environment presets in `presets.json`.** Rejected: presets need several
  components at once. A content document also works without a project, and
  agents and version control read it.

## Revisit when

- Presets need to carry entity transforms, such as the sun's rotation.
- Several scenes are open and the window should choose the container.
