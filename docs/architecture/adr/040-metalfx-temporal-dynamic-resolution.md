---
status: implemented
updated: 2026-09-01
authority: adr
---

# ADR-040: MetalFX temporal reconstruction with dynamic resolution

## Status

**Accepted.** Metal exposes MetalFX temporal reconstruction as a cold renderer
strategy. It may run at a fixed internal scale or consume completed Metal GPU
submission intervals to select a bounded dynamic scale. Spatial reconstruction
from ADR-039 remains the zero-initialized renderer API default. The macOS sample
application selects MetalFX with dynamic resolution by default; Vulkan has no
MetalFX mode.

## Context

ADR-039 separated the physical output extent from the Metal scene extent and
folded a linear spatial upscale into tonemap. That made resolution an honest
workload control, but it did not reconstruct subpixel detail or adapt to load.
The production Bistro orbit also runs at a 2560x1440 physical target for its
1280x720 logical hidden window on the owner M1 Pro. A fixed scale that survives
the heaviest camera interval gives up detail for the entire orbit.

VKR already publishes the temporal inputs MetalFX needs: scene-linear HDR,
device depth, current-to-previous rigid motion, Halton jitter, reset reasons,
and retained previous transforms. MetalFX is nevertheless a different consumer
from the portable same-resolution resolve. The portable resolve can select the
newest GPU-completed history image, but MetalFX advances private history on
every scaler encode and requires motion to target that exact preceding encode.
Its resource requirements, history ownership, dynamic input rectangle, output
extent, synchronization, and platform support must be explicit rather than
hidden behind the existing TAA executor.

## Decision

1. `VkrUpscaleMode` selects either `SPATIAL` or `METALFX_TEMPORAL` once during
   renderer initialization. MetalFX is rejected on non-Metal backends and on a
   device that does not support the Metal 4 MetalFX temporal scaler. There is no
   silent fallback that would change image quality or workload identity.
2. The authored graph stages internal HDR color, depth, and motion into
   native-sized private MetalFX input textures. `inputContentWidth` and
   `inputContentHeight` delimit the active upper-left rectangle. MetalFX writes
   native-resolution scene-linear HDR; exposure, bloom, tonemap, UI, and text
   then run in the native output domain. The portable temporal resolve is
   omitted in this mode.
3. Motion remains `previous_uv - current_uv` from unjittered transforms.
   Multiplying the normalized components by the active content width and height
   gives MetalFX's current-pixel-to-previous-pixel displacement. Current jitter
   is passed separately, depth is declared non-reversed, and MetalFX resets on
   every VKR temporal reset or invalid-history frame. The previous transform
   address, view-projection matrix, and source frame all come from the exact
   immediately preceding scaler frame, never the newest completed portable-TAA
   history. A missing exact predecessor resets the scaler. When that transform
   is still in flight, the current submission waits on its producer through the
   existing Metal shared event and retains the transform through the consuming
   submit. This is GPU queue ordering, not a CPU wait. `VKR_TAA_DISABLED` cannot
   disable the jitter needed by this consumer.
4. Scaler creation queries the device's supported factor interval and covers
   the exact rounded input extents at both configured scale endpoints. Required
   formats, usage flags, and native extents are checked when graph images are
   realized, not in the per-frame encode. Graph textures are untracked heap
   resources, so every scaler owns a public `MTLFence` through its `fence`
   property. A target resize first proves the GPU idle, then replaces the
   scaler and its retained fence; scaler and graph resources are released only
   after their normal completion-safe owners permit it.
5. Dynamic resolution is allocation-free and completion-driven. Metal 4 commit
   feedback supplies GPU start/end intervals tagged with the scale that
   produced them. Duplicate submissions and samples from an older tier are
   ignored. An EMA uses alpha `0.2`; three samples above `1.02 * target`
   downshift, 45 below `0.82 * target` upshift, and every transition imposes a
   30-sample cooldown. Regular tiers are 0.05 apart from the configured maximum;
   an exact lower endpoint is retained when it does not lie on that lattice.
   Every scale change resets temporal history before the new extent is used.
6. A zeroed enabled policy defaults to scale range `[0.5, 1.0]` and a
   13.333333 ms GPU-work target. Manifests expose the mode, bounds, and target.
   Workload fingerprints include them. Reports and capture summaries record the
   observed scale, internal width/height, and transition count; summary schema
   version 5 converts older records with scale `1.0` and spatial semantics.
7. `tools/cases/performance/bistro_metal_production_metalfx_dynamic.case.json`
   is the production 75 FPS case. It keeps automatic exposure, bloom, GTAO,
   four 2048-pixel shadow cascades, immediate presentation, and the production
   camera path. Its minimum `0.334` scale stays below the M1 Pro device's 3x
   maximum factor after integer extent rounding.
8. The macOS sample application enables the same policy when it selects Metal:
   initial scale `0.8`, range `[0.334, 1.0]`, and target `13.333333 ms`.
   Selecting Vulkan leaves the zero-initialized spatial policy unchanged.
9. Either Metal validation environment variable makes native MetalFX an
   unsupported cold configuration on the installed wrappers. The renderer
   rejects explicit MetalFX requests before command encoding, preserving
   harness workload identity. The sample application instead selects the
   existing fixed-scale spatial reconstruction and portable TAA path and logs
   that diagnostic substitution. It does not report that path as MetalFX or
   enable dynamic resolution.

## Consequences

- Native Release execution passes at fixed and changing input extents. A
  2560x1440 Bistro capture from an 855x481 input passes the harness and produces
  final color `sha256:cc01f02817d955cb54a0d152077052da27975e864313731759f04736caa2f397`
  (report `sha256:771443f42165c0b3f12c844888c80091c9d1f0cdd7d4a8e7382e3cfc420a09a8`).
  This is structural evidence, not an accepted quality baseline.
- A fixed-scale 1920x1080 Bistro orbit from a 960x540 input reproduces the
  camera-motion warp before exact predecessor selection (report
  `sha256:a35a6399299bc00733160341d6b7920ed9f012dff356b3c2e1ec1d049b7e58d0`).
  The same frames after the correction pass with report
  `sha256:82251bcbd3cba720145d1addfe9b7c19445e78af19e55595d92ad2e207a0af44`.
  A no-history spatial reference at the same checkpoints passes with report
  `sha256:c881cad9f0185508e531d2df0809fe91ec066929645e136f9f41f702b56a5e48`.
  Manual review of frames 4-6 shows that the corrected MetalFX geometry matches
  the reference and that the duplicated facade, spatial gaps, and displaced
  bright region are gone. These are local dirty-tree diagnostics, not an
  accepted moving-image baseline. MetalFX still receives no reactive mask, so
  broader sky, deformation, and disocclusion acceptance remains open.
- The earlier pre-correction dirty-tree five-process Bistro observation covers
  1,500 measured frames at scale `0.334` and 855x481 internal resolution after
  12 controller transitions. Aggregate frame-wall mean is 10.281 ms and p95 is
  12.704 ms. One child reaches 13.595 ms and misses the 13.333 ms target. Report
  digest: `sha256:fad5793a029f8e3af7db2cb01b2d8b9d862493c25bdbbc503bce599caa7c0363`.
  It no longer measures the corrected history-ordering implementation.
- A post-correction local child measures 300 frames while dynamic resolution
  uses scales `0.40-0.45`: frame-wall mean is 11.734 ms and p95 is 14.143 ms.
  The five-repetition parent stopped after the second child because the first
  registered `Shadow.Cascade.1` while the second omitted it, making the pass
  catalogs incompatible. The incomplete report is
  `sha256:056aa1d0c7e691cc16ee56f4ddf1c9e84f889a5d746f58b119aa4d91c2cc1cd6`.
  This is gross-regression evidence only and does not establish solid 75 FPS.
- Both timing observations are non-authoritative because the implementation
  tree is dirty and the local profile is observational. No accepted baseline
  changed.
- The original API-validation assertion, `_outputTextureBarrierStages not
  set`, exposed a VKR contract violation: the graph supplied untracked heap
  textures while leaving the scaler's public fence nil. Assigning a retained
  `MTLFence` removes that assertion. Encoding then reaches a separate Apple
  wrapper defect: MetalFX sends `globalTraceObjectID` to both
  `MTL4DebugComputeCommandEncoder` and
  `MTL4GPUDebugComputeCommandEncoder`, neither of which implements that
  selector on macOS 26.5.2 with MetalFX 31.8. VKR does not use a private
  selector or catch a partially encoded Objective-C exception. API-only,
  GPU-only, and combined validation runs now complete through the explicitly
  logged spatial/portable-TAA diagnostic path; an explicit MetalFX harness
  child exits at initialization with `VKR_RENDERER_ERROR_UNSUPPORTED_INPUT`.
  Validation-disabled Metal 4 execution remains the native MetalFX correctness
  gate until Apple fixes the wrappers.
- Dynamic resolution cannot recover beyond the device's maximum factor. The
  production Bistro path reaches its minimum tier, so additional 75 FPS margin
  requires measured renderer optimization or an explicit quality-policy change.
- MetalFX owns a temporal algorithm that Vulkan does not execute. Its parity
  ledger entry remains **UNALIGNED** by design; shared motion/depth producers
  remain bilateral obligations.

## Alternatives considered

### Replace the portable TAA path everywhere

Rejected. MetalFX is Apple-specific and cannot define Vulkan behavior. The
portable same-resolution resolve remains the default shared consumer and a
useful comparison path.

### Scale the physical target or post-process chain

Rejected. That would lower UI/text and final post resolution, change capture
semantics, and undo ADR-039's output/scene separation.

### Drive resolution from CPU frame time

Rejected. CPU wall time includes scheduling and presentation effects and may
react to work that resolution cannot reduce. The controller uses completed GPU
work tagged with its producing tier and never blocks the frame to obtain it.

### Recreate all viewport resources on every tier change

Rejected. Native-sized MetalFX staging/output textures remain fixed while the
active content rectangle changes. Viewport-domain graph resources already use
completion-safe replacement and retirement when their extent changes.

## Revisit when

- Apple ships Metal 4 debug wrappers that implement the selector used by
  MetalFX temporal encode;
- owner review establishes an accepted Bistro moving-image quality floor;
- repeated clean-tree evidence needs more margin than the device's 3x factor
  permits; or
- Vulkan gains a separately selected temporal upscaler with its own native
  evidence and explicit algorithm identity.
