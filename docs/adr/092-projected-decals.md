---
status: partial
updated: 2026-10-07
authority: adr
---

# ADR-092: Projected decals

## Status

Accepted (partial). The tiled pipeline draws projected decals: a `decal`
component's box lays its material's base colour over the opaque surfaces
inside it before lighting. The desktop pipeline ignores decal components
until it applies them in its G-buffer resolve (owner decision, 2026-10-07).
Decals change base colour only; normal, roughness, metalness, occlusion and
emissive decals are future work. The editor shows a decal's icon but no box
outline.

## Context

Bistro's grime, cracks and stains are mesh decals: imported geometry that the
importer pushes off its surface along the normal
(`vkr_decal_normal_offset_meters`, [ADR-030](030-offline-mesh-optimization-and-cooking.md)).
On the tiled pipeline they draw as blended surfaces in `Tiled.Blend`, lit
with their own material over the resolved image, so a decal's pixels pay
for lighting twice and the decal cannot change the material of the surface
under it.

Deferred renderers write decals into the G-buffer, either through separate
decal buffers composed into it or, as Fox Engine does, directly into its
channels while keeping the surface's shading model, so the decal inherits
the surface's lighting response. The tiled pipeline has no G-buffer: it
shades forward in one multisampled render pass
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 5), and a G-buffer
kept in tile memory measured 1.7 to 3.4 ms slower with four samples. The
forward equivalent is a clustered decal: the forward shader finds the decals
over a pixel in a world-space lookup and changes the surface's inputs before
it lights them, so the surface's own material model shades the result.

## Decision

1. **Authoring.** A decal is the world component type `decal`
   (`vkr_scene_decal_type`, `SceneDecal` in
   [`vkr_scene_system.h`](../../runtime/src/renderer/systems/vkr_scene_system.h)).
   Its entity's world transform places a box whose inside is [-0.5, 0.5] on
   each axis; scale sizes the box. The decal projects along the box's -Y
   onto the surfaces facing its +Y, as the editor places objects with +Y on
   the surface under the cursor. Seen from its +Y with its -Z up, its
   texture reads upright and unmirrored: u runs along +X and v, whose zero
   is an image's bottom row in VKR, along -Z. The component names a `.mt`
   material file (empty uses `assets/materials/dev/dev_decal.mt`), an
   opacity, the angles between a surface's normal and the box's +Y at which
   the decal starts to fade and is gone (60 and 80 degrees by default), a
   depth fade toward the box's near and far faces, a sort order and an
   enabled flag. Scene files, undo, Details, the agent channel's component
   operations and Cmd handle it as they handle other world types; the
   Create menu lists Decal under Basic.
2. **Materials.** A scene keeps one material reference per decal component
   (`vkr_scene_decal.c`). The reference loads on the first frame after the
   component appears or changes and is released when the component leaves,
   its entity is destroyed or the scene shuts down. A material file that
   does not load leaves the decal without a material, so it draws nothing
   rather than the default surface material over its box.
3. **Frame selection.** Each frame the runtime collects the enabled, visible
   decals of the rendered and additive scenes with an invertible box, keeps
   the 64 nearest the camera by the distance to their bounds
   (`VKR_MAX_FRAME_DECALS`), and orders them for compositing by ascending
   sort order, then the order their components were added in. It keeps the
   kept decals' textures resident and builds a camera-independent world
   grid of at most 256 cubic cells at least 2 m wide over their bounds,
   with a 64-bit decal mask per cell (`VkrDecalGrid`,
   [`vkr_decal.h`](../../renderer/src/vkr_decal.h)). The point-light grid and
   the decal grid size their cells through the same `vkr_world_grid_fit`.
   The world payload carries the table and grid (`VkrWorldPassPayload`);
   frame validation rejects a non-finite or singular box, unordered fades
   and a grid that cannot hold the table.
4. **Tiled shading.** The Metal frame root names a 48-byte
   `VkrMetalPacketDecals` record at byte 568, the root's former tail
   padding, with 80-byte rows and the grid's masks. After the surface's
   base colour, including the terrain layer blend, `vkr_metal_tiled_decals`
   in [`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal)
   looks up the pixel's cell, iterates the union of its SIMD group's masks
   so each decal row loads once for the group, and lays each decal whose box
   holds the pixel over the base colour: the material's base colour texture
   times its tint, weighted by the texture's alpha, the opacity and both
   fades. The angle fade uses the interpolated vertex normal facing the
   viewer. The decal's texture coordinates take explicit gradients from the
   world position's screen derivatives, so their samples stay defined in
   the lanes' divergent branches. Opaque, alpha-tested and terrain surfaces
   take decals; glass, blended surfaces and world text do not.
5. **Variants.** Decal shading would cost the opaque pass registers on every
   pixel, as probe and light code does (ADR-087, decision 11), so the opaque
   and alpha-tested pipelines gain a decal variant axis
   (`VKR_METAL_TILED_DECAL_VARIANT_COUNT`). A frame takes the variants with
   decals only when the sphere around some decal's bounds meets the view
   frustum (`vkr_metal_packet_tiled_decal_variant`); other frames run the
   same shader code as before decals. The editor's inspection variants
   always shade decals. The tiled pipeline creates 50 shading pipeline
   states instead of 30.
6. **Desktop.** Vulkan does not apply decals yet. The desktop G-buffer
   resolve would apply them between `vkr_vk_resolve_surface` and
   `vkr_vk_finish_surface_normal` in
   [`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang),
   with the same box, fade and compositing rules.

### Bistro evidence

Release, M1 Pro, 2026-10-07, on the lightmap-baked Bistro street view
(`tiled_bistro_decals_capture`, `local-offscreen`, 1280×720): Bistro's
stain-atlas, crack and rotated dirt decal materials project onto the
cobblestones from 1 m tall boxes, and an orientation decal
(`decal_orientation.mt`, `test512.png`) shows its label in its +X, +Z
corner, upright from above (report
`sha256:a8ff5a3c0e12f7fde6f6e5bc6c6e0761d88c5ea18d120c2b87b21f086ee67545`).
The same case passes with Metal API validation enabled and reports no
message (`local-metal-offscreen-validation-serial`,
`sha256:304cbd3d106e37f83bc89664b66f34d09f40e2b8a77af4f5f767a6e138886e9c`).
Without decals, `tiled_bistro_baked_capture` matches the build before decals
within run variation: 4 of 921,600 final-colour pixels differ by more than 2
of 255 from the earlier build, 3 between two runs of the same build, and
depth is identical
(`sha256:452dd6c21824aad8c6384e599a86e7141e3556a8c693cf9d009195d02ce1026b`,
`sha256:4ad06ae73a5f86ef00a56504a4a3142f74cb27ff533661f468d674ec187f89ef`,
`sha256:fd603755302b6d1071b6d53470f0ba068a6427c891b9812a43fa5f337e4c3e02`).
Bistro's own decal textures are faint grime, so the cracks and dirt read as
subtle marks at 9 to 14 m.

Cost, local and non-authoritative (uncommitted tree, one process per run,
300 measured frames, alternating order), at native 2560×1440 on the static
street view with four decals in view (`tiled_bistro_decals_static_native`)
and without them (`tiled_bistro_street_static_native`):

| Measurement | Without decals | With decals |
|---|---|---|
| `Tiled.Opaque` median / p95, `local-windowed-gpu-single` | 8.25 / 8.34, 8.23 / 8.32 ms | 8.77 / 8.85, 8.75 / 8.82 ms |
| `gpu.submission` median / p95, `local-windowed-gpu-submission-single`, 10 m boxes | 11.61 / 11.74, 11.56 / 11.71 ms | 12.15 / 12.22, 12.15 / 12.25 ms |

(`sha256:63ec11919b4a2a134b503098181152b2d186a6c6097b4b01fee20eb06bc0e95e`,
`sha256:03c826c48d2f89ab71419464d2bbe7e0d983c9353ce8f9d4bb77d35aca8827c6`,
`sha256:c125917c13fa07418552e9f2941c221963d1b7c1a7201a4f494104a5a55a6d36`,
`sha256:3e25288e5b51fe37623a09618d0a18e7b311cc0c0375e62b2fdbb6eff44dea78`;
`sha256:f5d30888411f90b0fbc1c51d9d91bed4f533715c7bb793a5082b1a41d1196f3f`,
`sha256:3507a942f8962f256af6527025433a7c1b46162c4551afe63bd4929c70698db2`,
`sha256:ed20eb65db64f74743a5226abef33750e292c96ea18b6c3bff85a2bccb5ba4f8`,
`sha256:ff3307dd864df337e9db1178da83359ddf7fc146d541405e22ad2e580b343435`.)

The decal variant costs `Tiled.Opaque` about 0.5 ms however little of the
image the decals cover. Diagnostic builds kept the cost at 0.55 to 0.7 ms
when the loop never ran past the grid lookup, without the texture sample,
without the SIMD-group mask union and with UV derivatives taken in the loop,
so the decal code's presence in the forward shader costs it, as unused light
code does (ADR-087, decision 11). Frames without a decal in view keep their
cost: the lightmap-baked orbit (`tiled_bistro_baked_native`,
`local-windowed-gpu-submission-single`) took 11.22 / 16.09 and
11.19 / 15.79 ms median / p95 before decals and 11.08 / 15.77 and
11.16 / 15.65 ms after
(`sha256:181d48fe96a439733c27c1b05611c8d1faa0353270a77066fc59b8959b8efbbc`,
`sha256:0c64d1c5b8c2de83c913c14bc1ae661322c3682ea5f2cccde306f9f44d6fba4f`,
`sha256:184fdd69c00e6473b71315629e672b57b1c549b835048e498457a3e12f5c5d8e`,
`sha256:9f9d6a00092ff1ef9f87cb0fb304b60a90e31b3e5ea4a89a3cd6c323f2294359`).

## Consequences

- A decal changes a surface's albedo before the lightmap, sun, local lights
  and environment light it, so baked and dynamic lighting both show it.
- A frame with any decal in view pays about 0.5 ms of `Tiled.Opaque` on
  Bistro at native resolution on the M1 Pro, about half the margin the
  baked orbit leaves under the 16.7 ms budget (ADR-087). Only draws that
  touch a decal would pay if the culling pass sorted them into buckets of
  their own; the draw buckets are a shared GPU ABI, so that is future work.
- A decal box lays its texture on every opaque surface inside it that faces
  its +Y within the fade angles, including moving and skinned meshes that
  pass through it.
- Beyond 64 decals the farthest drop, so a frame can lose a distant decal
  when many are near.
- A material's base colour sampler repeats, so a texture whose border is
  opaque bleeds its opposite edge into a decal's outermost texels.
- The tiled pipeline compiles 20 more shading pipelines at startup.

## Alternatives considered

- **G-buffer decals in tile memory.** Writing material inputs into an image
  block and lighting them in a tile pass would let decals change every
  surface channel, but it reverses ADR-087's forward decision, which a
  G-buffer measured against at four samples.
- **Mesh decals only.** They need authored geometry per surface and light
  twice; they remain supported for imported content.
- **Per-draw decal lists.** Assigning decals to draws in the culling pass
  would avoid the grid lookup, but a draw's bounds hold many decals in a
  street, and the visible draw row's spare bits hold one probe index, not a
  64-bit decal mask.
- **Branch on a decal count instead of a variant.** Unused decal code would
  still cost the opaque pass registers on frames without decals.

## Revisit when

- Decals need normal, roughness or emissive channels, an atlas rectangle, or
  a receive-decals flag on meshes.
- The desktop pipeline applies decals.
- A scene keeps more than 64 decals near the camera.
- A decal-heavy view misses the budget: then give decal-touching draws their
  own draw buckets so only their pixels take the decal variant.
