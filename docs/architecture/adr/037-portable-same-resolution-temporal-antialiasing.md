---
status: partial
updated: 2026-08-26
authority: adr
---

# ADR-037: Portable same-resolution temporal antialiasing

## Status

**Accepted (partial).** Metal and Vulkan share one same-resolution TAA contract
and resolve algorithm. Vulkan CPU, shader, offscreen capture, and focused
synchronization-validation evidence passes on the RX 6700 XT. Metal host ABI
validation passes on Windows; native Apple shader and runtime validation remain
open.

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
- moving-camera history requires exact temporal index/generation and primitive
  identity, reprojected device depth, and image bounds. A stationary camera
  admits coverage transitions so subpixel silhouettes can accumulate;
- a 3x3 current-color clamp bounds accepted history, including the stationary
  coverage path, while transmission and ordinary-blend luminance change reduces
  its weight through a reactive value computed in the resolve;
- history color, depth, identity, primitive, and transform resources use the
  graph `HISTORY` instance domain. Only a completed producer may be read, and
  the Vulkan implementation selects a completion-safe output independently of
  the active frame slot;
- UI and screen text compose after temporal reconstruction;
- first frame, frame gaps, extent, scene, projection, camera cut, render mode,
  enablement, and explicit invalidation reset accumulation;
- `VKR_TAA_DISABLED=1` disables jitter and history acceptance. The resolve
  performs a passthrough while preserving the authored graph topology; and
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
  needs one safe output.
- Static and rigid geometry have exact rejection inputs. Stationary-camera
  coverage transitions rely on the neighborhood clamp instead of identity and
  depth rejection; moving cameras retain strict rejection.
- Skinned deformation, procedural vertex motion, particles, animated material
  parameters, and exposure discontinuities do not yet have explicit
  motion/reactive producers.
- Sky pixels can accumulate while the camera is stationary. They reject during
  motion because they have no visibility identity.
- The luminance-derived reactive value covers composed transmission and
  ordinary blend, but it is not a substitute for authored material reactivity.
- Metal and Vulkan execute the same algorithm and packet semantics. Backend
  differences are limited to resource binding, synchronization, and clip-space
  lowering.
- The one-sample fallback remains available without maintaining a second graph
  or raster topology.

## Alternatives considered

### FSR 3.1 on Vulkan and MetalFX on Metal

Rejected for the first shipping path. It would establish two algorithm and
quality contracts before the shared motion, identity, reset, and history
lifetime had production evidence.

### Visibility-buffer MSAA

Not considered by this implementation. It requires a separate multisample
resource, edge shading, transparency, picking, HZB, capture, and pipeline
project and does not address shader or specular temporal shimmer.

### One retained history image or frame-slot output

Rejected. A writable retained image can be reused before the GPU completes its
last reader. Mapping Vulkan output directly to one of three active frame slots
also fails when two queued frames retain distinct history inputs and outputs.
The five-instance Vulkan `HISTORY` ring is the minimum completion-safe bound:
`2N - 1` instances for `N = 3` frame slots.

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

Native Apple shader/runtime validation, broader animation/disocclusion clips,
and owner acceptance of changed final-color baselines remain open.

## Revisit when

Revisit the resolve algorithm when internal render scaling is required, authored
reactive inputs cover animated materials and particles, native Metal validation
exposes a portability defect, or matched Release GPU evidence shows that a
vendor temporal implementation provides materially better quality or cost
without splitting the frontend contract.
