---
status: implemented
updated: 2026-09-09
authority: adr
---

# ADR-060: Independent screen-space diffuse indirect lighting

## Status

Accepted. Production graph, native commands and completion-protected history
ownership are integrated. Current Metal captures cover TAA, MetalFX, camera
movement and no-TAA; a small resize passes Metal API validation. Native Vulkan execution remains
unavailable on this host.

## Context

Baked diffuse volumes provide static multi-bounce lighting but cannot follow all
dynamic emission or lighting changes. Screen-space diffuse tracing can supplement
uncovered regions, with explicit limits on off-screen information and work.

## Decision

Offer SSGI as an optional mode, disabled by default and independent of SSR and
final TAA. Trace at half resolution with one cosine-weighted ray per receiver
and at most 24 hierarchy decisions. A separate current-frame R32F depth hierarchy
provides visibility. Deferred lighting writes a full-resolution RGBA16F source
containing direct punctual/rectangle light and emission, excluding environment,
probe and baked-volume indirect light, SSR, fog and post-processing.

The graph owns six image families: the direct source, half-resolution depth
pyramid, half-resolution raw RGBA16F radiance, and a retained color/depth/identity
tuple in RGBA16F, R32F and RG32UI. Current images have one instance per frame slot;
each history member has frame-slot count plus two instances. Three frame slots
require 24 images and 51.855 MiB at 1280×720, before allocator alignment.
This corrects the earlier 29 MiB estimate, which undercounted both the full-size
source and history instances. The user approved the corrected budget.

Reconstruct the nearest covered receiver within each half-resolution footprint;
no additional receiver image is allocated. Each valid receiver traces one
cosine-weighted direction from a deterministic 256-phase Hammersley sequence.
A valid screen miss writes zero radiance and remains part of the estimator.

The temporal pass applies a 3×3 raw spatial filter before history clamping. It
reconstructs each nearest covered neighbor, rejects depth or normal discontinuities,
and uses compact bilateral weights; valid misses remain zero-valued samples in
the normalized average. This preserves constant radiance on a locally continuous
surface without mixing discontinuous receivers.

History is the color/depth/identity tuple matching the transform that supplies
motion: its instance, submit value, frame, scene generation,
lighting/resource revisions, dimensions, and retained shadow state must match.
Metal uses its existing submission-event wait; Vulkan uses its existing same-queue
image barriers. These order the shared predecessor before CPU-observed completion.
Other producers must already be complete.
The temporal pass reprojects the unjittered motion coordinate onto the raw grid by
adding the producer's previous-minus-current raster jitter. FSR derives that
jitter from its active upscale sequence length; non-FSR paths, including MetalFX,
use the fixed eight-phase sequence. No-TAA leaves the offsets zero.

Four bilinear history color taps independently validate depth and stable identity.
Their RGB values resolve with bilinear weight multiplied by each tap's history
confidence. This changes one color/depth/identity lookup into four, adding nine
history texture accesses per temporal invocation without new images or rays.

Composite the diffuse residual after deferred lighting and before SSR. Apply
receiver diffuse energy, albedo and material AO once. Do not apply GTAO visibility
or multi-bounce compensation to this new traced path. An exact valid baked-volume
cell suppresses the SSGI contribution; the existing bake remains authoritative
there. SSR can then reflect the completed opaque diffuse lighting.

## Ownership and lifetime

The public frame borrows an enable flag. Renderer preparation owns normalized
quality controls, the 256-phase index, and temporal invalidation. The graph owns
image dependencies; Metal and Vulkan realize storage and retain independent
completion-proven history tuples. Failed or canceled submissions do not publish
new history.

Portable TAA, FSR and MetalFX select the immediate transform predecessor for
motion continuity. SSGI consumes its exact matching tuple through the existing
queue dependency, even when the GPU has not completed it. An older completed
tuple cannot replace that motion reference. When no compatible predecessor exists,
SSGI sets history invalid and uses the current one-ray estimate without waiting or
recreating images.

## Consequences

SSGI adds dynamic screen-visible diffuse bounce outside valid baked-volume cells.
It cannot recover off-screen emitters or occluders and does not replace baked
multi-bounce lighting. Low ray count requires spatial and temporal filtering; changed content or a
missing compatible predecessor can temporarily lose accumulated detail. Storage and
traversal costs are explicit and remain optional.

## Alternatives considered

Requiring SSR and sharing its hierarchy saves storage but couples the two features.
Tracing the final opaque HDR would bounce existing indirect light again. Lowering
source precision or resolution could reduce storage, but those quality changes
are outside the accepted first scope.

## Revisit when

Native measurements require a lower budget, or dynamic bounce inside valid baked
coverage needs a different composition policy.

## Implementation and verification

[Shared controls and GPU parameters](../../renderer/src/vkr_ssgi.h), the
[graph declarations](../../assets/render_graphs/main.rendergraph.json), and the
five native phases are integrated: depth base/mips, trace, temporal, and
composite. Deferred lighting writes the isolated direct/emissive source only
when SSGI is enabled.

`VkrSsgiParams` remains 288 bytes. Its former unused tail at bytes 280 and 284
is `history_jitter_uv_x/y`; no root or image allocation changes. The Metal and
Vulkan temporal shaders implement the same four-tap, confidence-weighted
reconstruction and exact-predecessor contract.

Earlier Release, reflection and Metal API records cover the preceding
completed-history implementation. The current revision passes Release app/editor
builds, all ten SSR/SSGI SPIR-V validation checks, compiled parameter reflection,
Vulkan host syntax and a serial Metal API resize check. Current Bistro captures
reduce excess displayed variation above the SSGI-off control by 86.6% in the
reported view; a no-TAA emission fixture also runs successfully.
[The correction record](../../assets/verification/renderer-features/ssgi-ssr-history-correction.txt)
retains commands, hashes and limits; native Vulkan remains unavailable.
The existing source-isolation, emission and
baked-volume observations remain evidence for the SSGI source and composite
policy, not for this temporal change.

The source-isolation capture has 98,304 byte-identical HDR pixels with SSGI
on/off. The emissive fixture gains red bounce at 2,853 non-emissive opaque
pixels. A valid enclosing baked-volume cell suppresses that contribution at
all 39,722 covered samples, producing byte-identical on/off HDR. The same
geometry without the volume retains the visible bounce. Editor composition
uses its own `scene_pre_transmission` target; a 257×193 editor capture passes.

The serialized Release Bistro observations at 1280×720 on Apple M1 Pro used
24 warmup and 16 measured frames, with TAA, SSR and GTAO enabled. The added
SSGI passes total 2.594 ms mean: trace 0.720 ms, spatial/temporal 0.712 ms,
composite 1.000 ms, and depth construction 0.162 ms. Live graph storage grows
by 18 images and 39,321,304 bytes (37.50 MiB) with the current two frame slots.
These are single-process local observations from a dirty tree, not an
authoritative performance comparison. The profile command was
`./build_release/tools/vkr_harness profile --case tools/cases/local/ssgi_bistro_on_profile_local.case.json --profile tools/profiles/local-offscreen-gpu-single.json`
with graphics validation variables unset; the paired off case changes only
SSGI. [The measurement record](../../assets/verification/renderer-features/ssgi-cost-numeric.txt) retains
both run identities and distributions.

Native Vulkan execution now passes on Windows with an AMD Radeon RX 6700 XT,
driver 26.6.3 and Vulkan API 1.4.315. The reported device loss exposed three
independent defects: sampled graph slots were indexed through storage descriptor
arrays, the variable trace loop needed an explicit `[loop]` policy for the AMD
compiler, and the composite pass allocated but did not populate its frame root
before reading material rows. The Vulkan shader now uses typed sampled arrays,
preserves the authored 24-step bound with explicit loop control, and the host
fills the composite root before dispatch. SSR composite used the same root
pattern and is corrected by the same ownership rule.

The Release emission on/off reports are
`20260912T103829.989Z-000bd8` (`6980163ae7bcc5a7cfe9d93c42cbbc26c6154b05ebd448fdc423e2b6eed6dd62`)
and `20260912T103835.198Z-001187`
(`21858b94498b2fe0d265a0ef6b0c0d2a172acef8d45ef8ebb56fa8fa04667ba6`).
The Release Bistro report is `20260912T103625.242Z-002637`
(`db8d1d6000d6407206037b0f718d145181c0e2e7b55bbe312211767fecc2e93c`).
Focused Debug synchronization validation is
`20260912T111516.304Z-004249`
(`8368380187d0358eadf810dfe0b0a1e187db988ade6933ac00da749dc2018ac1`).
The child loaded Khronos validation and reported no API or synchronization
errors. These dirty-tree local reports establish bounded native Vulkan
execution, not authoritative acceptance or pixel parity. Same-revision Metal
execution and bilateral comparison remain unavailable on this host, so SSGI
stays **UNALIGNED** under [ADR-044](044-shader-cross-backend-contract.md).

The retained MetalFX Bistro camera-sweep record preserves all 924,963 captured
motion pixels relative to SSGI disabled after the earlier motion-producer fix.
[The regression record](../../assets/verification/renderer-features/screen-effects-stability.txt)
predates synchronized selection and four-tap validation, so it does not establish
the current temporal contract.
