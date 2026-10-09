---
status: partial
updated: 2026-10-09
authority: adr
---

# ADR-094: Surface themes and the art pass

## Status

Accepted (partial). A surface theme binds surface tags to materials for a
container's brush faces. A material's `world_size` sets how large its
texture lies on those faces. `art.lint` reports what the art pass still
lacks. Layered Standard materials followed in
[ADR-095](095-layered-standard-materials.md); project tags and per-region
themes in world-partition cells are not implemented
([the artist toolkit proposal](../proposals/artist-toolkit.md), Part 6 and
Phase 3).

## Context

Brush faces carry a surface tag and show its fixed greybox look until the
art pass gives them a material
([ADR-084](084-agent-channel-and-level-design-toolkit.md#surface-tags-and-greybox-looks)).
Before this decision the art pass could only set a material on each face
(`face.set_material`). A level of thousands of faces then needed thousands
of assignments, and a face added after the art pass showed greybox until
someone assigned it. The proposal binds materials by tag, so new faces take
the bound material at once. It also sizes textures in meters from the
material. The owner settled the theme scope on 2026-10-09: the scene and the
container; world-partition cells may add regions later.

## Decision

1. **Theme documents.** A `.surfaces` file is JSON:
   `{"version": 1, "materials": {"<tag>": "<.mt path>", ...}}`, with paths
   content-root relative.
   [vkr_surface.h](../../runtime/src/level/vkr_surface.h) owns reading,
   writing and resolution (`VkrSurfaceTheme`). A key that names no tag fails
   the read, so a typo does not unbind a tag silently. The runtime, Bakery's
   brush proxies and the lightmap baker share that code.
2. **Selection.** A `surface_theme` component (`theme`, a path) is a scene
   singleton. A container's own instance wins; without one, the World's
   applies (`VkrSceneWorldState.surface_theme`).
3. **Look order.** A face shows the first that applies:
   1. the greybox view, which shows every face's greybox look;
   2. the face's own `material`;
   3. the material the container's theme binds to its tag;
   4. the World theme's binding for the tag;
   5. the tag's greybox look.

   Marks and the clip and trigger roles keep their fixed looks.
   `vkr_surface_face_material` encodes the order.
4. **World size.** `.mt` files take `world_size=` (one or two sizes, 0.01 to
   1000 m) and `surface=` (a tag). Graph settings carry both, and an
   instance's own lines replace its graph's. A face that shows a material
   projects its UVs in repeats of the material's world size: the face's
   `uv_scale` multiplies it. Its default of 1 therefore shows the material
   at its real size. Greybox looks keep their 4 m grid. Without
   `world_size`, a repeat covers 1 m, as before.
5. **Rebuilds.** The brush system reads the theme files when a container's
   or the World's theme path changes, or when the editor reports changed
   material or theme files. In those cases every brush of the container
   rebuilds. A changed `world_size` rebuilds the brushes once the material's
   live replacement publishes. The material system's replacement copies
   `world_size` and `surface` with the other fields.
6. **Bakes.** The lightmap baker and Bakery's brush proxies read the
   scene's own theme and the materials' world sizes, as the runtime does.
   The World's theme reaches neither, as the World's sun and sky do not.
7. **Editor.** The Material panel shows a theme as a table with a material
   path per tag. **Use in scene** and **Use for World** select it. Graph
   settings and instances gain an Art pass section with World size and
   Surface. The Art palette's THEME rows are:
   - **New theme**, which makes `assets/surfaces/theme_<n>.surfaces` and
     selects it for the scene;
   - **Bind tag**, which binds the selected face's tag to the open
     material;
   - **Open theme**.

   Theme writes go through the document journal
   ([ADR-093](093-material-graphs-and-art-workbench.md)), so they undo in
   order with scene edits and agents' writes are reviewed.
8. **Operations.**
   - `surface.list`: the tags with their face counts and bindings, and the
     marks.
   - `surface.theme.create`.
   - `surface.theme.bind`: `tag` and `material`, or `materials`; the
     default theme is the container's own.
   - `surface.theme.select`: a scene edit.
   - `art.lint` reports:
     - unbound tags;
     - materials that do not open or parse;
     - missing or uncooked textures;
     - texel density outside a range given in px/m (default 128 to 2048);
     - an untextured non-metal base colour outside sRGB 30 to 240 in its
       brightest channel;
     - an untextured metallic factor between 0.1 and 0.9.

   `material.patch` settings and `material.set_param` take `world_size` and
   `surface`. `material.list` lists themes. `material.open` opens them. The
   [vkr-art skill](../../.codex/skills/vkr-art/SKILL.md) teaches the order
   of work.

## Consequences

- Faces added after the art pass show their tag's bound material without
  another assignment, and the greybox view still shows layout.
- A theme or `world_size` edit rebuilds every brush of the affected
  containers once. It is an authoring action; games do not edit themes.
- Texel density in `art.lint` reads the width from a texture's cooked `.vkt`
  header, else from a PNG's header; other source formats skip the check.
- The project's density range is an argument, not a project setting, until
  project settings hold art rules.

## Evidence

- `./build_release/tests/vulkan_renderer_tester --suite run_brush_tests`
  (2026-10-09) covers theme resolution:
  - the container's binding wins over the World's;
  - a face's own material wins over both;
  - a tag with a mark still takes its binding;
  - the greybox view shows greybox looks over everything;
  - untagged faces bind nothing;
  - a theme writes and reads back the same, and an unknown tag fails with
    its name.
- `--suite run_material_graph_tests` covers the art keys:
  - `world_size` and `surface` parse, in one- and two-number forms;
  - bad values reject the material;
  - a graph raised from a definition keeps both through its document and
    lowering;
  - an instance reads its own lines.
- On a headless Bistro editor (Release, Metal), the agent channel ran these
  steps on brushes above the Bistro streets:
  - `surface.theme.create` and `surface.theme.select` bound brick to a red
    instance, and the brick wall showed it;
  - `surface.theme.bind` of concrete changed the platform, and the agent's
    `undo` unbound it;
  - `art.lint` reported the unbound concrete, wood and tile tags and a
    binding to a missing material;
  - the greybox view showed greybox looks over the bindings;
  - `material.open` showed the theme's table in the Art workbench.
- In a second run, a plaster wall bound to a tinted greybox-grid material
  showed 4 m repeats at `world_size=4`. After `material.set_param` with
  `world_size` 1, it showed 1 m repeats, with no reload.
- The World theme's fallback was not exercised natively: the headless Bistro
  run loads no World container. The CPU test covers its resolution.
- Not run: a lightmap bake or a Bakery cell proxy of a scene with a theme.
  Their theme and `world_size` reading builds, and the lightmap suites
  pass without a theme.

## Alternatives considered

- **Bindings per face.** Rejected: faces added after the art pass would
  stay greybox, and every layout edit would need a new assignment.
- **A theme reference on each brush.** Rejected: districts take themes by
  container (owner, 2026-10-09). A brush that needs a unique material sets
  its faces' own material.
- **`world_size` replacing the face's UV scale.** Rejected: a face's scale
  would stop meaning anything once a material has a size. As a multiplier
  it keeps its meaning: repeats of the material.

## Revisit when

- World-partition cells need their own themes (per-region themes).
- Project settings gain art rules (tags, density range).
