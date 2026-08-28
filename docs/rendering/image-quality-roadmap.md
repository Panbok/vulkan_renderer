---
status: partial
updated: 2026-08-28
authority: design
---

# Image quality roadmap

**Document status:** Active roadmap. Presentation, portable same-resolution
TAA, automatic exposure, bloom, and GTAO G0-G2 ship; the architecture status
specification remains the authority.

**Windows Vulkan structural closure (2026-08-28); absolute exposure parity is
open.** The apparent GTAO regression was a
two-channel normal-map decode defect outside this roadmap. Automatic exposure
also supplied a storage-image descriptor index to Vulkan's sampled-image array,
causing the histogram to meter unrelated images as history instances rotated.
The shared Metal/Vulkan decoder now reconstructs positive tangent-space Z, the
exposure graph and executor use one sampled-image contract, and the harness
waits for renderer asset publications before measuring its smoke fixtures.
Windows exposure, bloom, GTAO, Bistro, and rotated deferred structural evidence
passes. Owner review at darker Bistro cameras shows stable Vulkan automatic
exposure converging near `3x`, visibly brighter than the supplied Metal frame.
Bloom-off isolation leaves HDR metering and resolved exposure unchanged. A
matched current Metal HDR/exposure capture is now the blocking parity gate.
See [Windows Vulkan post-effect parity investigation](windows-vulkan-post-effect-parity-investigation.md).

**Scope:** Rendering work that changes displayed image quality, plus the D3D12
evaluation that is intentionally kept out of the image-quality argument.

## 1. Recommendation

Retain the portable same-resolution TAA path implemented on Metal and Vulkan.
It establishes one backend-neutral temporal contract without depending on FSR,
MetalFX, or multisampling.

The production path owns jittered and unjittered camera matrices, rigid opaque,
transmission, and ordinary-blend motion, completion-safe transform and image
history, exact visibility identity, stationary-camera coverage accumulation,
moving-camera rejection, authored material reactivity, bounded composition
reactivity, reset reasons, and motion/history debug views. Manual exposure is
applied only after scene-linear temporal history. The existing final tonemap
draw applies output-space FXAA after temporal reconstruction. UI and screen text
remain after both stages.

Do not schedule MSAA as part of this implementation. The visibility-buffer MSAA
design remains a separate proposal with no production sample-count path.

Next image-quality work should close inputs that still lack an authored signal:
deformation and procedural vertex motion, particles, and dynamic material
changes. Automatic exposure phases E0-E3, bloom phases B0-B1, and GTAO phases
G0-G2 now ship. Bloom remains scene-linear and post-temporal: exposure meters
the pre-bloom source, and the combined HDR result is exposed before
tonemapping. GTAO uses a dedicated current-frame view-depth pyramid and
attenuates indirect diffuse only. Deterministic cases still default exposure to
manual and bloom and GTAO to disabled unless explicitly testing the feature.
Any future pre-exposure domain must rescale or reset history explicitly.

FSR frame generation is not part of this roadmap.

## 2. Order of work

| Order | Work | Owner document | Decision |
| --- | --- | --- | --- |
| 1 | Windows DPI correctness | [Presentation DPI and transfer function](presentation-dpi-and-transfer-function-spec.md) | Implemented. Per-Monitor V2 and physical client pixels ship; mixed-DPI display evidence remains pending. |
| 2 | One linear-to-sRGB presentation contract | Same document | Implemented. Both backends use linear shader output and blending into sRGB attachments; replacement final-color goldens await owner review. |
| 3 | Temporal-input foundation | [Visibility-buffer anti-aliasing evaluation](visibility-buffer-msaa-spec.md) | Implemented for rigid opaque, transmission, and ordinary-blend geometry: jitter, own-surface motion, exact identity, authored material reactivity, reset rules, completion-safe history, and debug views ship. Deformation, procedural motion, particles, and dynamic material-change signals remain open. |
| 4 | Automatic exposure | [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md) | Implemented through E3, absolute cross-backend acceptance open. Packet version 20 carries mode, manual fallback, and EV compensation; two graph passes meter the post-temporal HDR source into a 256-bin histogram and resolve percentile-clipped, completion-safe adaptation. Fullscreen/editor tonemap consume current GPU state directly, delayed diagnostics expose the histogram, scalar metrics expose the EV decision, and canonical capture records the completed multiplier. Production initialization selects automatic exposure; manual remains the byte-identical path and the deterministic harness default. Metal runtime evidence passes. Windows Vulkan now declares and selects the histogram input through the sampled-image contract and fixed-camera traces are stable. Darker Bistro cameras converge near `3x` and fail owner visual parity against the supplied Metal image; an exact-camera native Metal HDR/exposure trace is required before backend correction or shared policy retuning. Exposure stays post-temporal; any future pre-exposure domain must rescale or reset history. |
| 5 | Portable same-resolution TAA and post-TAA FXAA | [ADR-037](../architecture/adr/037-portable-same-resolution-temporal-antialiasing.md) | Implemented and validation-clean on Metal and Vulkan without an additional graph pass or full-resolution resource for transparent inputs. Broader motion fixtures, deformation/procedural/particle inputs, and final-color owner acceptance remain open. MSAA is not part of this slice. |
| 6 | Bloom, then GTAO | [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md) | Implemented through Bloom B0-B1 and GTAO G0-G2 on Metal and Vulkan. Bloom uses bounded odd-extent downsample/reverse-upsample chains and scene-linear pre-exposure combine. GTAO uses a dedicated current-frame R16 view-depth pyramid, full-resolution three-slice, three-step horizon evaluation, edge-aware denoise, branchless white fallback, and indirect-diffuse-only lighting. Direct captures expose both pipelines. Metal and Windows Vulkan Release runtime evidence passes, and focused validation children are clean. Authoritative matched performance and final-color owner acceptance remain open. |
| 7 | D3D12 | [D3D12 backend evaluation](../architecture/d3d12-backend-evaluation.md) | Do not schedule for image quality. Revisit only for a concrete delivery, tooling, CI, or driver requirement. |

Windows DPI and output transfer landed independently. A DPI change affects
windowed pixel count and screenshots. It does not invalidate fixed-extent
offscreen goldens. The transfer-function change does change final-color bytes
and requires explicit golden review.

## 3. Verified implementation status

### 3.1 Windows physical client dimensions ship

`vkr_platform_init()` establishes Per-Monitor V2 before window creation.
`vkr_window_windows.c` performs initial and existing-window non-client sizing
with the relevant monitor/window DPI and applies `WM_DPICHANGED` through the
existing resize event path. Client, cursor, and picking coordinates remain in
one physical-pixel domain, matching the macOS backing-pixel contract.

This can explain a large Windows sharpness loss at display scaling above 100%,
but the owner observation is not yet a matched capture. The code defect is
closed; its contribution to the reported visual gap remains a hypothesis until
a same-extent comparison exists.

### 3.2 The backends share one output transfer

Metal and Vulkan write linear tonemapped color to sRGB output attachments.
Vulkan rejects window surfaces without a usable BGRA8/RGBA8 sRGB format, uses
an sRGB offscreen final-color image, and no longer applies the gamma-2.2 shader
approximation. Retained UI and text RGB is decoded from authored sRGB once on
the CPU, alpha stays linear, and both backends blend in linear RGB before the
attachment encode.

Encoded final-color bytes intentionally changed. New goldens remain unpublished
until explicit owner review.

### 3.3 Portable same-resolution TAA ships

Packet version 22 carries renderer-owned temporal identity, camera state,
the versioned exposure contract, bloom controls, and GTAO controls.
`vkr_temporal_prepare()` derives the Halton jitter, jittered projection,
current output-grid view-projection, reset reasons, and history-valid flag at
the frontend boundary. Stable mesh and instance slots provide temporal indices
and generations without per-draw allocation.

Both backends lower the same graph contract:

- `Temporal.TransformHistory` publishes current rigid transforms into a
  completion-protected `HISTORY` buffer;
- G-buffer resolve, transmission layer-0 shading, and ordinary-blend MRT
  rendering emit current surface motion and validity;
- ordinary blend overlays bounded exact index/generation/primitive identity
  into the existing vbuffer, while transmission retains its layer-0 identity
  and depth;
- `Temporal.Resolve` reads the newest completed history and writes color,
  depth, identity, and primitive history;
- Metal realizes each graph `HISTORY` resource with `N + 2` instances for `N`
  command slots and chooses a completion-safe output independently of the
  command slot. Temporal and HZB inputs may remain distinct without restarting
  accumulation or introducing a CPU/GPU wait;
- moving-camera history searches the four metadata texels corresponding to the
  bilinear history-color footprint. Opaque and transmission require exact
  identity, primitive, bounds, and depth; blend requires exact identity,
  primitive, and bounds. Rejected texels cannot contribute history color: partial
  footprints use masked, renormalized bilinear reconstruction, while fully valid
  footprints keep hardware bilinear sampling. A stationary camera permits
  clamped coverage accumulation;
- accepted stationary-camera pixels below `0.01` pixel of surface motion use
  `0.99` history retention to suppress the eight-phase EMA residual. Camera or
  surface motion retains the responsive `0.9` path;
- PBR `temporal_reactivity` remains active at rest, while moving-camera
  composition reactivity is capped at `0.75`;
- manual exposure remains post-temporal, so scene-linear history is
  exposure-independent; and
- the existing final tonemap/composite draw applies directional FXAA with a
  cardinal-neighborhood subpixel blend in tonemapped linear output space before
  UI and screen text.

`VKR_TAA_DISABLED=1` preserves the graph but selects unjittered passthrough.
`VKR_FXAA_DISABLED=1` selects the center sample in the same final draw. The
`temporal_motion` and `temporal_history` debug modes expose the core temporal
inputs through the harness.

The remaining temporal gaps are deformation and procedural vertex motion,
particles, dynamic material-change signals, and broader
animation/disocclusion acceptance. Native Apple M1 Pro Metal API/GPU shader
validation passes. MSAA remains unimplemented: no authored sample-count field,
Vulkan graph realization rejects counts other than one, backend graphics
pipelines use one sample, and production shader, picking, HZB, SDSM,
transmission, blend, and capture paths assume one-sample images.

### 3.4 Eight-bit linear albedo remains a separate risk

`GBuffer.Resolve` stores linear diffuse albedo in `R8G8B8A8_UNORM`. That can
quantize dark materials, but no isolated VKR capture proves that it is visible
after lighting and tonemapping. Keep it on the backlog until a channel capture
and final-color comparison establish a visible defect. A format change adds
bandwidth to every opaque pixel and needs measured evidence.

### 3.5 Scene-linear bloom ships

Packet version 21 carries explicit enable, threshold, knee, and intensity
controls. Production enables defaults while deterministic harness cases remain
disabled unless they author every control; `VKR_BLOOM_DISABLED=1` is a cold
forced bypass.

Both backends realize one authored graph slice after temporal resolve and after
the pre-bloom exposure histogram input. A half-resolution prefilter feeds a
bounded downsample chain. Reverse repeat expansion writes a separate
deepest-first accumulation chain, then a full-resolution combine pass adds the
resolved result to the original HDR source before exposure and tonemapping.
Fullscreen and editor branches consume the same combined resource.

The shared kernel pins the non-finite/firefly sanitizer, scene-linear soft knee,
and Karis weight. Backend-native tap code supplies the selectable 13-tap/4-tap
downsample and 9-tap upsample filters. Direct capture channels expose
`hdr_pre_bloom`, `bloom_prefilter`, `bloom_result`, and `hdr_combined`.

Focused CPU/graph tests, odd-extent Release execution, five-channel capture, and
a strictly serial Metal API/GPU shader-validation profile pass. Native Vulkan
validation, authoritative matched bloom-on/off performance, and owner acceptance
of any changed final-color baseline remain open; no performance ranking is
claimed.

### 3.6 Full-resolution GTAO ships

Packet version 22 carries explicit enable, radius, and power controls.
Production enables the XeGTAO-derived defaults while deterministic harness
cases remain disabled unless they author every control;
`VKR_GTAO_DISABLED=1` is a cold forced bypass.

Both backends realize the same authored current-frame graph slice before
deferred lighting: one full-resolution `R16_SFLOAT` positive view-depth image
whose first five levels are AO inputs, one full-resolution horizon-evaluation
pass writing `R8_UNORM` raw visibility and directional edges, and one
edge-aware 3x3 denoise writing the final `R8_UNORM` visibility. Evaluation uses
three slices and three steps per slice. It rotates the current G-buffer world
normal into view space and never samples the completion-delayed HZB.

Deferred lighting receives a white fallback when GTAO is disabled, so the
shader contains no feature branch. GTAO multiplies only indirect diffuse after
material AO. Direct analytic light and the existing material-only
specular-occlusion approximation remain unchanged. Direct captures expose view
depth, G-buffer normal, raw AO, denoised visibility, and final color.

Focused CPU/graph/harness tests, isolated-cold Release snapshots, a keyframed
thin-occluder snapshot, per-pass Metal timing attribution, and strictly serial
Metal API/GPU shader validation pass. Windows Vulkan now adds non-vacuous
exposure, bloom, static GTAO, moving GTAO, owner-camera Bistro, cold/warm
deferred, and focused synchronization-validation evidence. Authoritative
matched GTAO-on/off performance and owner acceptance of any final-color
baseline remain open; no performance ranking is claimed.

## 4. AA direction

Portable same-resolution TAA is the selected production direction. The
visibility buffer's exact temporal index/generation and primitive identity
provide stronger disocclusion rejection than the removed forward+ path, while
motion and depth reject same-surface reprojection failures.

The current boundary is deliberately narrow:

| Question | Current TAA answer |
| --- | --- |
| Geometry and cutout edges | Halton jitter plus history accumulation and neighborhood clamp |
| Rigid motion | Previous object and camera transforms with exact source-frame tags across opaque, transmission, and ordinary blend |
| Disocclusion | Extent, identity, and primitive rejection; opaque/transmission also retain device-depth rejection |
| Transmission and ordinary blend | Own-surface rigid motion plus authored material reactivity; stable composition accumulates at rest and moving-camera luminance fallback remains capped |
| Residual high-contrast edges | Output-space FXAA in the existing final tonemap draw; `VKR_FXAA_DISABLED=1` selects the center sample |
| UI and screen text | Composited after temporal reconstruction and FXAA |
| Deformation, procedural motion, particles, dynamic materials | No explicit per-frame vectors/reactive changes; history is not guaranteed correct |
| Temporal fallback | `VKR_TAA_DISABLED=1` selects unjittered passthrough |
| Cross-backend parity | One temporal resolve and FXAA algorithm on Metal and Vulkan |

MSAA remains unimplemented and is not a prerequisite for continuing TAA quality
work. Reopen a different AA algorithm only with matched quality and Release GPU
evidence against this path.

## 5. What not to schedule

### D3D12 for image quality

D3D12 does not fix DPI, transfer functions, motion vectors, or AA. A third
implementation may be valuable later, but it widens validation and maintenance
before the current two-backend image contract is stable.

### Mesh shaders for parity

The current candidate classification and backend-native indirect submission
already solve the project's draw-submission problem. Apple7 supports Metal mesh
shading, so "Metal cannot follow" is not a valid rejection. The real rejection
is simpler: no VKR measurement shows that replacing the current path would pay
for a second geometry pipeline, and indirect mesh capabilities differ across
the hardware floor.

### Unmeasured fp16 and subgroup rewrites

The punctual-light rows and cull atomics are plausible optimization targets.
They are not image-quality work and have no matched VKR evidence. Leave them out
of this roadmap.

## 6. Evidence and completion gate

ADR-037 accepts the portable TAA architecture. Current evidence records:

1. three 1600x1200 held Bistro recorded-camera views with TAA enabled and
   disabled, plus focused held transparency/emissive and slow moving-camera
   captures;
2. motion-vector and history-acceptance debug captures, including full
   stationary acceptance and a moving-camera bilinear-footprint correction
   that reduces exact rejection from 12.291% to 2.852% without relaxing
   identity, primitive, or depth checks;
3. matched local dirty-tree Release GPU timings at identical extent and work:
   three 120-frame observations before and after measure +0.03364 ms mean in
   `Temporal.Resolve.Fullscreen` and +0.07904 ms mean in
   `Post.Tonemap.Fullscreen` at 1600x1200;
4. a synchronization-validation-clean RX 6700 XT run;
5. an exclusive native Apple M1 Pro Metal API/GPU shader-validation run in
   which both temporal passes execute for all six measured frames and all 11
   state-matrix assertions pass, report digest
   `sha256:5f5c4e0b7422c9f8c66775cd94f5295fe9a302071de31939a0dc79181382d15c`;
   and
6. direct local visual improvement on transparent surfaces, bright emissive
   silhouettes, foliage, lamps, railings, chairs, and table edges without broad
   whole-frame blur. A far pendant crop's adjacent-luma mean-square edge energy
   is 36.6% below the FXAA bypass; and
7. a Release Metal Bistro history-ownership diagnostic after decoupling history
   from command slots, report digest
   `sha256:ae59f34f5296d7d918a9c8c58114ffade7cfd96ec134064fa9cec79adb420191`.
   Its four independent replays cannot establish interactive temporal
   stability, and the owner subsequently confirmed that the visible jitter was
   unchanged. It is retained only as history-lifetime evidence; and
8. a follow-up channel split at the reported fixed Bistro view. Raw deferred
   emissive is bit-identical across the selected jitter phases and every pixel
   accepts history. The visibility-buffer deferred path instead lacked the
   normal-footprint roughness filter already used by forward shading. Metal and
   Vulkan now apply that filter before analytic and environment specular
   lighting, and the owner confirms that it materially reduces the artifact.
   Later coefficient, punctual roughness, angular-variance, and inverse-square
   attenuation experiments did not establish a causal improvement and are not
   retained. The accepted stationary `0.99` path makes the fixed camera nearly
   stable. The moving path now masks rejected bilinear history-color texels and
   renormalizes partial footprints. Debug/Release builds and focused Metal
   API/GPU shader validation pass; moving-camera owner acceptance remains open.

Still required: deformation, disocclusion, moving-transparency and animation
fixtures, authoritative clean-tree performance evidence, and explicit owner
acceptance of changed final-color goldens. The implementation remains partial
until those gates pass.

## 7. Primary references

- [AMD FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
- [AMD temporal super-resolution integration guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
- [Apple MetalFX temporal scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase)
