---
status: implemented
updated: 2026-10-09
authority: adr
---

# ADR-103: Reflection probe and diffuse volume authoring

## Status

Implemented. In a project scene the editor creates reflection probes. It
shapes them and other lights and volumes with outline handles, and saves
probes into the scene document, where Bake lighting captures them. The
diffuse volume's bake box is set in Bake settings and dragged in the
viewport. The Scene toolbar scrubs the time of day.

## Context

ADR-100 deferred these items from [the artist toolkit
proposal](../proposals/artist-toolkit.md) (Part 7):
- creating and shaping reflection probes and diffuse volumes;
- drag handles on the outlines;
- a Scene toolbar time control.

Probes are load-baked. A scene document lists them in a top-level
`reflection_probes` array. The loader turns each entry into a "Reflection
Probe N" entity with a `reflection_probe` component, and Bakery's
`bake_scene` captures the document's entries. Before this decision, editing
a probe went wrong in three ways:
- **Legacy scene.** An edit read back from the overlay reset the
  component's runtime `slot`. The probe went dark after a reload.
- **Project scene.** A touched probe entity has no document id. The overlay
  record that named it failed to bind, and that rejected the whole sidecar.
  The bake then failed: "Authored override target is unavailable for
  baking".
- **Bake.** A new probe had no capture destination, and lowering refused an
  enabled probe without one.

Probe capture also failed for any scene with saved edits:
- The effective bake scene was named `scene.json`, and the resource system
  loads only `*.scene.json`.
- Harness children did not know the built-in FPS module's components, which
  every starter scene carries.

## Decision

1. **Runtime state survives edits.** The edit layer's commit keeps the
   existing component's TRANSIENT properties (`vkr_type_keep_transient`), as
   the type contract says: runtime state is never edited.
   - A probe's `slot` and its new transient `source` survive overlay,
     Details and handle edits. `source` is the entry of the document's
     `reflection_probes` it stands for.
   - `vkr_scene_set_typed` still stores what it is given, so a script still
     sets its own transient fields.
   - A duplicate resets its copy's transient properties to their defaults,
     so it owns none of the original's slot or entry.
2. **Authored enabled.** The loader keeps a probe's authored `enabled` in
   its component. The world update turns the runtime probe on only when it
   has a source cubemap. Saving therefore no longer writes "disabled" for a
   probe that is waiting for its first bake. Such a probe lowers marked
   `bake_pending`, so it stays dark instead of reflecting the scene's
   environment, as an unbaked legacy probe without a cubemap does.
3. **The document holds probes.** On save of a project scene, the editor
   rebuilds the document's `reflection_probes` from the probe entities.
   - The scene records which entries its probes stand for
     (`reflection_probe_entries`).
   - In document order, an entry a probe stands for takes the probe's
     center, extents, blend distance, intensities and enabled state. It
     keeps its other members, such as its baked `asset`, or drops out when
     the probe was deleted.
   - An entry no probe stands for, such as one past the runtime's 16 or one
     the loader skipped, stays as it is.
   - Created probes follow in entity order.
   - After the document is published, each probe's `source` and the entry
     record follow the new array, so later saves in the same session match
     entries correctly.
4. **The overlay writes only what it can read back.**
   - In a project scene it skips touched and deleted records without a
     document id, such as loader-made probe entities. When it skips another
     load-baked object, such as the diffuse volume, its status names how
     many keep their document values.
   - It skips created entities that carry a load-baked component, and world
     partition cells leave them out too.
   - A legacy scene keeps writing touched probes, which now reload with
     their slot.
5. **Creation.** The scene edit layer accepts a `reflection_probe` on a
   created entity.
   - `probe.create` takes a world `center`, half `extents` (default
     [4, 2.5, 4]) and `values`, in the open project scene only, up to the
     runtime's 16 probes.
   - The Lighting palette's Probe button creates one where the Scene's
     center meets a surface, at 2.5 m.
   - A created probe draws its box and handles at once. It reflects after
     the scene is saved and Bake lighting captures it.
6. **Bakery.**
   - `bake_scene` gives an enabled probe without an `asset` a scene-owned
     `probe-cube` record before it captures it. This is the record that new
     projects already got.
   - Lowering emits an enabled probe without an asset without a cubemap,
     and the runtime keeps it off.
   - The effective bake scene is `bake.scene.json`.
7. **Harness children** register the built-in FPS module's component types
   before any scene, as the editor and the player do. They run no scripts.
8. **Diffuse volume box.**
   - Bake settings' diffuse group adds `bounds_min` and `bounds_max`. Both
     zero cover the whole scene, and validation requires max above min
     otherwise.
   - A set box goes into the request as `bounds` (min, then max), which
     Bakery passes as `--bounds`.
   - While Bake settings is open, the Scene draws the box in orange with a
     handle on each face.
   - The window adds **Box around selection** (the selection's world box,
     1 m wider on every side) and **Whole scene**.
   - Like all bake settings, the box lasts for the session.
9. **Outline handles.** With the Select tool, the selected light or volume
   shows a cross on each handle. A drag edits live as one undo step (one
   gesture), and Escape restores the start; Escape before any movement
   leaves no undo step. The handles are:
   - point light range, along +X;
   - spot range, along its axis, and spot outer cone, from the rim, clamped
     above the inner cone and below 90°;
   - rectangle width and height, symmetric about the center;
   - the six faces of a look volume's or decal's box, through the entity's
     scale and position, with the opposite face fixed;
   - the six faces of a reflection probe's world box;
   - the six faces of the diffuse box, editing the session setting.
10. **Toolbar time.** With a World time of day, the Scene toolbar ends with
    a clock and the hour the scene shows. The chip is highlighted while the
    hour differs from the World's start, and the wheel steps a quarter hour.
    Its popup has:
    - an hour slider (whole minutes);
    - **Keep HH:MM as the World's starting hour** (undoable);
    - **Back to the starting hour**.

    The palette and the chip read the clock from the rendered scene's
    resolved World state.

## Consequences

- A probe goes from creation to capture in one session, and its later edits
  survive save, reload and rebake with its cubemap kept.
- The editor writes `reflection_probes` in project scene documents. A
  deleted probe's asset record stays until project cleanup.
- Probe creation stays in project scenes. A loose scene file has no bake
  outputs.
- Starter-scene projects can bake probes, and harness captures of project
  scenes with FPS components load.
- Probes still cap at 16 in the runtime, below Bakery's 64.

## Evidence

All runs used headless Release editors on Metal, on 2026-10-09. The scripts
are in `.scratch/artist/` in the working tree.
- **Handles on Bistro** (`phase7_handles.py`). Each drag went through
  `ui.drag` from the handle's projected pixel:
  - A point light's range went from 3 to 8.10 m, and a Cmd `undo` returned
    it to 3 in one step.
  - A 2 m rectangle light became 5.84 m wide, centered.
  - A look volume's +X face moved its center from -8 to -6.57 and its scale
    from 4 to 6.85, so the -X face stayed at -10.
  - Bistro's indoor probe's +X face moved its center from -7.5 to -3.29 and
    its half extent from 5.5 to 9.71, so its -X face stayed at -13.
- **Probe lifecycle** in the FPS Arena starter project (`phase7_probe.py`).
  A project World is needed, and Bistro opens without one.
  - `probe.create` made a probe with its box and handles.
  - `scene.save` wrote it into the document's `reflection_probes`.
  - Bake lighting gave it a `probe-cube` asset and a 3 MB `probe.vkt`, and
    the reload loaded its cubemap.
  - Moving it to x = 1 and saving updated the entry and kept its asset.
  - The next launch loaded the saved overlay without a rejection.
- **Repeated saves** (`phase7_probe_saves.py`). In the same scene, after
  the baked probe had loaded:
  - Probes A and B were created and saved, which added two entries without
    assets.
  - Deleting A and saving left the baked entry and B.
  - Moving B to x = 5 and saving changed B's entry.
  - Duplicating B and saving added a third entry without an asset.

  The baked entry kept its `asset` throughout.
- **Diffuse box.** The same bake sent `bounds` [-6, 0, -6, 6, 4, 6]. The
  diffuse job fitted 147 cells to that box and skipped the open arena as
  having no closed room, as ADR-054 expects. The capture shows the orange
  box with its handles.
- **Time chip** (`phase7_time.py`). With a World time of day at 9:
  - the chip read 09:00;
  - `lighting.time 18.25` made it 18:15 and highlighted it;
  - the popup's Keep set the World's hour to 18.25.
- **Probe capture of a starter scene.** Before the harness change, a
  snapshot of FPS Arena's runtime scene failed on `fps_player`; after it,
  the snapshot passed. A copy named `scene.json` failed to enqueue, which
  the rename fixes.
- **Review.** An independent code review found these defects in the first
  version, and each is fixed above:
  - entry matching by load-time index went stale after a save;
  - unbaked probes reflected the environment;
  - transient keeping in `vkr_scene_set_typed` overrode script fields;
  - duplicates copied the original's slot and entry;
  - created probes went into partition cells;
  - entries the loader skipped were dropped.
- **CPU suites.** The new `test_scene_loader_reflection_probe_edit_keeps_runtime`
  checks two things:
  - an edit through the edit layer, from type defaults, keeps the probe's
    slot and source;
  - the world update leaves a probe without a cubemap off while its
    component stays enabled.

  `vulkan_renderer_tester` passed its 98 other suites. The local-socket
  suite was left out, as it fails on this host with and without the
  sandbox. `check_path_contract.py` and `check_bakery_bundle.py` passed.
- **Painted scatter** (ADR-102). On a Bistro copy, 6 areas and 27 copies
  saved and reloaded unchanged.
- **Not run:** Vulkan, and a probe capture of Bistro, which is not a
  project scene. Timing was not measured: handles and the chip run only
  while their object is selected or the chip is shown.

## Alternatives considered

- **Probes as entity components in documents.** Rejected: every reader,
  Bakery's bake and lowering included, takes the top-level array.
  Rewriting that array on save keeps one store.
- **Document-only probe edits through the journal.** Rejected: a new probe
  would show nothing until a reload, and moves would not preview.
- **Diffuse box stored in the scene.** Deferred with the other bake
  settings, which last for the session (ADR-100).

## Revisit when

- Probes need more than 16 at runtime, or per-probe capture resolution in
  the editor.
- Projects store bake settings.
- Harness children need script modules beyond the built-in FPS module.
