---
status: proposed
updated: 2026-08-25
authority: design
---
# Image quality roadmap

**Document status:** Plan only. The architecture status specification remains
the authority for shipped behavior.

**Scope:** Rendering work that changes displayed image quality, plus the D3D12
evaluation that is intentionally kept out of the image-quality argument.

## 1. Recommendation

Fix presentation first. Then build and validate a backend-neutral temporal-input
contract before choosing the shipping anti-aliasing algorithm.

Do not accept 4x MSAA as the default yet. It is a credible option for sharp
geometry and cutout edges, but the current proposal understates its cost and it
does not address specular, normal-map, shader, or subpixel temporal shimmer. The
visibility-buffer topology makes good temporal rejection possible because it
provides exact surface identity, but the renderer still lacks previous object
transforms, motion vectors, jitter, history reset rules, and transparency masks.

Do not start by dropping FSR 3.1 into the graph. Start with the inputs that FSR
3.1, MetalFX, and any custom TAA implementation all require. Once those inputs
pass debug and motion tests, use FSR 3.1 Native AA as a Vulkan prototype and
MetalFX temporal scaling at an API-supported scale as the Metal prototype. If a
single cross-backend algorithm is more important than vendor tuning, evaluate a
portable TAA resolve against the same inputs instead.

FSR frame generation is not part of this roadmap.

## 2. Order of work

| Order | Work | Owner document | Decision |
| --- | --- | --- | --- |
| 1 | Windows DPI correctness | [Presentation DPI and transfer function](presentation-dpi-and-transfer-function-spec.md) | Implemented. Per-Monitor V2 and physical client pixels ship; mixed-DPI display evidence remains pending. |
| 2 | One linear-to-sRGB presentation contract | Same document | Implemented. Both backends use linear shader output and blending into sRGB attachments; replacement final-color goldens await owner review. |
| 3 | Temporal-input foundation | [Visibility-buffer anti-aliasing evaluation](visibility-buffer-msaa-spec.md) | Implement previous transforms, jitter, motion, masks, extents, reset rules, and debug views without selecting an AA consumer. |
| 4 | Automatic exposure | [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md) | Implement after the presentation domain is fixed and before the temporal bakeoff. Preserve manual exposure for deterministic cases. |
| 5 | AA prototypes and bakeoff | [Visibility-buffer anti-aliasing evaluation](visibility-buffer-msaa-spec.md) | Compare temporal native AA with 2x and 4x MSAA before accepting an AA ADR. |
| 6 | Bloom, then GTAO | [Post, exposure, bloom, and ambient occlusion](post-exposure-bloom-and-ambient-occlusion-spec.md) | Implement separately and measure separately. GTAO needs its own current-frame depth pyramid. |
| 7 | D3D12 | [D3D12 backend evaluation](../architecture/d3d12-backend-evaluation.md) | Do not schedule for image quality. Revisit only for a concrete delivery, tooling, CI, or driver requirement. |

Windows DPI and output transfer can land independently. A DPI change affects
windowed pixel count and screenshots. It does not invalidate fixed-extent
offscreen goldens. The transfer-function change does change final-color bytes
and requires explicit golden review.

## 3. Verified implementation status

### 3.1 Windows physical client dimensions ship

`vkr_platform_init()` now establishes Per-Monitor V2 before window creation.
`vkr_window_windows.c` performs initial and existing-window non-client sizing
with the relevant monitor/window DPI and applies `WM_DPICHANGED` through the
existing resize event path. Client, cursor, and picking coordinates remain in
one physical-pixel domain, matching the macOS backing-pixel contract.

This can explain a large Windows sharpness loss at display scaling above 100%,
but the owner observation is not yet a matched capture. The code defect is
closed; its contribution to the reported visual gap remains a hypothesis until
a same-extent comparison exists.

### 3.2 The backends share one output transfer

Metal and Vulkan now write linear tonemapped color to sRGB output attachments.
Vulkan rejects window surfaces without a usable BGRA8/RGBA8 sRGB format, uses
an sRGB offscreen final-color image, and no longer applies the gamma-2.2 shader
approximation. Retained UI and text RGB is decoded from authored sRGB once on
the CPU, alpha stays linear, and both backends blend in linear RGB before the
attachment encode.

Encoded final-color bytes intentionally changed. New goldens remain unpublished
until explicit owner review.

### 3.3 No anti-aliasing or temporal input contract ships

The renderer has no camera jitter, motion-vector image, previous-transform
table, temporal color history, reactive mask, or history-reset protocol. Graph
image descriptions contain a sample-count field, but production remains
one-sample-only:

- the JSON graph loader has no sample-count field;
- Vulkan graph realization rejects every sample count other than one;
- Vulkan and Metal graphics pipeline factories hard-code one sample;
- Metal graph texture creation does not select multisample texture types; and
- the visibility, picking, HZB, SDSM, transmission, blend, and capture paths all
  assume single-sample images.

MSAA and temporal reconstruction are both real renderer projects. Neither is a
small post-process toggle.

### 3.4 Eight-bit linear albedo remains a separate risk

`GBuffer.Resolve` stores linear diffuse albedo in `R8G8B8A8_UNORM`. That can
quantize dark materials, but no isolated VKR capture proves that it is visible
after lighting and tonemapping. Keep it on the backlog until a channel capture
and final-color comparison establish a visible defect. A format change adds
bandwidth to every opaque pixel and needs measured evidence.

## 4. AA direction

The previous forward+ TAA result is useful evidence about that implementation,
not a permanent verdict on temporal AA. Forward+ did not provide the exact
instance and primitive identity that VKR's visibility buffer already stores.
That identity improves disocclusion and history rejection. It does not create
motion vectors or solve animated materials, transparency, particles, exposure
changes, or missing previous transforms.

The practical choice is:

| Question | Visibility-buffer MSAA | Temporal native AA |
| --- | --- | --- |
| Geometry and cutout edge sharpness | Strong | Strong when motion and masks are correct |
| Specular and subpixel shimmer | Mostly unchanged | Can reduce it through temporal supersampling |
| Motion ghosting risk | None | High when vectors, masks, or reset rules are wrong |
| Current VKR integration | Requires multisample graph, image, pipeline, resolve, depth, picking, transparency, and capture work | Requires previous transforms, jitter, motion, history, exposure, masks, and reset work |
| Future internal render scaling | Does not provide reconstruction | Directly reusable |
| Metal and Vulkan algorithm parity | Same algorithm is possible | FSR and MetalFX are different consumers unless VKR owns a portable resolve |

The recommendation is temporal-first because it solves more of the observed AA
problem and is required for future render scaling. Keep a bounded 2x/4x MSAA
prototype as the sharp, non-temporal control. Do not ship either until matched
motion clips and Release profiles establish its quality and cost.

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

## 6. Evidence and decision gate

Before accepting an AA ADR, record:

1. fixed-extent stills and motion clips for geometry edges, foliage, specular
   highlights, disocclusion, thin geometry, transparency, and camera cuts;
2. motion-vector and history-rejection debug views;
3. matched one-sample, temporal-native, 2x MSAA, and 4x MSAA Release profiles at
   the same internal and output extents;
4. per-pass GPU timings and the fraction of pixels that need per-sample edge
   shading for each MSAA case;
5. validation-clean runs on the RX 6700 XT and M1 Pro; and
6. explicit owner acceptance of changed final-color goldens.

An accepted decision must name one primary AA mode, its fallback, its reset and
overflow behavior, and the condition that would reopen the choice.

## 7. Primary references

- [AMD FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
- [AMD temporal super-resolution integration guide](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/docs/techniques/super-resolution-temporal.md)
- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
- [Apple MetalFX temporal scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase)
