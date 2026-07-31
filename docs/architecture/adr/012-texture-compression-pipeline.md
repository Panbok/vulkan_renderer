---
status: implemented
updated: 2026-07-31
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
UASTC, and write KTX2 files. `build.sh` builds/runs the packer by default. A
Release build refuses `VKR_VKT_PACK=0` and defaults
`VKR_VKT_PACK_STRICT=1`, making pack errors fatal.

Runtime resolution supports direct `.vkt`, a sidecar for a source path, and
optional source decoding. KTX2 currently accepts 2D, single-layer,
non-cubemap Basis payloads. It selects a target by texture class, device type,
and format support:

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

Runtime rollout controls are separate from build packing controls:

| Variable | Runtime effect |
|---|---|
| `VKR_TEXTURE_VKT_STRICT` | Require KTX2 `.vkt`; disable source and legacy fallback |
| `VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK` | Permit source image decoding |
| `VKR_TEXTURE_VKT_ALLOW_LEGACY` | Permit legacy raw `.vkt` reads |
| `VKR_TEXTURE_VKT_WRITE_LEGACY_CACHE` | Permit legacy cache writes |

`VKR_VKT_PACK_STRICT` makes the offline packing step strict; it does **not** set
`VKR_TEXTURE_VKT_STRICT` for the application process. A Release build therefore
proves that packing completed, not by itself that runtime source/legacy fallback
is disabled.

The vendored submodule is currently KTX-Software v4.4.2 at
`4d6fc70eaf62ad0558e63e8d97eb9766118327a6`.

## Consequences

**Positive**

- One portable intermediate can target several GPU-native compressed families.
- Mip data and texture intent metadata travel with the container.
- Build integration catches missing/failed packed assets in strict packing mode.
- Dual-path rollout preserves development compatibility while migration is in
  progress.

**Negative / risks**

- Runtime transcoding consumes CPU time and temporary memory; faster total load
  time must be measured rather than assumed.
- Current KTX2 decode rejects cubemaps, arrays, and non-Basis KTX2 payloads.
- KTX-Software adds a sizeable C/C++ dependency and build cost.
- Several build/runtime environment switches can be confused.
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
- Add a native post-transcode cache if repeat load time warrants it.
- Define compressed cubemap/array support for environment assets and streaming.
- Remove legacy/source fallback only after a runtime strict-mode validation run,
  not merely a strict pack.
