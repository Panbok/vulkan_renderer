---
status: partial
updated: 2026-08-27
authority: adr
---

# ADR-037: Portable same-resolution temporal antialiasing

## Status

**Accepted (partial).** Metal and Vulkan share one same-resolution TAA contract
and resolve algorithm. Vulkan CPU, shader, offscreen capture, and focused
synchronization-validation evidence passes on the RX 6700 XT. Metal host ABI
validation passes on Windows, and native Metal API/GPU shader validation passes
on the Apple M1 Pro.

This decision does not adopt or prototype multisampling. Visibility-buffer MSAA
remains absent.

## Context

The deferred visibility buffer supplies exact visible-row and primitive
identity, but temporal reconstruction also needs previous rigid transforms,
camera jitter, motion vectors, composition reactivity, reset semantics, and
completion-safe history. Implementing different vendor algorithms first would
split those contracts: FSR 3.1 does not target Metal, while MetalFX does not
provide an equivalent guaranteed same-resolution Native AA mode.

The prior renderer had no temporal lifetime model. A graph `RETAINED` resource
cannot serve as writable history because one physical instance may still be in
use by the GPU. History must instead follow completed frame resources.

## Decision

Use one portable, same-resolution TAA resolve on both selected implementations:

- an eight-sample Halton 2/3 sequence jitters camera-visible opaque, cutout,
  transmission, and ordinary-blend rendering;
- culling, shadow projections, HZB occlusion, and picking remain unjittered;
- G-buffer reconstruction uses the jittered projection, while motion uses
  unjittered current and selected-history view-projection matrices;
- one 32,768-entry completion-protected transform table stores rigid model
  matrices with stable temporal index, object generation, and source frame;
- motion is previous UV minus current UV, and the resolve adds it to current UV.
  Both positions already use the unjittered output grid, so applying a second
  jitter delta would double-count sample motion;
- opaque/cutout G-buffer resolve, transmission layer-0 shading, and
  ordinary-blend MRT rendering emit the visible rigid surface's own motion.
  Transmission retains its vbuffer identity and depth. Ordinary blend overlays
  bounded exact index/generation/primitive identity into the existing vbuffer;
  an unencodable surface writes an invalid-foreground sentinel instead of
  aliasing history. Blend validates identity and primitive but not current
  transparent depth because the reused resources have no spare channel;
- moving-camera history searches all four texels in the bilinear history-color
  footprint. Opaque and transmission require exact temporal identity, primitive,
  reprojected device depth, and image bounds; blend requires the exact identity,
  primitive, and bounds available from its overlay. History color is reconstructed
  only from matching texels, with bilinear weights renormalized after rejected
  surfaces are removed; a fully matching footprint retains the hardware bilinear
  path. A stationary camera admits coverage transitions so subpixel silhouettes
  can accumulate;
- a 3x3 current-color clamp bounds accepted history, including the stationary
  coverage path. PBR materials author `temporal_reactivity` in `[0,1]`;
  transparent validity carries it into resolve, where it remains active at
  rest. During camera motion, luminance divergence still provides a fallback
  reactive value capped at `0.75`;
- accepted pixels with a stationary camera and less than `0.01` pixel of
  surface motion retain `0.99` of history before reactivity. Other pixels keep
  the responsive `0.9` retention and motion attenuation. This suppresses the
  periodic residual of the eight-phase jitter sequence without applying the
  slow path to moving geometry;
- history color, depth, identity, primitive, and transform resources use the
  graph `HISTORY` instance domain. Only a completed producer may be read, and
  the Vulkan implementation selects a completion-safe output independently of
  the active frame slot. Metal gives every `HISTORY` resource `N + 2`
  instances for `N` command slots and selects the least-recently-used completed
  instance across all declared history images and buffers. The temporal and
  HZB readers may select different completed producers without coupling the
  next output to a command slot or adding a CPU/GPU wait;
- the existing final tonemap/composite draw applies edge-selective FXAA with a
  cardinal-neighborhood subpixel blend in tonemapped linear output space; UI
  and screen text compose afterward;
- manual exposure remains outside temporal storage. `Temporal.Resolve` reads
  and writes scene-linear HDR; `Post.Tonemap` applies exposure afterward. A
  manual exposure step therefore requires neither history rescaling nor reset;
  any future pre-exposure path must add one at its new domain boundary;
- first frame, frame gaps, extent, scene, projection, camera cut, render mode,
  enablement, and explicit invalidation reset accumulation;
- `VKR_TAA_DISABLED=1` disables jitter and history acceptance. The resolve
  performs a passthrough while preserving the authored graph topology.
  `VKR_FXAA_DISABLED=1` selects the center sample in the existing final draw;
  it does not add or remove a graph pass; and
- render modes `temporal_motion` and `temporal_history` expose motion validity
  and history acceptance through the harness and application debug cycle.

The renderer owns derived temporal packet fields. Packet producers provide
unjittered camera state, monotonic frame identity, scene generation, and stable
object identity; they do not author backend history resources.

## Consequences

- Every displayed world frame gains a full-resolution resolve and four
  full-resolution history image types. Vulkan uses five physical instances of
  each `HISTORY` image or buffer: with three frame slots, two queued frames can
  retain distinct input and output instances while the recording frame still
  needs one safe output. Metal uses `N + 2` instances: the `N - 1` submitted
  command slots, one completed temporal input, and one completed HZB input can
  occupy `N + 1` distinct instances while the recording frame still needs a
  completion-safe output. The current two-slot Metal configuration therefore
  uses four instances instead of two.
- Static and rigid geometry have exact rejection inputs. Transmission and
  ordinary blend now use their own rigid motion instead of opaque-background
  motion. Moving-camera rejection validates the complete bilinear history
  footprint rather than one integer metadata texel, and the accepted metadata
  mask also filters the color gather. Stationary-camera coverage transitions rely
  on the neighborhood clamp instead of identity and depth rejection.
- Skinned deformation, procedural vertex motion, particles, and dynamic
  material-change signals still lack explicit motion/reactive producers.
- Sky pixels can accumulate while the camera is stationary. They reject during
  motion because they have no visibility identity.
- Authored material reactivity is independent of camera motion. The
  composition-derived luminance path remains only a conservative
  moving-camera fallback.
- Stationary zero-motion pixels converge more slowly after a source-color
  change. Moving geometry, camera motion, authored reactivity, and normal
  history rejection retain the responsive path.
- Manual exposure discontinuities are safe by ordering, not by a reset:
  completion-protected history remains scene-linear HDR.
- FXAA consumes directional and cardinal-neighborhood samples in the existing
  final draw. It adds no full-resolution pass, graph resource, or transient
  image and remains after temporal reconstruction but before UI.
- Metal and Vulkan execute the same temporal and FXAA algorithms. Backend
  differences are limited to resource binding, synchronization, and clip-space
  lowering.
- The one-sample temporal and center-sample FXAA fallbacks remain available
  without maintaining a second graph or raster topology.

## Alternatives considered

### FSR 3.1 on Vulkan and MetalFX on Metal

Rejected for the first shipping path. It would establish two algorithm and
quality contracts before the shared motion, identity, reset, and history
lifetime had production evidence.

### Visibility-buffer MSAA

Not considered by this implementation. It requires a separate multisample
resource, edge shading, transparency, picking, HZB, capture, and pipeline
project and does not address shader or specular temporal shimmer.

### One retained history image or command-slot output

Rejected. A writable retained image can be reused before the GPU completes its
last reader. Mapping Vulkan output directly to one of three active frame slots
also fails when two queued frames retain distinct history inputs and outputs.
The five-instance Vulkan `HISTORY` ring is the minimum completion-safe bound:
`2N - 1` instances for `N = 3` frame slots.

The same rejection applies to Metal, but its bounded submission model has a
different minimum. `N - 1` in-flight outputs plus distinct completed temporal
and HZB inputs can occupy `N + 1` instances. A new output therefore requires
`N + 2`; tying the output to the active command-slot index intermittently found
every alternative still in use and restarted temporal accumulation.

## Evidence

Windows RX 6700 XT evidence on 2026-08-26:

- `cmd.exe /c .\\build_test.bat` passes the complete CPU suite and Vulkan
  shader build, including temporal state, graph, packet ABI, Metal host ABI, and
  harness render-mode tests.
- `build_debug/tools/vkr_harness.exe profile --case
  tools/cases/local/p20_vulkan_state_matrix_validation.case.json --profile
  tools/profiles/validation-windowed.json` passes two repetitions with Khronos
  synchronization validation. Final review report digest:
  `sha256:fabbb7694fffa5e504fa72fa033765f5571a68a3b2c2a3471495d36e849966c2`.
- Three held 1600x1200 Bistro recorded-camera cases pass with TAA enabled and
  disabled. Enabled report digests for views 0, 4, and 8 are
  `sha256:ec533fabdbc643e811289774b814f2ac2820ad2860bf170da11878ca38ffe8ca`,
  `sha256:3d4cb65d3d484860ab44f870d2d6aa89d83447e5379d4f688022dae4385f4426`,
  and
  `sha256:48a2f1ffeacca8567b1dc013cdf28cb982b01dfb4e72f56ec7e148de7965a7c5`.
  Disabled report digests are
  `sha256:0c06dab5d2553168dbafe0c46e2994bf1fadc4574a4f68ad21fe6419b4e70f81`,
  `sha256:ccacedf62f606d28b502b1c9248b1b0b3bf690a62c35019dcadab924cdadb725`,
  and
  `sha256:0f414fe59b8dd13719ee1f2a9341ab3249d5833dbdd71b9609bbb4861a746efb`.
  Local visual inspection shows reduced stair-stepping on roof silhouettes,
  balcony rails, lamps, foliage, chairs, and table edges without broad
  whole-frame blur. Enabled stationary history debug captures accept all
  1,920,000 pixels after warmup. Each channel is an independently replayed
  Bistro process, so asynchronous asset-ready source-frame differences prevent
  treating these stills as a pixel-delta oracle.
- The focused 640x480 slow-orbit case passes with strict moving-camera
  rejection: 70,180 of 307,200 pixels accept history. Report digest:
  `sha256:d446a6bd45c10192e0ff0615d38b721e3fd85a01defb611c0d03dfdded335502`.
- Matched dirty-tree Release observations over 74 state-matrix samples report
  `Temporal.Resolve.Fullscreen` at 0.085050 ms mean enabled and 0.057834 ms
  passthrough, an incremental 0.027216 ms at 640x480.
  `Temporal.TransformHistory` adds 0.000648 ms mean. Enabled and disabled
  report digests are
  `sha256:639a1d95ef00c3e67633c13a1200ef52cd6f3b1701c1d3f7bcde84cad0263fce`
  and
  `sha256:a0d08c79602b17c21dfaef01785638587fd9c8db5e2ebf1312039d57f1b69a8a`.
  These local runs isolate incremental pass cost; they are not authoritative
  whole-frame performance evidence.
- Cold and warm Release application launches against one fresh explicit
  `VKR_PIPELINE_CACHE_PATH` both exit cleanly and save a 273,712-byte
  production Vulkan cache.

Follow-up transparency and post-TAA edge evidence on the same host:

- `cmd.exe /c .\build_test.bat` again passes the complete CPU, shader, graph,
  Vulkan ABI, Metal host ABI, and harness suite.
- The focused Debug synchronization-validation profile passes two repetitions
  with no validation/VUID events and all three overflow/invalid-resolve
  assertions at zero. Report digest:
  `sha256:1941ddd35291371b3264af1b389ecd8f00a3b0ac5a0af3d15cdec70dea4dc677`.
- Matched 1600x1200 Bistro interior captures with FXAA enabled and bypassed have
  report digests
  `sha256:914bf364a5384e5951234ec1f25d21efcb2a3ef79e8ced54a325e7b1a54caa23`
  and
  `sha256:b4c5bb21aacbf65690abac3ad18ce9892b829bb7a748cab9c8873f2e25df9551`.
  Nearest-neighbor inspection shows smoother high-contrast pendant silhouettes
  while chair, table, and glass detail remains materially sharp. The full-frame
  mean absolute RGB delta is `0.3066/255`; this is matched composition evidence,
  not a temporal oracle.
- A held exterior capture retains lantern, window-mullion, roof-antenna, and
  facade detail. Report digest:
  `sha256:db7cc8f3ae04fb0fd91189fd3c6d6c1493034d7381841e7992095305fa098f1c`.
  A slow interior translation retains strict disocclusion rejection while
  exercising pendants and glassware. Report digest:
  `sha256:3aa3ba26d5bd2c93901c43d0aea5d795c28507c6278a8bb11290ab7a5a6afcab`.
- Three independent 120-frame Release timestamp-on observations per mode at
  1600x1200 measure `Post.Tonemap.Fullscreen` at `0.4641 ms` mean with FXAA and
  `0.4126 ms` bypassed: `+0.0515 ms`. Mean p95 is `0.4740 ms` versus
  `0.4237 ms`: `+0.0503 ms`. These dirty-tree local runs isolate final-pass
  cost; they are not authoritative whole-frame performance evidence.
- Cold and warm Release application launches against one fresh
  `VKR_PIPELINE_CACHE_PATH` both exit cleanly and save a 276,412-byte
  production Vulkan cache.

Distance-dependent emissive and minified-edge follow-up on the same host:

- A same-position 35/70/110-degree FOV sweep isolates projected footprint from
  world-space lighting. Static history accepts every pixel at all three sizes;
  the bright pendants are opaque emissive geometry in the `unlit` channel.
- In an identical slow 1600x1200 translation, complete bilinear-footprint
  metadata validation reduces exact red history-rejection pixels from
  235,986/1,920,000 (12.291%) to 54,761/1,920,000 (2.852%). The corrected
  report digest is
  `sha256:b4f9b8522a696ae2a48920557725797c56a10c5359f609fadb848682338e8010`.
- A fixed far pendant crop has adjacent-luma mean-square edge energy 96.495
  with the cardinal subpixel refinement versus 152.128 with FXAA bypassed, a
  36.6% reduction. This is matched composition evidence, not a perceptual
  metric.
- Three independent 120-frame Release timestamp-on observations before and
  after at 1600x1200 measure `Temporal.Resolve.Fullscreen` p50 means
  0.39631 to 0.42995 ms (+0.03364 ms, +8.49%) and
  `Post.Tonemap.Fullscreen` p50 means 0.50836 to 0.58740 ms
  (+0.07904 ms, +15.55%). These dirty-tree local pass measurements make no
  whole-frame claim.
- `cmd.exe /c build_test.bat` and the final Debug build pass. The two-repetition
  focused synchronization-validation profile passes with report digest
  `sha256:fc4b7b5b88ee72e63ecebe218e9e086e182366f7ae33d65c25cec874f1f56ebe`.
  Cold and warm Release launches both exit cleanly against one 277,816-byte
  production Vulkan cache.

The transparent-input follow-up adds:

- Debug `temporal_motion` snapshots show nonzero, surface-shaped motion validity
  for layered transmission and ordinary blend on the RX 6700 XT. Reports
  `20260826T161445.433Z-002484` and `20260826T161548.652Z-001fbd` pass with
  digests `sha256:9df6684c1d367be3e0897f2d66225067238a35774886c1c20977d5fed25758a7`
  and `sha256:ccb5a7c365cd4cc4ed41ebed5826e1e404b37a1718c5758c1fb7c8580e877be3`.
- The complete CPU/shader suite passes, including material-authoring default,
  clamp, and explicit-value coverage. A two-repetition Debug synchronization
  validation profile passes without a validation message: report
  `20260826T162548.886Z-001ccb`, digest
  `sha256:6871a28e71b17f53bc338f51d2633f80a73243ac5d0e4fd7c25aa523c6a045ed`.
- Two matched 320x240, two-process, 120-frame Release timestamp-on observations
  measure `World.Blend.Fullscreen` from 0.03986 to 0.04654 ms mean
  (+0.00668 ms, +16.76%) and `Temporal.Resolve.Fullscreen` from 0.04425 to
  0.04517 ms mean (+0.00092 ms, +2.08%). Environment, workload, and policy
  fingerprints match. Before report `20260826T161757.111Z-0026a6`, digest
  `sha256:371237a9ff522d88376921156f7ff385ed621f45475d2e66a1fcd882b4f5ef73`;
  after report `20260826T162209.513Z-001ea8`, digest
  `sha256:ab7aa35ebb4597ab50de26d8c8e0d1eb4eaa4f4d7cccb7cf3a16e82cd19395d9`.
  Dirty-tree local measurements support only per-pass cost, not a whole-frame
  performance claim.
- Release cold and warm production pipeline-cache profiles pass. Cold report
  `20260826T162620.515Z-004346`, digest
  `sha256:0e5da67dc8640a9850b1721f5d074a9254ca3bbea140ebe99e816db9b343e13b`;
  warm report `20260826T162634.357Z-00280c`, digest
  `sha256:03251df14acdc3c3805babc64525b7afaeabccebde0743ee963a4ed4848a6afe`.

Native Apple evidence on 2026-08-26 adds:

- `./build.sh Debug` and `./build.sh Release` pass after correcting the native
  temporal root layout and the alpha-blend vertex-root binding contract.
- `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1
  build_debug/tools/vkr_harness profile --case
  tools/cases/local/p20_metal_state_matrix_validation.case.json --profile
  tools/profiles/local-metal-dual-validation-serial.json` passes one exclusive
  Apple M1 Pro repetition. `Temporal.TransformHistory` and
  `Temporal.Resolve.Fullscreen` each execute for all six measured frames, all
  11 state-matrix assertions pass, and the validation log contains no Metal
  diagnostic beyond the two enablement notices. Report digest:
  `sha256:5f5c4e0b7422c9f8c66775cd94f5295fe9a302071de31939a0dc79181382d15c`.
  This local dirty-tree diagnostic is correctness evidence, not performance or
  baseline evidence.
- Cold and warm Release application launches against one fresh explicit
  `VKR_PIPELINE_CACHE_PATH` both exit cleanly with the same non-empty
  1,660,656-byte Metal archive.
- The native `./build_test.sh` build completes, but the runner does not reach
  the late Metal host-ABI group: it reproducibly stops first in the untouched
  `test_tangent_generation_repairs_parallel_accumulation` mesh test. The native
  pipeline-reflection gate above covers the repaired Metal ABI directly, while
  the complete Windows CPU/shader suite remains the host-contract evidence.

The 2026-08-27 stabilization follow-up adds:

- Metal decouples every graph `HISTORY` image and buffer from command-slot
  ownership. An `N + 2` pool chooses the least-recently-used completion-safe
  output, while temporal and HZB readers independently choose their newest
  completed producers. Debug and Release builds pass. A focused single-process
  moving-camera snapshot passes Metal API and GPU/shader validation, digest
  `sha256:8ae74c51813b2b40beffbdb032c6020dc9b56bbddb29ea22b20746a10fb09a1f`.
  The owner confirmed that this lifetime fix did not change the reported visual
  jitter, so its independent-replay capture remains correctness evidence only.
- A fixed-camera channel split shows stable deferred emissive and full history
  acceptance. The missing input was the normal-footprint roughness filter used
  by forward shading. Metal and Vulkan now apply the same bounded two-axis
  normal variance before deferred analytic and environment specular lighting.
  The owner confirms that this materially reduces the shimmer. A larger
  coefficient and a punctual roughness floor produced no visible change and
  were reverted. Per-light angular widening and inverse-square specular
  attenuation were removed during final review because no causal owner evidence
  justified their broader lighting changes or per-pixel cost.
- Accepted stationary pixels below `0.01` pixel of surface motion now retain
  `0.99` history; the owner reports that this makes the fixed-camera result
  nearly stable. Moving-camera source review found that metadata admitted one
  matching texel while ordinary bilinear color still included rejected
  neighbors. The moving path now filters color with the exact
  identity/primitive/depth mask and renormalizes partial bilinear weights.
  Fully valid footprints and the stationary path retain hardware bilinear
  sampling.
- Final Debug and Release builds pass. A focused single-process Metal API/GPU
  shader-validation snapshot passes without a diagnostic, digest
  `sha256:a4cda1da5c8b0be57521aeb75a1dda0d3817c078a195f2b3caa2c33abe125bbc`.
  Cold and warm launches reuse one non-empty 1,684,736-byte archive. A matched
  local moving-camera observation records `Temporal.Resolve.Fullscreen` at
  1.3505 ms mean and 1.4261 ms p95 before, versus 1.3840 ms and 1.4441 ms after.
  The dirty worktree makes this a bounded local cost observation, not a
  performance result. Interactive moving-camera owner acceptance remains open.

Deformation/procedural/particle motion, dynamic material-change signals,
broader animation/disocclusion clips, and owner acceptance of changed
final-color baselines remain open.

## Revisit when

Revisit the resolve algorithm when internal render scaling is required,
authored reactive inputs cover animated materials and particles, or matched
Release GPU evidence shows that a vendor temporal implementation provides
materially better quality or cost without splitting the frontend contract.
