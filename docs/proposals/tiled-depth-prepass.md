---
status: proposed
updated: 2026-10-06
authority: proposal
---

# Tiled depth pre-pass

The tiled pipeline's opaque pass lays depth for every visible opaque draw in a
depth-only pre-pass, then shades
([ADR-087](../adr/087-gpu-class-graphics-pipelines.md), decision 5). This
proposal checks two things on the M1 Pro: whether the pre-pass and shading
positions are guaranteed to agree, and whether the pre-pass still pays for
itself now that opaque shading never discards. It is a sequence of Mac
experiments, each with a decision rule, for a session with Metal and the
Bistro tiled cases.

## Current baseline

`Tiled.Opaque` (`vkr_metal_packet_record_tiled_opaque` in
[`vkr_metal_packet_record.inc`](../../renderer/src/metal/internal/vkr_metal_packet_record.inc))
records, in one multisampled render pass:

1. The pre-pass: `tiled_depth_pipeline`, which has no fragment stage, draws
   the opaque buckets (bucket bit 2 clear) with `tiled_prepass_state`
   (`Less`, depth write on). Alpha-tested buckets are skipped.
2. Shading with `tiled_shade_state` (`LessEqual`, depth write on): the opaque
   buckets with fragment variants that never discard, then the alpha-tested
   buckets, which lay their own covered depth.
3. The clear sky (`LessEqual`, no write), then the tile resolve.

The depth states and pipelines are created in
[`vkr_metal_packet_setup.inc`](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc)
(`vkr_metal_packet_create_tiled_pipelines` and the `tiled_*_state` block).
Both raster passes use the same vertex function, `vkr_metal_tiled_vertex` in
[`tiled.metal`](../../renderer/src/shaders/metal/msl/world/tiled.metal), which
decodes the full vertex and computes normal, tangent and lightmap outputs that
the depth-only pipeline does not consume.

Already measured, not to be repeated:

- Adding alpha-tested depth to the pre-pass raised `Tiled.Opaque` from
  8.50 to 9.00 ms median and was not kept (ADR-087).
- The forward prototype without a pre-pass cost 110.15 ms against 81.56 ms
  with one ([tiled-pipeline.md](tiled-pipeline.md#first-prototype-measurements)).
  That shader could discard on every draw, which disables hidden-surface
  removal. Opaque variants no longer discard, so this result does not decide
  the current design.
- In the tile-deferred series, dropping the pre-pass saved 0.7 ms at one
  sample and cost 1.3 ms more at four samples. No forward run without a
  pre-pass exists since the opaque variants stopped discarding.
- On the widest views the pass is geometry-bound: the pre-pass and the
  forward pass both draw every opaque triangle (tiled-pipeline.md).

The desktop pipeline (Vulkan) has no depth pre-pass and needs none.
`VBuffer.Opaque` draws each triangle once, writing depth and a triangle id
with an early-depth fragment for opaque buckets
(`vk_visibility_opaque_fragment` in
[`deferred.slang`](../../renderer/src/shaders/vulkan/slang/world/deferred.slang)),
and material shading runs once per pixel afterwards in compute. Only
alpha-tested draws sample a texture in that pass.

## Defect: no position invariance between the two pipelines

Shading at `LessEqual` assumes the shading pipeline computes the same depth
as the pre-pass for the same vertex. The two are separate pipeline
compilations of one vertex function. `VkrMetalTiledVertexOutput::position`
([`draw.metalh`](../../renderer/src/shaders/metal/msl/common/draw.metalh)) is
not `[[invariant]]`, and the library
([`library.recipe.json`](../../renderer/src/shaders/metal/library.recipe.json))
is compiled without `-fpreserve-invariance`, under Metal's default fast math.
Where the shading depth rounds farther than the pre-pass depth, the opaque
fragment fails its test, the sky fails too, and the sample keeps the clear
colour. No such speck has been reported; the guarantee is missing, not the
image.

## Experiments, in order

Run everything in Release with Metal validation variables unset, on Bistro
only, one GPU process at a time. Do not publish baselines. Record for every
run the command, the report digest and the decisive values in this document;
move accepted decisions into ADR-087 and narrow or remove this proposal.

Commands used by the steps below:

```sh
./build_release.sh

# Image: final colour, scene colour and depth at a static Bistro view.
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION \
  ./build_release/tools/vkr_harness snapshot \
  --case tools/cases/local/tiled_bistro_baked_capture.case.json \
  --profile tools/profiles/local-offscreen.json

# Whole-frame GPU time on the production orbit.
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION \
  ./build_release/tools/vkr_harness profile \
  --case tools/cases/local/tiled_bistro_baked_native.case.json \
  --profile tools/profiles/local-windowed-gpu-submission-single.json

# Per-pass attribution (Tiled.Opaque) on the same orbit.
env -u MTL_DEBUG_LAYER -u MTL_SHADER_VALIDATION \
  ./build_release/tools/vkr_harness profile \
  --case tools/cases/local/tiled_bistro_baked_native.case.json \
  --profile tools/profiles/local-windowed-gpu-single.json
```

Image checks compare captures per pixel. Run each side twice: two runs of the
same build can differ by a few pixels (2026-10-06 on Vulkan: up to 20 of 255
on 5 pixels), so a single before/after pair cannot separate a change from
run-to-run noise. Timing checks alternate the two builds, two runs each, and
use the whole-frame and per-pass reports together, as the ADR-087
measurements do.

### 1. Position invariance

Change: mark `position` in `VkrMetalTiledVertexOutput` `[[invariant]]` and
compile the Metal library with `-fpreserve-invariance`. The bakery passes a
recipe `arguments` array to `metal` (`vkr_bakery_metallib_run` in
[`vkr_bakery_shaders.c`](../../tools/bakery/vkr_bakery_shaders.c)); confirm
the recipe field reaches the compile before relying on it. This is a shader
change: follow `vkr-shaders` and check that no Vulkan-side contract changes.

Accept when: the snapshot shows no change above the same-build noise, and
`Tiled.Opaque` and `gpu.submission` do not regress beyond run spread.
Keep it even at a small cost; it closes a correctness gap.

### 2. Opaque pass without the pre-pass

Change, as an experiment build only: skip the pre-pass loop in
`vkr_metal_packet_record_tiled_opaque`. Opaque shading then lays depth itself
with `LessEqual` and depth write on; hidden-surface removal orders opaque
fragments because the opaque variants never discard. Alpha-tested buckets
still shade after the opaque ones and test against their depth.

Expected image: unchanged, since the same surfaces win the same samples.
Measure at the production four samples.

Decision rule:

- If `gpu.submission` median and p95 both improve beyond run spread with the
  image unchanged, remove the pre-pass, `tiled_depth_pipeline` and
  `tiled_prepass_state`, and record the result in ADR-087 decision 5.
  Experiment 1 then has no second pipeline to agree with; keep it only if
  another pass still depends on equal depth.
- If it regresses or is within spread, keep the pre-pass and continue with
  experiment 3.

### 3. Partial pre-pass (only if experiment 2 loses)

Lay depth only for draws likely to occlude much of the screen: large and near
opaque draws, selected during GPU culling into their own indirect range, so
the pre-pass draws far fewer triangles and shading still benefits where
occlusion is deep. Open choices: the selection metric (projected bounds area
against a threshold, or the nearest N draws), and whether selection costs
more than it saves. Accept on the same image and timing rule as experiment 2,
with the p95 view as the main target.

### 4. Position-only pre-pass vertex function (only if a pre-pass remains)

Give the pre-pass a vertex function that decodes and transforms the position
only, with the same position code as the shading vertex so experiment 1's
invariance still holds. Accept on the experiment 2 rule.

## Acceptance evidence

- Snapshot pairs (two runs per side) for every kept change, with digests.
- Alternated Release `gpu.submission` and pass-timing reports on the M1 Pro
  for every kept change, with median, p95 and spread.
- ADR-087 decision 5 updated with the kept structure and its measurements.
