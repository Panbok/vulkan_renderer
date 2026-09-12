---
status: implemented
updated: 2026-09-12
authority: adr
---
# ADR-012: KTX2/UASTC texture artifacts with capability-selected transcode

## Status

Accepted.

## Context

Texture source images are unsuitable as the normal runtime distribution format,
but the selected GPU compression family varies by device.

## Decision

The texture packer writes KTX2 containers with Basis UASTC payloads using the
runtime `.vkt` extension. Runtime detection uses the container signature, not
the extension alone. The loader selects a transcode target from texture class,
sRGB intent, device class, and advertised BC7, BC5, ASTC, ETC2, and EAC RG11
support. Every selected target has a libktx transcode mapping; RGBA32 is the
terminal fallback.

For ordinary texture jobs, the offline packer filters `color-srgb` RGB channels in linear light using the
[sRGB transfer functions](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html),
then encodes each mip back to sRGB. Alpha and all
channels of `color-linear`, `normal-rg` and `data-mask` use linear area averages,
rounded to the nearest byte. Each destination footprint covers its full source
area, including fractional texel overlap at odd dimensions. Mip dimensions
remain `max(1, previous / 2)`; the base level and normal G-to-A storage mapping
are unchanged. The filter borrows disjoint image buffers owned by the packer
and uses bounded stack scratch; it adds no runtime work or GPU resources.

`vkr.pack_settings` records `mips=rgba8-area-srgb-v2`. Repacking rejects the
old `rgba8-box-v1` identity even when the source hash matches. Existing assets
must be repacked to receive the new mips; runtime loading does not recook them.

Explicit color-texture jobs can supply `--alpha-cutoff <0..1>` and optional
`--alpha-factor <0..1>` (default 1). These are the material cutoff and base-color
alpha multiplier. Cutout RGB uses alpha-weighted linear-light filtering and
returns to straight color storage; alpha is averaged linearly. Cutoff zero
retains ordinary color filtering because every texel passes. The base mip is
unchanged. After building the complete unadjusted chain, each lower mip scales
alpha to the closest attainable level-0 texel-center coverage at the inclusive
material alpha test. Adjusted alpha never feeds the next reduction. Equal alpha
bins remain equal, and zero alpha stays zero. Small mips, bilinear/trilinear
sampling and lossy compression can prevent exact rendered coverage.

The explicit policy sets `vkr.alpha_mask` and extends `vkr.pack_settings` with
`cutout=weighted-coverage-v1`, cutoff and factor. It is accepted only with an
explicit output and color class, so folder inference cannot apply a guessed
threshold to blend or data textures. Ordinary packing retains its existing
filter policy. Runtime sampling and alpha-test shaders are unchanged.

glTF material cooking calls the same packing library for MASK base-color
textures, including prepared specular-glossiness textures. Generated direct
`.vkt` paths contain the source-content hash, cutoff/factor float bits and the
shared cutout policy version. Equal recipes share a variant; differing material
policies produce separate files. The material's texture reference is written
only after successful atomic variant publication. Original images and ordinary
sidecars remain available to opaque/blend consumers. Generated textures join the
mesh dependency list, so recooking records their content in the mesh artifact.

Materials whose consumers have non-unit vertex alpha retain ordinary filtering
with a cooker diagnostic: one texture-wide correction cannot account for that
spatially varying shader multiplier. Source images are required to bake variants;
packed-only source inputs fail with a diagnostic. Changed material cutoffs or
factors require recooking to update the bake.

For compatible glTF normal/metallic-roughness inputs, cooking emits a paired
`normal_rg` and `data_mask` recipe. It reconstructs positive tangent Z from
unscaled decoded XY, scales XY, then normalizes, with a flat-normal fallback
when squared length is at most `1e-12`. It retains the full mean normal through
area reductions.
It also retains the mean fourth power of effective perceptual roughness
`r = roughness_factor * texture.G`. The encoded direction is the normalized mean;
the encoded roughness is
`(min(1, mean(r^4) + min(0.25, 2*(1-L^2)/(L*(3-L^2)))))^(1/4)`, where `L` is
the mean normal length clamped to one. At zero length, use a flat direction and
variance 0.25. This uses a
[vMF concentration approximation](https://graphicrants.blogspot.com/2018/05/normal-map-filtering-using-vmf-part-3.html)
for a bounded isotropic GGX width adjustment, not an exact convolution of GGX
lobes. The fourth-power domain and variance cap match the existing runtime
GGX filter; screen-space specular AA remains enabled.

Unnormalized double-precision moments feed later mips; quantized directions and
broadened roughness never feed another reduction. Constant inputs retain their
roughness, including zero; runtime shading retains its existing roughness floor.
MR R/B/A keep ordinary area filtering. A material without an MR image receives a
texture with neutral AO/metallic channels. Normal strength and roughness factor
are folded into both base and lower mips, within byte/compression precision;
generated materials set those two factors to one and retain the metallic factor.
Changing the factors afterward requires recooking. An authored zero normal scale
flattens the normal; an omitted scale defaults to one.

Pairing requires decodable external images with identical extents, untransformed
UV0 and default or repeat/linear/trilinear samplers. Converted specular-glossiness
images can participate when their source mappings satisfy the same constraints.
Mismatched extents, unsupported mappings/samplers, packed-only images and absent
source files retain both ordinary texture references and factors with a diagnostic.
Present but malformed source images fail the cook before material publication. This does
not add runtime support for glTF UV transforms or authored sampler state.

Policy version 2 corrects the order of normal-strength application. Version 1
variants must be regenerated from source through the cooker; existing version 1
files are not overwritten or reused by version 2 recipes.

Both output paths and metadata include a shared policy version, both source
identities (or absent MR), normal strength and roughness factor. Equal recipes
share files. Each file publishes atomically; the material references and mesh
dependencies publish only after both succeed. The two file renames are not a
filesystem transaction. Already completed recipe files may survive a later
failure. Existing textured pairs add no texture reads; factor-only materials
gain an MR texture and its existing shader sampling path.

The CLI and mesh cooker link one tool-owned packing library. The synchronous
C entry points borrow path strings and release decoded images, mip buffers and
KTX objects before returning, including exception/failure exits. Paired filtering
allocates moments from mip 1 onward and retains only the current and next moment
levels. The importer releases paired-source hashing bytes before image decoding,
keeps borrowed path strings until return, and retains output paths in its load
arena. There is no nested process to survive editor
cancellation. Successful content-cache variants may remain after a later mesh
cook failure; failed material publication never references an unfinished file.

The runtime accepts 2D, array, cubemap, and cubemap-array KTX2 payloads. It
caches target-native transcode output. Strict `.vkt` mode disables source and
legacy-raw fallback; development configuration retains those migration paths.

## Consequences

Texture class is part of the asset contract: color, normal, and data textures
cannot share an arbitrary transcode policy. Writable and resize paths must
reject block-compressed textures. A legacy raw `.vkt` is migration support, not
the canonical artifact.

## Alternatives considered

Shipping one pre-transcoded format requires separate asset sets. Loading only
raw source images shifts decode and bandwidth costs into runtime loading.
A separate normal-variance texture would support more dynamic material changes,
but needs additional runtime representation and sampling. Paired cooked variants
were selected to use the existing portable material inputs.

## Revisit when

The supported device profile adds a compression family or the runtime no longer
needs legacy `.vkt` compatibility.

## Code evidence

- [packer](../../tools/vkr_vkt_packer.cpp)
- [offline mip filter](../../tools/vkr_vkt_mips.h)
- [normal/roughness moments](../../tools/vkr_vkt_normal_roughness.h)
- [synchronous cooker entry point](../../tools/vkr_vkt_packer.h)
- [glTF material integration](../../tools/assets/mesh_loader_gltf.c)
- [analytic mip checks](../../tests/src/texture_vkt_tests.c)
- [selection and KTX2 load](../../runtime/src/renderer/systems/vkr_texture_system.c)
- [texture contract](../../runtime/src/renderer/systems/vkr_texture_system.h)
