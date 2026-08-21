---
status: implemented
updated: 2026-08-21
authority: adr
---

# ADR-023: Vulkan 1.4 Bindless Capability Profile with Required Descriptor Buffers

## Status

**Accepted** — the selected production Vulkan strategy enumerates, reports,
selects, and creates a Vulkan 1.4 device through this immutable profile. Its
production offscreen and windowed prepare/submit paths pass deterministic
readback, recursive SPIR-V ABI reflection, synchronization validation, and
descriptor-buffer GPU-assisted validation with layer 1.4.357. The target AMD
Radeon RX 6700 XT exposes the common floor plus core surface, Win32-surface, and
swapchain extensions, but neither surface/swapchain-maintenance extension. The
accepted portable baseline therefore uses per-image present semaphores and a
completed submit that consumes the reacquired image's acquire semaphore as its
presentation-completion proof. Swapchain maintenance remains outside the
profile floor but is now enabled when available to obtain explicit per-image
present fences. The post-V7 native Windows rerun passes focused offscreen and
windowed synchronization validation plus descriptor-buffer GPU-assisted
validation on the same target device. The standalone V0 source and build target
were removed after V7, while its historical evidence and the older layer
1.4.335 GPU-assisted limitation remain recorded in
[the bindless Vulkan backend specification](../bindless-vulkan-backend-spec.md)
§12.

## Context

[ADR-021](021-metal-first-bindless-backend.md) chose Metal 4 as the first native
bindless backend and deferred "modern Vulkan for Windows and Linux" to Stage 6.
[ADR-022](022-gpu-pointer-resource-model.md) established the portable resource
model: generation-handled logical identity, memory placement separated from
buffer addressability, backend-native bindless texture references, and
graph-declared dependencies lowered per backend. Neither ADR fixed which Vulkan
capabilities the future backend requires.

The [umbrella design](../bindless-gpu-pointer-renderer-spec.md) §11 sketched a
profile that preferred `VK_EXT_descriptor_heap` "when SDK/driver support and
shader tooling are mature" and otherwise proposed evaluating
`VK_EXT_descriptor_buffer`. Two things have changed since that wording:

1. `VK_EXT_descriptor_heap` is **absent from the observed SDK 1.4.313 headers**.
   Khronos now documents it as the successor to descriptor buffers, but this
   repository still has no target SDK, driver, validation, debugger, or shader
   evidence for it. It cannot be selected for this profile on a specification
   page alone, so treating it as the preferred implementation leaves the profile
   undecided.
2. The §11 wording lists descriptor buffers' "backend-defined sizes" as a
   drawback. That understates how contained the variance is — see the Decision.

At the time of this decision, the shipping Vulkan 1.2 backend enabled almost
nothing from this space. It requested `VK_API_VERSION_1_2` and turned on only
tessellation shaders and sampler anisotropy, plus opportunistic multi-draw
indirect, indirect first instance, depth-bias clamp, and shader draw parameters.
It enabled no descriptor indexing, buffer device address, timeline semaphores, or dynamic
rendering. There was nothing to extend; the bindless backend required a new
device setup. ADR-026 subsequently removed that Vulkan 1.2 implementation.

One environment fact dominates the consequences. The original local Vulkan
observation was MoltenVK `apiVersion 1.2.296` on an Apple M1 Pro without
`VK_EXT_descriptor_buffer`. After updating to SDK 1.4.357.0, the device reports
Vulkan 1.4.334 and MoltenVK 1.4.1 but still does not expose the required
extension. Whatever profile is chosen, if it requires descriptor buffers then
**the backend cannot run on the development machine at all.** Advertising
Vulkan 1.4 does not satisfy this profile by itself.

## Decision

Define one immutable capability profile, assembled during initialization and
`const` before the backend is exposed, and reject any device that does not
satisfy it with a precise, per-device report. There is no fallback path and no
silent degradation.

### 1. `VK_EXT_descriptor_buffer` is required

Descriptor buffers are the global texture heap. There is no fallback to
descriptor pools, descriptor sets, or per-material descriptor binding. A device
without `descriptorBuffer == VK_TRUE` is rejected.

The reason this is workable despite backend-defined descriptor sizes is one
property, stated here because every other decision depends on it:

> **Backend-defined descriptor sizes never reach the shader.**

The shader declares an unbounded texture array and indexes it with a 32-bit
integer. The driver, not the application, turns that index into a layout-defined
byte address. The application computes byte offsets only when *writing* a
descriptor, host-side. A descriptor size varying across drivers is therefore
contained in the heap layout/writer module and changes zero bytes of shader ABI.

Two corollaries are part of the decision rather than implementation detail:

- **Two descriptor-buffer bindings by policy.** Sampler and resource descriptors
  have distinct usage bits, category binding limits, range limits, and address
  spaces. The profile checks all of those limits independently rather than
  assuming the total binding limit is sufficient.
- **Separate sampled-image and sampler descriptors, never combined ones.**
  `combinedImageSamplerDescriptorSingleArray == VK_FALSE` on some drivers splits
  a combined array into two non-contiguous sub-arrays, making index arithmetic
  driver-dependent. Avoiding combined descriptors removes that variance entirely.
  It also keeps the CPU lowering symmetric with the Metal row, which already
  separates texture references from sampler references.

### 2. Every required entry is queried and rejected against

The required floor is enumerated in the backend specification §3.2. It comprises
buffer device address, 64-bit shader integers, shader draw parameters for the
packet vertex ABI, timeline semaphores, the descriptor-indexing minimum set with
runtime descriptor arrays and non-uniform sampled-image and storage-image
indexing, scalar block layout, host query reset, dynamic rendering,
synchronization2, maintenance4, shader demotion for alpha cutout, maintenance5,
`VK_EXT_descriptor_buffer`, a queue family with graphics, compute and transfer,
and — for windowed targets only — `VK_KHR_surface`, the native platform surface
extension, and `VK_KHR_swapchain`. A submit timeline does not
by itself prove presentation resources may be safely recycled or destroyed.
The baseline therefore sizes render-complete semaphores by actual swapchain
image count and reuses one only after that image is acquired again **and** the
acquire semaphore has been consumed by a successful submit whose timeline value
has completed. Reacquire return alone is not completion proof. After recreation,
the same completed acquire-wait submit from the successor swapchain permits
collection of predecessors retired before it. When supported for a windowed
target, `VK_KHR_swapchain_maintenance1` is enabled and per-image present fences
provide the explicit proof instead; the extension remains outside the required
capability floor.

**No entry is assumed to be guaranteed by the core version.** Several of these
features are promoted into core Vulkan and some are mandatory to support at some
version, but the profile does not depend on that: it queries and rejects against
every entry as though it were optional. This is not defensive padding — the
specification requires that a supported feature still be *enabled* at device
creation, or device creation fails, so the query and the enable are needed
regardless.

This ADR deliberately does **not** claim "Vulkan 1.4 core plus exactly one
extension." That framing was considered and dropped: the specification's
per-version mandatory-support tables could not be retrieved in full when this
was written, and the versions appendix that was retrievable does **not** list
descriptor indexing, its sub-features, or 64-bit shader integers among the
features newly required in 1.4. Asserting the framing would put an unverified
claim into a rationale document.

A limit floor is also rejected against, covering per-stage, per-set, and
combined per-stage resource counts against the configured heap capacities;
push-constant size; bound descriptor sets; total/resource/sampler
descriptor-buffer binding counts; per-buffer descriptor ranges; total descriptor
address space; and explicit `vkGetDescriptorSetLayoutSupport` checks for both
concrete heap layouts.

`descriptorBindingPartiallyBound` and the update-after-bind feature bits are not
part of that floor. Descriptor-buffer bindings already have equivalent
partially-bound/update-after-bind semantics without enabling those features, and
descriptor-buffer layouts cannot use the update-after-bind-pool create flag.
Requiring the redundant bits would narrow the target matrix without changing
the selected behavior.

Those layout-support calls require a logical device, so evaluation has two
phases rather than pretending every check happens during physical-device
selection. First reject on physical extensions, features, limits, queues, and
formats. Then create a candidate device for the highest-ranked viable physical
device, run both concrete layout-support checks, and either retain it and
finalize its immutable profile or destroy it and continue to the next candidate.
Post-create rejection is recorded against that same physical device.

### 3. Rejection is precise and per device

A fixed-capacity, allocation-free report is emitted **per candidate physical
device**, so a two-GPU machine explains both. Each entry names its kind — API
version, instance extension, device extension, feature, limit, queue, format,
device creation, or layout support — its name, whether it was required, whether
it was present, and a detail string. Each device header records the device name
and, from the driver
properties, the driver identifier, name, info string, and conformance version.

That driver group is load-bearing rather than decorative. Descriptor-buffer
defects are driver-version specific, and because this backend is developed
remotely, this is the only field set that makes a Windows bug report actionable
without sitting at the machine. The report is exposed through the harness so a
failing runner emits a machine-readable capability row rather than an opaque
device error.

### 4. Optional capabilities require an explicit policy

An optional performance capability is recorded unconditionally at startup. A
code path may consume it only after a named harness case demonstrates the
improvement. Until then the field exists, is reported, and nothing reads it.
There is no hidden performance fork and no opportunistic enable.

`VK_KHR_unified_image_layouts`, mesh shaders, device-generated commands, shader
objects, graphics pipeline libraries, pipeline binaries, and host image copy are
all in this category, each with its measurement gate recorded in the backend
specification §3.5. The indexed vertex-pulling path stays authoritative per
[ADR-013](013-draw-submission-strategy.md) until a mesh path measures better
under identical conditions.

`VK_KHR_swapchain_maintenance1` is a correctness exception rather than a
performance experiment. A shutdown validation failure demonstrated that
reacquire return alone was insufficient proof for destroying presentation
resources. Windowed devices therefore enable the extension when both its
feature and extension are present and attach a per-image present fence to each
present operation. The portable completed acquire-wait-submit proof remains the
fallback and keeps the extension outside the required floor.

`descriptorBufferCaptureReplay` is the exception that is not a performance
feature: it is enabled in Debug and diagnostic configurations when present so
graphics debuggers can capture, and never in Release, because it constrains
driver allocation.

## Consequences

**Positive**

- The bindless model gets a real global texture heap with 32-bit indices, which
  is what the material row, the draw root, and the shader ABI all assume.
- Descriptor-size variance is contained to one host module and never reaches a
  shader, a material row, or the ABI manifest.
- The Vulkan material row can be denser than Metal's without forcing either
  backend onto the other's representation, which is exactly what ADR-022
  anticipated.
- Rejecting rather than falling back means there is one code path to validate,
  not two, and no untested degraded mode.
- The per-device rejection report makes a remote Windows failure diagnosable
  from its output alone.

**Negative / risks**

- **This excludes MoltenVK, and therefore the development machine.** The backend
  cannot run locally at all. Every runtime gate requires Windows hardware, and if
  none is available the work cannot start. This inverts ADR-021's own premise for
  choosing Metal first — that the most uncertain shader and resource ABI is
  tested on the machine used for daily iteration.
- **Graphics-debugger capture is degraded without
  `descriptorBufferCaptureReplay`.** Losing RenderDoc or PIX on the only platform
  that can run the backend is a sharper practical cost than device coverage, and
  it compounds the remote-development problem.
- **GPU-assisted coverage depends on the validation-layer version.** Khronos
  validation layer 1.4.335 disables all shader instrumentation checks when
  `VK_EXT_descriptor_buffer` is enabled; 1.4.357 executes and passes the V0 path.
  Reports must name the layer version, keep setup adjustments visible, and treat
  an instrumentation self-disable as an unavailable gate rather than clean
  validation evidence.
- Requiring an extension with narrower deployment than descriptor sets means
  older-driver users get a hard rejection with no renderer.
- `VK_EXT_descriptor_heap` is the standardized successor and may become the
  better long-term fit, so a future migration is plausible. Containing
  index-to-descriptor translation in one heap module reduces that migration's
  surface, but shader/layout/capture-tool compatibility still needs evidence.
- The portable WSI path must retain old swapchains until a successor acquire
  semaphore has been consumed by a completed submit. When maintenance1 is
  available, its present fence supplies the explicit proof. The retirement
  history is bounded and reports capacity exhaustion instead of destroying
  under an unproven condition.
- Recording optional capabilities without consuming them means the profile
  carries fields that nothing reads for some time. This is deliberate; the
  alternative is an opportunistic enable that forks behaviour without evidence.

## Alternatives Considered

- **Wait for `VK_EXT_descriptor_heap`.** It is the standardized successor and is
  closer to the source article's raw-heap model. Rejected because it is absent
  from the observed SDK 1.4.313 headers and this project has no target-driver,
  validation, debugger, shader-tooling, or rendered evidence for it; waiting
  would block the backend on an unproven target. Retained as the forward
  migration trigger.
- **Descriptor indexing with conventional descriptor pools as the required
  baseline, descriptor buffers as an opt-in.** The widest driver support, and the
  shader ABI would be identical because indices stay indices. Rejected by owner
  decision in favour of the descriptor-buffer model; recorded because it is the
  natural fallback if descriptor-buffer driver maturity proves inadequate, and
  because it would not change any shader.
- **Per-material descriptor sets, as the retired Vulkan 1.2 backend did.**
  Rejected outright: it is the model the whole bindless direction exists to
  remove, and it would put per-material binding back in the draw loop.
- **Mesh shaders, device-generated commands, and shader objects in the required
  floor.** Rejected because it narrows the hardware matrix furthest, diverges
  most from the proven Metal semantic model, and has no measured justification.
  Each remains available as a capability-gated path after its own measurement.
- **Enable optional capabilities opportunistically when present.** Rejected
  because it creates behaviour that differs by device with no evidence that the
  difference is an improvement, and it makes every bug report ambiguous about
  which path ran.
- **Require `VK_KHR_swapchain_maintenance1` and present fences.** Rejected for
  the baseline because the RX 6700 XT target does not expose the extension.
  Present fences are consumed when available, without making them mandatory.
  The selected portable algorithm does not infer completion from reacquire
  return or an unrelated submit timeline: the successful submit that consumes
  the reacquired image's acquire semaphore must itself complete before the
  prior present is treated as finished. The same proof on a successor image
  permits retired-swapchain collection.
- **Assume the Vulkan 1.4 core version guarantees the floor and skip the
  queries.** Rejected on two grounds: the mandatory-support tables were not
  verifiable when this was written, and a supported feature must be enabled
  explicitly at device creation regardless, so the queries are not optional work.

## Revisit When

- `VK_EXT_descriptor_heap` reaches the target SDK, drivers, validation layers,
  debugger, and shader tooling. The heap module should contain most of the
  implementation change because indices stay indices, but the migration still
  reruns pipeline-layout, reflection, capture, and shader ABI gates.
- Real Windows device data shows the required floor excludes machines the project
  needs to support.
- Descriptor-buffer driver defects or debugger limitations make the development
  loop unworkable, at which point the descriptor-pool baseline above becomes the
  live alternative.
- The specification's Feature Requirements tables are checked directly and either
  confirm or refute which floor entries are guaranteed by core Vulkan 1.4. Record
  what was checked; do not assert it from memory.
- A measured case justifies consuming one of the optional capabilities, at which
  point that capability moves out of the recorded-only set and gets its own
  evidence.
- MoltenVK exposes Vulkan 1.4 with descriptor buffers, restoring a local
  development loop. Currently unlikely.
