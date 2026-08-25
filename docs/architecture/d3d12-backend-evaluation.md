---
status: proposed
updated: 2026-08-25
authority: design
---
# D3D12 backend evaluation

**Document status:** Evaluation and recommendation. No D3D12 decision or
production code exists.

**Scope:** Whether a third `VkrRendererImpl` is justified, what the current
coarse seam can reuse, and which claimed benefits survive a correctness review.

**Related:** [ADR-023](adr/023-vulkan-1-4-bindless-capability-profile.md),
[ADR-024](adr/024-shared-bindless-gpu-cores.md), and
[ADR-025](adr/025-selected-renderer-implementation-strategy.md).

## 1. Recommendation

Do not schedule D3D12 for image quality. It does not fix Windows DPI, output
transfer, temporal inputs, AA, exposure, bloom, or AO.

Do not schedule it for vague "driver stability" either. A second Windows API
gives an alternate runtime and debugging path. It does not guarantee fewer
driver defects, and it doubles the Windows GPU validation matrix.

Revisit D3D12 when one concrete requirement is recorded:

- a required feature or deployment target is blocked on the Vulkan path;
- PIX capture is needed for a problem that RenderDoc and Radeon GPU Profiler
  cannot diagnose;
- WARP-backed software correctness testing has enough value to justify a full
  renderer implementation;
- a product requirement mandates D3D12; or
- backend breadth is an explicit portfolio goal after the current validation
  gaps have narrowed.

If accepted, the first milestone must reproduce the existing renderer. Do not
bundle sampler feedback, DirectStorage, ray tracing, mesh shaders, VRS, or frame
generation into backend bring-up.

## 2. Seam readiness

The coarse seam is suitable for a third implementation. `VkrRendererImplKind`
currently has Metal and Vulkan values, and ADR-025 explicitly names a third
implementation as a revisit condition. Normal frames cross only the coarse
prepare and submit operations. Per-pass and per-draw dispatch remains private to
the selected implementation.

Adding a third enum value is mechanically small. Adding a third backend is still
an architectural change because it adds:

- another platform factory and capability report;
- target, queue, submission, presentation, and completion ownership;
- descriptor heaps and generation-safe publication;
- graph resource realization and dependency lowering;
- graphics and compute pipeline creation, DXIL reflection, and cache policy;
- capture, picking, IBL, pass timing, diagnostics, and metrics;
- shader build and packaging;
- CPU, software-adapter, hardware, validation, resize, cache, and visual gates;
  and
- a permanent third maintenance and evidence matrix.

The previous draft estimated implementation size from current backend line
counts. Delete that estimate. Source lines are volatile, and a translation can
be shorter while still carrying the same ownership and validation cost.

### Reusable code

The authored graph, compiler, packet contract, metrics, capture state machine,
GPU memory range core, slot table, submit ring, ABI manifest machinery,
visibility data, packed geometry, and scene-facing systems are designed for
reuse.

The Vulkan Slang shader bodies are also a useful starting point for DXIL. They
are not drop-in binaries. D3D12 needs a new resource-declaration layer, root
signature, descriptor-indexing contract, DXIL reflection checks, shader
capability profile, and pipeline-cache keys. Metal remains a split Slang and MSL
implementation, so a D3D12 backend would not create a single-source
three-backend shader tree by itself.

## 3. Semantic mapping

These mappings have low semantic drift when the D3D12 implementation keeps them
private:

| VKR/Vulkan concept | D3D12 mapping | Required caution |
| --- | --- | --- |
| Descriptor-buffer texture and sampler indices | Shader Model 6.6 `ResourceDescriptorHeap` and `SamplerDescriptorHeap` indexing | Descriptor lifetime and completion-gated slot reuse remain VKR invariants |
| Buffer device address roots | GPU virtual addresses and root SRV/constants | Root-signature and alignment rules differ; validate the host/DXIL ABI |
| Synchronization2 dependency | Enhanced Barriers | The mapping is close, not identical. Layout, sync, access, queue ownership, and discard rules need a typed lowerer |
| Dynamic rendering scope | RTV/DSV binding or D3D12 render-pass API | Keep it implementation-private. Do not add a generic command RHI |
| Indirect-count draw submission | `ExecuteIndirect` with a count buffer | Command signature, root updates, capacity, and count-buffer bounds must match the packet ABI |
| Timeline completion | `ID3D12Fence` values | Resource retirement and frame-slot reuse still wait on proven completion |
| Pipeline cache | Pipeline library or cached PSO data | Cache compatibility, driver identity, corruption, and cold/warm evidence need their own policy |

Calling these "zero drift" would be misleading. They preserve the same
high-level contract, but each API has different valid-usage and lifetime rules.

## 4. Optional D3D12 features

Optional features are not backend acceptance criteria.

### PIX

PIX is the clearest D3D12-only development gain. It combines event timing,
resource and pipeline inspection, pixel history, and shader debugging in one
tool. Vulkan already has RenderDoc and Radeon GPU Profiler, and RGP remains the
better direct source for RDNA occupancy and hardware-counter analysis. A PIX
requirement should name the missing diagnostic, not rely on general preference.

### WARP

WARP can provide a display-independent software D3D12 correctness target. It is
useful for graph execution, descriptor bounds, deterministic captures, and
pipeline creation. It cannot validate AMD/NVIDIA/Intel drivers, GPU lifetime
behavior under real concurrency, or performance. A WARP pass supplements
hardware evidence.

### Agility SDK

The Agility SDK lets an application ship a newer D3D12 runtime without requiring
the same feature to arrive through an OS update. It does not replace the GPU
driver. Microsoft still requires drivers that support the features an
application targets. Describe this as runtime-delivery independence from the OS,
not independence from drivers or OS minimums.

### Sampler feedback

Sampler feedback could inform the existing texture residency policy. Do not
assert a tier for the RX 6700 XT from memory. Query
`D3D12_FEATURE_D3D12_OPTIONS7`, record the reported tier, then design a policy
only if measured texture demand shows that feedback would change a decision.

Sampler feedback affects streaming quality and residency. It is not a free
no-op on other backends. The fallback remains the current demand and budget
policy, and both modes need equivalent missing-texture behavior.

### DirectStorage

DirectStorage is an asset-delivery project, not a reason to fork rendering.
Its GPU resource and decompression integration can be evaluated after the async
loader has a measured I/O or CPU-decompression bottleneck. Do not attach it to
the first D3D12 milestone.

### VRS, ray tracing, and mesh shaders

These change rendering behavior and need separate designs. They are not backend
translations.

Apple7 supports Metal mesh shading and ray-tracing APIs, but the exact indirect
mesh and hardware-acceleration capabilities differ from later Apple families.
Vulkan and D3D12 support also depends on the queried device profile. The current
GPU-driven candidate and indirect paths already ship across both VKR backends,
so no measured need justifies another geometry path.

Ray tracing on one target remains an optional enhancement over raster fallbacks.
It is not a D3D12-specific capability argument while Vulkan can expose the same
class of feature on the Windows GPU.

## 5. Vulkan availability claims

Do not freeze a current driver matrix into this evaluation.

The production Vulkan profile already reports required and optional features per
device. Capture that report on the target RX 6700 XT before claiming that a
specific Vulkan feature is missing. Distinguish:

- absent from the Vulkan specification or project SDK;
- present in headers but not exposed by the installed driver;
- exposed but not enabled by the immutable VKR profile;
- enabled but blocked by shader tooling, validation, or debugger support; and
- available but deliberately unused because no measured case justifies it.

That distinction matters. `VK_EXT_descriptor_buffer` is already required and
proven on the RX 6700 XT. Newer descriptor models, unified layouts, work graphs,
and other fast-moving extensions must be checked against the actual SDK and
capability report at decision time.

## 6. Acceptance shape if revisited

The first accepted D3D12 slice should be a parity milestone:

1. immutable capability report and exact rejection reasons;
2. offscreen target and one deterministic text/picking fixture;
3. shared memory, submit-ring, slot-table, ABI, and capture callers;
4. full authored graph with existing opaque, transmission, blend, post, UI,
   picking, capture, and timing categories;
5. windowed acquire, resize, present, and destruction completion;
6. DXIL reflection and host ABI validation;
7. cold and warm pipeline-cache behavior;
8. WARP correctness where supported plus one real hardware path; and
9. no new rendering feature or image-quality change.

Only after that milestone passes should optional D3D12 features compete for
their own measured slices.

## 7. Decision record

A third selected implementation changes the scope of ADR-025. Write a new ADR
when D3D12 is accepted. It should preserve the coarse selected strategy, name
the platform and capability floor, define retirement and completion ownership,
and state whether D3D12 supplements or replaces Vulkan on Windows.

Do not write that ADR while this recommendation remains "not scheduled."

## 8. Primary references

- [Microsoft DirectX 12 Agility SDK getting started](https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/)
- [Microsoft Enhanced Barriers release](https://devblogs.microsoft.com/directx/agility-sdk-1-608-0/)
- [Microsoft sampler-feedback tiers](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_sampler_feedback_tier)
- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
