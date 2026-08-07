---
status: investigation
updated: 2026-08-05
authority: investigation
---

# Bistro Baseline Shading Investigation

## Conclusion

This document diagnoses the now-historical **pre-HDR** Bistro generation
committed in `b48eb82`, records the implemented follow-ups, and preserves the
correction of an initially false visual-completion claim. That baseline's flat,
grey, opaque appearance had several independent causes:

- its recorded frames contain no analytic directional or point-light
  contribution, and every cascade reports zero shadow draws;
- the glTF compatibility path lowers 234 specular-glossiness materials to a
  lossy metallic-roughness approximation and discards 109 packed
  specular/glossiness textures;
- all 18 `KHR_materials_transmission` materials remain on the opaque path;
- the historical frame wrote an LDR/8-bit IBL result directly to the sRGB
  target with no exposure or tonemap; and
- the scene used one outdoor environment globally, including inside the café.

These findings explain the dominant classes of artifact, but the baseline did
not contain controlled ablations that assign every pixel-level symptom to one
cause. In particular, the original attribution of white foliage directly to a
dropped specular/glossiness texture was too strong: the named foliage materials
do not carry that texture. Their high gloss is authored as a factor, while the
compatibility path's material loss is principally the missing specular factor.

All implementation slices shipped on 2026-08-04: alpha-tested shadow routing,
the PCF basis fix, prepared specular-glossiness lowering, graph-declared
transmission, bounded glTF punctual lights, caster-relevant cascade fitting,
fragment-local IBL, and transitive scene-content fingerprints. ADR-017 through
ADR-019 own the material, transmission, and bounded spatial-lighting decisions.
Owner testing then exposed three remaining fundamental failures in the first
completion candidate: no visible final-color shadows, persistent indoor
over-lighting, and widespread conductor-like gloss. That candidate and its
baseline proposal are withdrawn.

The correction identified three concrete causes:

- the actual `bistro.scene.json` authored neither an environment override nor a
  reflection probe, so the full outdoor HDR environment illuminated the café;
- the specular-glossiness solver omitted the Khronos rule that perceived
  specular below dielectric F0 is non-metallic, causing dark, zero-specular room
  texels to solve toward metallic; and
- the mesh cache still referenced version-1 generated textures after the
  converter changed, bypassing regeneration.

The corrected implementation adds the sub-F0 branch, generated namespace v2,
a bounded café probe, shared scene-environment probe maps, and an analytic-direct
`lighting` capture that excludes IBL, emissive, and transmission. The probe
initially reused those outdoor scene maps; the final 2026-08-05 owner audit
replaced that source with authored indoor diffuse irradiance and disabled local
specular IBL because no indoor reflection capture exists.

A later owner camera walk exposed two more independent defects in that
candidate. First, its camera-ranked global 16-light set changed membership as
the camera moved, so stationary receivers switched nearby fixtures on and off.
Second, retaining authored F0 was insufficient while the shader still assumed
white grazing reflectance: zero-specular legacy materials regained a circular,
camera-moving environment highlight through implicit `F90 = 1`. The completed
intermediate path used a stable 128-light scene table with 12 receiver-local
references, derived F90 from retained F0, published optional per-texel
dielectric F0 maps, and used mesh-cache version 12.

Five later owner cameras proved that stable object-level assignment was still
the wrong granularity. Broad material-merged receivers overlapped dozens of
lights; the diagnostic replay counted 44,200 influencing pairs and discarded
34,305 behind the 12-reference cap. The completed punctual-light path now uses
a fixed 384-cell world grid with a full 128-bit light mask per cell and exact
fragment range/cone rejection.

The first IBL ablations appeared to assign the moving wall boundary to
environment specular and motivated normal-footprint filtering, specular AO, and
horizon rejection. That conclusion was incomplete. The final direct-diffuse
and normal-channel audit proved the remaining sharp camera-translating boundary
was analytic diffuse: `faceforward` flipped two-sided normals from the view
vector. PBR now orients them from `SV_IsFrontFace`, making the receiver
orientation primitive-defined rather than camera-defined. The receiver-level
completion claim and every prior unaccepted baseline proposal remain invalid.

After those corrections and the curved-wall audit closed the last owner
question, the owner authorized a replacement golden on 2026-08-05. The current
fixture uses fourteen supplied world-space cameras at 1600x1200. All previews
were reviewed before guarded exact-digest acceptance; the current generation
and reproducibility evidence are recorded below.

### Current status

The current status authority is
[renderer-architecture-spec.md](../architecture/renderer-architecture-spec.md),
not this investigation.

| Area | Status on 2026-08-05 | Authority / evidence |
|---|---|---|
| Bistro directional sun | Implemented at authored intensity `0.75` | Current scene plus architecture spec §3.7 |
| HDR environment and half-float IBL chain | Implemented | [ADR-016](../architecture/adr/016-hdr-environment-format.md) |
| HDR scene color, exposure, and ACES-fitted tonemap | Implemented | Architecture spec §3.7 and feature table |
| PDF/solid-angle prefilter source LOD | Implemented | [HDR/IBL implementation spec](hdr-environment-ibl-spec.md) |
| Constant ambient while IBL is active | Removed; ambient is now the no-IBL fallback | `world/pbr.slang` |
| Blend/cutout draw routing | Implemented independently for world and shadow lists | `application_build_world_payload()` and `vkr_draw_alpha_routing()` |
| PCF light-space hash origin | Corrected and CPU-pinned against the shader basis | `vkr_shadow_light_space_origin_from_view()` |
| Specular-glossiness material conversion | Implemented | Prepared, cached repack into base-color/metallic-roughness plus uniform or optional per-texel dielectric F0; F90 is derived from retained F0; [ADR-017](../architecture/adr/017-prepared-specular-glossiness-lowering.md) |
| `KHR_materials_transmission` | Implemented | Separate graph feedback and transmission stages; [ADR-018](../architecture/adr/018-graph-declared-transmission-feedback.md) |
| `KHR_lights_punctual` | Implemented | Point, spot, and directional import into a stable 128-light scene table; a bounded 384-cell world grid carries complete 128-bit masks to fragment-local exact rejection, with table/grid metrics |
| Localized indoor IBL | Implemented for Bistro | Two local probes selected per draw, fragment AABB weights, and global remainder. The café volume uses an authored indoor diffuse cubemap; local specular is zero until an indoor reflection capture exists; [ADR-019](../architecture/adr/019-bounded-forward-spatial-lighting.md) |
| Scene-content provenance | Implemented | Sorted transitive manifest includes prepared glTF material files, generated textures, and present packed siblings; its digest participates in the workload fingerprint and is published as a report artifact |
| Accepted Bistro baselines | Backend-pinned fourteen-view post-correction goldens with deterministic text; prior rejected proposals remain invalid | Legacy Vulkan generation `sha256:c3596ff14cdf206d0be4138840957925bd18353dd5d8eab339bfdec575df3564` and Metal generation `sha256:3db4f4d2294e5fdbc3618e64c4b2baf03bf66051dee0c4ff452e341d20cae51d`; fresh explicit compares pass all fourteen rows |

The final calibrated local HDR/sun run is
`20260804T085151.570Z-00b294`, report digest
`sha256:8ebe00c3a2c596482049b9dbdb0be523cbd5790b60d2df65675e233c57bab678`.
All five replays were validation-clean, and bright-luma coverage was
`0.05%-1.05%`, down from `6.44%-21.38%` in the uncalibrated HDR run. It
intentionally differs from the guarded pre-HDR generation and was neither
proposed nor accepted as a replacement baseline.

The final focused implementation captures are:

- spec-gloss unlit parity: `20260804T150302.250Z-0053fa`, report
  `sha256:8eca9278d2f0b4dd2e134e92327fa1cff2c643d3850ada33a98067b0dbe92fa2`;
  the two panel-center RGB samples are both `80 51 26`;
- layered transmission fullscreen: `20260804T150225.308Z-005383`, report
  `sha256:16fdedcf0c10b0a6fbb34c8e8fd38cb976f210eec0ddd025be14427a5e401505`;
- layered transmission editor: `20260804T150236.277Z-0053c5`, report
  `sha256:806dc87acb535652b8e483e8f7f20594843ea8953581db6691133783c30f891e`;
- broad-mesh local IBL: `20260804T150248.830Z-0051d5`, report
  `sha256:c52ca78e9ca874c6545ae6bc509443ddf1f64e179820ca52b40cd0691b30b1bb`;
  and
- five-position shadow-factor motion: `20260804T145726.557Z-004c7f`, report
  `sha256:dda6678cf3d51084b7060f4d0f24959d986aff5c62630eed70e3e6ebef8c6753`.

All are non-authoritative local/dirty correctness evidence. Their isolated
children passed without Vulkan validation diagnostics. The motion aggregate and
all five children share workload fingerprint
`sha256:057284f0a65d988e40a80c51e1687e5a2b1dc5065be74d3f7c9c11c85e60ab24`;
its published 689-asset manifest has canonical content digest
`sha256:5ff556fa5b2928e06bda039ad3b893f1989ca7c511f1ec45909404aa2a403740`.

The first claimed final five-view run, `20260804T151505.151Z-00644f`, was
execution-clean but visually wrong. Its proposal at
`build/_artifacts/baseline/20260804T152024.898Z-0064a8/plan.json`, confirmation
digest
`sha256:a2aa7caf8cdea47d87596a829c1cf2c4a8107b7571ba01dd69e9df1edb5dcfdd`,
is invalid and must not be accepted or reused. The current pointer was never
changed.

Corrected evidence from the regenerated version-2 material cache is:

- interior offscreen final color: `20260804T162828.639Z-009182`, report digest
  `sha256:f7ef2dd6192f9a48b334e86e6f71006538c16cfd50db10489411a3f190c8bead`;
- paired exterior final color, analytic-only lighting, and shadow factor:
  `20260804T172035.207Z-00bcd0`, report digest
  `sha256:34e18ba5faea7885d57dc02cf2ac26394bc7931373d49a3ee13af000a3093690`;
- paired interior final color, HDR scene color, unlit material, analytic-only
  lighting, and shadow factor: `20260804T170935.156Z-00b50b`, report digest
  `sha256:25c2f4f5ef0b3a95d2aa0dd2a6dc12c334351d2e8f2cf9234f8a67e48b81e593`;
- final five-view Bistro replay: `20260804T170504.198Z-00b23e`, report digest
  `sha256:f72404b517488195d013f27303bdb7de02c236d6f346d3284dd0a62f9062ac31`,
  with five passing children, empty validation stderr, shared workload
  fingerprint
  `sha256:f69476b246d98fc2edddffeae76963822fbe22b5c41eb8f6228b2b66455e6f22`,
  and a 1,442-asset manifest that includes 36 present generated sg2 `.vkt`
  siblings;
  and
- hidden-window application-equivalent final color:
  `20260804T163924.287Z-0099c7`, report digest
  `sha256:589e1225182276256ef1c75cb4de5fbc0ec94c74ef7c3349231673faf0c8991a`.

Full-resolution review shows a readable café with localized fixture/tableware
highlights instead of a broad silver wash. The exterior paired capture exposes
bounded analytic light and coherent shadow-factor regions aligned with darker
final-color receivers. On the same upper façade, the lit receiver at pixel
`(700,210)` has factor `175/255` and final RGB `(53,49,48)`, while the shadowed
receiver at `(800,210)` has factor `0/255` and final RGB `(29,29,27)`. The
five-view aggregate exit `4` is the expected
baseline incompatibility after scene and generated-content changes, not a child
execution or validation failure.

The corrected five-view run is proposed as generation
`sha256:364c0da293733ec9ec6e59996f2707a3df6573f0347a3087ae70aabc3cb43b95`.
The no-mutation plan is
`build/_artifacts/baseline/20260804T171130.209Z-00b65a/plan.json`, confirmation
digest
`sha256:c7d22dff733c8aa68ddd4ecf8858a67dadb53ee078b985bc763d4c5771005d00`.
It has not been accepted; the accepted pointer remains generation
`sha256:3045cea88015bb0efb35f6f0b0b47384c0ac7b71b8502bd731b537d3aff8d465`.

Later owner camera testing invalidated that second proposal as a visual
completion candidate too. It must not be accepted: it still used camera-ranked
global light membership and implicit white F90 on zero-specular legacy
materials. The replacement owner-camera replay
`20260804T200153.974Z-011f62`, report digest
`sha256:f2248e5a4cd3d5dccec164edfe11e7957291c78d84bb38737d86b290a6a88eb3`,
captures final color, analytic-only lighting, and unlit output at both supplied
endpoints. Full-resolution review shows the circular sign highlight removed at
both positions; all six children pass, record a 72-light scene table with zero
table drops for every one of the seven motion frames, and contain no validation
diagnostics.

The final five-view replay from the same corrected binary is
`20260804T202607.453Z-013306`, report digest
`sha256:138efcb9fab10b0079eb4f0115f6b138cf16a641d986b0c4d3879ebe53ee8112`.
All five children pass, share workload fingerprint
`sha256:a454bef90819a886d68bafb085052e3d3739864de0907c724a2d2736b8f84631`,
retain all 72 scene lights with zero scene-table drops, and contain no
validation diagnostics. Full-resolution review found coherent exterior,
café, courtyard, façade, and narrow-street responses without the circular sign
highlight or the earlier broad interior gloss. Aggregate exit `4` is only the
expected incompatibility with the pre-HDR accepted baseline.

That run is proposed as generation
`sha256:2b7d1f695fbc6a715b63a4be980a0d5331a939feecb4a9ae5ca29085c6252abf`.
The no-mutation plan is
`build/_artifacts/baseline/20260804T203308.761Z-01348d/plan.json`, confirmation
digest
`sha256:70ac562cfa980fef21ddeaf4567b6b3f0e9ab17af3fba5027518d4d97045f044`.
It has not been accepted; the accepted pointer remains generation
`sha256:3045cea88015bb0efb35f6f0b0b47384c0ac7b71b8502bd731b537d3aff8d465`.

The matched local/dirty 3200×2400 Release observation
`20260804T200948.429Z-0120be`, digest
`sha256:b1f1eb28c3d00458e0568583884a506d78f061139841f6acb4e148aacc3ed9db`,
completed two stable-warmup repetitions with the same 206-draw work volume.
CPU submit p50 was 4.21 ms versus 5.32 ms in the earlier observation; opaque
GPU p50 was 93.27 ms versus 87.32 ms. The workload fingerprints differ and both
runs permit dirty provenance, so these figures are diagnostic rather than an
authoritative performance comparison.

The five owner-camera fragment-grid diagnostic is
`20260804T211344.780Z-01503c`, report digest
`sha256:983626b51329458472ad11473567d84538fc771fe257d3c481233e204e72f0e2`.
Its 14 passing children measured 44,200 influencing receiver/light pairs and
34,305 receiver-list drops. The no-specular-IBL ablation
`20260804T212546.324Z-0159ae`, digest
`sha256:d7142a153238f7eb8b75cd830366298bdc0220233ec9645cc19bd3a7b2657c34`,
and no-IBL control `20260804T213104.407Z-015d63`, digest
`sha256:9a754ea1147d9806bd7a2295401beed16644e07887cfba24c2a160e13f0ec4a2`,
appeared to isolate the camera-moving wall boundary to specular IBL. The later
direct-diffuse and normal-channel audit supersedes that attribution.

The final optimized five-camera replay is
`20260804T223449.580Z-000d77`, report digest
`sha256:2de757f5aa6d221fdf03b53d80f98ea2c0a691c695e28d5f6b50c1d55d5136e1`.
All five children and all 35 metric assertions pass: the stable table retains
all 72 lights with zero drops, the grid is 363 cells with 1,635 references and
a 47-light maximum cell, and full-resolution inspection shows the moving IBL
boundary removed and the previously dark fixture regions lit. This is
non-authoritative local/dirty correctness evidence with no accepted baseline.
At the time, startup still reported the repository-wide
`VUID-VkPipelineLayoutCreateInfo-descriptorType-03016`: the PBR layout then
exposed 17 fragment samplers on a device whose limit is 16. The driver continued
and the captures passed, but the log was not validation-clean; §8 of the
architecture status records the later resolution of that compatibility issue.

The final 2026-08-05 five-camera replay is
`20260805T102236.609Z-01020d`, report digest
`sha256:ee1810dbaa479cf627458f24de79096a97ec12850f07b55658b1a46a2728d6de`.
All five exact 1600×1200 children pass with workload fingerprint
`sha256:b99fd4de02f3e8c76938d83cc4ab17d4c4d3bc2bede84b5a6e76cbdff970ad97`
and no VUID, validation error, error, fatal, sampler-limit, or invalid-command
diagnostics. Full-resolution review shows no sharp camera-translating wall
boundary. The entrance remains dark/readable and the two interior views are
warm and bounded by the authored café diffuse probe rather than the outdoor
environment. The PBR layout now reflects 17 sampled images but only 13 sampler
descriptors, and IBL conversions use immutable per-bake descriptor state with
GPU-deferred release. This report is local/dirty correctness evidence; it is not
an authoritative baseline or performance result.

The optimized local/dirty 3200×2400 Release report is
`20260804T222805.692Z-000857`, digest
`sha256:1437beecdf19427241f5c65e86cc4de433658a11b57c2b216e3bb8eebfabf4e7`.
Against the same-fingerprint first grid report
`20260804T221657.247Z-01866a`, digest
`sha256:258957483456afd2299952bd5aa8ed7422479ad276ecd2b7e864da15fe372b85`,
exact sphere/cell membership and zero-contribution early rejection reduce
World Opaque p50 from 109.528 ms to 93.243 ms (-14.87%). The older receiver
report has a different workload fingerprint; its 93.272 ms World Opaque and
103.326 ms frame-wall p50 are therefore observational context, not a formal
comparison. The optimized run records 103.452 ms frame-wall p50.

## Evidence and limits

### Baseline inspected

The accepted generation inspected by this historical investigation was:

```text
tools/baselines/local.offscreen/smoke.bistro.snapshot/generations/
3045cea88015bb0efb35f6f0b0b47384c0ac7b71b8502bd731b537d3aff8d465/
```

That root was superseded and removed when regression authority was consolidated
into the backend-pinned Vulkan-plus-text and Metal-plus-text roots. Its report
records a Debug Apple M1 Pro/MoltenVK run at 1600×1200, two-image
offscreen target, `bgra8_srgb` color, `d32_sfloat` depth, and four cascades.
The run is explicitly non-authoritative (`profile.local_only` and
`provenance.dirty`). That is sufficient for the structural metric and code-path
findings below; it is not timing evidence and must not support a performance
claim.

The asset counts in this investigation were recomputed from the local
`assets/models/bistro-lights.gltf`, whose observed SHA-256 is
`0b95f6fb8cd1bddc2607056b86537cf10943c9388e0b5f9f8f90128995850d6f`.

### Historical reproducibility gap, now closed for new runs

`assets/models/` and `assets/scenes/` were ignored by Git when the historical
baseline was accepted. It therefore does not preserve the exact scene or model
bytes that produced it. The corrected `assets/scenes/bistro.scene.json` is now
explicitly unignored so its environment, sun, model choice, and probe bounds can
be reviewed with the implementation; the source model remains external content.
The historical harness workload fingerprint included the scene **path**,
camera, renderer configuration, and captures, but not the scene file or its
transitive asset digests. An ignored asset could change without changing the
Git dirty flag or workload fingerprint.

Consequences:

- the pixels and metrics are durable evidence of what ran;
- the historical scene-file contents and the asset counts cannot be
  reconstructed from commit `b48eb82` alone; and
- the historical generation cannot be repaired retroactively.

The harness now resolves every transitively loaded scene, mesh, material,
texture, environment face, glTF/GLB/OBJ/MTL dependency, prepared glTF material
file, generated texture, and existing packed sidecar. It publishes sorted path,
size, and SHA-256 entries and incorporates the canonical manifest digest into
workload identity. Missing, unreadable, or repository-escaping dependencies
fail the run. Asset-backed statements about the old generation retain the
historical limitation; new reports do not.

## Historical baseline findings

### 1. No analytic light contributed to the accepted frames

Every shadow metric in every accepted view is zero:

```text
draw.shadow.cascade0..3.opaque_calls  mean=0  total=0
draw.shadow.cascade0..3.alpha_calls   mean=0  total=0
visibility.objects_culled_shadow      mean=0  total=0
```

`vkr_lighting_system_init()` starts with the directional light disabled, and
`sync_directional_light_cb()` enables it only after finding an enabled
`SceneDirectionalLight`. The scene used for the historical investigation had
no such component. The CSM pass therefore recorded no casters, and the direct
sun branch in `world/pbr.slang` did not run. Any shadow-shaped detail visible in
the accepted frames was not produced by the CSM pass.

The loaded `bistro-lights.gltf` contains eight
`KHR_lights_punctual` definitions: one directional `Sun` with intensity `0`
and seven point-light definitions instanced by 72 nodes. The alternative
`bistro.gltf`, which the scene does not load, gives its `Sun` intensity `6830`.
The current scene resolves the missing-key-light problem by authoring its own
directional component at intensity `0.75`.

#### Punctual-light import — resolved 2026-08-04

The original gap was not just a parser omission:

- `VKR_MAX_POINT_LIGHTS` is 16, so importing all 72 Bistro point-light node
  instances would exceed the current fixed shader array by 56;
- `point_light_insert_sorted()` kept the lowest render IDs, not the most
  influential lights for the camera or shaded region, and dropped excess
  lights without an overflow metric or warning; and
- `vkr_lighting_system_apply_uniforms()` stored
  `color * intensity` in `point_light_data`, stores intensity again, and
  `world/pbr.slang` multiplies the two. Point-light radiance therefore scales
  with intensity squared.

The intermediate implementation replaced that traversal-order set with a
camera-ranked global set of 16. Although its ties were stable, camera movement
changed membership at the capacity boundary. The two owner cameras in
`smoke.bistro.camera.light.pop` reproduced the resulting fixture on/off
behavior.

The scene loader now imports point, spot, and directional light nodes through
their scene transforms. CPU-side synchronization retains 128 lights in a stable
camera-independent table and builds a camera-independent 3D grid with at most
384 cells. Every cell carries a full 128-bit mask; finite light spheres are
inserted conservatively, cell size grows rather than dropping membership, and
unbounded polynomial lights use a global mask. The fragment selects its cell,
iterates set bits, rejects zero range/cone contribution before BRDF work, and
applies exact inverse-square/range/cone attenuation.
Raw color and intensity remain separate and intensity is applied once. Metrics
report scene-table overflow plus grid cells, references, maximum membership,
and global lights. CPU tests pin separated-cell locality, conservative range
coverage, all 128 bits, unbounded lights, and deterministic rebuilds.

### 2. Specular-glossiness conversion was lossy — resolved 2026-08-04

The historical compatibility path in
`vkr_mesh_loader_gltf_write_material_file()` does this:

```c
if (material->has_pbr_specular_glossiness) {
  base_color = specular_glossiness->diffuse_factor;
  metallic = 0.0f;
  roughness = 1.0f - specular_glossiness->glossiness_factor;
  base_color_texture_view = &specular_glossiness->diffuse_texture;
  metallic_roughness_texture_view = NULL;
}
```

Observed scale in `bistro-lights.gltf`:

| Fact | Count |
|---|---:|
| Materials | 254 |
| `KHR_materials_pbrSpecularGlossiness` materials | 234 |
| Packed `specularGlossinessTexture` values discarded | 109 |
| `glossinessFactor >= 0.98` | 34 |
| High-gloss materials that also have a packed texture | 15 |
| Materials whose authored `specularFactor` maximum exceeds `0.6` | 48 |

The conversion loses:

- per-texel glossiness from texture alpha for all 109 textured materials;
- per-texel specular color from texture RGB;
- the material-level `specularFactor`; and
- the conversion from authored specular F0 to corresponding metallic and base
  color values.

Two corrections matter when interpreting the captures:

1. Only 15 of the 34 high-gloss materials also have a packed texture. The
   named foliage materials (`LMBR_00000a6_leaf_b`,
   `LMBR_00000b3_Foliage`, and `LMBR_000017e_Foliage`) have
   `glossinessFactor = 1` but **no** specular/glossiness texture. Their low
   roughness follows the authored factor; it is not created by dropping a map.
   Their authored achromatic specular factors are also close to the runtime's
   dielectric F0. The accepted images are consistent with high-gloss foliage
   reflecting the environment, but they do not isolate the importer as the
   cause of the white highlights.
2. "There is not one correct metal" was false. The 20 non-spec-gloss
   materials include metallic-roughness entries such as
   `LMBR_000001c_metal_grain_01` and `LMBR_0000040_black_metal` with
   `metallicFactor = 1`. The loss applies to conductor-like materials authored
   through the legacy extension, not every metal in the scene.

#### Decision implemented

ADR-017 selects prepared conversion. Import combines factors and texture texels
into versioned generated base-color and metallic-roughness PNGs. Authored
dielectric F0 remains a uniform when only factors exist and an optional linear
RGB companion when a packed specular/glossiness texture exists. The shader
derives legacy grazing reflectance from that retained value instead of assuming
white F90. Generated images and `.mt` sidecars publish atomically; mesh-cache
version 12 invalidates older and interrupted results. Numeric factor/texel
tests, PNG round trips, repeat/resume preparation, and focused snapshots pin
the conversion and ownership path.

### 3. Transmission was ignored — resolved 2026-08-04

Bistro declares `KHR_materials_transmission` on 18 materials. All 18 have
`alphaMode: OPAQUE`, as expected for the extension: transmission is not base
color alpha. The historical importer did not read the extension, so those
materials remained ordinary opaque PBR draws.

The accepted run contains:

```text
draw.world.opaque_draws       mean=143.2  total=716
draw.world.transparent_draws  mean=1.4    total=7
```

The source asset has 231 `OPAQUE`, 20 `MASK`, and 3 `BLEND` materials. The low
transparent count therefore reflects classification and unsupported
transmission, not a demonstrated blend-sort failure.

The original document cited `vkr_draw_batcher_finalize()` as proof of
production sorting, but `VkrDrawBatcher` has no production caller. The real path
is `application_build_world_payload()` plus `vkr_visibility.c`:

- blended candidates receive a back-to-front depth key;
- `application_pack_transparent_sort_key()` includes `vis_slot` as a stable
  tie-breaker; and
- `vkr_draw_candidate_depth_compare()` sorts that total key before unmerged
  emission.

The production transparent order is therefore deterministic for the current
packet traversal. No separate stable-sort fix is required.

#### Distinct material and pass contract implemented

ADR-018 implements the production path without mapping transmission to base
color alpha:

1. preserve transmission, IOR, and optional volume data independently of glTF
   `alphaMode`;
2. render skybox and opaque world color first;
3. expose that pre-transmission HDR color through a declared graph resource;
4. render transmissive surfaces from a separate list/pass that samples the
   feedback resource without sampling and writing the same image; and
5. keep ordinary blended transparency depth-ordered around an explicit policy
   for its relation to transmission.

The fullscreen and editor graphs now split opaque, HDR feedback copy,
transmission, and ordinary blended stages. The material/import contract carries
transmission, IOR, thickness, attenuation distance/color, and textures.
Refraction samples only the feedback image; Beer-Lambert attenuation and
Fresnel are applied, opaque output alpha is one, and double-sided normals face
the viewer. Layered-glass fixtures pass in fullscreen and editor snapshots.

### 4. HDR presentation and IBL defects were historical and are now resolved

The accepted generation used:

- six 8-bit sRGB JPEG cubemap faces as the environment;
- `R8G8B8A8_UNORM` irradiance and specular-prefilter targets;
- an 8-bit BRDF LUT;
- implicit-derivative `Sample()` inside the GGX importance-sampling loop;
- constant ambient in addition to IBL; and
- direct output to `bgra8_srgb` with no exposure or tonemap.

That chain cannot preserve HDR range and provides little linear precision for
dark, smooth gradients. It plausibly explains the flat environment and the
concentric chromatic bands reported on the interior floor. The geometry of
those arcs is consistent with quantization of a view-dependent reflection on a
plane, but no isolated format-only ablation was captured; "8-bit IBL storage is
the proven sole root cause" would overstate the evidence.

ADR-016 and the HDR/IBL implementation spec replaced the whole chain with:

- a 4096×2048 Radiance HDR source and seam-safe equirectangular-to-cubemap
  conversion;
- RGBA16F environment, irradiance, prefilter, BRDF LUT, and scene color;
- explicit PDF/solid-angle source LOD for the GGX prefilter;
- packet-carried manual exposure (default `0.30`) and an ACES-fitted tonemap;
  and
- constant ambient only when IBL is disabled.

These are implemented facts. Any claim that banding, bake cost, memory cost, or
frame time improved still needs the specific before/after evidence named in the
HDR/IBL spec. SH L2 remains a possible diffuse-irradiance representation, not a
fix for source range and not a measured win in this renderer.

The reflection-probe foundation is fragment-aware. Extraction selects the two
closest local probe AABBs that overlap each draw's world-space bounding sphere,
plus a fixed global fallback descriptor. The shader evaluates and normalizes
per-fragment AABB/blend-distance weights and assigns unused weight to the global
environment. A broad-mesh fixture spans local influence, transition, and
global-only regions. Bistro now supplies an authored indoor diffuse cubemap for
its café volume; diffuse irradiance uses the geometric surface direction,
whereas box projection is restricted to specular reflection rays.

## Current shadow follow-ups

The accepted pre-HDR frames could not validate CSM because they recorded no
shadow draws. The authored sun now makes the following paths observable.

### 1. Alpha-masked caster routing — resolved 2026-08-04

Historically, `application_material_is_cutout()` called
`vkr_material_system_material_has_transparency()`, which is true only for
`VKR_MATERIAL_ALPHA_BLEND`, and did not evaluate the material's cutout mode.

As a result:

- the 20 Bistro `MASK` materials enter the opaque world/shadow lists;
- the world PBR shader still discards them because it receives an alpha cutoff;
  but
- the opaque shadow shader has no texture or cutoff and writes the entire
  triangle silhouette.

Conversely, the three `BLEND` materials enter the alpha-shadow list but receive
cutoff zero. The calibrated sun run reports a mean of three alpha shadow calls
per cascade, matching the three `BLEND` materials rather than the 20 masks.

`application_build_world_payload()` now resolves the effective
`VkrMaterialAlphaMode` once per candidate and uses
`vkr_draw_alpha_routing()` to make two independent decisions: `BLEND` controls
the world transparent list and `CUTOUT` controls the alpha-tested shadow list.
The count and population passes consume the same mapping. CPU regressions pin
all three modes, including the loader-backed cutout resolver.

The five-view Bistro snapshot
`20260804T105412.251Z-010923` records a mean of 20 alpha calls in every cascade
for every isolated replay, matching the 20 source `MASK` materials rather than
the three `BLEND` materials. Its five replay children passed with no validation
diagnostics. The aggregate exits 1 only because the accepted guarded generation
is still the intentionally pre-HDR baseline. A focused direct-layer snapshot
from the final rebuilt binary, `20260804T110741.708Z-0115f8`, exits 0 and
preserves fine branch/foliage
silhouettes in the canonical 2048×2048 `shadow_cascade_0` preview. Both runs are
non-authoritative local/dirty correctness observations, not performance
evidence, and no baseline was proposed or accepted.

### 2. `light_space_origin.x` basis sign — resolved 2026-08-04

`vkr_shadow_compute_light_view()` constructs `right = cross(up_ref, dir)` and
passes `up = cross(dir, right)` to `mat4_look_at()`. The resulting view X basis
is `-right`, while `shadow_light_space_xy()` reconstructs `+right`.
`vkr_shadow_compute_cascade_matrix()` historically stored:

```c
out_light_space_origin->x = left - view.columns.col3.x;
```

The shader's `+right` coordinate requires the negated value
`view.columns.col3.x - left`. The mismatch changed only the PCF hash cell, not
the projected shadow shape.

`vkr_shadow_light_space_origin_from_view()` now owns the sign conversion: X is
negated relative to view space while Y keeps its sign. CPU regressions cover
translated, rotated, and near-vertical light views by comparing the CPU origin
against the shader's reconstructed right/up coordinates. The fixed-camera
Bistro snapshots above validate the rendered path. The
`smoke.bistro.shadow_motion.snapshot` case adds five adjacent camera positions
and captures the canonical shadow-factor diagnostic. It is correctness evidence
only; no temporal-performance claim is made.

### 3. Caster-relevant scene-bounds Z fitting — resolved 2026-08-04

When `scene_bounds.use_scene_bounds` is true,
`vkr_shadow_fit_relevant_caster_z()` now clips the scene AABB against each final
cascade XY rectangle and derives Z only from intersecting caster bounds. Shipped
presets remain unchanged and opt out. CPU tests pin intersecting and disjoint
bounds, so the latent path no longer extends every cascade to the whole scene.

### 4. Behavior-preserving shadow lookup cleanup — resolved 2026-08-04

`world/pbr.slang` now computes view position once and reuses resolved cascade
coverage around blended lookups. The PCF kernel, cascade choice, blend band, and
fallback behavior are unchanged. This is a static behavior-preserving cleanup;
no speedup is claimed, so it does not substitute instruction counts for a
same-configuration Release measurement.

### 5. Curved-wall third owner audit — world-stationary shadow, verified 2026-08-05

Three later owner cameras at positions
`[28.8405819, 3.67631197, 29.54776]`,
`[34.3669891, 3.67631197, 31.860878]`, and
`[40.8812637, 3.67631197, 34.5874672]` showed a dark boundary moving left on
screen as the camera translated right. Eight-channel replay
`20260805T120354.711Z-0177c1` isolated the boundary to raw directional shadow
factor and direct diffuse; unlit, normals, material parameters, direct
specular, and IBL were continuous. Cascade debug placed it inside cascade 1,
and every camera submitted the same 234 opaque indirect commands plus 20
alpha-tested calls to every cascade with zero shadow-union culls.

Scene-depth companion capture `20260805T130951.874Z-013a91` makes the remaining
distinction explicit. At screen row 600 the boundary appears at x=934 in the
first camera and x=393 in the second, but unprojection places both samples at
the same wall coordinate: `(33.8091, 2.7661, 21.4949)` and
`(33.8652, 2.7642, 21.4966)`. Rows 520, 680, and 760 agree to at most 5.6 cm,
with the lower samples agreeing within centimetres. The third camera has moved
entirely beyond the fixed edge into the occluded region. The soft diagonal
silhouette is therefore a world-stationary sun shadow from scene geometry, not
a camera-locked CSM, IBL, normal, or material artifact. Enabling the dormant
scene-bounds Z fitter did not alter it and was not retained. No renderer or
content correction is warranted for this observation.

## Completed implementation order

| Order | Work | Required evidence before completion |
|---:|---|---|
| 1 | ~~Separate blend vs cutout extraction; fix PCF origin X~~ **Shipped 2026-08-04** | CPU routing/basis tests, direct and five-view Bistro shadow captures, clean Vulkan validation |
| 2 | **Shipped:** prepared spec-gloss lowering with retained dielectric F0/F90 | ADR-017, numeric/PNG fixtures, isolated unlit parity and owner-camera captures, versioned generated cache |
| 3 | **Shipped:** graph-declared transmission | ADR-018, opaque/feedback/transmission/blend stages, layered-glass fullscreen/editor captures |
| 4 | **Shipped, corrected 2026-08-05:** punctual import, stable scene table, and fragment-local bitmask grid | ADR-019, numeric glTF fixture, grid locality/range/capacity/determinism tests and table/grid metrics |
| 5 | **Shipped:** relevant-caster fit and lookup cleanup | CPU fit tests and moving-camera shadow-factor case; no speed claim |
| 6 | **Shipped:** fragment-local IBL foundation | Broad-mesh authored probe fixture and local/transition/global capture |
| 7 | **Accepted 2026-08-05:** fourteen-view Bistro golden | Full preview review, exact-digest guarded acceptance, fresh fourteen-view snapshot, and explicit compare |

Do not combine every item into one visual change. One vertical slice per
comparison makes regressions attributable. The guarded baseline may be
superseded after an explicitly reviewed milestone, but `baseline accept` is not
an implementation gate and must not be run without owner authorization.

## Resolved questions and retained boundaries

- Legacy specular-glossiness is repacked during prepared import; retained
  dielectric F0 is a property/optional texture in the one PBR path, not a
  separate runtime material variant.
- Transmission uses declared scene-color feedback and refraction; it is not a
  blend-alpha approximation.
- Point lights remain bounded forward lighting with a stable 128-light scene
  table and a fixed 384-cell fragment-local bitmask grid. GPU-built clustering
  remains a measured scalability option, not a correctness prerequisite.
- A separate transitive scene-content manifest owns the canonical digest,
  including prepared runtime material/texture caches, and that digest is a
  workload-fingerprint field.
- The corrected tracked scene explicitly loads `bistro-lights.gltf`; the
  historical ignored scene did not preserve that authoring decision in Git.

## References

- Historical pre-HDR/shared-root generations were removed after consolidation.
- Current Vulkan case: `tools/cases/smoke/bistro_snapshot.case.json`
- Current Metal case: `tools/cases/smoke/bistro_metal_text_snapshot.case.json`
- Current accepted roots:
  `tools/baselines/local.offscreen/smoke.bistro.vulkan.text.snapshot/` and
  `tools/baselines/local.offscreen/smoke.bistro.metal.text.snapshot/`
- Status authority:
  [renderer-architecture-spec.md](../architecture/renderer-architecture-spec.md)
- HDR rationale:
  [ADR-016](../architecture/adr/016-hdr-environment-format.md)
- HDR implementation and evidence:
  [hdr-environment-ibl-spec.md](hdr-environment-ibl-spec.md)
- Related proposal, not current-status authority:
  [shadow-transmission-transparency-improvements.md](shadow-transmission-transparency-improvements.md)
- Material context:
  [pbr-material-system-design.md](pbr-material-system-design.md) and
  [gltf-loader-design.md](../assets/gltf-loader-design.md)
- CSM context:
  [cascading-shadow-mapping-design.md](cascading-shadow-mapping-design.md)
