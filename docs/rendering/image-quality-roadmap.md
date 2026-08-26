---
status: partial
updated: 2026-08-26
authority: design
---

# Image quality roadmap

**Document status:** Active roadmap. Presentation and portable same-resolution
TAA ship; the architecture status specification remains the authority.

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
changes. Automatic exposure remains separately ordered because deterministic
cases still need manual exposure; if it introduces pre-exposure, it must rescale
or reset temporal history at that new boundary.

FSR frame generation is not part of this roadmap.

## 2. Order of work

| Order | Work | Owner document | Decision |
| --- | --- | --- | --- |
| 1 | Windows DPI correctness | [Presentation DPI and transfer function](presentation-dpi-and-transfer-function-spec.md) | Implemented. Per-Monitor V2 and physical client pixels ship; mixed-DPI display evidence remains pending. |
| 2 | One linear-to-sRGB presentation contract | Same document | Implemented. Both backends use linear shader output and blending into sRGB attachments; replacement final-color goldens await owner review. |
| 3 | Temporal-input foundation | [Visibility-buffer anti-aliasing evaluation](visibility-buffer-msaa-spec.md) | Implemented for rigid opaque, transmission, and ordinary-blend geometry: jitter, own-surface motion, exact identity, authored material reactivity, reset rules, completion-safe history, and debug views ship. Deformation, procedural motion, particles, and dynamic material-change signals remain open. |
| 4 | Automatic exposure | [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md) | Implement after the presentation domain is fixed. Preserve manual exposure for deterministic cases. Current manual exposure is post-temporal and needs no reset; a future pre-exposure domain must rescale or reset history explicitly. |
| 5 | Portable same-resolution TAA and post-TAA FXAA | [ADR-037](../architecture/adr/037-portable-same-resolution-temporal-antialiasing.md) | Implemented on Metal and Vulkan without an additional graph pass or full-resolution resource for transparent inputs. Native Apple runtime validation, broader motion fixtures, deformation/procedural/particle inputs, and final-color owner acceptance remain open. MSAA is not part of this slice. |
| 6 | Bloom, then GTAO | [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md) | Implement separately and measure separately. GTAO needs its own current-frame depth pyramid. |
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

Packet version 19 carries renderer-owned temporal identity and camera state.
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
- moving-camera history searches the four metadata texels corresponding to the
  bilinear history-color footprint. Opaque and transmission require exact
  identity, primitive, bounds, and depth; blend requires exact identity,
  primitive, and bounds. A stationary camera permits clamped coverage
  accumulation;
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
particles, and dynamic material-change signals. Native Metal runtime evidence
and broader animation/disocclusion acceptance also remain open. MSAA remains
unimplemented: no authored sample-count field, Vulkan graph realization rejects
counts other than one, backend graphics pipelines use one sample, and production
shader, picking, HZB, SDSM, transmission, blend, and capture paths assume
one-sample images.

### 3.4 Eight-bit linear albedo remains a separate risk

`GBuffer.Resolve` stores linear diffuse albedo in `R8G8B8A8_UNORM`. That can
quantize dark materials, but no isolated VKR capture proves that it is visible
after lighting and tonemapping. Keep it on the backlog until a channel capture
and final-color comparison establish a visible defect. A format change adds
bandwidth to every opaque pixel and needs measured evidence.

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
4. a synchronization-validation-clean RX 6700 XT run; and
5. direct local visual improvement on transparent surfaces, bright emissive
   silhouettes, foliage, lamps, railings, chairs, and table edges without broad
   whole-frame blur. A far pendant crop's adjacent-luma mean-square edge energy
   is 36.6% below the FXAA bypass.

Still required: native M1 Pro shader/runtime validation, deformation,
disocclusion, moving-transparency and animation fixtures, authoritative
clean-tree performance evidence, and explicit owner acceptance of changed
final-color goldens. The implementation remains partial until those gates pass.

## 7. Primary references

- [AMD FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
- [AMD temporal super-resolution integration guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
- [Apple MetalFX temporal scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase)
