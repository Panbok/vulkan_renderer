---
status: implemented
updated: 2026-09-08
authority: adr
---

# ADR-060: Independent screen-space diffuse indirect lighting

## Status

Accepted. Production graph, native commands, and completion-gated history are
integrated. Focused Metal output and lifetime checks pass. Native Vulkan
execution remains unavailable on this host.

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
surface without mixing discontinuous receivers. Temporal acceptance then validates
only completed color/depth/identity tuples against motion, stable identity, depth,
and lighting/resource revisions.

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

Portable TAA and MetalFX may use an immediate in-flight predecessor for shared
motion continuity. SSGI selects only a completed tuple from that same producer.
An older completed SSGI tuple must not replace the reconstruction motion reference.
When no compatible completed tuple exists, SSGI sets history invalid and uses the
current one-ray estimate without waiting or recreating images.

## Consequences

SSGI adds dynamic screen-visible diffuse bounce outside valid baked-volume cells.
It cannot recover off-screen emitters or occluders and does not replace baked
multi-bounce lighting. Low ray count requires spatial and temporal filtering; changed content or a
missing completed tuple can temporarily lose accumulated detail. Storage and
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

The Release wrapper passed after the spatial filter. Generated Vulkan SPIR-V
passed validation and reflects the 288-byte parameter record, five SSGI roots
(304/32/320/368/416 bytes), and the 160-byte deferred root; generated Metal
source reflects matching parameters and 320/320/352/400/448-byte native roots.
See [the retained reflection review](../../assets/verification/renderer-features/ssgi-final-spirv.txt).
Metal API validation proved completed tuple reuse across TAA jitter without image
recreation and correct disable/re-enable/resize behavior in
[the lifecycle record](../../assets/verification/renderer-features/ssgi-lifecycle-api-completed.txt). The
spatial emission capture completed in
[the retained run record](../../assets/verification/renderer-features/ssgi-emission-on-spatial.txt).

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

Native Vulkan execution and bilateral comparison remain unavailable, so SSGI
stays **UNALIGNED** under [ADR-044](044-shader-cross-backend-contract.md).

The MetalFX Bistro camera-sweep regression now preserves all 924,963 captured
motion pixels relative to SSGI disabled. Before the fix, selecting older completed
SSGI transforms approximately doubled motion vectors and distorted reconstruction.
[The regression record](../../assets/verification/renderer-features/screen-effects-stability.txt)
retains commands, report digests, and before/after captures.
