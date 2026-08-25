---
status: proposed
updated: 2026-08-25
authority: design
---
# Vulkan, D3D12, and Metal comparison for VKR

**Document status:** External capability comparison. D3D12 is not implemented.
This document does not override the architecture status specification or the
accepted backend ADRs.

**Targets:** Production Vulkan 1.4 on the RX 6700 XT, production Metal 4 on the
M1 Pro, and a possible future D3D12 implementation on Windows.

## 1. Ground rules

Compare semantic contracts, not API vocabulary. VKR has one coarse selected
implementation seam, not a generic command RHI. Vulkan and Metal own their
images, pipelines, descriptors, encoders, command submission, synchronization,
capture, and presentation directly.

The portable part is the data and behavior that already has two real callers:

- versioned render packets;
- the authored render graph and backend-neutral dependency schedule;
- generation-safe asset publication;
- GPU memory, submit-ring, slot-table, ABI, and capture cores;
- visibility candidates and packed geometry;
- scene lighting and material semantics; and
- metrics and harness reports.

A D3D12 backend should consume those contracts. It should not force Vulkan and
Metal through a new low-level interface.

## 2. Current renderer shape

The current frame is a GPU-driven visibility-buffer renderer:

1. prepare a windowed or offscreen target;
2. publish completed assets and frame data;
3. classify camera and shadow candidates on the GPU;
4. rasterize opaque/cutout visibility and depth;
5. build normal-Z maximum HZB history;
6. resolve material attributes into a compact G-buffer;
7. light into HDR;
8. composite four bounded transmission layers;
9. render ordinary blend and feature-local text/UI;
10. tonemap and present; and
11. perform requested picking, capture, and timing work through declared graph
    categories.

Vulkan and Metal share this topology but not command encoding or shader source
form. Vulkan production shaders are Slang. Metal production shaders are split
between Slang and MSL because the current resource and toolchain contracts do
not all lower through one source.

Any comparison that describes a classic rasterized G-buffer, a generic
`beginPass/endPass` RHI, a forward+ transparency renderer, or one Slang source
for all production shaders is not a description of this tree.

## 3. Portable semantic mapping

| VKR need | Vulkan implementation | Metal implementation | D3D12 mapping if added |
| --- | --- | --- | --- |
| GPU-addressed buffers | Buffer device address and physical-storage-buffer Slang ABI | Metal GPU addresses and reflected root records | GPU virtual addresses and root SRV/constants |
| Bindless textures and samplers | Required descriptor buffers with separate sampled-image, storage-image, and sampler slots | Resource IDs, argument tables, and bounded publication rows | SM 6.6 resource and sampler descriptor heaps |
| Dependency lowering | Synchronization2 stages, accesses, and layouts | Metal 4 barriers, encoder boundaries, hazard and residency rules | Enhanced Barrier sync, access, and layout groups |
| Graphics scopes | Dynamic rendering | Metal render command encoders | RTV/DSV binding or D3D12 render-pass API |
| GPU-generated draws | Indirect-count draws in fixed state buckets | GPU-encoded ICB ranges | `ExecuteIndirect` and count buffers |
| Completion | Timeline submit values plus WSI-specific semaphores/fences | Shared-event and command completion | `ID3D12Fence` values plus DXGI present ownership |
| Pipeline persistence | Driver pipeline cache | Metal archive/dataset path | Pipeline library or cached PSO data |
| Readback | Shared capture state with Vulkan copies | Shared capture state with Metal blits | Shared capture state with D3D12 copies |

The table names similar concepts. It does not imply identical valid-usage rules
or identical performance.

## 4. Capability facts and corrections

### 4.1 GPU pointers and bindless access

All three APIs can express GPU-addressed buffer roots and heap-indexed textures.
The token representation remains backend-private. VKR already chose 32-bit
descriptor indices for Vulkan and native resource identifiers for Metal. Do not
widen shared material rows merely to make a future D3D12 row look identical.

Slang `DescriptorHandle` examples are not the production VKR ABI. The shipped
Vulkan resource header uses explicit global arrays and indices, and Metal uses
its own reflected records.

### 4.2 Barriers and render scopes

Vulkan synchronization2 and D3D12 Enhanced Barriers both expose stage/sync,
access, and layout-like facts. Their enums and discard/ownership rules differ.
Metal has its own encoder and barrier constraints. The shared graph should keep
declaring semantic access while each implementation lowers it directly.

D3D12 has no need for a Vulkan-style render-pass object, but it does have an
optional render-pass API. "D3D12 has no passes" is not a useful architecture
claim. VKR cares that the implementation can begin and end the attachment scope
required by the authored graph.

### 4.3 fp16 and subgroup operations

The APIs and target GPUs expose 16-bit and subgroup/SIMD operations, but VKR's
production use is selective. HDR storage being RGBA16F does not mean all HDR
arithmetic should be half precision. Position reconstruction, depth, histogram
range, BRDF intermediates, and accumulation need individual error and
performance evidence.

Wave width must not be hard-coded. Vulkan/Slang code should remain
width-agnostic, and Metal code should use SIMD-group operations without assuming
that a future device matches M1 execution width.

### 4.4 Mesh shading and ray tracing

The earlier comparison incorrectly marked mesh shading and ray tracing as
absent on M1-class Apple7. Apple's feature tables list mesh shading for Apple7
and ray-tracing APIs from Apple6. Later Apple families add different indirect
mesh and ray-tracing capabilities, so API presence does not prove that one
cross-platform path has matching submission or cost.

VKR already ships GPU candidate classification and indirect indexed drawing.
Mesh shaders need a measured geometry or submission problem before they justify
a second path. Ray tracing needs a raster fallback and its own cross-device
quality and performance contract. Neither belongs in a portable required floor.

### 4.5 Variable-rate shading and sampler feedback

VRS has concepts on all target APIs, including Metal rasterization-rate maps,
but device limits and image-quality behavior differ. Treat it as an optional
rendering mode, not a backend translation.

D3D12 sampler feedback has no identical VKR Vulkan/Metal input today. It may
improve texture-residency decisions, but the D3D12 device must report its tier
and a measured workload must show that feedback changes a useful decision.

### 4.6 Upscaling and temporal AA

FSR 3.1 officially targets DX12 and Vulkan. Its Native AA mode performs temporal
AA without spatial upscaling, but still requires jitter, motion vectors, depth,
exposure, reactive masks, and transparency/composition masks.

MetalFX temporal upscaling is available on Apple7 and consumes the same broad
class of inputs. It is not the same algorithm and Apple documents it as an
upscaler. Do not assume same-resolution Native AA support.

The portable renderer contract is therefore the temporal input set, reset
rules, and UI ordering. The temporal algorithm remains implementation-specific
unless VKR authors one portable resolve. See
[the AA evaluation](visibility-buffer-msaa-spec.md).

## 5. Optional per-platform branches

Use an API-specific branch only when:

1. the current capability report proves availability;
2. a concrete VKR case demonstrates value;
3. the fallback produces correct output and remains maintained;
4. the branch does not leak backend checks above `VkrRendererImpl`; and
5. its performance and visual evidence are recorded separately.

Examples that may meet this bar later include sampler feedback, MetalFX,
hardware ray tracing, VRS, or platform-native pipeline compilation features.
Their presence in an API is not a reason to implement them.

## 6. D3D12-specific assessment

D3D12 would add:

- PIX as an integrated D3D12 debugger and profiler;
- WARP as a software correctness target;
- Agility SDK delivery of a newer D3D12 runtime independently of large OS
  feature updates; and
- another Windows driver/API route.

Agility still needs a driver that supports each targeted hardware feature. WARP
does not substitute for hardware performance, presentation, or vendor-driver
validation. These are useful tools, not image-quality features.

The implementation cost remains a complete selected renderer. Slang-to-DXIL
reduces shader translation work but does not provide target ownership,
descriptor lifetime, graph realization, pipeline reflection/cache,
presentation, capture, or validation.

The recommendation remains [do not schedule D3D12 yet](../architecture/d3d12-backend-evaluation.md).

## 7. Claims deliberately removed

The previous draft named Capcom RE Engine as the reference and mapped internal
render-graph, material, and submission decisions to it. Public game feature
lists do not establish those internal contracts. The comparison no longer uses
an external engine as rationale for VKR decisions.

It also removed these unsupported shortcuts:

- "Metal cannot do mesh shaders";
- "M1 cannot do ray tracing";
- "Agility removes driver dependencies";
- "one Slang source already serves all production backends";
- "fp16-first is always faster"; and
- "D3D12 features are parity-safe because other backends can no-op them".

## 8. Primary references

- [Apple Metal feature set tables](https://developer.apple.com/metal/feature-sets/)
- [Apple MetalFX temporal scaler](https://developer.apple.com/documentation/metalfx/mtlfxtemporalscalerbase)
- [Khronos Vulkan specification](https://docs.vulkan.org/spec/latest/)
- [Microsoft DirectX 12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/)
- [Microsoft Agility SDK setup and driver requirements](https://devblogs.microsoft.com/directx/gettingstarted-dx12agility/)
- [AMD FSR 3.1 integration overview](https://gpuopen.com/presentations/2024/FidelityFX_Super_Resolution_3-1_Release-Overview_and_Integration.pdf)
