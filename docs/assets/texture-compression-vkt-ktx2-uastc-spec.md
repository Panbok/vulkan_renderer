---
status: partial
updated: 2026-07-31
authority: spec
---
# Texture Compression Spec: `.vkt` via KTX2 + Basis Universal (UASTC)

## Purpose and Scope

Define a documentation-only, decision-complete design for moving `.vkt` texture assets from the current raw RGBA cache format to a self-contained compressed asset format based on **KTX2 + Basis Universal (UASTC)**.

Target runtime platforms:

- Apple Silicon (MoltenVK)
- Windows desktop (RX6700XT baseline)
- Linux desktop (RX6700XT baseline)

This document is a **spec only**. It intentionally does not implement code changes.

## Non-Goals

- No code implementation in this task.
- No shader/material pipeline rewrites in this task.
- No production rollout in this task.
- No GLTF pipeline migration in this task (GLTF/PBR integration is future work).

## Current State Analysis

### 1) Texture system (`lib/src/renderer/systems/vkr_texture_system.c`)

Current behavior is a raw decode cache:

- `.vkt` files are treated as a custom binary cache (`VKR_TEXTURE_CACHE_MAGIC`, `VKR_TEXTURE_CACHE_VERSION`, raw RGBA bytes).
- Cache invalidation depends on source file mtime.
- Source images are decoded through `stb_image` (`stbi_load_from_memory`) into RGBA.
- Upload path assumes uncompressed data and computes byte size from `width * height * channels`.
- Texture request query parsing (`?cs=srgb|linear`) already exists for colorspace intent.

Key implication:

- Existing `.vkt` is not a portable compressed asset container. It is a local decode cache format tied to source-image presence and mtime checks.

### 2) Texture loader (`lib/src/renderer/resources/loaders/texture_loader.c`)

Current behavior:

- `can_load()` accepts source image extensions (`png`, `jpg`, `jpeg`, `bmp`, `tga`).
- Query suffix is stripped for extension check.
- No explicit `.vkt` extension support in `can_load()`.

Key implication:

- Asset identity is image-first today; `.vkt` is an internal cache sidecar, not a first-class input format.

### 3) Vulkan backend (`lib/src/renderer/vulkan/vulkan_backend.c`)

Current behavior:

- `renderer_vulkan_create_texture()` assumes uncompressed upload byte math (`width * height * channels`).
- Upload is staging-buffer based and uses buffer-to-image copies for traditional uncompressed layouts.
- Mip generation path is runtime blit-based where supported.
- Update/write/resize paths validate using uncompressed channel-based expected byte sizes.

Key implication:

- Compressed block formats (ASTC/BC7) need explicit region/mip/byte-size aware upload contracts, not channel-count byte assumptions.

## Target Asset Format

## `.vkt` as KTX2 Payload (Self-Contained)

Policy:

- `.vkt` remains the engine-facing extension, but file contents are KTX2 with Basis UASTC payload.
- Runtime must be able to load from `.vkt` **without** source `.png/.jpg/.tga`.
- Source images become authoring inputs, not runtime requirements.

Required metadata carried in KTX2 key/value data:

- `vkr.colorspace_hint` (`srgb` or `linear`)
- `vkr.has_transparency` (`0` or `1`)
- `vkr.alpha_mask` (`0` or `1`)
- `vkr.source_hash` (optional, for tooling traceability)
- `vkr.asset_version` (tooling schema version)

Notes:

- Colorspace query at request time (`?cs=...`) remains authoritative for final runtime view when needed.
- Metadata is for policy, diagnostics, and fallback behavior; it is not a replacement for explicit request intent.

## Transcode and Format Selection Policy

Runtime transcode target selection from UASTC payload:

1. Apple Silicon (MoltenVK):
- Prefer `ASTC 4x4` (`srgb` or `unorm` variant based on request colorspace).
2. Windows/Linux (RX6700XT baseline):
- Prefer `BC7` (`srgb` or `unorm` variant based on request colorspace).
3. Fallback:
- Use `R8G8B8A8` (`SRGB` or `UNORM`) when preferred compressed target is unsupported.

Deterministic priority order:

1. Requested colorspace (`?cs=` if provided; else default policy)
2. Platform preferred compressed family (ASTC or BC7)
3. Device capability check at runtime
4. Fallback to uncompressed RGBA8

Failure behavior:

- If KTX2 decode/transcode fails, return loader error with explicit reason.
- No silent fallback to source-image decode when runtime is configured as `.vkt`-only mode.

## Runtime Loading and Upload Flow

## Resolution order

Given a texture request path (may include query):

1. Parse request query and strip query suffix for filesystem lookup.
2. If request already points to `.vkt`, load it directly.
3. Otherwise try sidecar `<source_path>.vkt`.
4. If `.vkt` missing:
- In strict runtime mode: fail.
- In development fallback mode (optional future toggle): decode source image and optionally produce `.vkt`.

## Decode/transcode job output contract (proposed)

Job output should be upload-ready data with explicit regions:

- target `VkrTextureFormat`
- mip level count
- array layer count
- per-region byte offsets and sizes
- contiguous upload byte buffer
- transparency/alpha-mask metadata

This contract must not assume `width * height * channels` byte math.

## Vulkan upload behavior (proposed)

- Create image with target compressed/uncompressed format and exact mip count from payload.
- Issue one `VkBufferImageCopy` per mip/array region from payload metadata.
- Do not runtime-generate mipmaps for already-provided compressed mip chains.
- Preserve existing queue/synchronization model (transfer+graphics phases) while extending copy region setup.

## Vulkan Backend Design Changes (Proposed, Not Implemented)

## Proposed API/Type changes (design intent only)

1. Expand `VkrTextureFormat` with compressed formats:
- BC7 UNORM/SRGB
- ASTC 4x4 UNORM/SRGB

2. Add explicit upload payload structures:
- `VkrTextureUploadRegion` (mip, layer, extent, byte offset, byte size)
- `VkrTextureUploadPayload` (buffer + region list + mip/layer counts + compression flag)

3. Add capability query for texture compression support:
- ASTC support flag
- BC7 support flag

4. Add/create texture entrypoint that accepts structured upload payload, while keeping legacy path for uncompressed callers.

## Proposed constraints for compressed textures

- `write_texture` and partial region writes should reject compressed textures unless block-aligned region update is explicitly supported.
- `resize_texture` should reject compressed textures for first rollout (or require full re-upload path).
- `texture_update` sampler-only updates remain valid.

## Build and Tooling Strategy

Dependency model:

- Vendor deterministic versions of required runtime/tooling dependencies (KTX/Basis stack) in-repo.
- Avoid network-dependent runtime builds for core compression path.

Offline pack pipeline (proposed):

1. Input: source textures.
2. Convert to KTX2 + UASTC payload.
3. Generate/store mip chains in container.
4. Emit `.vkt` files consumed by runtime loader.

Build-script integration (proposed):

- Add pre-build texture pack step before compile.
- Support incremental packing by source hash/timestamp.
- Keep shader build path independent.

## Compatibility and Migration

## Legacy `.vkt` policy

- Existing raw-cache `.vkt` format is legacy and should be explicitly version-detected.
- Migration path:
1. Phase A: runtime recognizes legacy format and logs migration warning.
2. Phase B: tooling regenerates `.vkt` as KTX2/UASTC.
3. Phase C: strict runtime mode disables legacy format support.

## Rollout stages (proposed)

1. Add parser/capability plumbing behind feature flag.
2. Support dual-path loading (legacy + new).
3. Enable new path by default in development.
4. Enable strict `.vkt`-only mode for release packaging.

## Validation and Acceptance

Functional checks:

1. `.vkt`-only runtime load works with source images removed.
2. Platform target selection is correct (Apple->ASTC, Win/Linux->BC7, fallback->RGBA8).
3. Colorspace behavior is correct for `?cs=srgb|linear`.
4. Mip chain upload correctness for compressed and fallback formats.
5. Unsupported-format fallback behavior is deterministic and logged.

Stability/perf checks:

1. No memory growth across repeated load/unload scene cycles.
2. Texture dedup behavior in batch loading remains correct.
3. Runtime load time and upload time are measured against current baseline.

## Test Cases and Scenarios (Documented, Not Implemented)

1. `.vkt`-only runtime load with source images absent.
2. Platform transcode target selection correctness (ASTC vs BC7).
3. Colorspace correctness (`?cs=srgb|linear`) with compressed targets.
4. Mip chain upload correctness and shader sampling validity.
5. Failure/fallback behavior for unsupported compressed formats.
6. Scene reload stability and memory growth checks.

## Assumptions and Defaults

- Universal `.vkt` source encoding is KTX2 + Basis UASTC.
- Runtime `.vkt` payload is self-contained.
- Dependency policy is vendored and deterministic.
- This task changes docs only and performs no implementation outside `/docs`.
