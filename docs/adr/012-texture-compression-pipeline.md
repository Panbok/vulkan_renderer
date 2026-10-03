---
status: implemented
updated: 2026-10-03
authority: adr
---
# ADR-012: Host-native KTX2 texture artifacts

## Status

Accepted.

## Context

Texture source images are unsuitable as the runtime distribution format, and
the supported GPUs ([ADR-083](083-supported-hardware-matrix.md)) sample two
compression families: every Apple silicon Mac samples ASTC, and every RDNA 2
and Ampere GPU samples BC. A transcodable intermediate (Basis UASTC, removed
2026-10-03) served both from one file, but it is 8 bits per texel on every
device, transcodes only to 4x4-block formats, costs a runtime transcode and
cache, and decided sRGB against UNORM at load.

## Decision

Every `.vkt` is a KTX2 container of host-native blocks, cooked by `vkr_bakery`
on the machine that uses it, and only source images are tracked in the
repository. The host encoding is ASTC on Apple silicon and BC on x86-64; each
platform's bundles build on that platform (ADR-078). The runtime uploads the
stored blocks as they are, with no transcode or transcode cache. A file that
holds Basis blocks, or a format the device cannot sample, fails its load with
an instruction to rebuild it with `vkr_bakery` on this host.

A texture request resolves only to a cooked file: the path itself when it
names a `.vkt`, else the source's `<source>.vkt` sidecar. A missing file fails
the load; there is no source-image fallback and no legacy raw `.vkt` reader.
An explicit `source=only` query still decodes a source image, for generated
UI images (content thumbnails, template previews, MTSDF atlases), and probe
cubemaps given as six face images decode their faces directly.

A request's explicit `cs=` and `tc=` keep choosing the sampling space, as
they chose the transcode target before: ASTC 4x4, ASTC 6x6 and BC7 upload
through their sRGB or UNORM view (the blocks are identical), sRGB only for a
colour-sRGB class sampled in sRGB; without the query, the stored format
stands. Mip 0 holds the source bytes either way, so a class inferred
differently at cook time changes only the filtering of lower mips.

The build cooks the engine textures every package and install carries (the
UbuntuMono atlas and the mannequin's textures, listed in
`cmake/vkr_engine_content.cmake`) beside their sources, through the
`vkr_engine_textures` target the app, harness, editor and player depend on.
Scene, fixture and repository textures cook through `assets/bakery.json`.

On Apple silicon the ASTC encoding (`--encoding astc`, astcenc's `fastest`
preset) stores 6x6 blocks for colours and data masks and 4x4 for normals and
alpha-tested colours; on x86-64 BC is the default (below). On Bistro's converted colours, paired normals
and metallic-roughness, `fastest` measured 52.6, 40.1 and 49.2 dB against 52.2,
38.5 and 47.4 dB for UASTC `faster`, at four to nine times its speed. The
settings identity records `encoding=astc-6x6-fastest` or `astc-4x4-fastest`,
so files encoded before 6x6 re-encode. Final and preview files take
host-neutral names (`<source>.vkt`, `.preview` variants), so tracked
references name the same file on every host; the identity tells the hosts'
encodings apart and re-encodes a file cooked on another host. ASTC normals
store alpha as one, and BC5 stores none; every shader samples XY.
The packer calls astcenc directly, since libktx exposes no search limits: other
classes keep the preset, and normals stop searching a block at 39 dB with one
candidate. On baked Bistro normals that encoded 1.8 times faster and scored
41.2 dB in RG against 40.9 for UASTC `faster`.
The 6x6 blocks cost 3.56 bits per texel against 8, which cuts colour and
data-mask memory by 2.25 times for the M1 memory floor (ADR-083). On one in
six of Bistro's level-0 images (31 colours, 158 Mpx; 32 metal-roughness masks,
133 Mpx; astcenc `fastest`, 2026-10-03), colours measured 43.8 against
51.5 dB for 4x4 (worst image 33.6 against 40.4 dB) and masks 51.4 against
72.6 dB (worst 39.6 against 59.6 dB), at 1.2 and 1.4 times the encode speed.
Alpha-tested colours lost about 10 dB in RGB at 6x6 and keep 4x4, since
their alpha decides coverage; normals keep 4x4 because their error enters
shading directly. On Metal a 1024² Bistro colour rendered from 6x6 blocks
measured 46.5 dB in final colour against its UASTC transcode, and 47.5 dB
as a linear texture. ASTC LDR support covers every block size, so the
existing ASTC capability gates 6x6 on both backends.
A third encoding, `astc-fast`, encodes every class as ASTC 4x4 with Apple's
system encoder (AppleTextureEncoder, macOS only), with channels weighed
equally and blocks accepted below a mean square error of 2^-12. It is for
textures only the editor shows (ADR-077's `texture_encode_speed`): on Bistro's
pre-encode data it ran 2.2 times astcenc's speed on paired normals (36.6
against 40.4 dB in RG; the encoder saturates there at any threshold), 3.4
times on metallic-roughness (62.0 against 67.4 dB) and 2.6-3.3 dB below
astcenc on alpha-weighted colours. Its identity records
`encoding=astc-4x4-system-equal-t12-v1` and its files carry `.astc-fast`
names, so neither ASTC encoding satisfies the other's recipe.
A native BC encoding, `bc`, serves x86-64 hosts, whose desktop GPUs all
sample BC; managed imports choose it by default there, and it is built only
on x86-64 (`cmake/vkr_bc7e.cmake`), so other hosts reject it. Colours and
data encode BC7 (vkFormat 145 unorm, 146 sRGB) with Binomial's bc7e
([vendored](../../vendor/bc7enc_rdo.md)); normals encode BC5 (141) from R and
G with rgbcx. On one in six of a Bistro finalize's pre-encode images (84
level-0 images dumped before encoding; a Ryzen 5 2600 on 12 threads, whole
class as one mean square error), UASTC `faster` scored 51.1 dB in RGB on
colours at 4.1 Mpx/s, 65.8 dB on metallic-roughness at 16.5 and 41.7 dB in RG
on paired normals at 2.4. bc7e's `veryfast` profile scored 54.1 dB on colours
at 13.0 Mpx/s, better on 25 of 28 images; rgbcx's BC5 scored 48.5 dB on
normals at 566 Mpx/s, since BC5 keeps the two channels apart. Data masks keep
bc7e's default profile, whose channel-rotation modes uncorrelated channels
need: 65.3 dB for the class at 31 Mpx/s, 0.5 dB below UASTC because one
texture loses 1.9 dB under every BC7 profile, while the mean per image is 70.2
against 66.6 dB and 8 of the 12 textured images score higher; accepted
(2026-09-28). The faster profiles drop those modes and do not keep constant
masks exact. Compressonator's CMP_Core was measured and rejected: 2.3-3.1
Mpx/s and 50.4-52.8 dB on colours, 50.6-61.9 dB on data masks. The
identities record `encoding=bc7-bc7e-veryfast-v1`, `bc7-bc7e-default-v1` and
`bc5-rgbcx-v1`, and files carry `.bc` names. `bc-fast`, the editor's fast
speed, encodes colours with bc7e's `ultrafast` profile (51.4 dB at 54 Mpx/s,
`bc7-bc7e-ultrafast-v1`) and data masks with bc7e limited to modes 4, 5 and 6
and rotations 0, 2 and 3 (`bc7-bc7e-m456-r023-v1`), under `.bc-fast` names;
normals encode as `bc` does. On another one-in-six sample of 42 masks that
profile scored 64.7 dB for the class (mean 63.0, worst image 49.5) against
65.9 for the default profile and 63.5 (62.3, 49.1) for UASTC `faster`, at 63
against 36 and 17 Mpx/s, and kept constant masks exact.
The encoder repeats one encoded block row for a uniform image, and paired
bakes tabulate base-level moments over the 65,536 normal X/Y byte pairs; both
produce the bytes the direct computation does. A data-mask row encodes each
distinct block once, solid blocks grouped ahead of the rest, and copies the
result to its repeats (176 against 113 Mpx/s on Bistro's masks, same bytes).
bc7e is built for four-lane SSE2 and SSE4 only (2026-09-29): on a Ryzen 5
2600 that encoded Bistro's colours 1.55 times as fast as eight-lane AVX2 (83
against 53 Mpx/s, 52.276 against 52.275 dB, masks 64.411 against 64.414),
and without fused multiply-adds every x86 host encodes the same bytes.
The base colour of an opaque glTF material, whose alpha no shader reads
(both backends output alpha one for opaque materials and test or blend it
only for cutout and blend), packs with alpha one under its own identity
(`alpha=opaque-v1`, `basecolor_opaque` names). Bistro's RGBA diffuse maps
carry stray alpha of 251-254 in a few percent of texels, which sent whole
blocks down bc7e's alpha modes: opaque, its colours encoded at 203 against
83 Mpx/s and 52.309 against 52.276 dB in RGB. A Bistro render of the
finalized scene against the one before these changes: PSNR 67.2 dB, mean
0.003/255, 13 of 691,200 pixels above 10/255.

A load limit drops the mips of a 2D texture above a maximum extent after
decode, for every KTX2 load. The texture then
loads as its first mip within the limit; the kept mips move to 16-byte-aligned
offsets at the front of the upload bytes. Cubemaps, arrays and single-level
images load unchanged, and the smallest mip always remains. The Graphics
setting `texture_resolution` selects 1024, 2048 or full resolution, applies
at the next start, and defaults to 2048 on Metal and full resolution on Vulkan
(ADR-083's memory floor). Cooked files do not change.
Bistro, measured on the M1 Pro (Metal Release, a one-repetition copy of
`bistro_metal_production_040`, `local-windowed-gpu-single`, 2026-10-03):
texture memory fell from 3.176 to 1.995 GB and driver allocation from 5.86 to
4.65 GB (reports `a6f36ee8…`, `a477dc1b…`). Its 74 textures at 4096² hold
most of the difference. Final colour against the unlimited load measured
109.5 dB PSNR from the street overview of `bistro_windowed_snapshot` (maximum
1/255) and 88.2 dB from a street-level facade view at 1920x1440 output, with
three G-buffer albedo pixels above 10/255.

For ordinary texture jobs, the offline packer filters `color-srgb` RGB channels in linear light using the
[sRGB transfer functions](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html),
then encodes each mip back to sRGB. Alpha and all
channels of `color-linear`, `normal-rg` and `data-mask` use linear area averages,
rounded to the nearest byte. Each destination footprint covers its full source
area, including fractional texel overlap at odd dimensions. Mip dimensions
remain `max(1, previous / 2)`; the base level
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
variance 0.25. The encoded normal depends only on the normal source and scale,
so its output is named and recorded by those inputs and shared by every
roughness input and factor; the roughness output keeps the full pair key. This
uses a
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

The runtime accepts 2D, array, cubemap, and cubemap-array KTX2 payloads of
native blocks, and uncompressed RGBA16F cubemaps. Tools that read texels on
the CPU decode native blocks: the material baker through libktx's ASTC
decoder and basisu's BC7 and BC5 unpackers.

## Consequences

Texture class is part of the asset contract: color, normal, and data textures
take different block sizes and sRGB intent at cook time. Writable and resize
paths must reject block-compressed textures. A fresh checkout renders
nothing textured until `vkr_bakery` cooks it, and a `.vkt` cooked on one host
does not load on the other family's GPUs. The mannequin samples its source
normal and ORM images instead of paired bakes, whose hash-named copies were
host-specific, so it loses the paired normal-variance roughness widening.

Paired roughness outputs multiply full-resolution textures by distinct factor
and scale combinations. Before normal outputs were shared, a managed Bistro
import produced 374 paired files (2.2 GiB), 198 of them for materials without a
roughness map, and one normal map had 24 identical variants; the 187 normal
variants had 119 distinct (source, scale) inputs. Its materials referenced
577 textures (3.4 GiB); with render targets the Scene exceeded the former fixed
5 GiB Metal cap ([ADR-024](024-shared-bindless-gpu-cores.md)).

## Alternatives considered

One transcodable UASTC file for every device kept 8 bits per texel and 4x4
blocks everywhere, plus a runtime transcode and its cache; it was removed for
the M1 memory floor. Tracking both an ASTC and a BC copy doubles the tracked
bytes and re-commits them whenever an encoder changes. Loading only raw source
images shifts decode and bandwidth costs into runtime loading.
A separate normal-variance texture would support more dynamic material changes,
but needs additional runtime representation and sampling. Paired cooked variants
were selected to use the existing portable material inputs.

## Revisit when

The supported device profile adds a compression family, cross-platform
bundles become a requirement (bc7e would need an ARM build to cook BC on a
Mac), or a BC7 profile reaches the former UASTC `faster` bar on the hardest
data mask at a useful speed.

## Code evidence

- [packer](../../tools/vkr_vkt_packer.cpp)
- [offline mip filter](../../tools/vkr_vkt_mips.h)
- [normal/roughness moments](../../tools/vkr_vkt_normal_roughness.h)
- [synchronous cooker entry point](../../tools/vkr_vkt_packer.h)
- [glTF material integration](../../tools/assets/mesh_loader_gltf.c)
- [BC encoder build](../../cmake/vkr_bc7e.cmake)
- [analytic mip checks and native ASTC, BC7 and BC5 loads](../../tests/src/texture_vkt_tests.c)
- [encoding names, identities and formats](../../tools/checks/check_editor_texture_tiers.py)
- [KTX2 load and sRGB view](../../runtime/src/renderer/systems/vkr_texture_system.c)
- [engine texture cook](../../cmake/vkr_engine_content.cmake)
- [CPU block decode](../../tools/bake/vkr_bake_material.cpp)
- [texture contract](../../runtime/src/renderer/systems/vkr_texture_system.h)
