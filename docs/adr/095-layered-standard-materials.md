---
status: implemented
updated: 2026-10-09
authority: adr
---

# ADR-095: Layered Standard materials

## Status

Accepted. Any opaque Standard material can blend up to three layer
materials over its own surface. The layers are weighed by:
- the vertex colour;
- a mask texture;
- the slope of the world normal;
- the world height.

This is the terrain blend of
[ADR-084](084-agent-channel-and-level-design-toolkit.md#terrain), generalized
behind the same flag branch, so it adds no shader variant and no pipeline.

## Context

The artist toolkit's Standard tier includes layering: mossy brick, painted
metal with wear and tiled floors with grime
([the artist toolkit proposal](../proposals/artist-toolkit.md), Part 3). The
terrain kernel already blended four texture sets by vertex colour on both
pipelines, behind material flag 2048. The owner settled the shader shape on
2026-10-09: a flag branch first, and a variant axis only if Phase 3 measured
a cost on draws without layers.

## Decision

1. **Mask sources.** `VkrMaterialLayerMask` gives four sources:
   - `vertex_color`, the terrain's source;
   - `texture`, a mask texture's RGBA (slot `VKR_TEXTURE_SLOT_LAYER_MASK`);
   - `slope`, the world normal's Y;
   - `height`, the world position's Y.

   The vertex colour and the mask texture weigh layers 0 to 3, normalized.
   Slope and height blend layer 1 over layer 0 by `smoothstep` across
   `layer_mask_range` (x below y).
   `vkr_layer_mask_weights` in
   [terrain_kernel.slangh](../../renderer/src/shaders/shared/terrain_kernel.slangh)
   gives the weights on both pipelines. The Metal tiled shader reads its
   interpolated world normal and position. The Vulkan G-buffer resolve
   computes them from the triangle's vertices only for the slope and height
   sources.
2. **Rows.** The layer segment of each material row gains the mask texture,
   its sampler, the source and the range:
   - Metal: 240 bytes (`VkrMetalTerrainMaterialGpuRow`);
   - Vulkan: 176 bytes (`VkrVulkanTerrainMaterialGpuRow`), checked by SPIR-V
     reflection.

   The common rows do not change, and Metal tracks 22 texture references a
   material.
3. **Files.** A `.mt` names its layers with `layer1=` to `layer3=`, as `.mt`
   paths resolved against the file. It names the mask with `layer_mask=`,
   `layer_mask_range=` and `layer_mask_texture=`.
   `vkr_material_loader_compose_layers` then parses each layer and copies in
   its factors and its base colour, normal and ORM maps.
   - A layered material is opaque PBR, without transmission or subsurface.
   - A layer cannot be layered itself.
   - A mask without layers fails the file.
   - Layer slots without maps sample the white and flat defaults.

   `vkr_material_loader_replace_terrain` composes terrain layers through the
   same function.
4. **Graphs.** A `layer` node names a layer `.mt` and can be exposed as a
   parameter. A `layer_blend` node takes up to three layers, a `mask` and a
   range or mask texture. It feeds the surface output's `layers` input.
   Lowering writes the keys above and counts `layers` and their samples. A
   layered definition becomes the same graph (`vkr_material_graph_from_definition`).
   The canvas, the inspector and `material.patch` edit both nodes.
5. **Bakes.** The lightmap baker accepts the layer keys and bakes a layered
   material as its layer 0, because it does not evaluate masks. It also
   accepts `surface=` (ADR-094), which does not change light.

## Consequences

- Draws without layers run the same code as before; the branch costs nothing
  measurable on Bistro (Evidence).
- Every material row's layer segment grows by 48 bytes on Metal and 32 on
  Vulkan, whether or not the material is layered.
- Layers share layer 0's UVs. Per-layer tiling, height blending by layer
  heights and detail normals remain proposal work.
- A layered material's lightmap shows layer 0's albedo.

## Evidence

- `./build_release/tests/vulkan_renderer_tester --suite
  run_material_pbr_tests` (2026-10-09):
  - `test_material_layered_keys`: a layered `.mt` parses its layers'
    factors, maps resolved against each layer file, mask source, range and
    mask texture. Five invalid files fail, each for its own reason.
  - `test_material_terrain_composes_layers` passes unchanged on the shared
    composition.
- `--suite run_material_graph_tests`: `test_graph_layers` raises a layered
  definition to a graph that lowers back to the same material. A falling
  range and a blend without a layer each name the `layers` node.
- On a headless Bistro editor (Release, Metal tiled pipeline), a layered
  graph with a moss layer was bound by theme to a platform and a wall above
  the streets:
  - **slope** (0.6 to 0.8) put moss on the platform top and grid on the
    walls;
  - **height** (30.5 to 32.5 m) faded the wall to moss upward;
  - **texture**, with the greybox grid as mask, mixed the layers by its RGBA.
- The same run under Metal API validation (`MTL_DEBUG_LAYER=1`, one process)
  reported no validation error and shut down cleanly.
- Timing, `vkr_harness profile --case
  tools/cases/performance/bistro_material_orbit_1440.case.json --profile
  tools/profiles/performance-windowed-gpu-submission.json`:
  - Configuration: Release, AppleClang 21, Apple M1 Pro, Metal 4,
    2560×1440 pixels, windowed hidden, immediate present.
  - Each side: five children of 120 warmup and 300 measured frames.
  - Before: commit `eed0670d`, report
    `sha256:eca7986f5defd02faf5f2489dbb078a8b8535e05dd02526be8cb8a90a699e66e`.
  - After: commit `0173c2f9`, report
    `sha256:680f205b7ce61e72de6702dedfc9457d38b43895ccc14f3392687ad65aff8725`.
  - Both passed and are authoritative, with equal environment, workload and
    policy fingerprints.
  - `gpu.submission` mean: 19.608 ms before (per child 19.56 to 19.64 ms),
    19.567 ms after (per child 19.50 to 19.61 ms), −0.2%, within the spread.
  - World draw calls, commands and objects tested are equal.
  - The ktx submodule carries the two patches the build applies
    (`cmake/vkr_asset_targets.cmake`). For both runs it was ignored as dirty
    in the local git configuration, so `git status` judged only tracked
    sources.
- Unavailable:
  - Vulkan native execution and `spirv-val` of the changed resolve modules,
    which wait for a Windows host. The macOS build compiles the Slang to
    SPIR-V.
  - Register counts of the shading variants: no offline Metal compiler
    report was taken.

## Alternatives considered

- **A layered shader variant.** Rejected for now (owner, 2026-10-09): the
  flag branch measured no cost on draws without layers.
- **Layer masks from separate scalar nodes in the graph.** Rejected: the
  Standard tier lowers to row data, and a mask source with a range is what
  the fixed shaders read.

## Revisit when

- A scene with many layered draws measures a cost in the branch.
- Layers need their own UV transforms or height blending.
- The baker learns to evaluate masks.
