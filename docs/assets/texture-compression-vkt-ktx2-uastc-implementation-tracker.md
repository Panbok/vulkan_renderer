# Texture Compression KTX2/UASTC Implementation Tracker

## Scope
- Feature: transition `.vkt` from raw RGBA cache to KTX2 + Basis UASTC runtime asset.
- This tracker is the source of truth for implementation status and per-phase details.
- Current cycle target: complete Vulkan payload upload (`P4`) and unblock KTX2 decode/transcode (`P3`).

## Phase Status Board
| Phase | Status | Owner | Branch | Changed Files | Tests Run | Acceptance | Blockers |
|---|---|---|---|---|---|---|---|
| P0 | COMPLETED | Codex | current | docs/assets/texture-compression-vkt-ktx2-uastc-implementation-tracker.md | manual schema check | Tracker created with required schema | none |
| P1 | COMPLETED | Codex | current | lib/src/renderer/vkr_renderer.h; lib/src/renderer/renderer_frontend.c; lib/src/renderer/vulkan/vulkan_backend.h; lib/src/renderer/vulkan/vulkan_backend.c; lib/src/renderer/vulkan/vulkan_utils.c; lib/src/renderer/vulkan/vulkan_device.c; lib/src/renderer/systems/vkr_texture_system.h; lib/src/renderer/systems/vkr_texture_system.c; tests/src/texture_format_tests.h; tests/src/texture_format_tests.c; tests/src/texture_vkt_tests.h; tests/src/texture_vkt_tests.c; tests/src/test_main.h; tests/src/test_main.c | ./build_test.sh; ./build.sh Debug | Runtime plumbing compiles and tests pass | none |
| P2 | COMPLETED | Codex | current | lib/src/renderer/resources/loaders/texture_loader.c; lib/src/renderer/systems/vkr_texture_system.c | ./build_test.sh; ./build.sh Debug | Resolution order + legacy/KTX2 sniffing integrated with dual-path fallback | none |
| P3 | IN_PROGRESS | Codex | current | lib/src/renderer/systems/vkr_texture_system.c (existing scaffolding) | pending | KTX2 decode/transcode integrated | Missing vendored KTX2/Basis decode+transcode stack |
| P4 | COMPLETED | Codex | current | lib/src/renderer/vulkan/vulkan_backend.c | ./build_test.sh; ./build.sh Debug | Payload-driven Vulkan upload path active with mip/layer region copies | none |
| P5 | NOT_STARTED | TBD | pending | pending | pending | Offline pack tooling + build hook integrated | pending |
| P6 | NOT_STARTED | TBD | pending | pending | pending | Strict rollout controls complete | pending |

## Decisions Log
- Default rollout policy: dual-path + migration warning.
- First implementation cycle scope: runtime plumbing only (no texture loading behavior changes).
- `.vkt` remains the runtime-facing extension.
- `P4` was executed before full `P3` completion because Vulkan payload upload work is independent and unblocks later KTX2 transcode integration.
- Dependency source for `P3/P5`: `vendor/ktx-software` git submodule pinned to `KTX-Software v4.4.2` (`4d6fc70eaf62ad0558e63e8d97eb9766118327a6`).

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
  - In progress; blocked on adding a deterministic in-repo KTX2/Basis decode+transcode dependency.

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

## Open Risks
- No in-repo KTX2/Basis dependency stack is present yet; `P3`/`P5` depends on this.
- Compressed texture write/resize semantics are intentionally restricted in rollout 1.
- KTX2 container detection exists, but runtime KTX2 transcode cannot proceed until dependency vendoring is implemented.

## Next Phase Entry Criteria
- `P0 -> P1`: tracker exists with required sections and initialized status board.
- `P1 -> P2`: API changes compile and tests pass.
- `P2 -> P3`: resolution/sniffing tests pass and behavior is deterministic.
- `P3 -> P4`: payload contract stable and validated. (`P4` implementation is complete; full value depends on `P3` decode output integration.)
- `P4 -> P5`: runtime upload path validated on target platforms.
- `P5 -> P6`: packaging flow stable and migration coverage complete.
