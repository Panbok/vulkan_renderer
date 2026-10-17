---
status: partial
updated: 2026-10-14
authority: adr
---

# ADR-119: Dynamic diffuse volume layer

## Status

Accepted (partial). On the desktop pipeline (Vulkan) with ray queries, a
dynamic layer of the sparse diffuse volume
([ADR-054](054-baked-diffuse-volumes.md)) traces the light the bake lacks
each frame: dynamic point, spot and rectangle lights, emissive movers, and
static lights changed since the bake. Where lamp lightmaps hold the static
lamps, a changed lamp takes its whole light group out of the bake, and the
layer lights the group instead. Baked reflection probes take the layer's light
at their capture origins. A scene whose volume is not baked yet gets a uniform
runtime grid whose layer traces all of its light. Two parts are not done:

- **Changed static lights** need the bake's light records
  (`baker_manifest.baked_lights` in the volume's `.bake.json`). The diffuse
  baker writes them; a volume baked before 2026-10-14 has none, and its
  static lights count as unchanged. Bistro volumes are local, untracked
  files, so each host rebakes its own.
- **The runtime grid** is opted into by the scene document; the editor and
  the project lowering do not emit `runtime_grid` for an unbaked volume yet.

The tiled pipeline stays baked ([ADR-087](087-gpu-class-graphics-pipelines.md)).

## Context

The owner chose DDGI-style volume updates on the desktop pipeline as its
dynamic diffuse light after screen-space GI was removed (2026-10-12,
[Short-history rendering](../proposals/short-history-rendering.md), section
6), with these decisions:

- the layer holds only light the bake lacks; the baked layers stay the
  authority;
- scenes without a bake get a uniform runtime grid with runtime relocation;
- 1.0 ms of GPU time at 2560 × 1440 on an RX 6700 XT, Bistro, for build,
  trace and update;
- (2026-10-13) everything in one step; without ray queries the layer is off;
  acceleration structures hold static and rigid moving geometry, not skinned
  meshes (since 2026-10-17 they hold skinned meshes too, see below); 128
  rays a probe;
- (2026-10-14) when 2048 probes a frame measured 3.6 ms, the budget wins and
  the probe count adapts to it;
- (2026-10-14, lightmaps with the baked volume and the layer) a static lamp
  changed in a group that lamp lightmaps hold takes the whole group out of
  the bake, since a lightmap cannot take one lamp away; baked reflection
  probes take the layer's light at their capture origins.

## Decision

1. **Capability.** The device enables `VK_KHR_acceleration_structure`,
   `VK_KHR_ray_query` and `VK_KHR_deferred_host_operations` when the
   candidate supports them and `VKR_RAY_QUERY_DISABLED` is unset
   ([vkr_vulkan_device.c](../../renderer/src/vulkan/vkr_vulkan_device.c)).
   The volume pipelines exist only then; `vkr_renderer_dynamic_volume_supported`
   reports it, and the volume stays baked elsewhere. The resource layout's
   binding 2 holds one acceleration-structure descriptor per frame slot: the
   AMD driver rejected `OpConvertUToAccelerationStructureKHR` (pipeline
   creation returned `VK_ERROR_UNKNOWN`).
2. **Acceleration structures**
   ([vkr_vulkan_volume_trace.c](../../renderer/src/vulkan/vkr_vulkan_volume_trace.c)).
   - One BLAS per geometry submesh, from positions a compute pass decodes
     (`vk_volume_blas_decode`), at most 256 builds and 131,072 triangles a
     frame, built with `ALLOW_COMPACTION` and compacted on the frame slot's
     next use; the original retires to the slot and goes when the slot comes
     round again. A slot's build memory is released once it has nothing to
     build.
   - The candidate pack records a ray row (geometry, submesh, transform,
     opaque and double-sided flags) per packed opaque row; a skinned row also
     records its deformed vertices, `Animation.Skinning`'s output, which now
     runs before `Volume.Build`. Each skinned row takes one of its frame
     slot's skinned BLASes (at most `VKR_VULKAN_SKINNED_BLAS_MAX`, 64, and
     `VKR_VULKAN_SKINNED_TRIANGLES_PER_FRAME`, 512k triangles): the decode
     reads the deformed positions, and the BLAS (`PREFER_FAST_TRACE`,
     `ALLOW_UPDATE`) refits in place to each frame's pose, rebuilding after
     `VKR_VULKAN_SKINNED_REBUILD_INTERVAL` (30) refits, when its entry held
     another submesh, or when the frame that built it never submitted. Only
     the slot's own TLAS names its skinned BLASes, so the slot's next frame
     refits them once this one has completed. A skinned row past either cap
     counts as missing. Each frame slot's TLAS covers the rows whose BLAS
     exists or builds that frame; an instance's custom index is its packed row, and its
     binding-table offset marks a double-sided material for the grid's
     inside test. The TLAS refits in place while its instance count and the
     scene's BLAS generation hold, and rebuilds every 30 uses.
   - Acceleration-structure storage is its own memory kind with 64 MB device
     blocks ([vkr_vulkan_memory.h](../../renderer/src/vulkan/vkr_vulkan_memory.h)):
     in 8 MB buffer blocks Bistro's BLASes spent the bounded block count and
     4K textures failed to allocate at 1440p.
3. **Passes.** `Volume.Build` and `Volume.Trace` run after `Cull.Upload`,
   whose candidate and instance rows the trace reads, under the graph
   condition `dynamic_volume_active`
   ([main.rendergraph.json](../../assets/render_graphs/main.rendergraph.json)).
4. **Selection**, on the CPU per frame
   ([vkr_dynamic_volume.c](../../runtime/src/renderer/systems/vkr_dynamic_volume.c)).
   - Sources: dynamic point and rectangle lights (sign +1); static lights
     matched to the bake's records by kind and position within 1 cm, a
     changed one adding itself (+1) and taking its record away (−1); records
     with no light (−1); emissive rows past the static candidates. A light's
     empty group name and a record's `default` name the same group.
   - Each source reaches a sphere where its irradiance falls to 0.05
     (32 m at most); the bricks it touches are active, and the bricks it
     left clear. Active probes take turns.
   - The renderer measures `Volume.Build` and `Volume.Trace` with GPU
     timestamps and sets how many probes the next frame may trace within
     1.0 ms, between 64 and 2048 (`vkr_renderer_dynamic_volume_probe_budget`).
5. **Trace** ([volume_trace.slang](../../renderer/src/shaders/vulkan/slang/world/volume_trace.slang)).
   One group per probe, 128 rays on a spherical Fibonacci set rotated each
   frame. Alpha-tested candidates commit where their base colour's alpha
   passes. A hit shades with albedo at texture level 4, the signed point
   lights and rectangle lights (each as a point at its centre with its
   area's form factor) with a shadow ray each, the layer's own light at the
   hit for the next bounce, and emission on dynamic rows. The rays project
   into L1 SH of E/π (`vkr_bake_sh_project_l1`) and blend in at 0.25.
   Surfaces read indices and vertices through the megabuffer bases in the
   pass roots: on Adrenalin 26.6.3 a row's index pointer, loaded from the
   geometry row, compiled to an address whose high word was its low word,
   and the GPU hung with no page fault.
6. **Lookup.** `dynamic_sh` (RGBA16F, the composed SH's probe layout, one
   band) holds the layer unscaled and signed;
   `packet_diffuse_volume_lookup` adds it to the composed layers before the
   clamp, so a removed light's negative part darkens but never below zero
   ([default.slang](../../renderer/src/shaders/vulkan/slang/world/default.slang)).
   It joins lookups once the renderer has cleared the texture.
7. **Runtime grid.** `"diffuse_volume": {"runtime_grid": true}` names no
   file. The runtime lays a uniform level-0 grid of at most 1024 bricks,
   probe spacing at least 0.5 m, over the static candidates' bounds plus
   0.5 m, and rebuilds it when they outgrow it
   (`vkr_dynamic_volume_grid_update`). Its one layer is empty; its probe
   records and moments are writable, and the renderer clears them (unmoved
   and valid; moments at twice the spacing) before a lookup binds the grid.
   Every probe takes turns. Its trace adds the sun with a shadow ray, the
   environment's diffuse response on a miss, every scene light and every
   emitter, and updates each traced probe's moments and relocation by the
   bake's rules: weighted mean and mean square of ray distance per moment
   texel (cosine to the 50th power), border copies across the octahedral
   seams, a move through the closest back face when more than a quarter of
   the rays see back faces, else away from a front face within a tenth of
   the spacing, within 0.45 spacings. A scene whose lamp lightmaps hold its
   static lamps gets no grid: the grid's layer would add those lamps a
   second time.
8. **Groups that leave the bake**
   (`vkr_dynamic_volume_update_baked_groups`). Lamp lightmaps
   ([ADR-104](104-desktop-baked-lamps.md)) hold light per group, so with
   them and a traced volume a group leaves the bake when one of its static
   lights differs from its record, has none, or a record of the group has no
   light (`VkrSceneLightGroups.left_bake`). The group's lamp layers then
   weigh nothing in the lightmaps and the volume
   (`vkr_scene_light_layer_weights`), its static lamps return to the
   runtime light lists, and the selection adds them whole (+1) and takes no
   record of the group away. Without lamp lightmaps a changed light keeps
   the signed path of decision 4; without the trace no group leaves.
9. **Reflection probes.** A baked probe's brightness follows the time of
   day by the ratio of the baked light at its capture origin
   ([ADR-019](019-bounded-forward-spatial-lighting.md)). The frame passes
   that light under the current weights (`light_baked`) in the probe row,
   and after the trace `vk_volume_probe_relight` writes one factor per
   probe: the baked light plus the layer's light there, over the baked
   light, clamped to [1/64, 16]. The layer's light is its constant term,
   weighed by probe validity and trilinear position as the capture record
   weighs the bake's layers, plus the signed sources' shadowed direct light
   averaged over normals, which a lamp group's baked light holds in its
   direct band ([ADR-054](054-baked-diffuse-volumes.md)). The probe loops
   multiply the probe's intensity by it (`packet_probe_relight`); a frame
   without a trace binds none, and probes keep their baked brightness.

## Consequences

- A dynamic light's bounce appears within a few updates of its probes and
  leaves when its bricks clear; at the budget a probe in reach updates about
  every few frames, and more sources slow the turns.
- The layer costs memory: on the café case at 1280 × 720, `mesh` allocations
  rose from 201 MB to 317 MB live (764 MB at peak while building, at the
  earlier 2048-probe settings).
- The CPU records take their own reserve of 80 MiB, made on the first
  traced frame: each frame slot's ray rows (72 bytes per packed opaque row,
  18 MiB at the 262,144-row cap) and BLAS work lists. In the renderer's
  128 MiB allocator a large level's rows left too little for the editor's
  viewport image, and frames without the layer held them too.
- Rectangle lights are approximated by their centre; spot cones use the
  shared punctual kernel.
- The grid on a scene as large as Bistro is coarse (4.88 m spacing for
  10 × 10 × 10 bricks); it serves blockout, not final lighting.
- Without records, a bake's static lights count as unchanged.
- A group that leaves the bake runs every lamp it has live: Bistro's 72
  lamps share the group `default`, so one changed lamp returns all 72 to
  the point-light lists. The group's bounce then builds up over the layer's
  turns.
- A probe's relight scales its whole cube by one number, like the time of
  day ratio: the cube still shows the room as it was lit at capture.

## Evidence

Windows, RX 6700 XT, Adrenalin 26.6.3.

- **Bounce follows the light.** `dynamic_volume_bistro_cafe_capture`
  (1280 × 720, manual exposure 30, the harness moving `Moving Lamp Rail` at
  1 m/s), Release, layer on against `VKR_DYNAMIC_VOLUME_DISABLED=1`: the
  `indirect_diffuse` mean rose from 31.9 to 36.4, 57.4 and 65.9 at frames 0,
  200 and 400 as the lamp moved in (reports `sha256:d0661984…` on,
  `sha256:e2667ed2…` off).
- **Validation.** Debug with `VK_LAYER_KHRONOS_validation` and
  synchronization: the café case (`sha256:0d50fcea…`) and
  `runtime_grid_bistro_cafe_capture` (`sha256:457ce6ca…`) reported no
  validation message.
- **Budget.** `profile`, `local-offscreen-gpu-repeated`, 2560 × 1440,
  5 × 300 frames, `dynamic_volume_bistro_cafe_timing`: `Volume.Build`
  0.045 ms and `Volume.Trace` 0.811 ms p50 (p95 1.048), frame wall 20.07 ms
  (p95 20.85) against 19.43 ms (p95 20.37) with the layer off
  (`sha256:93fc987f…`, off `sha256:b61c68ce…`). At a fixed 2048 probes with
  per-frame TLAS rebuilds the trace took 3.06 ms and the build 0.51 ms
  (`sha256:833f66b2…`).
- **No light change, no change.** `sparse_volume_bistro_street_on_mode9`
  (no dynamic light or mover), Release, layer on against off: depth is
  identical and `indirect_diffuse` differs on 13 pixels by at most 10/255,
  as two runs with the layer off differ on 15 pixels by at most 10/255
  (`sha256:ff668d5e…`, `sha256:58f11cac…`, `sha256:cbc41c93…`).
- **A changed lamp leaves the bake with its group.** The Bistro volume
  rebaked on 2026-10-14 records 72 static lamps in group `default`
  (`sha256:907a7e03…`). Release, `local-offscreen`, 1920 × 1080:
  - `bistro_lamps_baked_volume_night_capture`, nothing changed:
    `lighting.point.selected` 0, the lamps stay baked
    (`sha256:7e83ba77…`);
  - `bistro_lamps_baked_volume_night_changed_capture`, one static lamp the
    bake does not hold: 73 lamps run live and the added lamp lights the
    street (`sha256:765641c1…`);
  - the same case with `VKR_DYNAMIC_VOLUME_DISABLED=1`: 0 lamps live, and
    the added lamp does not light at all, as before this change
    (`sha256:91920115…`).

  The first run of the unchanged case selected all 72 lamps: a record
  named `default` did not match a lamp's empty group name, so every Bistro
  lamp counted as changed, in the signed path of decision 4 as well. The
  comparison now resolves the empty name.
- **Probes follow the layer.** `dynamic_volume_bistro_cafe_capture`, the
  relight against a shader catalog whose relight writes 1
  (`VKR_SHADER_CATALOG`): at frame 0 `final_color` changed on 27.9 % of
  pixels by more than 8/255, mean 50.5 to 60.8, in the café's reflections;
  `indirect_diffuse` changed on 0.14 %. Two runs with the relight differ on
  0.03 % and 0.13 % (`sha256:8d6d768c…` on, `sha256:912ed1f8…` off,
  `sha256:e6d45f08…` repeat).
- **Validation of the relight and the fallback.** Debug with
  `VK_LAYER_KHRONOS_validation` and synchronization: the café case
  (`sha256:ac51b819…`) and the changed night case (`sha256:72d145cf…`,
  which logged the group leaving: `0x0 -> 0x1`) reported no validation
  message.
- **A large level.** A copy of the editor's Level Design Test scene
  (20,792 packed rows, 12,934 bricks), headless Release editor at
  910 × 557: the frame took 150 to 195 ms with the layer and 12.8 ms
  without it. Preparing the TLAS instances read each instance back from
  the mapped instance buffer, device memory on this GPU, and assigned its
  bitfields in place, which also reads: 130 ms of CPU a frame. Writing each
  instance whole and keeping its BLAS on the CPU took the preparation to
  1.5 ms and the frame to 14.4 ms.
- **CPU tests.** `build_test.bat` (format, path and libc guards, Debug
  build): 804 passed, with `test_changed_lamp_group_leaves_bake`.
- **The hang.** Until the megabuffer bases were passed in, any frame whose
  rays hit geometry lost the device. Radeon GPU Detective showed waves
  stalled on the index loads with no page fault, and the disassembly the
  bad address.

## Alternatives considered

- **A band in `DiffuseVolume.Compose`**, as the proposal sketched: compose
  reruns only when layer weights move, while the layer changes every frame;
  adding it at lookup costs three texel loads a corner.
- **A layer holding all light, replacing the bake**: the owner chose the
  additive layer first; the grid is that mode for unbaked scenes.
- **Fixed probe counts**: 2048 probes cost 3.6 times the budget.

## Revisit when

- A scene's lamps need groups of their own: with Bistro's one group, any
  changed lamp runs all 72 live.
- The editor bakes a volume: it should write `runtime_grid` until then.
- A scene needs a finer grid than 1024 bricks allow, or the all-light layer
  should replace the bake for time of day.
