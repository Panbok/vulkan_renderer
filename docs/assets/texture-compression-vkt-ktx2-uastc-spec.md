---
status: implemented
updated: 2026-08-25
authority: spec
---
# Texture Compression Spec: `.vkt` via KTX2 + Basis Universal (UASTC)

## Purpose and Scope

Define VKR's implemented texture artifact and runtime cache contract. `.vkt`
stores a self-contained KTX2 + Basis Universal UASTC payload for portable
source delivery. Runtime selects and transcodes to a device-native format, then
persists the validated upload-ready result for subsequent processes.

Target runtime platforms:

- Apple Silicon Metal/Vulkan;
- Windows desktop;
- Linux desktop.

## Non-goals

- Runtime recompression of writable textures.
- Implicit texture packing during ordinary renderer builds.
- Treating modification time or file size as content identity.

## Historical pre-implementation analysis

The following inventory records the starting state that this implemented spec
replaced. It is not current runtime status.

### 1) Texture system (`lib/src/renderer/systems/vkr_texture_system.c`)

The prior behavior was a raw decode cache:

- `.vkt` files are treated as a custom binary cache (`VKR_TEXTURE_CACHE_MAGIC`, `VKR_TEXTURE_CACHE_VERSION`, raw RGBA bytes).
- Cache invalidation depends on source file mtime.
- Source images are decoded through `stb_image` (`stbi_load_from_memory`) into RGBA.
- Upload path assumes uncompressed data and computes byte size from `width * height * channels`.
- Texture request query parsing (`?cs=srgb|linear`) already exists for colorspace intent.

Key implication:

- Existing `.vkt` is not a portable compressed asset container. It is a local decode cache format tied to source-image presence and mtime checks.

### 2) Texture loader (`lib/src/renderer/resources/loaders/texture_loader.c`)

The prior behavior was:

- `can_load()` accepts source image extensions (`png`, `jpg`, `jpeg`, `bmp`, `tga`).
- Query suffix is stripped for extension check.
- No explicit `.vkt` extension support in `can_load()`.

Key implication:

- Asset identity is image-first today; `.vkt` is an internal cache sidecar, not a first-class input format.

### 3) Vulkan backend (`lib/src/renderer/vulkan/vulkan_backend.c`)

The prior behavior was:

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
- `vkr.pack_settings` (content-affecting packer settings identity)

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
4. If `.vkt` is missing or is a legacy raw cache, fail by default.
   Source-image and legacy loading require an explicit development/test
   override with `VKR_TEXTURE_VKT_STRICT=0`.

## Persistent device-transcode cache

Runtime transcode output is cached under
`$VKR_ASSET_CACHE_ROOT/textures`; the default root is
`build/_asset_cache`. The key hashes the exact resolved `.vkt` request-path
bytes supplied by the texture loader plus the selected `VkrTextureFormat`;
aliases may occupy separate entries, while the complete source content hash
prevents an incorrect hit. The entry validates:

- cache format version and endianness;
- the complete source `.vkt` content hash;
- target format, dimensions, mip and array counts;
- complete ordered layer/mip topology, exact extents, format-derived byte
  sizes, and bounded upload-region metadata;
- metadata and payload hashes.

Entries contain GPU-ready bytes and upload regions, so a hit performs no Basis
transcode. Writes use process/thread-unique temporary files and atomic replace;
corrupt or stale entries are deleted and rebuilt. Debug, Release, ordinary app
runs, profile children, snapshot children, and harness prewarm children share
the same persistent root. Pipeline-cache isolation never changes the asset-cache
root.

Metrics expose `asset.texture.transcode_cache.hits_total`,
`misses_total`, and `writes_total`.

## Startup residency and backend upload

Material readiness is not texture readiness. Async material finalization
publishes a row using semantic default textures, queues its texture paths, and
allows scene and mesh dependency closure to continue. The material system admits
at most eight texture requests concurrently.

Streamed material textures have queued, active, resident, and evicted states.
Normal loading is uncapped, so every texture can become resident. Every 60
frames the frontend reads backend heap usage/budget telemetry. Crossing 90%
automatically sets a texture limit targeting 80% total heap use; falling below
75% clears it. `VKR_TEXTURE_STREAM_BUDGET_MB` overrides the automatic policy,
and `vkr_material_system_set_texture_residency_budget()` remains the direct
runtime pressure boundary.

The next pump evicts least-recently-used resident slots until unique retained
upload bytes fit. Shared textures count once, and the incoming handle is pinned
before victim selection so eviction cannot destroy a resource about to be
published. Each victim material receives its semantic default before the old
reference is released; backend retirement happens only after the last resident
slot leaves. Continuously demanded evicted materials do not reload every frame.
A slot becomes eligible again only after its material was absent for at least
one packet-collection epoch and then returns.

Metal batches up to 64 prepared texture payloads into one aligned 32 MiB
upload-ring slice and one command buffer. A payload larger than 32 MiB receives
an aligned dedicated slice. The frontend ends and proves the batch before frame
recording; begin/end failure aborts frame preparation. Vulkan retains its
existing model: texture copies record into the active primary frame command
buffer and become live at that submit value.

Metrics expose `asset.material.texture_stream.pending`, `in_flight`, `resident`,
`evicted`, `resident_bytes`, `budget_bytes`, `applied_total`, `failed_total`,
`evicted_total`, and `pressure_stalls_total`.

## Decode/transcode job output contract

Job output is upload-ready data with explicit regions:

- target `VkrTextureFormat`
- mip level count
- array layer count
- per-region byte offsets and sizes
- contiguous upload byte buffer
- transparency/alpha-mask metadata
- texture type (`2D`, `2D_ARRAY`, `CUBE_MAP`, or `CUBE_MAP_ARRAY`)

This contract must not assume `width * height * channels` byte math.

KTX2 inputs must be 2D images with one or six faces. The decoder flattens
`layer × face` into the backend array-layer index, validates at most 2048
physical layers and 32768 upload regions, and emits one region per
layer/face/mip. Cubemaps require square faces; cubemap arrays require a physical
layer count divisible by six. The persistent target-transcode cache stores the
same flattened region contract.

`vkr_metal_texture_array_diagnostic` is the real shader consumer fixture. Its
compute kernel reads layer 1 from `texture2d_array` and cube 1/+X from
`texturecube_array`, then validates both returned colors on the CPU. This pins
typed shader access and physical layer/face selection independently of upload
metadata tests; it does not prove within-face texel orientation.

## Vulkan upload behavior

- Create image with target compressed/uncompressed format and exact mip count from payload.
- Issue one `VkBufferImageCopy` per mip/array region from payload metadata.
- Do not runtime-generate mipmaps for already-provided compressed mip chains.
- Preserve existing queue/synchronization model (transfer+graphics phases) while extending copy region setup.

## Backend implementation

## API and type contract

The implemented contract includes:

- compressed BC7, ASTC 4x4, ETC2, BC5, and EAC formats where supported;
- `VkrTextureUploadRegion` metadata for mip, layer, extent, byte offset, and
  byte size;
- contiguous upload bytes plus explicit mip/layer counts and compression flag;
- device capability flags that select one transcodable target;
- structured upload publication shared by Metal and Vulkan.

## Constraints for compressed textures

- Partial writes require an explicitly supported block-aligned path.
- Resizing requires complete replacement rather than implicit recompression.
- Sampler-only updates remain valid.

## Build and Tooling Strategy

Dependency model:

- Vendor deterministic versions of required runtime/tooling dependencies (KTX/Basis stack) in-repo.
- Avoid network-dependent runtime builds for core compression path.

Offline pack pipeline:

1. Read authoring textures.
2. Convert to KTX2 + UASTC.
3. Generate and store mip chains.
4. Emit self-contained `.vkt`.

`tools/pack_vkt_textures.sh` and `.bat` own explicit incremental packing.
Normal renderer builds do not pack implicitly, and shader compilation remains
independent.

The same packer accepts explicit layered inputs. `--layer` order is physical
array order; cubemaps use `+X,-X,+Y,-Y,+Z,-Z`, and cubemap arrays repeat that
six-face group per cube:

```sh
build_vkt_packer/tools/vkr_vkt_packer \
  --output assets/textures/environment.vkt \
  --type cube --texture-class color-linear \
  --layer right.png --layer left.png \
  --layer up.png --layer down.png \
  --layer front.png --layer back.png

build_vkt_packer/tools/vkr_vkt_packer \
  --output assets/textures/decals.vkt \
  --type 2d-array --texture-class color-srgb \
  --layer decal0.png --layer decal1.png
```

`--type cube-array` requires a positive multiple of six layers. Every input
must have the same extent and mip count; cube faces must be square. Layered
outputs use the same atomic temporary-file publication as ordinary 2D packing.
Incremental skips compare complete ordered source-content and pack-settings
metadata; modification time and file size do not establish identity. Disabling
source-hash metadata also disables incremental skipping.

## Compatibility and Migration

## Legacy `.vkt` policy

- Existing raw-cache `.vkt` is version-detected legacy input.
- The runtime can read legacy files when compatibility is enabled and reports a
  migration warning.
- Tooling emits KTX2/UASTC; strict mode rejects legacy and missing artifacts.

KTX2/UASTC 2D, 2D-array, cubemap, and cubemap-array loading; target selection;
compressed backend upload; legacy detection; ordinary and explicit layered pack
tooling; strict-by-default runtime resolution; persistent target-transcode
caching; automatic/manual pressure budgets; bounded material-texture eviction;
and typed Metal array/cube-array shader consumption are implemented.

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

Persistent-cache evidence on the Apple M1 Pro:

- format tests cover hit, source invalidation, payload corruption, atomic
  replacement, and self-healing miss behavior;
- one Release Bistro cold fill recorded 700 misses and 700 writes;
- 700 target-native entries occupy 4.21 GB;
- a focused material run recorded three hits, zero misses, zero writes, and
  completed in 2.39 seconds;
- full-scene warm Bistro still exceeded 60 seconds. Persistent transcode caching
  removes Basis CPU work but cannot remove 4.2 GB of process-local GPU upload
  and hundreds of publication submissions.

## Test Cases and Scenarios

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
- The KTX2/UASTC path, device target selection, compressed uploads, and
  persistent device-transcode cache are implemented.
