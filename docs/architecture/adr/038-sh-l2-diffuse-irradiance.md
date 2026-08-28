---
status: proposed
updated: 2026-08-28
authority: adr
---

# ADR-038: Second-order spherical-harmonic diffuse irradiance

## Status

**Proposed.** No production code or production call site exists. The staged
implementation and evidence gates are defined in
[sh-l2-diffuse-irradiance-spec.md](../../rendering/sh-l2-diffuse-irradiance-spec.md).

This proposal revises only the diffuse half of
[ADR-016](016-hdr-environment-format.md). It does not supersede that decision.
ADR-016 remains the accepted current state until this proposal is implemented
and accepted. Its equirectangular delivery, load-time cube conversion, and
`TextureCube` runtime sampling remain in force for the skybox draw, environment
source, and GGX specular prefilter.

## Context

The renderer stores the shader-facing diffuse environment response as a 64²
RGBA16F cubemap per environment and per local reflection probe. The
`ibl_irradiance` compute kernel
(`shaders/vulkan/slang/ibl/default.slang`, mirrored by
`shaders/metal/msl/ibl/diffuse_convolution.metal`) averages cosine-weighted
samples. This stores `D(n) = E(n) / pi`, where `E` is irradiance. Shading samples
`D` in `packet_environment_terms()`
(`shaders/vulkan/slang/world/default.slang`) and multiplies it directly by
diffuse albedo. Any replacement must preserve this normalization unless the
BRDF multiplication also changes.

Three facts motivate revisiting the representation.

**The bake is stochastic and samples an unfiltered source.** `ibl_irradiance`
integrates 128 cosine-weighted Hammersley taps per texel against mip 0 of a
source cube that is up to 256² and can contain an HDR sun disc. This combination
can produce persistent fireflies in the retained cubemap. No focused capture of
the artifact has been recorded. The implementation spec requires that evidence
before accepting a quality improvement.

**The cost model changed.** The prior SH assessment in
[bistro-baseline-shading-investigation.md](../../rendering/bistro-baseline-shading-investigation.md),
dated 2026-08-05, found no measured win. That assessment predates the GPU-driven
deferred path, which landed on 2026-08-20 (`b2cb363`) and replaced the legacy
forward topology the same day (`009fd4b`). Environment evaluation now runs once
per pixel in the deferred lighting compute pass, alongside G-buffer reads.
Worst-case environment sampling is one global source plus
`VKR_FRAME_IBL_PROBE_MAX` (16) probe samples for diffuse, and the same count for
specular prefiltering: up to 34 `TextureCube` samples per pixel.

The earlier result does not measure this topology in either direction. This
proposal therefore makes no performance claim without a matched deferred-path
measurement.

**The diffuse response is low frequency.** Clamped-cosine convolution has zero
transfer for odd bands above `l = 1`; its nonzero even-band coefficients decay
asymptotically as `l^-5/2`. Ramamoorthi and Hanrahan report low average error for
nine-coefficient reconstructions of their example environment maps. That is an
empirical reconstruction result, not a general claim that L2 contains 99% of
the energy. A 64² by six-face cubemap spends 24,576 texels on a signal that can
often be approximated by nine RGB coefficients, subject to the explicit indoor
probe quality gate below.

## Decision

**Represent the shader-facing diffuse response `D(n) = E(n) / pi` as
second-order spherical harmonics evaluated analytically in the lighting pass.
Retain cubemaps for the environment source, skybox, and specular prefilter.**

Six constraints define the implementation.

1. **Preserve the radiometric convention.** Projection folds the normalized
   clamped-cosine band factors `K0 = 1`, `K1 = 2/3`, and `K2 = 1/4` into the
   stored coefficients. Evaluation returns `D`, not raw irradiance `E`.
   Consequently, a constant source radiance `L` evaluates to `L`, matching the
   current cubemap path and requiring no new `/pi` in shading.

2. **Keep coefficients GPU-resident.** Projection writes a renderer-owned
   storage buffer. Neither packets nor CPU scene records carry coefficient
   values. This avoids readback and keeps the final packet change to a buffer
   address plus slot indices.

3. **Use completion-safe copy-on-write slots.** Slot 0 is an immutable, zeroed
   black sentinel. The pool has 36 reusable slots, enough for two generations
   of the maximum logical live set: retained fallback, active scene
   environment, and 16 probes. Projection writes a free candidate slot. The
   same command stream can consume it only after an explicit barrier, and the
   logical source commits it only after successful queue submission. The prior
   slot retires against its last reader submit serial. Scene reset retires
   slots; it never clears or overwrites storage that a submitted frame may
   read. Exhaustion is an explicit cold-path error and must not introduce a
   successful-frame wait.

4. **Keep the final probe descriptor 64 bytes.** The final Vulkan and Metal
   packet probe records replace their eight-byte irradiance reference at offset
   0 with an SH slot index and reserved `uint`. Fields from offset 8 onward do
   not move. Dual-representation A/B stages use a temporary side table keyed by
   packed packet probe ordinal, because a final probe row cannot carry both
   representations at once. Separate deferred-lighting pipeline variants keep
   representation selection out of the per-pixel and per-probe paths.

5. **Project a bounded, filtered source deterministically.** The pass reduces
   the mip whose face extent is at most 32, clamped to the last available mip,
   and weights texels by exact cubemap solid angle. A single workgroup produces
   a deterministic sum without floating-point atomics. Linear blit mip
   generation is a practical low-pass approximation, not an exact
   solid-angle-preserving filter. The implementation must compare the selected
   mip against a full-resolution reference before this approximation is
   accepted.

6. **Make non-negativity and deringing explicit quality policies.** L2
   truncation can ring and produce negative reconstructed values. Evaluation
   applies `max(D, 0)`. Optional authored windowing is validated at scene load
   and folded into coefficients, keeping the per-pixel path fixed. The clamp is
   not energy preserving, so the evidence records negative-lobe frequency and
   integrated response drift rather than treating it as mathematically exact.

Evaluation uses Sloan's shader-optimized form: seven `float4` values, 112 bytes
per slot, consumed as seven dot products plus fixed vector arithmetic. The pool
therefore occupies 4,144 bytes, including the black sentinel.

## Consequences

**Removed after the evidence gates pass.** The 64² diffuse response cubemap and
its allocation per environment and per probe (196,608 bytes each, up to about
3.3 MB at full probe occupancy); the `ibl_irradiance` and Metal
`diffuse_convolution` kernels and their pipelines; `VKR_IBL_IRRADIANCE_SIZE`;
up to 17 bindless sampled-texture references; and the `irradiance_cubemap`
handle and its retain/release paths. Sampler savings are not counted as 17:
Vulkan uses a canonical shared sampler whose descriptor is released only when
its reference count reaches zero, and Metal uses a constexpr sampler.

**Added.** One fixed-capacity SH slot pool with renderer lifetime; projection
and evaluation kernels for both backends; shared CPU/GPU arithmetic and unit
tests in `vkr_ibl_math.c`; completion-gated slot publication and retirement; a
temporary A/B side table; an indirect-diffuse capture channel; and an authored
deringing scalar.

**Packet ABI.** The final retirement stage advances
`VKR_RENDER_PACKET_VERSION` from 22 to 23. Each backend replaces its 16-byte
frame-root irradiance and retired-BRDF region with an eight-byte SH buffer
address, a global slot index, and a reserved `uint`. The spec defines the
backend-specific fields and offsets. The final 64-byte probe record changes
only at offsets 0 and 4. This breaking change lands with public packet fields,
backend records, shader declarations, and Metal offset assertions in one
stage.

**Synchronization inherits a known graph gap without weakening lifetime.** IBL
bake work remains outside the render graph, as recorded by the ADR-016
implementation spec, `render-graph-design.md`, and
`renderer-architecture-spec.md` §4. Explicit barriers still carry projection
writes to compute and fragment reads. Slot reuse additionally requires proven
GPU completion of its last reader. Declaring IBL bake work to the graph remains
separate work and is not a prerequisite for this representation change.

**Failure is bounded and dark.** Slot 0 evaluates to black.
`VKR_PACKET_FRAME_FLAG_IBL` continues to gate environment lighting against the
constant-ambient fallback. A failed projection does not publish its candidate
slot. Pool exhaustion reports an error at the cold bake/publication boundary;
it does not wait inside a successful frame or expose partial coefficients.

**Quality risk requires owner acceptance.** L2 cannot reproduce a small bright
emitter with a hard terminator. The Bistro café probe is the in-tree risk case:
an authored indoor cubemap dominated by a doorway. Its diffuse response can be
softer than the current cubemap result. A dedicated indirect-diffuse A/B capture
of this probe, including clamp and energy diagnostics, must be accepted before
the cubemap path is removed.

**No performance result is asserted.** The texture-fetch argument is a
mechanism. The spec requires matched Release deferred-lighting timestamps with
0, 1, 4, and 16 probes while both representations still exist in one binary.
The performance gate therefore runs before retirement. If results do not
separate, the accepted record must call the timing neutral and remove any
unsupported speed rationale.

## Alternatives Considered

**Keep the cubemap and fix only the bake.** More samples or a filtered source
mip can reduce stochastic noise without an ABI change. This is the fallback if
the SH quality gate fails. It retains diffuse texture fetches, allocations, and
descriptor references.

**Store raw irradiance.** Using the usual raw clamped-cosine multipliers
`pi`, `2pi/3`, and `pi/4` would require division by `pi` during shading. The
current path already stores `E/pi`; changing both representation and
radiometric convention adds risk with no benefit. Rejected.

**Third-order (L3, 16-coefficient) SH.** Odd bands above `l = 1` vanish under
clamped-cosine convolution. L3 therefore adds seven coefficients that do not
contribute to this diffuse response. Rejected.

**Spherical Gaussians.** These can better represent high-contrast directional
environments, including the café probe. Lobe count and fitting introduce new
authored or bake-time choices, and per-pixel cost scales with lobe count.
Revisit only if measured L2 quality is unacceptable.

**Ambient cube or HL2 basis.** These representations are cheaper and naturally
non-negative, but lower fidelity. Their small arithmetic saving is unlikely to
matter beside G-buffer reads and they do not provide the L2 format expected by
possible probe-volume GI work. Rejected.

**Offline projection.** Runtime projection is still required for probes that
use the runtime-baked scene environment source. Adding an offline path would
duplicate the representation and validation work. Rejected.

## Revisit When

- The indirect-diffuse A/B capture shows that the café probe or another
  authored indoor volume is materially worse and windowing does not recover it.
  Keep the cubemap and improve its bake instead.
- The selected-mip projection cannot meet the full-resolution reference
  tolerance. Project a higher mip or mip 0; do not preserve the 32-texel target
  at the expense of response accuracy.
- Release deferred-lighting timings at 16 probes do not separate. The decision
  may survive for memory and code reduction, but the performance rationale must
  be recorded as neutral.
- Concurrent environment generations exceed the bounded pool. Reassess the
  capacity from measured logical lifetime and submit retirement; do not add a
  hot-path wait or overwrite a live slot.
- Probe-volume or ray-traced diffuse GI changes the underlying storage model.
