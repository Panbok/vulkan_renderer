---
status: partial
updated: 2026-08-29
authority: design
---

# SH L2 diffuse irradiance implementation spec

Implementation specification for
[ADR-038](../architecture/adr/038-sh-l2-diffuse-irradiance.md): replace the
baked 64² diffuse-response cubemap with second-order spherical-harmonic
coefficients evaluated in deferred lighting.

**The final production implementation exists; acceptance evidence is partial.**
Packet version 23 carries SH slots, both selected backends project and evaluate
the shared L2 contract, and the diffuse-cubemap representation and temporary A/B
ABI are retired. Measurements and captures explicitly marked open below are
future gates, not completed evidence.

The preceding cubemap representation and its rationale are documented by
[hdr-environment-ibl-spec.md](hdr-environment-ibl-spec.md) and
[ADR-016](../architecture/adr/016-hdr-environment-format.md). ADR-016 remains
authoritative for equirectangular delivery, source cubemaps, skybox sampling,
and specular prefiltering; its diffuse-cubemap portion is historical.

## Implementation status

Implemented on 2026-08-29:

- shared CPU/GPU projection, packing, windowing, and evaluation math;
- a 37-slot, 4,144-byte renderer-owned coefficient pool with an immutable black
  sentinel and completion-gated publication, reader tracking, retirement, and
  reuse;
- selected-mip deterministic projection and final deferred-lighting evaluation
  on Metal and Vulkan;
- scene `sh_deringing`, environment-source aliasing, and final packet-version-23
  frame/probe records;
- the `indirect_diffuse` capture mode and four final-path probe-count fixtures;
- retirement of diffuse cubemap allocation, baking, descriptors, handles,
  shaders, pipelines, representation selection, and temporary A/B records.

Evidence retained in this change:

- `./build_test.sh`: exit 0, 551 tests;
- `./build.sh Debug` and `./build_release.sh`: exit 0, including both shader
  libraries;
- Vulkan projects through a lazily published `Texture2DArray` view of the
  source cubemap, so integer face/texel/mip loads match Metal `read()` and the
  CPU reference without emitting invalid cube-image `OpImageFetch`. Pipeline
  creation reflects every field and the 48-byte extent of
  `VkrVulkanIblShRoot` from the compiled SH entry point;
- one exclusive Metal API/GPU shader-validation state-matrix run: exit 0, all
  11 assertions passed, no validation diagnostic, report digest
  `sha256:101be997281ecc0c81cf66984ceb2cf8c0ae0e132ae90ec0bcf7068fd7c6ded0`;
- focused `local.sh_ibl.single_probe_validation` under the same exclusive Metal
  validation profile: exit 0, exactly one packed probe in all six measured
  frames, no validation diagnostic, report digest
  `sha256:13d8ab1fe4b2a2533fc5f72130ba9115a30bffef7b0914fb424aca8f73ed3e41`;
- normal Debug application cold and warm launches against one fresh explicit
  Metal pipeline archive: both exit 0; archive size 2,128,224 bytes.
- focused RX 6700 XT Vulkan API plus synchronization validation: two complete
  repetitions, exactly one packed probe in every measured frame, no
  VUID/error/fatal diagnostics, report digest
  `sha256:72302ea31066819c54e36e488302467a4609c1cd9b75ac0c8d327ee070d1b815`;
- normal Release Vulkan cold and warm launches against one fresh explicit
  pipeline cache: both exit 0; the cache is 342,484 bytes and remains
  `sha256:60b58bb30b41e216b8f4142fc51bff3a53b46f80ab965cda19d9e56b7825c26c`;
- one backend-neutral Release `indirect_diffuse` case on Vulkan captures three
  visible 640x480 frames with identical data digest
  `sha256:a5b95003b45846451508acb29b481e6bd013945daa69b6075c8737b7aac1c64a`;
  the passing local report digest is
  `sha256:b38e590ac8358d6ac61462109ae014bd59c6e6578374355b40950cea1f0d18c7`.
  The same clean Release case on an Apple M1 Pro / Metal 4 GPU passes with
  report digest
  `sha256:6e8431ecff0626fedb99104236941f0f4546fa0fef7a6ab1cf07c47a0eaa6451`.
  Its three captures are byte-identical at
  `sha256:183eed0d3d791e410cbfa00876aa80556aecf4135480dc538f2f705e5c0c78b1`;
  each has 114,572 non-black pixels, 8 distinct RGBA values, and 111,384 pixels
  at the dominant foreground value `(152, 157, 165, 255)`. The Metal and Vulkan
  payload digests differ. The Metal payload is retained in portable generation
  `sha256:7492b6406ad11123e0cb5f0f943f5c74bd908e3f72b13750c1a8fd1196f6e726`.
  The historical Vulkan PNG was not retained, so the configured `2/255`
  maximum delta, `0.1/255` mean error, and `0.001` failed-pixel ratio remain
  unevaluated until the next Windows cross-backend snapshot;
- clean Release `sh_ibl_probe_sweep_16_sh_l2` profile: five stable 300-frame
  repetitions, exactly 16 packed probes throughout, authoritative report digest
  `sha256:9d13f1159a8c813bb1c4865725d1e05fceea23156aa6538053058867ef1a9589`;
  deferred-lighting GPU time was 1.15275 ms p50, 1.14742 ms mean, and 0.20832 ms
  standard deviation across 1,500 samples.

Still open: deterministic GPU repetition of the CPU projection fixtures, a
retained numeric comparison of the native `indirect_diffuse` payloads, owner
review of the café probe and clamp/energy diagnostics, submitted-frame
reload/lifetime stress, the remaining 0/1/4-probe performance cases, and a
valid comparative performance result. macOS builds the Vulkan sources but
cannot run this repository's descriptor-buffer backend. The two native capture
reports establish deterministic execution, but digest inequality alone cannot
evaluate the authored image tolerances.

## Scope

In scope:

- shared CPU/GPU L2 projection, packing, and evaluation arithmetic;
- a renderer-owned, completion-safe SH coefficient pool;
- one deterministic projection kernel per backend;
- explicit windowing and non-negativity policies;
- the historical temporary dual-representation ABI and the final packet ABI
  migration;
- an indirect-diffuse replay capture for quality comparison;
- retirement of diffuse cubemap resources, with the actual pre-evidence
  sequencing gap recorded below;
- cold/warm cache, scene-reload, and backend validation evidence.

Out of scope:

- declaring IBL bake work to the render graph, which remains a known gap in
  `renderer-architecture-spec.md` §4 and
  [render-graph-design.md](render-graph-design.md);
- specular prefilter, skybox, and environment source cubemaps;
- probe-volume or ray-traced diffuse GI; and
- offline SH projection in the asset pipeline.

## 1. Data contract

### 1.1 Stored signal and basis

The stored coefficients reconstruct the current shader-facing diffuse response

```text
D(n) = E(n) / pi
```

not raw irradiance `E`. Projection folds the normalized clamped-cosine transfer
factors into the radiance coefficients:

| Band | Stored transfer factor |
|---|---:|
| `l = 0` | `K0 = 1` |
| `l = 1` | `K1 = 2/3` |
| `l = 2` | `K2 = 1/4` |

These are the raw irradiance factors `pi`, `2pi/3`, and `pi/4` divided by
`pi`. Shading continues to multiply the evaluated response directly by diffuse
albedo. A constant source radiance `L` must therefore evaluate to `L`, not
`pi * L`.

The real SH basis, coefficient ordering, signs, and cubemap face-to-direction
mapping live in shared `vkr_ibl_math.c` definitions mirrored exactly by both
shaders. Axis-direction tests are normative; prose names are not a second
authority for signs or coordinate handedness.

Nine RGB coefficients are packed in Sloan's shader-optimized form as seven
`float4` values, 112 bytes per slot:

```text
cAr, cAg, cAb   linear term, dotted with float4(n, 1)
cBr, cBg, cBb   quadratic term, dotted with n.xyzz * n.yzzx
cC              rgb term scaled by n.x*n.x - n.y*n.y; w is zero
```

The normal-derived operands are computed once per pixel and reused for the
global environment and all probes. `cC.w` is padding, is written as zero, and
cannot acquire a meaning without an ABI revision.

### 1.2 Slot-pool layout

The renderer creates one fixed-size storage buffer:

```text
VKR_SH_SLOT_BLACK       = 0
VKR_SH_LOGICAL_MAX      = 2 + VKR_SCENE_REFLECTION_PROBE_MAX = 18
VKR_SH_GENERATION_COUNT = 2
VKR_SH_REUSABLE_SLOTS   = 36
VKR_SH_SLOT_CAPACITY    = 37
VKR_SH_BUFFER_BYTES     = 37 * 112 = 4,144
```

The two non-probe logical sources are the retained fallback environment and the
active scene environment. Slot 0 is zeroed once at renderer creation and is
never rewritten or retired. It is the valid black fallback for missing or
failed projection.

The remaining 36 slots form a bounded copy-on-write pool. Capacity supports an
old and a replacement generation for every maximum-live logical source. This is
a capacity argument, not permission to assume two frames in flight. Reuse still
depends on the actual last-reader submit serial.

### 1.3 Slot state and publication

Reusable slots have explicit cold-path state:

```text
FREE -> RESERVED -> RECORDED -> PUBLISHED -> RETIRED -> FREE
                     |              |
                     +-> ABANDONED  +-> RETIRED
```

The owner follows these rules:

1. Projection reserves a `FREE` slot without waiting. If none exists, the bake
   reports pool exhaustion and retains the prior published slot, or black for a
   source with no prior result.
2. Recording writes only the reserved slot. A compute-write to
   compute-and-fragment-read barrier precedes any lighting read in the same
   submission.
3. A frame that consumes the new projection may reference the candidate only
   when bake recording and its barrier precede lighting in that command stream.
   The logical source commits the candidate after successful queue submission.
   Recording or submission failure abandons it and leaves the old publication
   unchanged.
4. Every submitted packet registers its referenced slots against that submit
   serial. Replacing, disabling, or unloading a logical source moves its old
   slot to `RETIRED`; it does not clear the bytes.
5. A retired slot returns to `FREE` only after proven completion of its greatest
   reader serial. Use the existing completion-gated slot-table pattern. No
   assumed frame lag and no successful-frame wait are allowed.
6. An abandoned pre-submit slot returns to `FREE` only after the command buffer
   that recorded it is reset or submission failure proves the GPU accepted no
   work. A submitted slot follows normal retirement even if later publication
   fails.
7. Destroying the renderer first waits through the renderer's normal device
   teardown. Scene reset only retires scene publications. The pool itself has
   renderer lifetime and survives scene reload.

The pool exposes cold-path counters for free, reserved, published, and retired
slots, plus exhaustion count. They are used by the reload gate and are not read
or formatted in the per-pixel or per-draw path.

### 1.4 Source aliasing and authored control

An independently authored environment or probe cubemap owns one published SH
slot and one `sh_deringing` value. A probe with
`uses_scene_environment_source` aliases the scene environment's published slot
and inherits its deringing value. It does not project duplicate coefficients or
apply a conflicting probe-local window.

`sh_deringing` defaults to `0`. The scene loader requires a finite value greater
than or equal to zero and reports the authored field path on failure. Validation
and normalization happen at scene load, never during lighting. Changing the
value creates a new projection generation; it never edits a published slot in
place.

The scene environment and independent probe records replace
`irradiance_cubemap` with their published SH slot. Their existing source and
prefilter texture ownership is unchanged. Retained fallback ownership remains
explicit in `vkr_world_resources.c`.

Vulkan retains the ordinary cube view for skybox and filtered prefilter
sampling. When a cubemap first becomes an SH source, the publisher lazily
creates a sampled 2D-array alias over the same six layers and publishes its
descriptor with the source image's current read layout. The SH kernel uses that
alias for exact integer loads. The alias descriptor and view retire with the
source texture after its last submit serial; non-SH cubemaps acquire no extra
descriptor.

### 1.5 Final packet ABI

Only SH3 advances `VKR_RENDER_PACKET_VERSION` from 22 to 23.

The final frame-root layout reuses existing padding without moving later fields:

| Backend | Old field(s) | Final field(s) | Size |
|---|---|---|---:|
| Vulkan | `irradiance_texture`, `irradiance_sampler` | `uint64_t sh_coefficients` | 8 |
| Vulkan | `reserved_brdf_texture`, `reserved_brdf_sampler` | `uint32_t sh_global_slot`, `uint32_t sh_reserved` | 8 |
| Metal | `irradiance_texture_id` | `uint64_t sh_coefficients_address` | 8 |
| Metal | `reserved_brdf_lut` | `uint32_t sh_global_slot`, `uint32_t sh_reserved` | 8 |

The prefilter fields between those regions remain unchanged. Shader-side
declarations, C records, public packet lowering, and Metal reflection/offset
assertions change together.

The final 64-byte backend probe record changes only at offsets 0 and 4:

| Offset | Old Vulkan | Old Metal interpretation | Final |
|---:|---|---|---|
| 0 | `irradiance_texture` | low 32 bits of `irradiance_texture_id` | `sh_slot` |
| 4 | `irradiance_sampler` | high 32 bits of `irradiance_texture_id` | `sh_reserved` |

`prefilter_texture` / `prefilter_sampler` on Vulkan and
`prefilter_texture_id` on Metal remain at offset 8. All fields from offset 8
onward retain their current offsets. `VkrFrameIblProbe` replaces its irradiance
texture handle with `uint32_t sh_slot`; packet validation requires
`sh_slot < VKR_SH_SLOT_CAPACITY` before hot-path lowering.

### 1.6 Historical temporary dual-representation ABI

SH1 and SH2 kept the version-22 root and probe records intact so a
single binary can render either representation. The retired BRDF root padding
temporarily stored an address to this 16-byte-aligned, per-frame table:

```c
typedef struct VkrShAbTable {
  uint64_t sh_coefficients;
  uint32_t global_slot;
  uint32_t probe_count;
  uint32_t probe_slots[VKR_FRAME_IBL_PROBE_MAX];
} VkrShAbTable; /* 80 bytes */
```

Vulkan interpreted `reserved_brdf_texture` and `reserved_brdf_sampler` together
as the 64-bit table address. Metal used `reserved_brdf_lut`. The table lived in
completion-safe per-frame upload storage. Unused probe entries were slot 0.

`probe_slots[i]` maps to packed packet probe ordinal `i`, not scene probe index.
It is filled in the same loop that skips unavailable probes and constructs
`VkrFrameIblProbe[]`. `probe_count` must equal the packed packet count. This
prevents a sparse scene-probe list from selecting the wrong coefficients.

The A/B build contained separate cubemap and SH deferred-lighting pipeline
variants. A cold representation setting selects the variant while recording the
pass. The SH variant reads `VkrShAbTable`; the cubemap variant reads the existing
probe fields. No representation branch executes per pixel or per probe. The SH
variant and its extra pointer load are removed or made unconditional with the
final ABI.

No `VkrShAbTable`, cubemap pipeline variant, or representation selector remains
in the final implementation.

## 2. Projection and evaluation

### 2.1 Source mip

Project the source mip whose face extent is the greatest available extent not
larger than 32. A source already smaller than 32 uses mip 0. Mip derivation
lives in `vkr_ibl_math.c` beside `vkr_ibl_derive_cubemap_size()` so CPU tests and
both backends agree.

The selected mip bounds work and low-pass filters high-variance emitters.
`vkCmdBlitImage2(..., VK_FILTER_LINEAR)` and Metal mip generation are image-plane
filters; neither is exact solid-angle-preserving integration. Exact solid-angle
weights during projection correct cubemap parameterization at the selected mip,
but cannot reconstruct energy lost or redistributed by downsampling.

SH0 therefore supplies a full-resolution CPU reference. The selected-mip path
is accepted only after constant, axis-emitter, and narrow-sun fixtures are
compared against that reference. The recorded evidence sets a justified error
tolerance before renderer integration. If the extent-32 approximation misses
it, increase the projection extent or use mip 0.

### 2.2 Solid-angle projection

Each texel contributes its radiance times the exact cubemap texel solid angle
and each of the nine real basis values. Use the standard `areaElement` integral
over `[s0, s1] x [t0, t1]`, not uniform weights or a center-point density
approximation. Normalize using accumulated solid angle. Full-cube weights must
sum to `4pi` within the CPU reference tolerance, including when a small source
forces mip 0.

After radiance projection, multiply coefficients by the normalized band factors
from §1.1 and by the window factor from §2.4, then pack the seven vectors.
Both production kernels fetch the selected mip at exact integer face/texel
coordinates. Metal uses `texturecube::read` and Vulkan uses `Texture2DArray.Load`
through the publication alias from §1.4; filtered directional sampling is not
part of projection.

### 2.3 Dispatch

Dispatch one 64-thread workgroup per destination slot. At face extent 32 the
domain contains `32 * 32 * 6 = 6,144` texels, or 96 texels per thread. Threads
accumulate nine RGB partial sums, reduce in groupshared memory with a fixed tree,
and one thread writes the slot.

One workgroup and a fixed reduction order make repetitions deterministic without
floating-point atomics. The low source mip bounds work; deterministic summation,
not the mip alone, removes stochastic Monte Carlo variance. Projection cost is
not a frame-time claim.

### 2.4 Windowing and clamp

Define the normalized sinc function exactly:

```text
sinc_pi(x) = 1                         when x = 0
sinc_pi(x) = sin(pi * x) / (pi * x)   otherwise
```

For `l_max = 2`, band `l` is multiplied at projection time by

```text
pow(sinc_pi(l / (l_max + 1)), sh_deringing)
```

`sh_deringing = 0` is identity. CPU and GPU implementations must agree for
finite validated inputs.

Evaluation applies component-wise `max(D, 0)` after reconstructing the RGB
response. This is a non-negativity policy, not a conservation proof: clamping
can add integrated energy by deleting negative lobes. Quality evidence records
the unclamped negative-value frequency, minimum value, and integrated response
before and after clamping for each fixture and owner-reviewed scene.

## 3. Evidence controls

### 3.1 Indirect-diffuse capture

Render mode 4 is `VKR_RENDER_MODE_DIRECT_DIFFUSE` and returns before environment
evaluation. It cannot validate this change.

SH1 adds `VKR_RENDER_MODE_INDIRECT_DIFFUSE` to the public render-mode enum and
implements it in the Vulkan and Metal deferred paths. The mode writes only the
environment diffuse contribution, including global/probe weights and diffuse
intensities, to `final_color`; direct light, specular IBL, emissive, ambient
fallback, bloom, and temporal accumulation do not contribute.

The harness replay catalog adds the logical channel `indirect_diffuse`, mapped
to `final_color` under that render mode. Its replay name, mode, and effective
configuration participate in the workload fingerprint. The Bistro outdoor
environment and café-probe A/B cases capture this channel with identical camera,
exposure, resolution, scene state, and representation-independent settings.

### 3.2 Historical representation control

The temporary `cubemap` versus `sh_l2` selector was a cold frame/case setting
that chose one deferred-lighting pipeline variant before command recording.
It was recorded in harness effective configuration and the policy fingerprint.
No per-probe or per-pixel representation branch was allowed.

The planned performance comparison treated representation and requested probe
count as declared independent variables. The comparison tool would report their
expected fingerprint differences and reject differences in device, driver,
binary, scene, resolution, camera, target, present mode, timing policy, or other
workload fields.

## 4. Staged work and current result

SH0 through SH3 are implemented. Their original gates remain below because the
code advanced through retirement before every evidence gate was retained. That
does not retroactively turn an unrun gate into a pass.

### SH0 historical plan: shared arithmetic and reference

Add L2 basis evaluation, normalized transfer factors, optimized packing,
windowing, exact texel solid angle, source-mip derivation, and a full-resolution
reference projector to `vkr_ibl_math.c` / `.h`. Add tests in
`tests/src/ibl_math_tests.c`.

Tests cover:

- constant radiance `L` evaluates to `L`, with higher bands zero within
  tolerance;
- positive and negative axis emitters catch basis order, signs, and cubemap
  handedness;
- an analytic directional delta reconstructs the expected normalized L2
  response;
- full-cube solid-angle weights sum to `4pi`;
- direct nine-coefficient and packed evaluation agree;
- `sh_deringing = 0` is identity and finite positive values match the formula;
- selected-mip projection is compared with full resolution for constant,
  axis-emitter, and narrow-sun fixtures;
- clamp diagnostics report negative lobes and integrated response drift.

Gate: `./build_test.sh`. Do not choose the selected-mip tolerance before the
reference comparison is recorded.

### SH1 historical plan: Vulkan dual representation

Add the coefficient pool, completion-gated lifecycle, temporary A/B table,
`ibl_sh` projection kernel, and a Vulkan SH deferred-lighting pipeline variant.
Select the cubemap or SH variant once while recording the pass. Record
projection through the existing IBL bake path with explicit buffer visibility.
Keep the diffuse cubemap allocation, bake, descriptors, packet fields, and
shader pipeline live.

Add the indirect-diffuse render mode and harness replay mapping in the same
stage. Both Vulkan deferred entry points use the same environment helper.

Gates:

- `./build_test.sh`;
- one focused Vulkan validation-layer case;
- deterministic repetition of the SH0 projection fixtures on GPU;
- indirect-diffuse A/B captures for Bistro outdoor and the authored café probe;
- owner review of the café result and its clamp/energy diagnostics;
- repeated fallback activation and scene reload while prior frames remain
  submitted, with no live-slot overwrite, wait, or pool growth.

A rejected café result or a lifetime failure blocks SH2 and SH3.

### SH2 historical plan: Metal parity and performance evidence

Mirror projection, evaluation, pool ownership, temporary A/B table, and
indirect-diffuse mode on Metal. A probe sourced from the scene environment uses
the same alias rule as Vulkan; do not preserve the current global-only Metal
bake limitation in the SH path.

Run visual and performance captures in normal Release configuration with Metal
validation variables unset. Run Metal API/shader validation separately, one
renderer process and one minimal reproduction at a time. Never combine Metal
shader validation with the A/B suite or parallel captures.

SH2 gates:

- `./build_test.sh`;
- one focused Metal API validation run and, if needed for a concrete issue, one
  separate minimal shader-validation run;
- the same indirect-diffuse A/B pair as Vulkan, plus backend parity comparison;
- the authoritative Release performance matrix in §5;
- recorded sampled-texture and memory deltas;
- owner acceptance of quality and the measured performance wording.

All SH2 evidence runs while cubemap and SH paths coexist in the same binary.

### SH3 historical plan: final ABI and cubemap retirement

The plan required every SH1 and SH2 gate to pass before this stage. The code now
contains the following final state even though the open evidence listed above
still prevents acceptance:

- advance `VKR_RENDER_PACKET_VERSION` to 23;
- apply the final frame-root, probe-record, public packet, shader, and Metal
  offset changes from §1.5 in one stage;
- remove the temporary table, cubemap pipeline variant, representation selector,
  and pointer indirection;
- remove diffuse cubemap creation, bake kernels/pipelines, descriptor
  publication, `VKR_IBL_IRRADIANCE_SIZE`, scene handles, and retain/release
  paths;
- preserve source cubemap, prefilter cubemap, and their ownership unchanged.

Final gates:

- `./build_test.sh`;
- one focused Vulkan validation-layer case;
- one focused, single-process Metal API validation case, with shader validation
  separate only if required by a concrete diagnostic;
- explicit cold and warm production pipeline-cache runs on both backends;
- final indirect-diffuse backend-parity capture;
- repeated load, unload, fallback activation, and replacement-generation tests
  with previously submitted frames still outstanding;
- slot metrics return to the expected black-sentinel-only baseline after proven
  completion, with no exhaustion or monotonically growing retired count;
- documentation updates in §6.

SH3 cannot rerun a cubemap-versus-SH performance comparison because the cubemap
path is gone. The plan expected an accepted SH2 comparison, but no such retained
record exists; §5 defines the honest recovery choices.

## 5. Performance evidence

The claim under test is limited to the deferred-lighting pass: replacing up to
17 diffuse `TextureCube` samples per pixel with SH evaluation may reduce that
pass's GPU time. Bake time and total frame time are reported for context, not as
the claimed result.

The final tree contains one dedicated scene fixture,
`assets/scenes/fixtures/ibl_probe_sweep.scene.json`, with 16 overlapping local probes and
a camera path that keeps the measured pixels inside all selected volumes. A
cold harness control `ibl_probe_limit` packs exactly 0, 1, 4, or 16 probes from
that fixture. Cases under `tools/cases/performance/` cover the four final SH
loads using the name pattern
`sh_ibl_probe_sweep_{0,1,4,16}_sh_l2.case.json`:

| Variable | Values |
|---|---|
| `ibl_probe_limit` | `0`, `1`, `4`, `16` |

The control is emitted in `effective_config`, fingerprints, and the report. The
fixture asserts the packed probe count so an unavailable prefilter cannot
silently reduce work. The cubemap cases and representation selector were
retired; these four cases characterize scaling of the final SH path only.

Run each case with
`tools/profiles/performance-windowed-gpu.json` in a normal Release build, using
the same clean binary, device, driver, windowed-hidden target, immediate present
mode, resolution, camera, and five deterministic repetitions. GPU timestamps
must be enabled and authoritative. Compare the deferred-lighting pass `gpu_ms`
distribution at each probe count and report p50, dispersion, and paired delta.
Do not substitute frame time or infer a result from fetch counts.

The planned same-binary cubemap/SH comparison was not retained before
retirement. A future acceptance record must not present the four final-path
cases as an A/B comparison. It may either restore the historical cubemap path on
a temporary measurement branch or explicitly accept memory/code reduction with
no measured speed claim.

The final 16-probe case was recorded on the clean `a3bcf8c` Release tree with:

```sh
build_release/tools/vkr_harness profile \
  --case tools/cases/performance/sh_ibl_probe_sweep_16_sh_l2.case.json \
  --profile tools/profiles/performance-windowed-gpu.json
```

All five 300-frame repetitions passed with stable warmup, exclusive GPU-lane
ownership, complete requested GPU timestamps, and exactly 16 packed probes.
The authoritative report digest is
`sha256:9d13f1159a8c813bb1c4865725d1e05fceea23156aa6538053058867ef1a9589`.
Across 1,500 samples, `Lighting.Deferred.Fullscreen` GPU time was 1.15275 ms p50,
1.14742 ms mean, and 0.20832 ms standard deviation; `frame.wall` was 8.59658 ms
p50 and 8.32950 ms mean. `IBL.Bake` GPU timing was unavailable for this Metal
timestamp scope. This is an authoritative characterization of one final-path
load, not a speed comparison; the 0/1/4-probe cases and a comparable historical
control remain open.

The factual final resource state is:

- no diffuse cubemap allocation or bindless sampled-texture reference;
- one 4,144-byte coefficient pool; and
- source cubemap, skybox, GGX prefilter, and their sampler ownership unchanged.

If the 16-probe timing does not separate beyond noise, record the result as
neutral. Memory and code reduction can still justify acceptance, but ADR-038
and the final status update must not claim a measured speedup.

Harness artifacts are regenerable. Carry the commands, report digests, relevant
fingerprints, and measured numbers into the completion record, then remove the
artifact tree in the same turn.

## 6. Completion

The implementation and status updates are present, but completion remains
partial until the open evidence in the implementation-status section is
retained. When that evidence closes, update in the same change:

1. this document's status, date, evidence commands, and accepted tolerances;
2. ADR-038 to Accepted, including the measured quality outcome and an honest
   performance result or explicitly neutral performance wording;
3. `renderer-architecture-spec.md` §3.7, §4, and any affected §8 issue;
4. `docs/README.md` and `docs/architecture/adr/README.md`;
5. [ADR-016](../architecture/adr/016-hdr-environment-format.md), clearly
   limiting its accepted diffuse-cubemap decision to history while retaining
   the source, skybox, and specular decisions;
6. [hdr-environment-ibl-spec.md](hdr-environment-ibl-spec.md), removing SH from
   its out-of-scope list and correcting its stale BRDF-LUT storage wording; and
7. relevant harness schema/catalog documentation for the new controls and
   `indirect_diffuse` replay channel.

Do not mark the proposal accepted from source presence alone. Acceptance needs
the final ABI, both backends, all lifetime gates, and retained evidence.

## 7. References

- Ramamoorthi and Hanrahan, *An Efficient Representation for Irradiance
  Environment Maps*, SIGGRAPH 2001: clamped-cosine transfer and the empirical
  nine-coefficient reconstruction result.
- Sloan, *Stupid Spherical Harmonics Tricks*, GDC 2008: optimized packing and
  windowing.
- [ADR-016](../architecture/adr/016-hdr-environment-format.md): accepted current
  cubemap decision.
- [ADR-028](../architecture/adr/028-gpu-driven-deferred-visibility-buffer.md):
  deferred topology that changed the cost model.
- [bistro-baseline-shading-investigation.md](bistro-baseline-shading-investigation.md):
  prior forward-path SH assessment.
