---
status: partial
updated: 2026-10-15
authority: adr
---

# ADR-120: Mover lighting origin

## Status

Accepted (partial). The draws of the brushes and blockout shapes a mover
moves take baked diffuse light and their reflection probe at one point, the
mover's lighting origin, on both pipelines. Metal runs it; the Vulkan side
compiles but has not run natively (see Evidence). Mesh renderer instances
under a mover still take baked light per pixel.

## Context

On the Level Design Test, the covered tram car `bm/tram` is a looping mover
whose child brushes `bm/tram/car/*` move with it. A brush a mover moves is
out of every bake and casts as a moving object
([ADR-088](088-baked-lightmap-sets.md),
[ADR-108](108-baked-lamp-direct-pages.md)). Without a lightmap, its
draws took diffuse light from the baked diffuse volume
([ADR-054](054-baked-diffuse-volumes.md)) at each pixel's world position, and
a reflection probe chosen for the draw or the pixel. The volume holds the
static world's light where the static world is, so as the car ran through
the tunnel the lamps' hot spots and baked shadows swept through its closed
interior.

The owner chose on 2026-10-15 to light each mover's load from one point:
the volume, with its lamp direct bands, sky share and, on the desktop
pipeline, the dynamic layer ([ADR-119](119-dynamic-diffuse-volume-layer.md)),
and the reflection probe are looked up at the lighting origin, while each
pixel keeps its own normal, so the SH response still follows the surface.

## Decision

1. **Origin.** The mover component's `lighting_offset` (metres, in its own
   space, default zero) is added to the centre of the world box of the
   meshes it moves. With no mesh the centre is the mover's origin. The
   offset is a plain vector property, so scene files, the inspector, the
   agent channel's `values` and IO property writes carry it with the other
   mover fields. An entity reference was not added: the offset covers the
   case at no lifetime cost.
2. **CPU publication**
   ([vkr_scene_brush.c](../../runtime/src/renderer/systems/vkr_scene_brush.c)).
   `brush_light_mover` finds the origin and stores it in each brush's own
   space, in the brush record and in the mesh
   (`vkr_mesh_manager_set_lighting_origin`, `VkrMesh::lighting_origin`, w
   one). It runs at the end of a brush update for the movers whose brushes
   were rebuilt, cleared or attached, or whose component changed, so a
   moving mover costs nothing per frame. Since the mesh carries the origin
   in its own space, the frame build
   ([vkr_scene_frame.c](../../runtime/src/renderer/systems/vkr_scene_frame.c))
   moves it with the model into `VkrInstanceDataGPU::lighting_origin` (world,
   w one). Other draws keep zero.
3. **Instance ABI.** The source instance grows from 96 to 112 bytes and the
   prepared instance from 144 to 160 (`lighting_origin` at byte 144 of
   `VkrPreparedInstanceGPU`, `VkrMetalPacketInstance` and
   `VkrVulkanInstance`). No smaller layout serves: the prepared row has no
   spare lane (each normal column's w is taken), so any origin, even half
   precision in the source's reserved lanes, grows it to the next 16 bytes.
   Half precision would also lose a brush whose pivot lies far from its
   geometry. Shaders that do not read the field fetch the same bytes as
   before; the growth costs upload bandwidth and memory, 16 bytes a row.
4. **Tiled pipeline.** `vkr_metal_tiled_lighting_position` selects the
   origin or the pixel position after the direct light, so the light loops
   keep only the instance index live. The volume lookup and the probe's box
   projection use it; `vkr_metal_packet_draw_probe` tests the origin as a
   point, so a mover's draw never chooses its probe per pixel, and the blend
   pass picks its pixel probe at it. Lightmapped draws are unchanged.
5. **Desktop pipeline.** `vk_diffuse_volume_lookup` reads the pixel's
   visible row to find its instance (the sample root adds `visible_rows`;
   the three `DiffuseVolume` passes read `gpu_visible_draws` at binding 13).
   The deferred lighting kernel, the forward blend path and the
   transmission shade pass the origin to the probe lookups, box projection
   included. The dynamic layer is read in the same lookup, unchanged.

## Consequences

- A mover's load is lit evenly and does not take the light of the places it
  passes through at each point of its body; inside lighting no longer
  varies across a large load, so a long train lights its far end as its
  centre does.
- A mover's surfaces pick one reflection probe, and box projection uses the
  origin, so reflections are those of one point inside the load.
- Brushes under a mover that turns keep the origin found at rest in their
  own space; it turns with them.
- Mesh renderer instances under a mover keep per-pixel lookups.

## Evidence

Metal, Release, M1-class Mac, 2026-10-15, local and non-authoritative: the
Release build compiles both shader libraries (`vkr_bakery shaders --backend
all`), the CPU suite (`vulkan_renderer_tester`) passes, and a headless editor
rode the Level Design Test tram (`bm/tram`) from its station into the tunnel
with lit and lighting-only captures every 2.2 s. Before the change the
lighting-only interior showed tunnel lamp tints and dark blotches crossing
the walls as the car moved; after it each interior surface is lit evenly by
its normal and only the overall tint changes along the route. The first run
after the instance growth failed every frame in the Metal frame upload until
the render graphs' instance buffers (`gpu_draw_instances`,
`transmission_gpu_draw_instances`) took the 160-byte stride.

Pending: a native Vulkan run of the tram (`spirv-val` and the instance and
sample-root reflection on Windows). No timing claim is made; the tiled change
adds one instance load per pixel after the direct light.

## Alternatives considered

- **Interior lighting flag.** A per-mover or per-material flag that drops
  the volume for the global environment or a constant inside a load. It is
  simpler but loses the light the load takes from where it is, so a car in
  a lit station and one in a dark tunnel would look alike.
- **Car-attached baked volume.** A small diffuse volume baked in the load's
  own space and moved with it. It gives the interior its own bounce and
  shadows but needs a second volume binding, a bake per mover and a blend
  with the world's light at the openings; one point lit by the world's
  volume answers the owner's complaint at much less cost.

## Revisit when

A load needs light that varies inside it (a long train in and out of a
tunnel), or mesh renderer instances under movers show the same sweep.
