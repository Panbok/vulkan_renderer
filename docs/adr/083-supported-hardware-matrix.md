---
status: partial
updated: 2026-10-06
authority: adr
---

# ADR-083: Supported hardware matrix and memory floor

## Status

Accepted (partial). A Vulkan device-memory budget is not implemented, and no
device below the memory floor has a Bistro measurement. Only the two reference
devices have native evidence.

## Context

The backends gate initialization on API capabilities
([ADR-023](023-vulkan-1-4-bindless-capability-profile.md) for Vulkan,
`MTLGPUFamilyMetal4` for Metal), but no document named the devices those
capabilities admit or how they differ. Performance and graphics work then
generalized results from one device. Bistro reaches the managed GPU budget on
the 16 GB M1 Pro, while the 12 GB RX 6700 XT has room left. Feature choices
such as 64-bit atomics or indirect mesh draws are available on some supported
families and absent on others.

## Decision

VKR supports three hardware families:

- Apple silicon Macs from M1 (Apple GPU family 7) with macOS 26 and Metal 4.
- AMD RDNA 2 and newer with Vulkan 1.4 on Windows x86-64.
- NVIDIA Ampere and newer with Vulkan 1.4 on Windows x86-64.

Intel Macs, Intel GPUs, NVIDIA Turing and older, AMD RDNA 1 and older, Linux and
Windows on Arm are not supported targets. Linux has no enabled implementation
(ADR-023); RDNA 2 support depends on `VK_EXT_descriptor_buffer`, which the
checked Windows drivers expose.

### Required API surface

| Backend | Initialization gate | Owner |
|---|---|---|
| Metal | macOS 26, `MTLGPUFamilyMetal4`, an MTL4 command queue and residency sets | [`vkr_metal_packet_setup.inc`](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc) |
| Vulkan | Vulkan 1.4, `VK_EXT_descriptor_buffer`, buffer device address, `drawIndirectCount`, `geometryShader` for `SV_PrimitiveID`, descriptor indexing, 16,384 sampled images plus the permanent rows and 2,048 samplers per stage | ADR-023, [`vkr_vulkan_device.c`](../../renderer/src/vulkan/vkr_vulkan_device.c) |

### Architectures

| Architecture | Products | Smallest memory | Native evidence |
|---|---|---|---|
| Apple7 | M1, M1 Pro, M1 Max, M1 Ultra | 8 GB unified | M1 Pro 16 GB, macOS 26.6.2 (reference) |
| Apple8 | M2 series | 8 GB unified | None |
| Apple9 | M3, M4 series | 8 GB unified (M3), 16 GB (M4) | None |
| Apple10 | M5 series | Not recorded | None |
| AMD RDNA 2 | RX 6000 | 4 GB (RX 6400, RX 6500 XT) | RX 6700 XT 12 GB, Windows 10, driver 26.6.3, Vulkan 1.4.315 (reference) |
| AMD RDNA 3, RDNA 4 | RX 7000, RX 9000 | Not recorded | None |
| NVIDIA Ampere | RTX 30 | 4 GB (laptop RTX 3050) | None |
| NVIDIA Ada, Blackwell | RTX 40, RTX 50 | Not recorded | None |

Apple family assignments come from Apple's Metal Feature Set Tables (21 May
2026). Vulkan rows rely on the gpuinfo reports in [Sources](#sources).

### Capability differences

These differences constrain renderer work. "Not used" means that no production
path depends on the capability.

| Capability | M1 (Apple7) | M2 (Apple8) | M3, M4 (Apple9) | RDNA 2 | Ampere | VKR use |
|---|---|---|---|---|---|---|
| Rasterization | Tile-based deferred | Tile-based deferred | Tile-based deferred | Immediate | Immediate | Pass and bandwidth costs do not transfer between columns |
| Graphics pipeline class | Tiled | Tiled | Tiled | Desktop | Desktop | The class follows the backend ([ADR-087](087-gpu-class-graphics-pipelines.md)) |
| Memory | Unified | Unified | Unified | Discrete; ReBAR or a 256 MiB BAR | Discrete; ReBAR | Vulkan falls back to host memory when the mapped heap is full (ADR-024) |
| BC formats | Yes | Yes | Yes | Yes | Yes | x86-64 managed imports |
| ASTC LDR | Yes | Yes | Yes | No | No | Apple managed imports |
| ETC2/EAC | Yes | Yes | Yes | No | No | Not used |
| SIMD or subgroup width | 32 | 32 | 32 | 64 by default; 32 to 64 | 32 | Wave intrinsics without subgroup-size control |
| 64-bit atomics | No | Min and max only, macOS | Full set | Buffer and image | Buffer and image | Not used |
| Mesh shaders | Direct draws only | Direct draws only | Direct, indirect and ICB draws | `VK_EXT_mesh_shader` | `VK_EXT_mesh_shader` | Not used |
| Ray tracing | API, no hardware units | API, no hardware units | Hardware | `VK_KHR_ray_query` | `VK_KHR_ray_query` | Not used |
| Lossy render-target compression | No | Yes | Yes | Not exposed | Not exposed | Not used |
| Samplers per stage in argument buffers | 996 | 996 | 500,000 | Not applicable | Not applicable | The Metal sampler cache holds at most 932 states below Apple9, keeping 64 for inline MSL samplers, and reuses the closest cached state beyond that |
| Temporal upscaler | None; spatial upscale with adaptive quality | Same | Same | FSR 3.1 | FSR 3.1 | ADR-087 (decision 12), ADR-052 |
| Memory budget source | `recommendedMaxWorkingSetSize` | Same | Same | `VK_EXT_memory_budget`, not enabled | Same | Metal only |
| `VK_EXT_descriptor_heap` | Not applicable | Not applicable | Not applicable | Missing on Windows drivers | Present | Not used; adopting it drops Windows RDNA 2 |

Apple9 GPUs allocate registers dynamically (Apple's Dynamic Caching). Occupancy
and register-pressure results measured on the M1 Pro do not predict Apple9.

### Memory floor

Production Bistro targets a 16 GB Mac or a discrete GPU with 8 GB
(decided 2026-10-03). Devices below the floor, 8 GB Macs and 4 to 6 GB discrete
GPUs, remain supported for rendering. The texture resolution setting
([ADR-012](012-texture-compression-pipeline.md)) is their reduced tier: Metal
defaults to the 2048 limit, which took Bistro's textures from 3.18 to 2.00 GB on
the M1 Pro, and 1024 lowers it further. Managed imports on Apple silicon
encode colours and data masks as ASTC 6x6 (ADR-012), 2.25 times smaller than
4x4. The D16 local shadow atlas
([ADR-019](019-bounded-forward-spatial-lighting.md)) takes Bistro's atlas from
768 to 384 MiB on every device.

The Metal managed allocation cap is two thirds of `recommendedMaxWorkingSetSize`
(ADR-024). The 16 GB M1 Pro on macOS 26.6.2 reports 12,124 MiB, so its cap is
8,082 MiB; the editor log records both values at startup. The ratio for an 8 GB
Mac is not measured. The cap also holds render targets, geometry, and transfer
rings, so textures receive less than the full cap. Vulkan has no device-memory
cap. It relies on the texture pressure policy (ADR-024), and a 12 GB RX 6700 XT
holds Bistro. Release `win_bistro_production` with
`local-offscreen-gpu-single.json` on that GPU (driver 26.6.3, `b7fd519f`, BC
textures) measured the M1 Pro's texture totals: 3.176 GB at full resolution, the
Vulkan default, and 1.995 GB at a 2048 limit. `memory.gpu.bytes.peak` was 4.84
and 3.56 GB; the driver reported no heap usage (`memory.gpu.heap_usage_valid`
0). At about 4.5 GiB, the full-resolution peak does not need Vulkan to default
to 2048. No 8 GB discrete GPU has run Bistro.

### Rules for performance and graphics work

1. Name the device, family, memory size and pipeline class with every
   measured result. Do not apply an M1 Pro result to another column without a
   measurement there.
2. A capability that is absent from a column that runs the code needs a
   capability boundary and a path that runs on the column without it: the M1
   column for the tiled pipeline and the passes both classes share, the
   RDNA 2 and Ampere columns for the desktop pipeline.
3. Judge memory changes against the 16 GB Mac cap. The M1 family is the
   memory-limited target; desktop Vulkan GPUs are not.
4. BC is the only block-compressed format family that every supported GPU
   samples. ASTC serves Apple only.

## Consequences

The M1 column sets the feature floor for the tiled pipeline and for the
passes both classes share, such as GPU culling and draw encoding; the RDNA 2
and Ampere columns set it for the desktop pipeline. A shared GPU-driven path
that needs indirect mesh draws requires a second path for M1 and M2, while a
desktop technique that needs 64-bit atomics runs on both desktop columns.
Texture memory reduction is a requirement on Metal: the 16 GB floor leaves
textures less than 8 GiB. Rows without native evidence stay unverified until a run on that device
records them.

## Alternatives considered

An 8 GB Mac and 4 GB GPU floor would force about a 4 GiB Bistro budget for every
device and much harder texture cuts. Recording only the two development devices
would leave feature decisions without a baseline for the supported families.
Raising the Apple floor to Apple9 would remove the M1 limits, but it would drop
every M1 and M2 Mac.

## Revisit when

A supported family changes, a driver snapshot changes a required extension, an
8 GB Mac or another column gets a native measurement, or the reduced texture
tier or a Vulkan memory budget ships.

## Sources

- Apple Metal Feature Set Tables, 21 May 2026: family assignments, BC, 64-bit
  atomics, mesh shading, lossy compression and argument-buffer limits.
- gpuinfo reports [42800](https://vulkan.gpuinfo.org/displayreport.php?id=42800)
  (RX 6800 XT) and [48951](https://vulkan.gpuinfo.org/displayreport.php?id=48951)
  (RX 6650 XT), Windows, driver 2.0.353; and
  [51549](https://vulkan.gpuinfo.org/displayreport.php?id=51549) (RTX 3070 Ti,
  616.64): compression formats, subgroup sizes, atomics and extensions.
- [NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI) hardware requirements,
  driver snapshot of 5 September 2026: `VK_EXT_descriptor_heap` support per
  architecture.
- `MTLDevice` query on the reference M1 Pro, 2026-10-03: working set, BC support
  and family membership.

## Code evidence

- [Metal managed budget](../../renderer/src/metal/internal/vkr_metal_packet_setup.inc)
  (`vkr_metal_packet_managed_budget`)
- [Vulkan device floor](../../renderer/src/vulkan/vkr_vulkan_device.c)
- [Texture pressure budget](../../runtime/src/renderer/systems/vkr_render_assets.c)
- [Texture format selection](../../runtime/src/renderer/systems/vkr_texture_system.c)
