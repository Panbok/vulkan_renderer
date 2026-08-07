---
status: partial
updated: 2026-08-07
authority: adr
---

# ADR-020: Parallel Renderer Implementation Boundary for the Bindless Path

## Status

**Accepted (partial)** — Stages 1–5 implement the walking, memory/lifetime,
material-row, backend-lowered dependency, packet/graph orchestration, and
pipeline-archive slices behind this boundary. The focused renderer exercises
all currently authored pass categories and the production asynchronous capture
lifecycle. The application now selects Metal at initialization, shared loaders
publish assets into it, and the harness has a backend-pinned Metal selector.
The allocator race is fixed and the accepted Metal Bistro-plus-text baseline
passes fresh comparison, completing Gate A evidence. Migration-only shader
fixtures, focused stage validators, and the superseded walking module have been
retired. Deterministic Metal subsystem/ABI behavior is covered by CPU tests;
backend-pinned harness snapshots exercise the production renderer, its
backend-owned shader artifacts, capture output, and API/GPU validation.
The Metal renderer is organized as private same-translation-unit lifecycle
domains rather than a second low-level interface; command-slot waits are
published through the shared metrics boundary without widening the legacy
Vulkan vtable.

## Context

[ADR-001](001-frontend-backend-separation.md) established
`VkrRendererBackendInterface` between the frontend and the Vulkan backend. The
interface has 86 entries and only one implementation. Its known leak is now
concrete: render-pass/target objects, instance descriptor state, vertex binding,
image layouts, and descriptor-write telemetry encode the Vulkan 1.2 renderer's
data model.

The [bindless GPU-address design](../bindless-gpu-pointer-renderer-spec.md)
changes more than API calls. Materials become GPU rows with native texture
references, buffers are reached by shader addresses, graph dependencies lower
differently, and render targets no longer imply persistent render-pass/
framebuffer objects. Implementing Metal by filling the existing table would
reconstruct legacy Vulkan semantics in Metal.

The first draft responded with a second `VkrGpuInterface` of approximately 25
function pointers and a 30-entry warning threshold. That interface had no caller
or implementation. Its size was a guess, and a second one-implementation vtable
would repeat ADR-001's unvalidated-abstraction problem rather than solve it.

At the same time, rewriting the shipping renderer in place would create an
extended period with no trustworthy backend. The Vulkan 1.2 implementation and
its evidence must remain usable until explicit retirement gates are met.

## Decision

Add the bindless path as a parallel renderer implementation selected once at
renderer initialization, while keeping `VkrRendererBackendInterface` unchanged
inside the Vulkan 1.2 implementation.

- Dispatch is resolved at coarse public operations: initialization, resource
  creation/destruction, frame preparation, packet submission, capture/metrics,
  resize, and shutdown. No per-draw/per-dispatch backend-type branch is allowed.
- Metal initially uses backend-private typed modules for device/queue, memory,
  resources, pipelines, recording, submission, capture, and metrics.
- `vkr_gpu.h`, if introduced early, contains only shared typed semantic records
  proven by a real call site. It does not begin as a speculative low-level
  function-pointer table.
- Public generation handles, packet payload semantics/validation, JSON graph
  authoring, dependency topology, scene/ECS/asset CPU data, and harness case
  semantics remain shared where current code or the walking slice proves them
  backend-neutral.
- GPU material rows, shader ABI, texture references, render-target realization,
  barrier commands, presentation, capture/readback, residency, and device-memory
  metrics belong to the selected implementation.
- A cross-backend low-level `VkrGpuInterface` is extracted only after Metal and
  modern Vulkan provide two concrete implementations of the same operation,
  ownership, and completion contract. Shared typed records still require
  multiple real callers before extraction.
- If sharing `renderer_frontend.c` adds branches to its backend call sites,
  split public lifecycle/packet validation from legacy Vulkan orchestration and
  bindless orchestration. Do not grow a branch ladder inside the hot path.

The implementation stages begin with a Metal capability/walking slice, not a
header-only seam. Stage 1 now selects offscreen versus `CAMetalLayer` target
operations once at initialization and contains no per-draw backend branch. That
focused slice is the evidence from which the final module boundary is
compressed. Stage 2 adds Metal-private placement/resource/ring APIs rather than
widening the legacy Vulkan table, and Stage 3 adds a Metal-private immutable
material table. These slices do not accept the application-wide seam by
themselves.

The focused Stage 5 packet renderer now supplies the first coarse
`prepare_frame`/`submit_packet` caller. It consumes shared packet and JSON graph
semantics while keeping Metal render-target realization, dependency encoding,
drawable presentation, readback, residency, and lifetime metrics private. Its
recording loop has no backend-type dispatch and never enters the legacy Vulkan
interface. Its asset-backed vertical resolves generation handles to placement
vertex/index buffers, cubemaps, and immutable material rows. Fourteen precreated
pipelines perform real shadow, skybox, opaque, transmission, blend, picking,
tonemap, editor, UI, copy, and production-size irradiance, prefilter, and BRDF
convolution work. A decoded RGBA16F equirectangular payload is converted to a
mipmapped cube, and a focused PBR fragment consumes the baked outputs. A declared
five-channel capture batch returns aligned color, depth, shadow-layer, and ID
data; cold capture and warm reload exercise the Metal 4 pipeline archive. This
proves the intended orchestration and hot-loop boundary. Version-9 packets now
carry shared directional/IBL controls, the bounded point-light table/grid, and
up to 16 frame-local reflection-probe descriptors,
and the immutable Metal material records
carry the production PBR scalar fields plus distinct base, normal, ORM, and
emissive texture references. Focused captures prove IBL, IBL-disabled
directional and punctual shading, directional shadow sampling, graph-declared
scene-color transmission, tangent-space normal mapping, and alpha cutoff. An opt-in
fixed-capacity Metal 4 timestamp
heap returns one named finite interval per executed pass. The fixed-capacity
Metal-private capture ring now retains request-owned result storage until
explicit release and uses submit-completion values to promote or abandon slots.
Completed Metal intervals now publish into the bounded shared application pass
table. Version-9 packets additionally carry up to 16 frame-local reflection
probe descriptors; Metal lowers them once per frame into native texture-reference
records, normalizes overlap, box-projects specular rays, and preserves global IBL
as fallback. A focused capture proves local-probe influence. Asynchronous
submissions retain completed intervals by submit value for later publication.
Version-9 packets also borrow prepared retained UI/world text geometry, logical
atlas handles, models, font controls, and picking IDs. Shared text systems retain
their shaped CPU geometry; Metal lowers it through dedicated world/UI/picking
pipelines, and exact capture pixels plus both text IDs validate the result.
The application frontend selects this implementation with `--renderer metal`;
its resource publisher resolves shared loader output at the coarse boundary,
and harness child selection remains outside all draw/dispatch loops. Gate A is
accepted; Vulkan 1.2 continues as the default until the separately scoped
default switch is implemented.

## Consequences

**Positive**

- Metal is not forced to emulate descriptor sets, classic render passes,
  framebuffers, or Vulkan layouts.
- The shipping Vulkan 1.2 path stays available and evidence-bearing throughout
  the migration.
- Backend dispatch is outside draw/dispatch loops.
- Shared code is based on observed commonality rather than an operation-count
  target.
- The eventual modern Vulkan implementation can validate which Metal modules
  are genuinely portable before a common low-level interface is accepted.

**Negative / risks**

- There are two renderer orchestrations during migration, with deliberate
  duplication where their GPU data models differ.
- Public resource/capture/metrics operations still need a coarse implementation
  dispatch layer.
- Some currently shared systems may prove semantically legacy-specific even
  though they contain no direct backend calls.
- If feature work lands only on Vulkan 1.2, the bindless path can remain a
  permanent incomplete branch; ADR-021's retirement gates do not solve that
  scheduling risk by themselves.
- Extracting a common Metal/modern-Vulkan lower layer later is a second refactor,
  justified only by the two implementations that make it real.

## Alternatives Considered

- **Implement Metal through `VkrRendererBackendInterface`.** Rejected because
  the table's render-pass, descriptor-instance, vertex-binding, layout, and
  telemetry contracts would make Metal reproduce the legacy renderer model.
- **Create the proposed ~25-entry `VkrGpuInterface` before implementation.**
  Rejected because the count and operations have no representative caller. It
  would be a second hypothetical seam with a magic size limit.
- **Rewrite the shared frontend onto a new seam immediately and adapt Vulkan
  1.2 to it.** Rejected because it couples state-authority, lifetime, shader,
  graph, and backend migration into one patch and puts the only shipping path at
  risk.
- **Duplicate the entire renderer with no shared public contract.** Rejected
  because generation handles, packet semantics, graph authoring, CPU scene data,
  and harness cases already provide real shared value.
- **Wait for modern Vulkan and retain one implementation.** Rejected because
  native Metal is the intended macOS path and the primary development machine
  would otherwise remain behind MoltenVK.

## Revisit When

- The Stage 1 walking renderer reveals that even coarse public dispatch is in a
  measured hot path.
- Stage 5 proves which current frontend modules are genuinely shared and which
  belong to the legacy implementation.
- The modern Vulkan path supplies the second concrete lowering, at which point
  repeated Metal/Vulkan operations can be compressed into a real
  `VkrGpuInterface`.
- Maintaining duplicated orchestration costs more than adapting the legacy
  backend to the proven bindless semantic model.
- The bindless path is abandoned or the Vulkan 1.2 path retires, removing the
  need for parallel implementations.
