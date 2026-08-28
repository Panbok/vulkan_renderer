---
status: partial
updated: 2026-08-28
authority: investigation
---

# Windows Vulkan post-effect parity investigation

**Conclusion.** The reported Windows/Vulkan image regression had two independent
backend defects. The black ground and crushed indirect lighting came from the
**tangent-space normal-map decode**: every shader read
`normal_texture.xyz * 2 - 1` and took
the sampled blue channel as tangent-space Z. The `VKR_TEXTURE_CLASS_NORMAL_RG`
transcode ladder in `vkr_texture_select_transcode_target_format()` selects
**BC5** on a discrete GPU and **ASTC 4x4** on an Apple-class device. BC5 is a
two-channel format, so Vulkan samples return `b = 0` and the decode yields
`z = -1` — the shading normal is pushed through the surface. ASTC is
four-channel, so Apple keeps the authored Z and the same code is correct there.

GTAO then amplifies the wrong normal into the visible artifact: a ground pixel
whose view-space normal points away from the reconstructed hemisphere
integrates to zero visibility and clamps to the `max(0.03, …)` floor in
`vkr_gtao_finalize_visibility()`. Indirect diffuse is multiplied by that floor
and the ground goes black.

Automatic exposure also was not stable. `Post.Exposure.Histogram` declared its
HDR input as `STORAGE_READ`, and Vulkan supplied that storage-array descriptor
index to a shader that indexes `g_textures`, the sampled-image descriptor
array. Those arrays have independent indices. As retained history instances
rotated, the histogram intermittently metered unrelated sampled images. Metal
binds the source texture object directly and did not have this failure mode.
The graph now declares the input as `SAMPLED`, and the Vulkan executor supplies
the matching sampled-image index.

That correction closes the descriptor-driven temporal instability, not the
separate adaptation-policy or absolute-HDR issues. The original exponential
interpolation violated the configured EV-per-second contract and could brighten
too quickly after a large luminance change; both backends now advance by a
bounded linear EV step. A matched native Metal run also proves that Vulkan's
shared exposure resolve is responding to a darker pre-bloom input rather than a
bloom difference. The remaining input mismatch is still a parity blocker.

The fix now ships through one shared Metal/Vulkan shader helper:
`vkr_normal_map_decode()` reconstructs positive tangent-space Z from XY after
normal strength and green-channel convention are applied. Vulkan forward,
Vulkan deferred/transmission, Metal forward, and Metal
deferred/transmission all call it. The exact Bistro camera now renders lit
ground with GTAO enabled on the RX 6700 XT.

**Scope.** Windows 10 Pro 19045, AMD Radeon RX 6700 XT, driver 26.6.3,
Clang 20.1.0, Release and Debug, plus the owner's Apple M1 Pro Metal Release
capture. The original Windows investigation was based on tree `a517999`; the
follow-up uses the fixes through `cac3c7e` plus the changes described below.
This is local implementation evidence, not an authoritative cross-device
performance comparison. The Metal implementation evidence is also summarized in
[post-exposure-bloom-and-ambient-occlusion-spec.md](post-exposure-bloom-and-ambient-occlusion-spec.md)
§7.

---

## 1. Reproduction

Four `tools/cases/local/win_bistro_*.case.json` cases were added as
reproduction fixtures. All run offscreen at 800x600 on the Bistro plaza camera
from the owner's Windows and macOS `CAMERA_SNAPSHOT` logs
(`position [10.0205421, 4.17517662, 27.8467007]`, `yaw 19.2320728`,
`pitch -3.26702642`), so the Windows capture and the owner's `1_mac` screenshot
frame the same view.

| Case | Purpose |
| --- | --- |
| `win_bistro_gtao_on` | Manual exposure, bloom off, GTAO on. Captures `gtao_view_depth`, `gbuffer_normal`, `gtao_raw`, `gtao_visibility`, `final_color` |
| `win_bistro_gtao_off` | Identical with `gtao_enabled: false` |
| `win_bistro_normals` | Identical with the `normals` logical channel (`VKR_RENDER_MODE_NORMAL`) |
| `win_bistro_production` | Production settings: automatic exposure, bloom, GTAO |

```sh
build_release/tools/vkr_harness snapshot \
  --case tools/cases/local/win_bistro_gtao_on.case.json \
  --profile tools/profiles/local-offscreen.json
```

Runs are local, dirty-tree, single-repetition diagnostics. They prove
causation, not performance, and are not authoritative baselines.

## 2. Findings

### 2.1 Two-channel normal maps decode to an inverted shading normal (Windows only)

**Status: resolved. This owned the reported artifact.**

`vkr_texture_select_transcode_target_format()` in
`lib/src/renderer/systems/vkr_texture_system.c` resolves
`VKR_TEXTURE_CLASS_NORMAL_RG` through this ladder:

| Device preference | Order | Channels sampled |
| --- | --- | --- |
| discrete (Windows/AMD) | BC5 → ASTC 4x4 → EAC R11G11 → RGBA8 | **BC5 is RG only** |
| integrated (Apple) | ASTC 4x4 → BC5 → EAC R11G11 → RGBA8 | ASTC 4x4 is RGBA |

Vulkan returns `(R, G, 0, 1)` for a format with two components. Every
normal-map decode in the tree assumes three:

- `vkr_vk_resolve_surface()` in `shaders/vulkan/slang/world/deferred.slang`
  (opaque G-buffer resolve **and** transmission shade share it);
- the forward path in `shaders/vulkan/slang/world/default.slang`;
- `vkr_metal_packet_gbuffer_resolve()` and the transmission resolve in
  `shaders/metal/msl/world/gpu_draws.metal`;
- the forward path in `shaders/metal/msl/world/default.metal`.

All four compute `sampled = tex.xyz * 2 - 1`, scale `xy`, flip green, and then
use `sampled.z` directly as the tangent-space Z. With `b = 0` that is
`z = -1`, so `mapped = T·x + B·y + N·(−1)` and the shading normal is reflected
through the tangent plane.

Two of the four ladder entries (BC5, EAC R11G11) are two-channel, so this is a
property of the selector, not of one GPU. ASTC and the RGBA8 terminal fallback
happen to hide it.

**Measured, at the plaza ground (`800x600`, pixel `(400, 480)`), Bistro
`local.win.bistro.gtao.on`:**

| Signal | Value | Correct value |
| --- | --- | --- |
| `world_normal.y` written by `GBuffer.Resolve` | `-0.71` (byte 37 of `n·0.5+0.5`) | `+1.0` |
| same, with the normal-map branch forced off | `+1.00` (byte 255) | `+1.0` |
| `gtao_visibility` | `8/255 = 0.031` — the `max(0.03, …)` floor | ≈ `0.9` for open ground |
| `final_color` | `(1, 1, 1)` | lit cobblestone |

The `normals` debug channel agrees with the G-buffer read taken from inside
`AO.Evaluate`, so the wrong normal is in `gbuffer_normal` itself, not in GTAO's
consumption of it.

**Evidence chain.**

1. `20260828T114207.021Z-0037fc` — GTAO on. `gtao_visibility` is at the 0.03
   floor across every ground surface; walls read 98–252.
   `sha256:649ff43439eb6a354a7009b987ffa6eadf252b1dca68fb72b0faf0c9d46b1d0e`
2. `20260828T114516.520Z-001aa1` — GTAO off, same frame. Ground renders
   normally. Isolates GTAO as the multiplier, not the source.
   `sha256:256327c7ac12f458addadf45d52408bc59577ce866404cf47652ace8f6d78d0d`
3. `20260828T115037.901Z-0010bc` — `normals` channel. Ground reads magenta
   (`n.y < 0`) instead of green.
4. `20260828T115948.249Z-002a6c` / `20260828T120057.536Z-002311` — `AO.Evaluate`
   instrumented to write `view_normal.y` and then `world_normal.y` into
   `gtao_raw`. Both put the plaza ground at `≈ -0.71`.
5. `20260828T120301.952Z-001ce9` — the normal-map branch forced off. The plaza
   ground reads exactly `+1.0`, a vertical wall reads `0.0`. The geometric
   normal is correct; normal mapping destroys it.
6. `20260828T120500.023Z-001eba` — Z reconstructed from XY, GTAO still on.
   Ground restored; `gtao_visibility` becomes ordinary contact occlusion under
   hedges, planters, and lamp bases.
   `sha256:9f0351496c95a1cd01811d2c3a7083d378b2b221a3d1acb3a5e04ae08d1aae39`
7. `20260828T120836.238Z-002b73` — production settings (automatic exposure,
   bloom, GTAO) with the same change. The Windows frame matches the owner's
   `1_mac` reference in structure and ground brightness.

**Fix.** Reconstruct Z after the XY scale and green flip, in all four decode
sites, in both backends:

```hlsl
sampled.xy *= material.material_surface.z;
sampled.y = -sampled.y;
sampled.z = sqrt(saturate(1.0f - dot(sampled.xy, sampled.xy)));
```

This is correct for every ladder entry, not only the two-channel ones: an
authored tangent-space normal is a unit vector with `z >= 0`, so the
reconstruction reproduces the stored Z within encoding error on ASTC and RGBA8
and supplies the missing one on BC5 and EAC R11G11. It also removes the
selector's channel count from the shading contract, so a future ladder change
cannot silently reintroduce this.

The implementation lives in
`shaders/shared/normal_map_kernel.slangh`. Both backend build pipelines consume
that file before their forward and deferred material shaders, so all five
concrete decode sites use one contract. `texture_vkt_tests.c` pins the
two-channel decode independently of the sampled blue value and retains the
1,024-combination transcode-ladder sweep.

Do not "fix" this by reordering the ladder to prefer ASTC on discrete GPUs.
BC5 is the right normal-map format there; the decode is what is wrong.

### 2.2 The GTAO, bloom, and exposure smoke fixtures render an empty scene on Windows

**Status: resolved. It made the earlier Windows evidence vacuous.**

`smoke.gtao.static`, `smoke.bloom.static`, and `smoke.auto_exposure.static` all
use `assets/scenes/fixtures/local_ibl_broad_mesh.scene.json`. On Windows that
scene rasterizes nothing:

- `20260828T113918.427Z-00233e` — `gtao_view_depth` is **uniformly `100.0`**,
  the case far plane, over all 481,401 texels. No fragment wrote depth.
- `gtao_raw` and `gtao_visibility` have the **same** digest
  (`sha256:371b492536d31d7f4c288349fe577604a894c56570c21e7427e97166570e1df4`)
  and are uniformly `255` — every pixel took the `encoded.x == 0` background
  early-out.
- `gbuffer_normal` and `final_color` are single-valued.

The Metal record for the same case reports view depth over `7.875–100`, raw
visibility `153–255` with 103 distinct values, and denoised `159–255` with 97
distinct values. The fixture renders there.

The exposure case fails its own assertion as a direct consequence:
`20260828T120616.112Z-002625` records
`post.exposure.accepted_texels = 0` across all 32 samples, so the resolve holds
the empty-histogram fallback and the multiplier stays pinned at the `0.30`
manual fallback. That is the correct behaviour for an all-black frame; the
frame is the bug.

This is *not* a general offscreen-target failure —
`smoke.sponza.offscreen_snapshot` (`20260828T114047.247Z-0017aa`) renders 162
distinct luminance values on the same profile. It is specific to this fixture.
`local_ibl_broad_mesh.scene.json` is the only fixture with a single
`"parent": null` entity, no `Root`, no lights, and a procedural `shape` cube;
start there.

The scene was not malformed. The harness began measurement after frontend CPU
asset readiness but before the selected renderer's asset publisher was idle.
It could therefore measure and then destroy the renderer while geometry and
three IBL texture publications were still pending. The child now waits for
`asset_publisher.publications_idle` before measurement. The Vulkan teardown
also cancels pending IBL ownership references before texture initialization is
discarded, so an early exit no longer leaves three live texture placements.

All four Windows cases now carry non-vacuity assertions. Release reports
`20260828T132225.940Z-0029d9`, `20260828T132231.389Z-002de8`,
`20260828T132234.597Z-002d00`, and `20260828T132237.683Z-001c4a` pass with
opaque geometry, zero publication omissions, and distinct exposure, bloom,
depth, raw-AO, denoised-AO, and final-color evidence.

### 2.3 The opaque G-buffer resolve transforms normals by the transposed model matrix

**Status: resolved latent correctness defect.**

`vk_gbuffer_resolve()` in `shaders/vulkan/slang/world/deferred.slang` calls:

```hlsl
mul(float4(surface.object_normal, 0.0f), instance.model)
mul(float4(surface.object_tangent.xyz, 0.0f), instance.model)
```

Slang stores the matrix column-major and loads it transposed, so `mul(v, M)`
lowers to `OpMatrixTimesVector` on the transposed load and computes **Mᵀ·v**.
Confirmed by disassembling `packet.gbuffer_resolve.comp.spv`. Every other site
in the tree computes `M·v`:

- `packet.world.vert.spv` — 4 `OpVectorTimesMatrix`, 0 `OpMatrixTimesVector`;
- `packet.transmission_shade.comp.spv` — 21 / 0, and `deferred.slang`'s
  transmission entry uses the `mul(instance.model, …)` operand order directly;
- Metal's `vkr_metal_packet_gbuffer_resolve()` uses
  `instance.model * float4(object_normal, 0.0)`.

The in-source comment claims the reversed order is required to match retained
raster captures. It names the SPIR-V opcode, not the arithmetic; the opcode
matches and the arithmetic does not.

For a rotation `R` this yields `R⁻¹n` where every other path yields `Rn`.
Bistro instances are translation-dominated, so swapping the operands produced a
**byte-identical** `normals` capture here — the defect is currently invisible.
It will not stay invisible for a scene with rotated instances, and it is a
divergence from Metal on the same packet.

Also note that `Mᵀ` is only the correct normal matrix under uniform scale;
non-uniform instance scale needs `(M⁻¹)ᵀ` on both backends. That is separate,
pre-existing, and shared.

**Fix.** The opaque resolve now uses
`mul(instance.model, float4(..., 0.0f))` for both normal and tangent, matching
Vulkan forward/transmission, Metal, and the host ABI. The rotated deferred
state matrix passes every opaque and transmission bucket with
`visibility.gbuffer.resolve_invalid = 0`; report
`20260828T132439.367Z-00228a`. Its lossless normal capture uses
`RG16_SNORM_LE` channel version 2.

### 2.4 The reported GTAO sampler defect is refuted

**Status: no defect.**

`vkr_vk_gtao_root()` in `vkr_vulkan_deferred.c` initializes
`.point_sampler = VKR_VULKAN_SENTINEL_SLOT_INDEX` and no GTAO record function
ever assigns it. All four GTAO entry points sample through
`g_samplers[root.point_sampler]`, so every GTAO tap uses descriptor slot 0.

This is intentional rather than accidental. The sentinel sampler is created
`VK_FILTER_NEAREST` / `VK_SAMPLER_MIPMAP_MODE_NEAREST` /
`CLAMP_TO_EDGE`, which is exactly Metal's `vkr_metal_gtao_point_sampler`.
Descriptor slot 0 is the renderer-wide documented sentinel contract, and the
root assignment names that slot explicitly. No binding change is required.

### 2.5 The exposure histogram used an index from the wrong descriptor array

**Status: resolved. This owned the reported exposure instability.**

The exposure shader samples its source through
`g_textures[root.source_texture]`. Vulkan descriptor buffers keep sampled-image
and storage-image slots in separate arrays, so `source_texture` must be a
sampled-image index. Before this correction:

- `Post.Exposure.Histogram` declared `temporal_history_color` as
  `STORAGE_READ`;
- `vkr_vk_record_exposure_histogram()` obtained binding 0 through
  `vkr_vk_deferred_storage_index()`; and
- the shader used that number in `g_textures`.

A Release fixed-step Bistro profile with a static camera, 120 warmup frames,
60 measured frames, and two repetitions passed the old non-vacuity assertions
while exposing the defect. Across 120 measured samples, accepted texels ranged
from 35 to 480,000, average log luminance from -2.6193 to 6.5061, target EV
from -8 to 0.1454, and the exposure multiplier from 0.00390625 to 1.1060.
Report digest:
`sha256:aeea1298b6baa9fcffe88d57734a75d29d705410c1cdd3de9f1e892091ad0d36`.

After changing the graph access to `SAMPLED` and selecting binding 0 through
`vkr_vk_deferred_sampled_index()`, the identical Release profile holds exactly
479,910 accepted texels in all 120 samples. Target EV spans less than
`4.77e-7`, adapted EV less than `1.49e-7`, and the multiplier less than
`1.20e-7`. Report digest:
`sha256:7f5d3524b7a02e5c4248e260795074b4b432edac8f501f1a16a2cabe85789f59`.
The production case now asserts the static camera's accepted-texel, target-EV,
and multiplier ranges, while the main-graph CPU contract pins sampled access.

### 2.6 Non-fatal defects observed in passing

| Observation | Where | Note |
| --- | --- | --- |
| `Vulkan memory pool destroyed with 3 live and 0 retired placements` at shutdown | `vkr_vulkan_memory.c` | Resolved by cancelling pending IBL bake references before texture initialization teardown; fixed harness runs have no pool error |
| `gbuffer_normal` capture declared `RG16_SNORM` but stored only an 8-bit preview | capture pipeline | Resolved: channel version 2 writes exact little-endian four-byte-per-texel `RG16_SNORM_LE` raw data plus a separate PNG preview |
| `memory.gpu.heap_usage_valid = 0`, `memory.gpu.heap.*.usage_bytes = 0`, `memory.gpu.driver.current_allocated_bytes = 0` | Vulkan metrics | Driver budget/usage is not populated on this device. Tracked totals are reported; the driver-side cross-check is not |
| 91 render-graph images / **450 MB** at 800x600 offscreen; `render_graph` GPU owner holds **916 MB** of 5.88 GB live | `local.win.bistro.production`, `20260828T120703.016Z-003819` | Not proven to be a defect. It is large enough to be worth an aliasing/instance-count audit before the next resolution increase |
| Repeated `Failed to prepare renderer frame` at the end of the owner's supplied Windows log | `renderer_impl_vulkan_prepare_frame()` | Not reproduced offscreen. `vkr_vulkan_renderer_prepare_frame()` returns `false` for terminal failure, zero-extent recreate, and `VK_ERROR_OUT_OF_DATE_KHR` without distinguishing them, so the log cannot say which. Needs a distinguishable result before it can be diagnosed |

### 2.7 Hypotheses tested and refuted

Recorded so they are not re-derived.

- **The GTAO slice direction has a Y-sign error** (screen UV Y is down, the
  view-space slice vector treats Y as up). A CPU port of the shader against an
  analytic ground plane at the owner's camera gives visibility `0.92–1.00` with
  the current code and `0.03–0.07` with the Y step flipped. The current form is
  the correct one.
- **The AO depth mip chain is uninitialized or mis-bound.** Forcing
  `depth_mip_sampling_offset = 30` pins every tap to mip 0.
  `20260828T115839.508Z-0039a9` is visually unchanged. Not the cause.
- **Slang lays matrices out row-major, so `mul(M, v)` is transposed.**
  `packet.gtao_evaluate.comp.spv` loads
  `_MatrixStorage_float4x4_ColMajornatural`, rebuilds the transpose, and issues
  `OpVectorTimesMatrix`, which is `M·v`. Correct. (The *reversed* operand order
  in §2.3 is a separate, real issue.)
- **The octahedral G-buffer encode/decode differs between backends.** The
  Vulkan and Metal helpers are line-for-line equivalent.
- **`VkrGtaoParams` is mislaid out on Vulkan.** The 192-byte struct maps
  field-for-field in the SPIR-V type, and `AO.PrefilterDepth` produces exactly
  the case far plane for background texels, which requires
  `projection_m22/m23/m32/m33` to be correct.
- **The Mac HUD showing `RG images: 0` and a zeroed GPU-memory breakdown is a
  Windows/Mac data divergence.** `rendergraph.*` gauges are published only when
  `renderer->impl.kind == VKR_RENDERER_IMPL_VULKAN`
  (`vkr_renderer_metrics.c`). The Mac zeros are a Metal metrics gap, unrelated
  to image quality.

## 3. What automatic exposure, bloom, and GTAO do on Windows

With §2.1 and §2.5 corrected, on `local.win.bistro.production` (120 samples):

| Metric | mean | min | max |
| --- | --- | --- | --- |
| `post.exposure.accepted_texels` | 479,910 | 479,910 | 479,910 |
| `post.exposure.target_ev` | -0.1612924 | -0.1612928 | -0.1612923 |
| `post.exposure.adapted_ev` | -0.1612923 | -0.1612925 | -0.1612923 |
| `post.exposure.multiplier` | 0.8942237 | 0.8942236 | 0.8942237 |

The histogram, resolve, and adaptation are temporally stable on this fixed
Vulkan workload. The later matched Metal trace in §3.1 supplies the absolute
comparison; neither static trace reproduces the former Vulkan oscillation.

Bloom and GTAO both execute and produce non-degenerate output on Bistro. No
per-pass GPU timing was collected; nothing here supports a cost claim.

### 3.1 Dark-camera brightness is exposure, not bloom

Owner review rejected the absolute brightness at
`position [-23.6858349, 7.45912027, 8.57657433]`, `yaw 23.0096149`,
`pitch -19.4072094`. One fixed-step Release bloom-on trace holds 479,065
accepted texels, average log luminance `-4.052836895`, target EV
`+1.578905821`, and exposure multiplier `2.987432003..2.987766981` over 60
measured frames. The decision is stable, but it lifts the scene by nearly
three times before tonemapping.

The matching bloom-off run produces the same `hdr_pre_bloom` digest,
`sha256:35e1de31b01ccfbd595aaf985b121725fc2984f6ec0f5b3fe7eef6179b2a80e5`,
and the same target EV and multiplier within floating-point noise. Its final
image removes only local halos. Bloom cannot own the diffuse-surface brightness
or the lantern-core clipping because histogram metering is upstream of bloom.

At the camera recorded by the supplied Metal reference, a 1600x1200 Vulkan run
accepts 1,915,909 texels, resolves average log luminance `-4.136658`, target EV
`+1.662727`, and multiplier `3.166142..3.166150`. A Vulkan manual `1.0x`
control is visibly closer to the supplied Metal image than the automatic
result. This is visual evidence of an absolute mismatch, not proof of which
backend owns it: the screenshots do not contain Metal histogram/HDR data and
were not produced by the current harness workload.

The matched native Metal run is now available. Report
`sha256:d9b3ed168b50250d9600aab4e88772c89c3a3b24895c936ac6ca29c1aecdd5f6`
holds 1,919,880 accepted texels, average log luminance `-3.309451`, target and
adapted EV `+0.835520`, and multiplier `1.784500` over all 60 samples. The
post-fix Vulkan report
`sha256:e32f6f67c453133c503e4fb2c2276a8d0a77028c7c166926719c915c4ef242f7`
holds 1,915,909 accepted texels, `-4.139783`, `+1.665852`, and `3.173010`.
Both are static and stable. Vulkan meters about `0.830 EV` less scene luminance,
and the common resolve consequently adds about `0.830 EV` more exposure.

The graph-to-shader audit still finds no backend-only bloom, exposure, or
tonemap binding difference. Both backends meter `temporal_history_color`, apply
the current exposure state to the selected pre-bloom or combined HDR source,
and use the same metering, bloom, and ACES arithmetic. The remaining divergence
is upstream of exposure.

A Vulkan GTAO-off control, report
`sha256:56ea12c564f3cee34de999265fc4bc255939ae4e0690a4584fb54a9e965f0c2a`,
resolves average log luminance `-3.450421`, target EV `+0.976490`, and multiplier
`1.967672`. That partitions the Vulkan frame but cannot by itself indict GTAO:
the Metal GTAO-off level is not yet known. The paired
`local.mac.bistro.exposure_parity.gtao_off` and
`local.win.bistro.mac_reference.gtao_off` cases are the next acceptance gate.
The GTAO-on cases now additionally capture view depth, raw visibility, denoised
visibility, and exact normals.

One backend-only lighting term was found and removed while tracing the owner's
view-dependent surface brightness report. Vulkan forward and deferred shading
multiplied diffuse IBL by
`1 + directional_visibility * directional_intensity`; Metal does not. This
made an environment contribution depend on the view's sampled directional
shadow. Its removal changes the exact static camera by only about `0.003 EV`, so
it is a real structural correction but not the absolute-HDR root cause. Do not
apply a Vulkan-only exposure compensation or shared metering retune before the
matched GTAO split.

The Bistro source assets and generated material cache are local derived assets.
Before the native Metal case, prepare the exact cooked mesh, generated
specular-glossiness textures, material files, and packed texture siblings:

```sh
./tools/cook_vkr_meshes.sh assets/models/bistro-lights.gltf
```

If `scene_manifest.missing` names a dependency from
`assets/materials/bistro-lights/gltf_mat_*.mt`, the derivative cache is
incomplete and the case has not started the renderer. The `?cs=...` and
`?tc=...` suffixes in material texture references are metadata, not filename
characters; the manifest strips them before filesystem resolution. Keep the
strict failure because running with a missing texture would substitute runtime
fallback content and invalidate a Metal/Vulkan workload comparison.

## 4. Resolution status

1. **Done:** shared positive-Z reconstruction for every Metal and Vulkan
   material decode, with deterministic CPU contract coverage.
2. **Done:** renderer-publication readiness gate, non-vacuity assertions, and
   pending-IBL teardown cancellation.
3. **Done:** opaque Vulkan deferred normal/tangent transform corrected and
   exercised by the rotated state matrix.
4. **Done:** exposure histogram input uses the sampled-image access and index
   contract. Static Release traces are stable and the graph/test contracts pin
   the correction.
5. **Done:** exact `RG16_SNORM_LE` G-buffer normal artifacts.
6. **Refuted:** the GTAO sentinel sampler is an explicit slot-0 contract.
7. **Closed:** a focused Debug synchronization-validation Bistro profile passes
   one complete repetition with zero Vulkan validation messages, report digest
   `sha256:b0cf4ee52a50ebbd5dc8114f7d662d4042a574863a32db63d273d40c116b5bf1`.
   Third-party implicit overlay layers were disabled for isolation. Earlier
   independent focused Debug synchronization-
   validation children pass with no Vulkan diagnostic, report digests
   `sha256:0c34cf08fec72b3e660989de10c441fe28016719afbe8b43ba0cb86496838d97`
   and
   `sha256:fc037520645a577fca23378df4df10d95ddbdc0de4d22914176ebe9d455f9303`.
   The stock two-child
   profile remains incomplete because its second Windows process stops during
   Vulkan loader startup after Galaxy overlay layer discovery, before renderer
   creation. This is not image-quality evidence and needs a harness/loader
   follow-up if two children in one parent are required.
8. **Done:** the exact-camera native Metal exposure case proves both static
   kernels are stable and localizes the `0.830 EV` difference to pre-bloom HDR.
9. **Done:** adaptation now advances by at most the configured EV/s times frame
   delta on both CPU and GPU, instead of treating EV/s as an exponential
   response coefficient. A focused Debug automatic-exposure snapshot passes
   synchronization validation with no diagnostic, report digest
   `sha256:b069c666f4c0bea6e9ccb9f8b640a72881e05224437d2aff47d5e1796243e6d2`.
10. **Done:** Vulkan-only directional-shadow scaling of diffuse IBL was removed
    from forward and deferred shading.
11. **Still open and a parity blocker:** run the matched Metal GTAO-off case and
   compare it with the Vulkan control, then compare the new GTAO depth/raw/final
   and normal captures. Also distinguish `prepare_frame` terminal,
   zero-extent, and out-of-date results; audit the large graph image residency;
   collect authoritative matched performance;
   obtain owner acceptance before replacing any final-color baseline.

## 5. References

- [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md)
- [Image quality roadmap](image-quality-roadmap.md)
- [Renderer architecture specification](../architecture/renderer-architecture-spec.md) §8
- [Texture format and colorspace design](texture-format-and-colorspace-design.md)
