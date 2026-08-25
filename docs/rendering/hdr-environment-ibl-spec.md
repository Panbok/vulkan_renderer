---
status: implemented
updated: 2026-08-04
authority: design
---

# HDR Environment and IBL Implementation Spec

Implementation specification for
[ADR-016](../architecture/adr/016-hdr-environment-format.md)
(equirectangular delivery, cubemap runtime) and for investigation items §4, §5,
and §7 of
[bistro-baseline-shading-investigation.md](bistro-baseline-shading-investigation.md).

Implemented on 2026-08-03, seam-corrected and lighting-calibrated on
2026-08-04. The renderer now
decodes Radiance HDR through a worker-safe prepared-load path, converts a 2:1
source to a full-mip cubemap, bakes half-float irradiance,
specular prefilter, and BRDF products, and presents through an RGBA16F scene
target plus an ACES-fitted tonemap. The default environment is the licensed
4K Citrus Orchard asset recorded in
[`citrus_orchard_puresky_4k.hdr.license.md`](../../assets/textures/citrus_orchard_puresky_4k.hdr.license.md);
the legacy six-face skybox remains the capability/load failure fallback and the
source for local reflection probes.

Asynchronous scene loads run the prepared decoder on resource workers. The
built-in default environment currently calls the same decoder synchronously
during cold renderer initialization; this is a measured-startup optimization
boundary, not work performed by the `IBL.Bake` executor.

The remaining architectural gap is unchanged: `IBL.Bake` records prepared
graphics work whose persistent resources are not declared to the render graph.
Those resources therefore retain explicit access-carrying barriers. Validation
is clean on Apple M1 Pro/MoltenVK; native Vulkan and other format/queue layouts
remain unverified rather than implicitly claimed.

## Scope

In scope:

- one required half-float working format for HDR environment and IBL data;
- float Radiance `.hdr` decode and half-float upload;
- equirectangular-to-cubemap conversion with a complete source mip chain;
- half-float irradiance, specular prefilter, and BRDF LUT storage;
- corrected PDF/solid-angle source-mip selection during specular prefiltering;
- scene schema and runtime state for alternative cubemap/equirect sources; and
- ownership, synchronization, failure, validation, and measurement contracts;
  and
- the HDR scene-color, packet-carried manual-exposure ACES tonemap, and
  exposure-equivalent canonical HDR capture path required to activate the
  default environment without hard clipping; and
- IBL-aware ambient composition: constant ambient is retained only as the
  no-IBL fallback.

Out of scope, tracked elsewhere:

- SH L2 irradiance (investigation §5a), which changes the storage model after
  this work;
- automatic exposure and camera adaptation; and
- BC6H/KTX2 HDR delivery, probe atlases, and offline environment baking.

## Architectural constraints

The existing `IBL.Bake` render-graph entry is a `compute`-kind orchestration
pass whose executor records graphics render passes and owns dynamically named
textures outside the graph. This implementation does not make that work
graph-authoritative. Until the P1 graph-integration gap is closed:

- every upload, color write, and later shader read needs an explicit
  access-carrying barrier;
- the executor must not claim graph ownership or rely on graph scheduling for
  resources it has not declared; and
- the architecture status must continue to describe IBL bake work as
  undeclared.

Before this work, the cold path lazily created pipelines, render passes,
writable cubemaps, and per-face render targets while a frame was being encoded.
The implemented path moves format selection, pipeline/render-pass creation,
image allocation, and face/mip target creation to initialization or resource
finalization. Finalization records non-blocking transitions into the active
frame; the `IBL.Bake` executor only binds prepared state and draws.

Other non-negotiable constraints:

- Vulkan formats and feature bits stay behind `lib/src/renderer/vulkan/`;
  frontend code receives immutable typed capabilities.
- No unsupported HDR format silently falls back to UNORM. The scene uses the
  existing LDR fallback and reports the environment bake as failed.
- Logical release invalidates a handle immediately; image, sampler, view, and
  staging reuse waits for the submit serial/fence that proves the final GPU use
  complete.
- A replacement environment becomes active only after every derived product is
  ready. Failure keeps the previous/fallback environment active and releases
  partial products.

## Why the order is mandatory

Loading HDR radiance before widening the storage chain would discard the range
at the first convolution target. Activating it before tonemapping would then
hard-clip the final sRGB target, and importance sampling from source mip 0 would
make bright texels firefly-prone.

The implementation preserved this order and activated the default HDR source
only after the HDR scene-color and tonemap path existed. That activation
intentionally changes production pixels; the legacy baseline remains guarded
by the harness review workflow.

---

## Phase 1 — One HDR working format and exact capabilities

Add one initial format to `VkrTextureFormat` in
`lib/src/renderer/vkr_renderer.h`:

| Enum | Vulkan format | Initial uses |
|---|---|---|
| `VKR_TEXTURE_FORMAT_R16G16B16A16_SFLOAT` | `VK_FORMAT_R16G16B16A16_SFLOAT` | Equirect upload, environment cubemap, irradiance, prefilter, BRDF LUT |

Do not add `R16G16_SFLOAT` or `B10G11R11_UFLOAT_PACK32` in this phase. The
BRDF LUT and 64² irradiance map are too small for those variants to recover
meaningful memory, while a different attachment format requires a compatible
render pass and pipeline. One RGBA16F path removes that state combination.
Packed float and BC6H remain measured follow-ups.

### Format metadata and lowering

The implementation surface is larger than four switches. Raw
`width * height * channels` upload-size arithmetic was wrong for every
multi-byte channel format, so one non-Vulkan metadata source now defines channel
count, block extent, and bytes per block for the backend, texture system, and
render-graph statistics. Vulkan lowering remains backend-local.

The affected surface is:

| Location | Required change |
|---|---|
| `vkr_renderer.h` | enum value |
| `vulkan_utils.c` | `VkrTextureFormat` → `VkFormat` |
| `vulkan_backend.c` | channel count, bytes per block/texel, and `VkFormat` → `VkrTextureFormat` |
| `vkr_texture_system.c` | frontend channel metadata; HDR must remain deliberately unsupported by the KTX/UASTC transcode table |
| `vkr_rg_compile.c` | bytes-per-pixel resource accounting (not “channel count”) |
| `vkr_rg_json.c` | stable JSON name if/when the format is graph-authored |
| texture creation/upload paths | replace raw `channels` byte-size arithmetic with format metadata and validate every payload region size |
| `tests/src/texture_format_tests.c` | forward/reverse mapping and byte-size cases |

Activation graph-authors an RGBA16F scene-color image, so
`VKR_CAPTURE_MAX_BYTES_PER_PIXEL` is eight. Scene-color capture reads all eight
source bytes per pixel and canonically applies the same ACES-fitted curve plus
linear-to-sRGB conversion before writing RGBA8 PNG; it never truncates the
half-float payload.

### Exact device support

Do not state that a feature combination is universal. At physical-device
selection, query both format features and exact image-creation support for:

- a 2D optimal-tiled `SAMPLED | TRANSFER_DST` RGBA16F source with linear
  sampling and one mip; and
- a cube-compatible optimal-tiled `COLOR_ATTACHMENT | SAMPLED` RGBA16F image
  with six layers, linear/trilinear sampling, and the requested mip count.

Use `vkGetPhysicalDeviceFormatProperties` for feature bits and
`vkGetPhysicalDeviceImageFormatProperties` for the real usage/create-flag
combination and limits. `STORAGE` is not used by the graphics bake and must not
be required. Publish only immutable typed results such as
`supports_hdr_ibl`, `hdr_ibl_max_cube_extent`, and
`hdr_ibl_max_mip_levels` through `VkrDeviceInformation`.

When the required combination is unavailable, log one capability failure,
mark the requested environment failed, and keep the LDR fallback. Do not create
an 8-bit target under an HDR handle.

### Render-pass compatibility

The previous `VKR_WORLD_RESOURCES_IBL_RENDERPASS_NAME` signature described an
`R8G8B8A8_UNORM` attachment, which was incompatible with half-float targets.
The IBL render-pass signature and all bake pipelines are now prebuilt for
RGBA16F before frame recording. Per-target format branching remains unnecessary
because every initial bake output uses the same format.

**Phase-1 gate:** mapping/metadata tests, `./build_test.sh`, an explicit
cold/warm production cache run, and a validation-layer run that creates,
attaches, samples, and destroys the half-float resources.

## Phase 2 — Widen IBL storage and replace the 8-bit LUT

`vkr_world_resources_create_writable_cube_texture()` in
`lib/src/renderer/systems/vkr_world_resources.c` now takes an explicit format.
The 64² irradiance cubemap and 256² full-mip specular prefilter pass RGBA16F and
validate that format against the prebuilt render-pass signature before
recording.

`assets/textures/ibl_brdf_lut.png` is a 128×128, 8-bit colormapped PNG. The PBR
shader consumes only `.rg`, but the general texture loader expands it to an
RGBA8 texture. Replace it with a one-time graphics bake into a persistent
128×128 RGBA16F target; `.rg` carries the DFG terms and `.ba` are unused. Do not
describe this as a compute pass: real compute dispatch is not exercised in the
current renderer. Its shader, shadercfg, render target, and pipeline are
prebuilt through the same RGBA16F-compatible IBL render pass.

Approximate device memory for this initial policy is:

| Resource | Approximate size |
|---|---:|
| 64² RGBA16F irradiance cube | 0.19 MiB |
| 256² RGBA16F prefilter cube with full mips | 4.0 MiB |
| 128² RGBA16F BRDF LUT | 0.125 MiB |

Changing storage precision is expected to reduce quantization bands, but
“banding is gone” is a validation result, not a design guarantee. Compare the
same Bistro views before/after, report the capture metrics, and leave flat
lighting directionality to the later HDR-source and analytic-light work.

## Phase 3 — Float `.hdr` decode and prepared upload

`vendor/stb_image.h` exposes `stbi_is_hdr_from_memory()` and
`stbi_loadf_from_memory()` for Radiance RGBE. Integrate the HDR branch into the
worker-side prepared-load path rather than decoding or uploading from the IBL
executor.

Requirements:

1. Identify HDR from content, not only the extension. This phase accepts
   Radiance `.hdr`; stb does not add EXR support.
2. Request three float channels from stb, validate positive dimensions and a
   2:1 equirectangular aspect, and reject non-finite samples.
3. Do **not** vertically flip environment images. The normal 2D texture path
   flips to VKR's bottom-left material-UV convention, but the equirect mapping
   uses `+Y → v=0`; reusing that policy turns the sky upside down.
4. Convert worker-side float RGB to IEEE-754 binary16 RGBA with alpha 1. Use a
   tested round-to-nearest conversion. Clamp radiance below zero to zero and
   above 65504 to 65504, then emit one warning per texture containing the
   clamped count and observed finite min/max. Never silently create infinities.
5. Return an explicit prepared upload payload with RGBA16F format and byte
   sizes. Do not pass it through a path that assumes one byte per channel.
6. Upload the equirect source with exactly one mip. Conversion always samples
   it with explicit LOD 0, so generating unused source mips adds work and can
   introduce seam filtering.
7. Bypass legacy raw `.vkt` cache read/write for HDR. Its version-3 header and
   payload describe 8-bit channels. KTX2/UASTC HDR is deferred until the cache
   format can represent the GPU format and bytes per block.

For a 4096×2048 source, an RGB32F decode is 96 MiB and the RGBA16F prepared
payload is 64 MiB, before staging and the derived cubemap. Both CPU buffers are
temporary: the stb buffer is freed after conversion, the prepared payload after
upload recording, and staging after proven submission completion. Allocation
failure and every decode/validation error must take the same cleanup path.

The equirect GPU handle stays alive through conversion. It may be logically
released after the conversion commands and their color-write→sample barrier are
recorded only if backend physical destruction is stamped with that frame's
submit serial; cancellation and failed submission must still preserve the
resource until completion/recovery proves it safe.

## Phase 4 — Source mip chain and prefilter LOD

The previous `ibl/specular_prefilter.slang` called `TextureCube.Sample()` inside
an importance-sampling loop. Derivatives did not describe those generated
sample directions, so the result effectively sampled source mip 0. The
implemented path supplies a complete source cubemap mip chain and selects
explicit LOD from the reflected-light PDF.

### Source mip contract

- The derived HDR environment cubemap has
  `floor(log2(face_size)) + 1` initialized mips. The conversion shader renders
  every face of every mip.
- The legacy six-face LDR loader currently creates one mip. Either extend its
  upload path to generate all six layers' mips when linear blits are supported,
  or pass `source_mip_count = 1`; an explicit LOD then clamps to zero and no
  improvement may be claimed for that source.
- The prefilter source sampler uses linear texel filtering and linear mip
  filtering. The output prefilter mips continue to encode material roughness.

Record `source_face_size` and `source_mip_count` with the per-draw bake push
constants. Compute the light-direction PDF, not only the half-vector density:

```text
D       = D_GGX(NoH, alpha)
pdf     = D * NoH / max(4 * VoH, epsilon)
omega_s = 1 / (sample_count * max(pdf, epsilon))
omega_p = 4 * PI / (6 * source_width * source_height)
lod     = 0.5 * log2(K * omega_s / omega_p)
```

Use `K = 4` as the initial prefiltered-importance-sampling calibration, matching
the derivation in
[Filament's IBL documentation](https://google.github.io/filament/Filament.md.html#annex/importance-sampling-for-the-ibl/pre-filtered-importance-sampling).
Clamp LOD to `[0, source_mip_count - 1]`, force zero for effectively smooth
surfaces, and call `SampleLevel`. A collapsed equation that omits `omega_p` (or
its π term) is not equivalent.

Add deterministic tests for the PDF/LOD helper at smooth, mid-roughness, and
grazing cases, including finite/clamped output. Image validation must still
check high-intensity point sources at several roughness levels; a numerically
valid LOD does not prove fireflies are acceptably bounded.

## Phase 5 — Equirectangular-to-cubemap conversion

### Prepared bake state

`vkr_world_resources_bake_cubemap()` already recorded a face×mip draw loop, but
previously created/destroyed a render target per iteration and hard-coded the
`source_cubemap` binding. It is now driven by a prepared bake job containing:

- prebuilt shader/pipeline/instance state and the reflected source binding;
- source and target handles plus target format;
- base face size and mip count;
- face/mip render targets or attachment views prepared before recording; and
- optional roughness/source-mip parameters.

Prepared targets are one-shot recording state. After their bake draw commands
are recorded they are logically destroyed immediately; Vulkan framebuffer,
view, and wrapper destruction remains deferred to the submit serial that proves
the recorded GPU use complete. Failed jobs release both prepared targets and
all partial texture products.

The opaque source handle does not require a Vulkan descriptor-type switch in
frontend code. The equirect shader reflects `Texture2D`, while convolution
shaders reflect `TextureCube`; their prebuilt pipeline layouts own that
difference. The recorder only sets the job's named source binding.

Equirect→cube is a third prepared job. It runs before diffuse and specular
convolution in `IBL.Bake`. Preserve explicit barriers for:

1. equirect upload transfer-write → fragment sampled read;
2. environment-cube color writes → convolution/skybox sampled reads; and
3. irradiance/prefilter color writes → material sampled reads.

Each barrier covers the initialized mip/layer range. The graph cannot infer
these hazards while the resources remain undeclared.

### Face size and mip count

For a valid 2:1 source:

```text
raw_size  = max(1, equirect_width / 4)
limit     = min(hdr_ibl_max_cube_extent, exact_format_max_extent)
face_size = highest_power_of_two_not_greater_than(min(raw_size, limit))
mip_count = floor(log2(face_size)) + 1
```

Rounding down avoids inventing source detail. A 4096×2048 source therefore
produces 1024² faces and an eleven-mip environment cube. At RGBA16F that cube is
approximately 64 MiB (67 MB decimal).

### Shader: `lib/src/renderer/vulkan/shaders/ibl/equirect_to_cube.slang`

Render the prepared fullscreen plane and reconstruct a direction from the
destination face index and top-left image-space UV. Do not derive the mapping
from capture view/projection matrices: that makes the destination convention
implicit and allows projection-sign changes to rotate or mirror individual
faces.

Destination array layers use Vulkan's cube order `+X, -X, +Y, -Y, +Z, -Z` and
the following unnormalized directions, where `s = 2u - 1` and `t = 2v - 1`:

| Layer | Direction |
|---:|---|
| 0 (`+X`) | `( 1, -t, -s)` |
| 1 (`-X`) | `(-1, -t,  s)` |
| 2 (`+Y`) | `( s,  1,  t)` |
| 3 (`-Y`) | `( s, -1, -t)` |
| 4 (`+Z`) | `( s, -t,  1)` |
| 5 (`-Z`) | `(-s, -t, -1)` |

The equirectangular lookup remains:

```slang
float3 d = normalize(input.direction);
float2 uv = float2(atan2(d.z, d.x) * (0.5f / PI) + 0.5f,
                   acos(clamp(d.y, -1.0f, 1.0f)) / PI);
```

Requirements:

1. Keep the CPU test helper and the shared Slang helper on the same explicit
   face convention. Test all twelve shared edges in both traversal directions,
   including the eight corners; cardinal centers alone do not detect edge
   reversal.
2. Record the face index and specular mip parameters through push constants.
   A descriptor-backed global or instance UBO slice is shared by every bake
   draw recorded for that frame/instance and is overwritten by later loop
   iterations before GPU execution.
3. Sample with `SampleLevel(..., 0.0f)`. Implicit equirect U derivatives jump
   at the ±180° seam and select an unrelated mip.
4. The source sampler uses U=`REPEAT`, V=`CLAMP_TO_EDGE`, no anisotropy, and no
   mip filtering. Do not reuse the cubemap target's all-clamp sampler.
5. Render every destination mip. Use a deterministic stratified footprint
   filter (initially 4×4) derived from destination-direction `ddx`/`ddy`, not
   arbitrary offsets in equirect UV. Measure bake cost before changing the
   sample count.
6. Validate orientation with known cardinal markers as well as a mathematical
   round trip: `+Y → v=0`, `-Y → v=1`, `+X → u=0.5`, and the `-X` direction
   straddles the wrapped seam. A direction round trip alone can still preserve
   a globally mirrored or rotated environment.
7. Keep source textures/samplers in descriptor set 1. Reflection fallback must
   classify a set from its numeric index, not its position in the compact
   reflected-set array; a draw-only shader legitimately has set 1 with no set
   0 resources.

### Scene schema and runtime ownership

The scene environment accepts exactly one enabled source:

```json
"environment": {
  "equirect": "assets/textures/<licensed-environment>.hdr"
}
```

The existing form remains valid:

```json
"environment": {
  "cubemap": { "base_path": "assets/textures/skybox", "extension": "jpg" }
}
```

A prepacked UASTC/Basis cubemap may be prepared on the scene worker and
published directly:

```json
"environment": {
  "cubemap": {
    "path": "assets/textures/environment.vkt?cs=linear&tc=color_linear"
  }
}
```

`cubemap.path` and the `base_path`/`extension` pair are mutually exclusive.
The direct file must decode as `VKR_TEXTURE_TYPE_CUBE_MAP`; 2D arrays and
cubemap arrays remain valid generic texture resources but are not environment
sources. The target-transcode cache retains all six face/mip regions.

If `enabled` is true, exactly one of `cubemap` or `equirect` must be present.
Both together are an invalid environment block: warn with both field names,
disable that custom environment, and use fallback IBL. Do not silently prefer
one source. Add a tagged `SceneEnvironmentImport` source kind so the invalid
combination cannot survive parsing.

The runtime state must distinguish at least:

```text
NONE → SOURCE_LOADING → CUBE_PENDING → CONVOLUTION_PENDING → READY
                                                    ↘ FAILED
```

Legacy cubemaps enter at `CONVOLUTION_PENDING`. Equirect sources own a temporary
2D delivery handle while `CUBE_PENDING`; successful conversion produces the
scene-owned persistent `source_cubemap`, then convolution produces the
scene-owned irradiance and prefilter handles. The source cubemap remains for the
skybox and future convolution. The temporary equirect is retired after proven
GPU completion and reloaded on an explicit rebake. Scene reset/unload releases
every handle on every partial state.

Local reflection probes remain cubemap-sourced in this implementation.

### Tonemap activation boundary

The conversion path can be validated with a dedicated HDR scene and diagnostic
exposure, but the default/Bistro final-color baseline must not switch to the HDR
environment until HDR scene color and tonemapping land. Otherwise values above
1 hard-clip at the `bgra8_srgb` target and the captured image is not evidence of
correct HDR presentation.

The implemented presentation contract carries `VkrFrameGlobals.exposure`
through render-packet version 3. Validation rejects non-finite or negative
values, the default is `0.30`, and the fullscreen, editor-viewport, and
canonical HDR-capture paths use the same scalar before the ACES-fitted curve.
Exposure changes presentation; environment intensity changes incident
radiance. They are deliberately separate controls. Bistro's authored sun is
calibrated from `3.0` to `0.75` so the analytic key does not mask HDR
environment tuning.

---

## Sequencing and checkpoints

| Phase | Observable checkpoint |
|---|---|
| 1 — Format/capabilities | Mapping and byte-size tests pass; exact RGBA16F image combinations create, attach, sample, and destroy under validation |
| 2 — Storage/LUT | Same-view Bistro comparison records the quantization change; no format/signature errors; load→unload returns handle counts |
| 3 — Float decode | A licensed or generated `.hdr` fixture preserves known finite values; vertical orientation, clamp warning, cache bypass, and cleanup tests pass |
| 4 — Prefilter LOD | Source mips are initialized; numeric PDF/LOD tests pass; roughness sweep shows bounded bright-source artifacts |
| 5 — Conversion/schema | Dedicated HDR scene produces correctly oriented, seam-safe cubemap mips; fallback/partial-failure paths are clean; the production default switches only through the active tonemap path |

No checkpoint is a performance result. Any claim about bake, load, or frame
cost needs the Release evidence below.

## Validation and evidence gates

### Deterministic CPU tests

- forward/reverse format mapping, channel count, bytes per texel/block, and
  payload-region size rejection;
- half conversion at zero, subnormal/normal boundaries, maximum finite,
  over-range, negative, infinity, and NaN inputs;
- 2:1 validation, face-size clamp/round-down, and mip-count derivation;
- scene-source tagged parsing, including missing, both-present, disabled, and
  legacy cubemap cases;
- cardinal direction↔UV mapping, all twelve cubemap edges/corners, seam
  equivalence, and direction round trip away from the pole's intentionally
  undefined longitude; and
- PDF/source-LOD reference cases and finite clamping.

Run the focused tests and `./build_test.sh`.

### Shader/pipeline and Vulkan gates

Run two normal application processes against one fresh explicit pipeline-cache
path for every new shader or compatible pipeline signature: the first must
initialize and save, and the second must load and save. Run Vulkan validation
layers through source upload,
full-mip conversion, both convolutions, BRDF bake, skybox/material sampling,
failure cleanup, scene reload, and shutdown. A green CPU suite does not cover
descriptor types, attachment formats, barriers, subresource ranges, or deferred
destruction.

Record the actual GPU/driver, target kind and image count. MoltenVK alone leaves
native Vulkan and other format/queue layouts unverified.

### Harness and baseline safety

`tools/cases/smoke/bistro_snapshot.case.json` is the backend-pinned legacy
Vulkan fourteen-view Bistro-plus-text fixture; its Metal mate is
`tools/cases/smoke/bistro_metal_text_snapshot.case.json`. The implemented
storage, environment, and tonemap changes intentionally change their pixels.
Run a fresh same-backend snapshot with
`tools/profiles/local-offscreen.json`, inspect every comparison/diff and
effective configuration, then create a baseline proposal with
`vkr_harness baseline propose`.

Do not run `baseline accept` as an implementation step. Baseline promotion
requires explicit owner authorization and the reviewed proposal digest. The
dedicated HDR-environment case validates HDR scene color and presentation; the
Bistro proposal records the intentional production-pixel change separately.

## Implementation evidence

- `./build_test.sh`, `./build.sh Debug`, `./build_release.sh`, and the historical
  Vulkan 1.2 pipeline-cache/threading gates passed at implementation time. Those
  legacy backend gates were retired with V7; current validation uses focused
  selected-implementation harness cases.
- Debug offscreen snapshot
  `20260803T205636.755Z-01405b` passes on Apple M1 Pro/MoltenVK with no
  VUID/error/fatal diagnostics. It captures `hdr_scene_color` from
  `R16G16B16A16_SFLOAT` through the canonical ACES path and reports both local
  probes ready.
- The five-repetition Release boot report
  `20260803T205253.704Z-013cad` passes with exact GPU totals and zero upload
  fence, queue-idle, or device-idle waits during measured frames. It is
  explicitly non-authoritative (`profile.local_only`, dirty provenance, and
  unstable warmup), has GPU timing disabled, and supports no speed claim.
- The five-view Bistro snapshot intentionally differs from its accepted
  pre-HDR baseline. The pre-seam proposal `20260803T210304.992Z-014b56` is
  obsolete and has not been accepted. The final calibrated local snapshot is
  `20260804T085151.570Z-00b294`, report digest
  `sha256:8ebe00c3a2c596482049b9dbdb0be523cbd5790b60d2df65675e233c57bab678`.
  All five replays completed without VUID/error/fatal diagnostics; bright-luma
  coverage fell from `6.44%-21.38%` in the uncalibrated HDR run to
  `0.05%-1.05%`. Its comparison failure is expected against the guarded
  pre-HDR generation, and no baseline was proposed or accepted.

### Lifetime, memory, and performance evidence

- Repeat load → bake → unload and confirm texture handles, pipelines, render
  targets, CPU allocator totals, and exact GPU owner totals return to the
  explained baseline after completion.
- Record peak CPU prepared/decode bytes, peak GPU bytes by owner, staging bytes,
  and whether live totals were exact.
- Assert zero render-thread fence/queue/device waits during resource finalization
  and bake recording. Any unavoidable cold-path wait must be named and measured,
  not hidden in the executor.
- Add stable pre-registered decode/conversion/convolution metrics and a dedicated
  HDR boot case. Use the Release boot profile with independent repetitions and
  report configuration, spread, report digest, authority reasons, and what was
  not measured. A Debug number or one process is an observation only.

## Deferred decisions and revisit triggers

- **Packed/BC6H storage:** consider `B10G11R11_UFLOAT_PACK32` only when measured
  resident memory justifies an additional render-pass/pipeline format variant.
  Prefer BC6H once the KTX2 pipeline has an HDR path.
- **Graph authority:** migrate dynamic persistent IBL resources and bake passes
  into declared graph resources when the graph gains the required import/
  lifetime model. Until then the explicit barriers above are mandatory.
- **Offline conversion:** move equirect→cube offline when authoritative boot
  evidence shows the runtime conversion misses its budget.
- **Source retention:** the default is release-after-completion and reload on
  explicit rebake. Revisit only if runtime environment rebaking becomes a
  measured use case.
- **Sun alignment:** analytic sun direction and HDRI bright-region alignment
  remain a scene-authoring/lighting decision. Do not infer a directional light
  from the environment without a separate accepted design.
- **Interior visibility:** Bistro still uses one outdoor environment globally.
  Existing reflection-probe candidates are selected once per draw from the
  object transform, so the broad Bistro mesh cannot reliably separate room and
  exterior fragments. A fragment-aware influence/visibility seam plus an
  authored indoor probe is required before claiming localized indoor IBL.
