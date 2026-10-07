---
status: proposed
updated: 2026-10-07
authority: proposal
---

# Decal channels and receivers

[ADR-092](../adr/092-projected-decals.md) decals change base colour only, on
every opaque surface inside their box. This proposal covers the next
decal features. Each one changes the shared decal contract, so both
pipeline classes must implement it ([ADR-087](../adr/087-gpu-class-graphics-pipelines.md),
decision 3).

## Current baseline

- The `decal` component has one opacity. The tiled forward shader and the
  desktop G-buffer resolve lay the material's base colour times its tint,
  weighted by the texture's alpha, the opacity and the angle and depth fades.
- Every opaque, alpha-tested and terrain surface inside a box takes the
  decal, including moving and skinned meshes.
- Mesh decals follow their glTF alpha mode. Bistro's three `BLEND` decal
  materials (`LMBR_000005f_Decals`, `LMBR_0000060_Decal_Bottomdirt`,
  `LMBR_0000061_Decal_Crack`), 381 triangles on three nodes, draw forward in
  `World.Blend` after SSR, fog and transmission on the desktop pipeline.
- Project packaging does not lower or package a material path inside a
  component, for decals and brush faces alike.

## Proposed work

1. **Normal and surface channels.** Split the opacity into base colour,
   normal and surface (roughness and metallic) opacities. The surface
   channel blends roughness and metallic from the material factors and its
   ORM texture. The normal channel decodes the material's normal map and
   rotates it onto the surface's shading normal, with the decal's u and v
   axes projected into the surface plane as the tangent frame, so a flat
   decal normal leaves the surface's normal unchanged. The desktop resolve
   blends all three before the metallic fold and the octahedral normal
   encoding. A Windows prototype of this blend matched the G-buffer outside
   the decal footprints on Bistro; the tiled forward shader would apply the
   same blend before lighting.
2. **Receivers.** A `decal_receiver` component at or above a mesh renderer
   decides whether its surfaces take decals; the nearest one wins. Without
   one, static meshes take decals and skinned meshes do not. Generated
   shapes, brushes and terrain always take them. The render bridge pushes the
   setting into the mesh instance, and a candidate flag outside the GPU's
   four candidate bits carries it to the backends. Vulkan can mark the
   instance row with bit 31 of `temporal_flags`, above every surface token;
   the tiled pipeline needs its own per-draw test.
3. **Mesh decals in the G-buffer.** A decal visibility layer: a raster pass
   after `VBuffer.Opaque` writes the nearest decal fragment's id into an
   `R32G32_UINT` image, depth-tested against a read-only
   `opaque_vbuffer_depth`, and the resolve blends that triangle's material.
   One layer keeps only the nearest of overlapping mesh decals and costs 8
   bytes per pixel, 15.8 MiB at 1920x1080.
4. **Packaging.** Lower and package component material paths, for decals
   and brush faces together.

## Unsettled decisions

- Whether the tiled pipeline takes the normal and surface channels at its
  forward shader's register cost (ADR-092 measured about 0.5 ms for the
  base-colour variant on the M1 Pro), or a tiled quality tier omits them.
- Whether skinned meshes take decals by default.
- Whether mesh decals move into the G-buffer at all: Bistro has no recorded
  defect in its three blended decals.

## Evidence needed

- Bistro captures on both pipelines: G-buffer or final colour differences
  only inside the decal footprints, and none on a receiver that is off.
- Matched timing of `GBuffer.Resolve` on the Windows Vulkan host and of
  `Tiled.Opaque` on the M1 Pro, with and without the new channels.
