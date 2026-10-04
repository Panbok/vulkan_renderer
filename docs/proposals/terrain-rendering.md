---
status: proposed
updated: 2026-10-04
authority: proposal
---

# Terrain rendering

Scene terrains exist ([ADR-084](../adr/084-agent-channel-and-level-design-toolkit.md#terrain)):
heightfields with four layer weights per sample, editing, tile meshes, the
four-layer terrain material and height field collision. Every tile renders at
full resolution. This proposal keeps tile LOD.

## Current implementation baseline

[vkr_scene_terrain.c](../../runtime/src/renderer/systems/vkr_scene_terrain.c)
builds one generated mesh per terrain with a submesh per 64-cell tile. Each
tile has its own geometry: a 65 by 65 vertex grid of `VkrVertex3d` and a
skirt two spacings deep. The GPU-driven path classifies one candidate per
tile and culls tiles by their bounds like any other submesh. Tiles join opaque
visibility and directional shadows through the existing material path. Each
vertex color carries its sample's layer weights, which the terrain material
blends in the G-buffer resolve.

## Settled decisions

The owner settled these on 2026-10-04.

| Decision | Choice | Rejected alternatives |
|---|---|---|
| Source | A heightfield built into tile meshes in the scene at load and after edits | Imported and cooked mesh tiles |
| Residency | The scene owns its terrains; all samples of a terrain stay resident, up to 1 km at 1 m spacing (1,025 samples a side) | Tile streaming now; it moves to a future World Partition proposal that streams terrain and scene content by cells over the tile-chunked file |
| Material | A terrain shader that blends four layer materials by the sample weights (implemented; weights ride in vertex colors) | A material row per layer combination |
| LOD | A shared GPU LOD table: classification picks a level per candidate and encoding writes that level's index range. Terrain tiles geomorph between levels, applied identically in raster, visibility resolve, transmission, picking and motion vectors | CPU selection before candidate publication; LOD without geomorphing |
| Seams | Skirts hide cracks between tiles at different levels | Stitching index buffers per neighbour combination |

## Remaining work

Add a LOD table that geometry rows reference, shared with automatic mesh LOD.
Classification chooses a level from the projected error and encoding writes
the level's index range. A terrain tile's levels decimate its grid by powers
of two and store the geomorph target of each vertex. Every pass that rebuilds
or rasterizes a tile applies the same morph factor, so depth, shading, picking
and motion vectors agree. Coarser levels thin the layer weights with their
vertices.

## Acceptance evidence

- Matched Release Bistro captures on Metal and Vulkan show identical layer
  blends at a painted terrain (the blend itself is implemented; native Vulkan
  is unverified).
- A 1 km terrain at 1 m spacing in view on an M1 stays within the frame
  budget in a matched Release report, with tiles beyond the near range at
  coarser levels and no visible popping or cracks in a camera sweep.
- Picking and motion vectors match the morphed surface during the sweep.
