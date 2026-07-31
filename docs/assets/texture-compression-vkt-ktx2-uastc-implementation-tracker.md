---
status: implemented
updated: 2026-07-31
authority: progress
---
# Texture Compression KTX2/UASTC Implementation Tracker

## Scope
- Feature: transition `.vkt` from raw RGBA cache to KTX2 + Basis UASTC runtime asset.
- This tracker is the source of truth for implementation status and per-phase details.
- Current cycle target: complete tooling/build-hook integration (`P5`) and rollout hardening (`P6`).

## Phase Status Board
| Phase | Status | Owner | Branch | Changed Files | Tests Run | Acceptance | Blockers |
|---|---|---|---|---|---|---|---|
| P0 | COMPLETED | Codex | current | docs/assets/texture-compression-vkt-ktx2-uastc-implementation-tracker.md | manual schema check | Tracker created with required schema | none |
| P1 | COMPLETED | Codex | current | lib/src/renderer/vkr_renderer.h; lib/src/renderer/renderer_frontend.c; lib/src/renderer/vulkan/vulkan_backend.h; lib/src/renderer/vulkan/vulkan_backend.c; lib/src/renderer/vulkan/vulkan_utils.c; lib/src/renderer/vulkan/vulkan_device.c; lib/src/renderer/systems/vkr_texture_system.h; lib/src/renderer/systems/vkr_texture_system.c; tests/src/texture_format_tests.h; tests/src/texture_format_tests.c; tests/src/texture_vkt_tests.h; tests/src/texture_vkt_tests.c; tests/src/test_main.h; tests/src/test_main.c | ./build_test.sh; ./build.sh Debug | Runtime plumbing compiles and tests pass | none |
| P2 | COMPLETED | Codex | current | lib/src/renderer/resources/loaders/texture_loader.c; lib/src/renderer/systems/vkr_texture_system.c | ./build_test.sh; ./build.sh Debug | Resolution order + legacy/KTX2 sniffing integrated with dual-path fallback | none |
| P3 | COMPLETED | Codex | current | lib/CMakeLists.txt; lib/src/renderer/systems/vkr_texture_system.c; lib/src/renderer/systems/vkr_texture_system.h | ./build_test.sh; ./build.sh Debug | Runtime KTX2/Basis decode+transcode path produces upload payloads with deterministic target selection | none |
| P4 | COMPLETED | Codex | current | lib/src/renderer/vulkan/vulkan_backend.c | ./build_test.sh; ./build.sh Debug | Payload-driven Vulkan upload path active with mip/layer region copies | none |
| P5 | COMPLETED | Codex | current | CMakeLists.txt; tools/CMakeLists.txt; tools/vkr_vkt_packer.cpp; tools/pack_vkt_textures.sh; build.sh | ./build_test.sh; VKR_TEXTURE_PACK_INPUT_DIR=/tmp/vkt_pack_test VKR_VKT_PACK=1 ./build.sh Debug | Fully programmatic `.vkt` KTX2/UASTC packer integrated and build-hooked without external KTX CLI dependency | none |
| P6 | COMPLETED | Codex | current | lib/src/renderer/systems/vkr_texture_system.h; lib/src/renderer/systems/vkr_texture_system.c; build.sh | ./build_test.sh; ./build/tools/vkr_vkt_packer --input-dir /tmp/vkt_pack_test --strict --verbose; VKR_TEXTURE_PACK_INPUT_DIR=/tmp/vkt_pack_test VKR_VKT_PACK=1 ./build.sh Debug | Runtime strict/dual-path controls and release pack strictness gate are active | none |

## Decisions Log
- Default rollout policy: dual-path + migration warning.
- First implementation cycle scope: runtime plumbing only (no texture loading behavior changes).
- `.vkt` remains the runtime-facing extension.
- `P4` was executed before full `P3` completion because Vulkan payload upload work is independent and unblocks later KTX2 transcode integration.
- Dependency source for `P3/P5`: `vendor/ktx-software` git submodule pinned to `KTX-Software v4.4.2` (`4d6fc70eaf62ad0558e63e8d97eb9766118327a6`).
- KTX runtime integration uses `ktx_read` linkage in `lib/CMakeLists.txt`.
- Offline pack flow uses in-repo `vkr_vkt_packer` (libktx API) instead of external `ktx`/`toktx` executables.
- Runtime rollout controls are environment-driven:
  - `VKR_TEXTURE_VKT_STRICT` enforces `.vkt`-only runtime and disables source fallback/legacy read.
  - `VKR_TEXTURE_VKT_ALLOW_SOURCE_FALLBACK`, `VKR_TEXTURE_VKT_ALLOW_LEGACY`, and `VKR_TEXTURE_VKT_WRITE_LEGACY_CACHE` tune dual-path development behavior.
- `build.sh` now enforces strict texture packing defaults for `Release` builds (`VKR_VKT_PACK_STRICT=1` unless explicitly overridden) and rejects `Release` builds with `VKR_VKT_PACK=0`.
- `NORMAL_RG` uses BC5/ASTC/EAC RG11 with a terminal RGBA target because
  libktx has no uncompressed two-channel Basis transcode target. EAC capability
  is probed independently from ETC2 RGBA.

## Phase Details
### P0
- Goal: create implementation tracker with fixed schema and initial statuses.
- Interfaces: documentation only.
- Implementation Steps:
  1. Create tracker file.
  2. Add fixed sections in required order.
  3. Initialize `P0..P6` rows and required fields.
- Failure Modes:
  - Missing required section or per-phase fields.
- Validation:
  - Manual section/order/schema check.
- Rollback:
  - Remove tracker file and recreate with required schema.

### P1
- Goal: add runtime plumbing for compressed formats and upload payload API.
- Interfaces:
  - `VkrTextureFormat` adds BC7/ASTC 4x4 variants.
  - `VkrTextureUploadRegion` and `VkrTextureUploadPayload` added.
  - `vkr_renderer_create_texture_with_payload(...)` added.
  - `VkrDeviceInformation` adds ASTC/BC7 support flags.
- Implementation Steps:
  1. Extend public enums/types/API signatures.
  2. Add frontend/backend entrypoint plumbing.
  3. Add Vulkan format mappings for new formats.
  4. Reject write/resize for compressed formats for rollout safety.
  5. Add tests for new format mapping and policy helpers.
- Failure Modes:
  - API mismatch between frontend/backend interfaces.
  - Missing format mapping to Vulkan `VkFormat`.
  - Compressed textures accepted by writable/resize paths.
- Validation:
  - Build tests and run test binary.
- Rollback:
  - Revert API additions and restore previous texture paths.
- Status:
  - Completed.

### P2
- Goal: implement `.vkt` resolution order and legacy vs KTX2 sniffing.
- Interfaces:
  - Texture resolution policy helpers in texture system.
- Implementation Steps:
  1. Add resolution order helper (`.vkt direct` -> sidecar -> source).
  2. Add file sniffing for legacy raw cache vs KTX2 signature.
  3. Keep dual-path behavior with warning for legacy.
- Failure Modes:
  - Incorrect path preference causing regressions.
- Validation:
  - Unit tests for resolution and sniff logic.
- Rollback:
  - Fall back to current source-image decode path.
- Status:
  - Completed.

### P3
- Goal: decode/transcode KTX2 UASTC into explicit upload payload.
- Interfaces:
  - New decode output contract based on `VkrTextureUploadPayload`.
- Implementation Steps:
  1. Integrate decoder/transcoder stack.
  2. Implement deterministic target selection and colorspace precedence.
  3. Populate payload regions and metadata.
- Failure Modes:
  - Unsupported target selection not falling back deterministically.
- Validation:
  - Unit tests and runtime load checks with source images removed.
- Rollback:
  - Disable KTX2 path via feature flag.
- Status:
  - Completed.

### P4
- Goal: payload-driven Vulkan texture upload.
- Interfaces:
  - Backend `texture_create_with_payload`.
- Implementation Steps:
  1. Create images from payload format/mip/layer metadata.
  2. Emit one `VkBufferImageCopy` per payload region.
  3. Skip runtime mip generation for prebuilt mip chains.
- Failure Modes:
  - Invalid region bounds/offset handling.
- Validation:
  - Upload correctness tests and sampling checks.
- Rollback:
  - Route to legacy `texture_create` path.
- Status:
  - Completed.

### P5
- Goal: add offline `.vkt` KTX2/UASTC pack tooling and build integration.
- Interfaces:
  - New tooling script(s) in `tools/`.
  - Build hook in `build.sh`.
- Implementation Steps:
  1. Add packer entrypoint and deterministic output policy.
  2. Hook pre-build pack step with incremental checks.
  3. Keep shader compilation path independent.
- Failure Modes:
  - Build-time packaging failures block dev workflow.
- Validation:
  - Packaging dry-run and incremental rebuild checks.
- Rollback:
  - Disable pack step and keep runtime fallback mode.
- Status:
  - Completed.

### P6
- Goal: rollout hardening with strict mode and migration controls.
- Interfaces:
  - Runtime flags for strict `.vkt` policy.
- Implementation Steps:
  1. Keep dual-path default in development.
  2. Add strict mode for release packaging flow.
  3. Finalize legacy support deprecation path.
- Failure Modes:
  - Strict mode enabled before assets are fully migrated.
- Validation:
  - Release-like runs with source textures removed.
- Rollback:
  - Re-enable dual-path fallback.
- Status:
  - Completed.

## Validation Log
- `P0`: tracker schema and required section order verified.
- `P1`:
  - `./build_test.sh` completed successfully, including new `Texture Format Tests` and `Texture VKT Tests`.
  - `./build.sh Debug` completed successfully.
  - Existing unrelated warnings remained in `bitmap_font_loader.c` and `vkr_rg_debug.c`.
- `P2`:
  - `./build_test.sh` completed successfully after decode-path resolution updates.
  - `./build.sh Debug` completed successfully.
  - Existing unrelated warnings remained in `bitmap_font_loader.c` and `vkr_rg_debug.c`.
- `P4`:
  - `./build_test.sh` completed successfully after payload upload path implementation.
  - `./build.sh Debug` completed successfully.
  - Existing unrelated warnings remained in `bitmap_font_loader.c` and `vkr_rg_debug.c`.
- `P3`:
  - Vendored dependency stack integrated via `vendor/ktx-software` submodule (`v4.4.2`).
  - `./build_test.sh` completed successfully with KTX linkage and transcode path compiled in.
  - `./build.sh Debug` completed successfully.
  - Existing unrelated warnings remained in `bitmap_font_loader.c` and `vkr_rg_debug.c`.
- `P5`:
  - Added `tools/vkr_vkt_packer.cpp` (programmatic packer): source decode, mip generation, KTX2/UASTC encode, and `vkr.*` metadata emission.
  - Added `tools/CMakeLists.txt` and root `CMakeLists.txt` integration for `vkr_vkt_packer` target.
  - Reworked `tools/pack_vkt_textures.sh` to run only the in-repo packer binary.
  - Updated `build.sh` to build `vkr_vkt_packer` before running the texture pack step.
  - `./build/tools/vkr_vkt_packer --input-dir /tmp/vkt_pack_test --strict --verbose` completed successfully.
  - `VKR_TEXTURE_PACK_INPUT_DIR=/tmp/vkt_pack_test VKR_VKT_PACK=1 ./build.sh Debug` completed successfully with pack step enabled.
  - `./build_test.sh` completed successfully after programmatic tooling integration.
  - Existing unrelated warnings remained in `bitmap_font_loader.c` and `vkr_rg_debug.c`.
- `P6`:
  - Added runtime policy controls in `vkr_texture_system`:
    - strict `.vkt`-only mode,
    - explicit source-fallback toggle,
    - explicit legacy read toggle,
    - explicit legacy sidecar write toggle.
  - Added strict-mode behavior in decode path:
    - fail when `.vkt` is missing/invalid and source fallback is disabled,
    - fail when legacy `.vkt` is disallowed in strict mode,
    - keep dual-path warning behavior in development mode.
  - Added release packaging gate in `build.sh`:
    - release builds require texture pack step enabled,
    - release builds default to strict pack mode.
  - `./build_test.sh` completed successfully.
  - `./build/tools/vkr_vkt_packer --input-dir /tmp/vkt_pack_test --strict --verbose` completed successfully.
  - `VKR_TEXTURE_PACK_INPUT_DIR=/tmp/vkt_pack_test VKR_VKT_PACK=1 ./build.sh Debug` completed successfully.
  - Existing unrelated warnings remained in `bitmap_font_loader.c` and `vkr_rg_debug.c`.
- Post-phase cleanup/semantic-compression pass:
  - Refactored duplicated extension parsing in `texture_loader.c` into local helpers.
  - Centralized frontend texture-create result mapping in `renderer_frontend.c`.
  - Centralized compressed-texture mutation rejection in `vulkan_backend.c` and preserved rollout behavior.
  - Removed shell `eval` from `tools/pack_vkt_textures.sh` by switching to argument-safe invocation.
  - `./build_test.sh` completed successfully.
  - `cmake --build build -j 8` completed successfully.
  - `VKR_TEXTURE_PACK_INPUT_DIR=/tmp/vkt_skip_nonexistent ./build.sh Debug` completed successfully.
- Post-P1 fallback hardening:
  - Added EAC RG11 format/capability/transcode support and replaced the
    unreachable RG8 terminal choice with RGBA.
  - Exhaustively validated all 1,024 selector combinations; every result maps
    to a libktx transcode target.

## Open Risks
- Compressed texture write/resize semantics are intentionally restricted in rollout 1.
- First full-repository texture pack can be expensive; incremental skips make subsequent builds fast.
- Strict runtime mode requires migrated `.vkt` coverage; enabling it too early will hard-fail legacy/source-only assets.

## Next Phase Entry Criteria
- `P0 -> P1`: tracker exists with required sections and initialized status board.
- `P1 -> P2`: API changes compile and tests pass.
- `P2 -> P3`: resolution/sniffing tests pass and behavior is deterministic.
- `P3 -> P4`: payload contract stable and validated. (`P4` implementation is complete; full value depends on `P3` decode output integration.)
- `P4 -> P5`: runtime upload path validated on target platforms.
- `P5 -> P6`: packaging flow stable and migration coverage complete.
