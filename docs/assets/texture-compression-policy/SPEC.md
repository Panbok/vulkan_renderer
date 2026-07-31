---
status: implemented
updated: 2026-07-31
authority: spec
---
# Robust Texture Compression Policy for .vkt (KTX2 + UASTC Source, Capability-Driven Runtime Transcode)

## 1. Purpose and Scope

This document defines the authoritative v2 policy for `.vkt` texture compression
selection in the renderer. The capability-driven ladders and loadable terminal
fallbacks are implemented.

Scope:

- Define deterministic runtime transcode selection rules for cross-platform use.
- Define texture intent classes used by selection logic.
- Define capability contract, metadata contract, fallback behavior, and rollout.
- Define the validation matrix and completion criteria.

Out of scope:

- Asset migration execution.
- Benchmark target setting beyond policy validation requirements.

## 2. Current Implementation Boundary

The runtime now implements:

- Offline packer emits a universal KTX2 + Basis UASTC source payload.
- Texture-class-aware ASTC, BC7, ETC2, BC5, EAC RG11, and RGBA8 selection
  ladders.
- Capability-gated selection with device type used only for preference order.
- Material/request/metadata intent resolution and strict `.vkt` controls.

libktx has no uncompressed two-channel Basis transcode target. `NORMAL_RG`
therefore uses capability-gated EAC RG11 below BC5/ASTC and terminates at the
loadable RGBA8 target. EAC support is probed independently from ETC2 RGBA.

## 3. Policy Invariants

These invariants are normative:

1. `.vkt` remains KTX2 + UASTC as the source container format.
2. Runtime chooses final GPU transcode format using device capabilities and
   texture class.
3. Platform and device type are preference inputs only; capability checks are
   authoritative gates.
4. Policy selection must be deterministic for the same input tuple
   `(texture class, request colorspace, device capabilities, device type)`.
5. Strict `.vkt` runtime mode must not silently decode source images.

## 4. Texture Classes

Runtime selection is based on explicit texture intent classes:

- `COLOR_SRGB`: Albedo/base-color/emissive/UI/skybox color inputs intended for
  sRGB sampling.
- `COLOR_LINEAR`: Linear color inputs that must not be sampled as sRGB.
- `NORMAL_RG`: Tangent-space or object-space normal maps where preserving vector
  precision in two channels is preferred.
- `DATA_MASK`: Non-color packed data textures (ORM, AO, roughness, metallic,
  specular mask, gloss mask, utility masks).

Implementation policy must treat class as first-class selector input.

## 5. Capability Contract (Policy-Level)

The runtime selector contract requires these capability flags:

- `supports_texture_astc_4x4`
- `supports_texture_bc7`
- `supports_texture_etc2`
- `supports_texture_bc5`
- `supports_texture_eac_r11g11`

Device type is a preference hint:

- Discrete GPU
- Integrated/tiled GPU
- Other/unknown

Capability flags determine whether a candidate format is valid. Device type only
affects candidate ordering.

## 6. Deterministic Format Ladders

All ladders are ordered candidate lists. Selector picks the first format that is
supported.

### 6.1 Color Classes (`COLOR_SRGB`, `COLOR_LINEAR`)

For integrated/tiled preference:

1. ASTC 4x4
2. ETC2 RGBA
3. BC7
4. RGBA8 fallback

For discrete preference:

1. BC7
2. ASTC 4x4
3. ETC2 RGBA
4. RGBA8 fallback

Colorspace application:

- `COLOR_SRGB`: choose sRGB variant where format family supports sRGB.
- `COLOR_LINEAR`: choose UNORM variant.

### 6.2 Normal Class (`NORMAL_RG`)

For discrete preference:

1. BC5 UNORM
2. ASTC 4x4 UNORM (RG payload packed into RGBA blocks)
3. EAC RG11 UNORM
4. RGBA8 UNORM fallback

For integrated/tiled preference:

1. ASTC 4x4 UNORM (RG payload packed into RGBA blocks)
2. BC5 UNORM
3. EAC RG11 UNORM
4. RGBA8 UNORM fallback

### 6.3 Data/Mask Class (`DATA_MASK`)

For integrated/tiled preference:

1. ASTC 4x4 UNORM
2. ETC2 RGBA UNORM
3. BC7 UNORM
4. RGBA8 UNORM fallback

For discrete preference:

1. BC7 UNORM
2. ASTC 4x4 UNORM
3. ETC2 RGBA UNORM
4. RGBA8 UNORM fallback

## 7. Colorspace Rules

1. sRGB formats are allowed only for `COLOR_SRGB`.
2. `COLOR_LINEAR`, `NORMAL_RG`, and `DATA_MASK` must always resolve to linear
   (UNORM) formats.
3. If request colorspace and class conflict, class semantics win for
   `NORMAL_RG`/`DATA_MASK` (remain linear).

## 8. Request and Metadata Precedence

Selector inputs are resolved in this strict order:

1. Explicit request query (for example `?cs=` and future class override).
2. Material slot intent mapping (slot-defined class and colorspace intent).
3. Embedded `.vkt` metadata hints.
4. Filename heuristics (lowest-priority fallback only).

If two higher-priority inputs conflict, selector logs the conflict and applies
the highest-priority source.

## 9. `.vkt` Metadata Schema

The following KTX2 metadata keys are required by policy:

- `vkr.texture_class`: one of `color_srgb`, `color_linear`, `normal_rg`,
  `data_mask`.
- `vkr.colorspace_hint`: one of `srgb`, `linear`.
- `vkr.has_transparency`: `0` or `1`.
- `vkr.alpha_mask`: `0` or `1`.
- `vkr.asset_version`: integer/string schema version.

Metadata is advisory for runtime intent resolution unless overridden by higher
precedence request inputs.

## 10. Failure and Fallback Behavior

1. Selection must always produce one deterministic, loadable target format.
2. If no compressed candidate is supported, selector falls back to uncompressed
   format defined by class ladder.
3. In strict `.vkt` mode:
   - no silent source-image decode fallback;
   - failures surface explicit load error.
4. Logs must contain deterministic fallback reasons:
   - missing capability,
   - class/colorspace conflict normalization,
   - strict-mode rejection path.

## 11. Implementation Status

The dual-path baseline, class-aware selector, capability plumbing, strict mode,
ETC2/BC5/EAC paths, and loadable RGBA fallback are implemented. Release flows
can require `.vkt`, while development keeps explicit compatibility controls.
Asset migration coverage is operational state rather than a guarantee of this
policy document.

## 12. Validation Matrix

Implementers must validate at minimum:

1. Color textures choose expected family under all capability combinations
   (ASTC/BC7/ETC2 availability permutations).
2. Normal textures prefer BC5 or ASTC per device preference and never choose
   sRGB; EAC and RGBA fallbacks remain loadable.
3. Data/mask textures stay linear and follow deterministic ladder.
4. Request precedence resolution behaves deterministically for conflicting
   request/material/metadata inputs.
5. sRGB/linear output correctness for color paths.
6. Strict mode correctly rejects missing/invalid `.vkt` without silent source
   fallback.
7. Scene reload cycles do not regress memory stability.
8. Every selector result maps to a libktx transcode target across the complete
   capability matrix.

## 13. Acceptance Criteria

Policy implementation is complete only when:

1. Selector determinism is proven via tests for identical input tuples.
2. Class-based target selection matches this spec ladders.
3. ETC2 and BC5 policy paths remain integrated into the selection contract.
4. Capability gates are authoritative; platform/device type never bypasses
   unsupported formats.
5. Failure/fallback logging is explicit and actionable.
6. The `NORMAL_RG` terminal fallback produces a loadable target.

## 14. Implemented Public API and Type Changes

The implementation includes:

1. `VkrTextureFormat` additions:
   - BC5 UNORM
   - ETC2 RGBA UNORM
   - ETC2 RGBA SRGB
   - EAC RG11 UNORM
2. `VkrDeviceInformation` additions:
   - ETC2 support flag
   - BC5 support flag
   - EAC RG11 support flag
3. A selector contract that accepts texture class and capability set as
   explicit inputs.

## 15. Assumptions and Defaults

1. Existing `docs/assets/texture-compression-vkt-ktx2-uastc-spec.md` remains as
   historical context.
2. This document is the authoritative v2 policy reference for robust texture
   compression selection.
3. Code is the implementation authority; this document defines the implemented
   selection policy.
