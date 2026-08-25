---
status: implemented
updated: 2026-08-25
authority: adr
---
# ADR-012: Offline KTX2/UASTC Packing with Runtime Transcode

**Status:** Accepted
**Implementation tracker:**
[texture-compression-vkt-ktx2-uastc-implementation-tracker.md](../../assets/texture-compression-vkt-ktx2-uastc-implementation-tracker.md)
**Policy:** [texture-compression-policy/SPEC.md](../../assets/texture-compression-policy/SPEC.md)

## Context

The former `.vkt` cache stored raw decoded pixels. Large texture sets benefit
from GPU block compression, but native format families vary across devices.
Maintaining separate source asset sets for BC, ASTC, and ETC targets would
complicate authoring and distribution.

## Decision

Use `.vkt` as the runtime-facing extension for two distinguishable containers:
the legacy raw cache and KTX2 with a Basis UASTC payload. Detect the container by
signature rather than extension alone.

`tools/vkr_vkt_packer.cpp` uses the vendored KTX-Software library to read source
images, build mip payloads, attach texture-class/colorspace metadata, encode
UASTC, and write KTX2 files. Texture packing is build-type-independent:
`tools/pack_vkt_textures.sh` and its `.bat` equivalent build the tool in the
shared `build_vkt_packer` tree, then update only stale `.vkt` sidecars. Every
root wrapper that compiles a CMake target invokes the platform packer after a
successful compile. Debug and Release still keep separate application output
trees and share the packer tree.

Runtime resolution supports direct `.vkt`, a sidecar for a source path, and
optional source decoding. KTX2 accepts 2D textures, 2D arrays, cubemaps, and
cubemap arrays with Basis payloads. It selects a target by texture class,
device type, and format support:

- color/data textures prefer BC7, ASTC 4×4, or ETC2 and can transcode to RGBA32
  when no compressed target is supported;
- normal maps prefer BC5 or ASTC, then capability-gated EAC RG11, with RGBA32
  as the universally transcodable fallback.

libktx has no uncompressed two-channel Basis transcode target, so the former
`R8G8_UNORM` terminal choice could never be valid. EAC RG11 is probed
independently from ETC2 RGBA and mapped to its libktx target; the terminal RGBA
choice guarantees every selector result has a transcode mapping. The unit
matrix covers all 1,024 combinations of texture class, device preference,
sRGB intent, and the five capability flags.

The decoded/transcoded bytes are uploaded through explicit mip/layer
`VkrTextureUploadRegion` entries. Writable/resize paths reject block-compressed
textures.

Target-native transcode output and region metadata persist under
`build/_asset_cache/textures`, keyed by complete source content and selected
target format. Material activation is independent of texture residency:
materials first publish with default textures, then a bounded eight-request
window loads texture resources and republishes completed material rows. Normal
loading is uncapped so full scene residency remains the default. Backend heap
budget telemetry is sampled every 60 frames. A 90% high-water mark applies a
texture limit targeting 80% total use; a 75% low-water mark clears it.
`VKR_TEXTURE_STREAM_BUDGET_MB` overrides automatic pressure, and the runtime
setter remains the direct policy boundary. Pressure counts shared GPU textures
once, pins incoming shared handles before eviction, republishes
least-recently-used slots with semantic defaults, and retires a texture only
when its last resident slot leaves. Continuously demanded evictions do not retry
until demand disappears and returns. Metal packs up to 64 texture payloads into
one aligned 32 MiB upload-ring slice and command buffer; Vulkan records copies
into the active frame command buffer.

KTX2 2D arrays, cubemaps, and cubemap arrays share the same prepared payload.
The decoder flattens layers and faces into physical array layers; Metal lowers
them to `2DArray`, `Cube`, or `CubeArray`, while Vulkan selects the corresponding
image view and cube-compatible flag. Scene environments may provide a direct
compressed cubemap through `environment.cubemap.path`; six-file cubemaps remain
compatible.
The Metal layered-texture diagnostic samples a nonzero 2D-array layer and a
nonzero cube from a cubemap array in a real compute shader and validates both
results.

Runtime rollout controls are separate from build packing controls:

| Variable | Runtime effect |
|---|---|
| `VKR_TEXTURE_VKT_STRICT` | Defaults on; set `0` only for explicit development/test compatibility |
| `VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK` | Effective only when strict mode is disabled |
| `VKR_TEXTURE_VKT_ALLOW_LEGACY` | Effective only when strict mode is disabled |
| `VKR_TEXTURE_VKT_WRITE_LEGACY_CACHE` | Permit legacy cache writes |

Every root build wrapper that performs CMake compilation invokes the matching
`tools/pack_vkt_textures.sh` or `.bat` after the compile succeeds; pack failure
fails the build. `VKR_VKT_PACK_STRICT` tightens both automatic and standalone
packing. Runtime strictness remains independent and defaults on. The packer
detects legacy raw outputs even when their timestamps are newer and replaces
them with KTX2/UASTC. Test wrappers disable runtime strictness only for fixtures
that exercise compatibility; they still run incremental packing.

The vendored submodule is currently KTX-Software v4.4.2 at
`4d6fc70eaf62ad0558e63e8d97eb9766118327a6`.

## Consequences

**Positive**

- One portable intermediate can target several GPU-native compressed families.
- Mip data and texture intent metadata travel with the container.
- Packing work is shared across build configurations and skipped for unchanged
  source textures.
- A successful root-wrapper build has refreshed every changed source texture;
  standalone strict packing remains available for distribution preparation.
- Dual-path rollout preserves development compatibility while migration is in
  progress.

**Negative / risks**

- Runtime transcoding consumes CPU time and temporary memory; faster total load
  time must be measured rather than assumed.
- Current KTX2 decode rejects non-Basis payloads and shapes outside 2D, 2D
  array, cubemap, and cubemap array.
- KTX-Software adds a sizeable C/C++ dependency and build cost.
- The first build after source/settings changes includes packing cost; unchanged
  builds still scan content identity before skipping current outputs. Direct
  CMake compilation bypasses wrapper policy and does not prove packed assets are
  complete.
- Block-compressed assets cannot use generic writable/resize paths.
- Output format and visual result can differ by device capability.

## Alternatives Considered

- **Per-platform native files.** Avoids runtime transcode but multiplies build
  outputs and asset resolution. Rejected for current workflow.
- **Raw decoded cache.** Simple but gives up block-compressed residency and file
  size. Rejected as the primary shipping path.
- **ETC1S intermediate.** Smaller distribution, lower quality than UASTC for the
  intended showcase. Rejected.
- **External `toktx`.** Less in-repo code but adds an external tool dependency.
  Rejected in favor of the programmatic packer.

## Revisit When

- Measure transcode time, peak temporary memory, packed size, and GPU residency
  on representative scenes/devices.
