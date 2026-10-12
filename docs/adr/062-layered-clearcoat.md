---
status: implemented
updated: 2026-10-12
authority: adr
---

# ADR-062: Layered clearcoat

## Status

Accepted. The desktop pipeline, which Vulkan implements, and the offline baker
are integrated. The tiled pipeline does not draw clearcoat yet
([ADR-087](087-gpu-class-graphics-pipelines.md), decision 8). The output
evidence below comes from the Metal desktop implementation, removed on
2026-10-06; native Vulkan execution is not yet recorded.

## Context

A transparent dielectric coat has its own normal and roughness. Sharing the
base normal loses that distinction.

## Decision

Add optional clearcoat factor, roughness and normal maps to prepared PBR
materials. Factor uses R, roughness uses G, and the independent normal uses RG
with positive-Z reconstruction. Scalars are `clearcoat_factor` (default 0),
`clearcoat_roughness` (default 0), and `clearcoat_normal_scale` (default 1).
Factor and roughness must be finite in [0,1]; normal scale must be finite and
may be signed, with zero producing a flat normal. All three maps are linear.
An absent coat normal uses the interpolated surface normal before base normal
mapping. The normal is oriented toward the view for lighting.

glTF cooking accepts external images, untransformed UV0, repeat addressing and
linear/mip-linear sampling. It rejects unsupported views and incompatible unlit
or specular-glossiness materials before publishing generated materials.
The parameter semantics follow [KHR_materials_clearcoat](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_clearcoat).
This is a bounded subset, not general glTF extension support.

The coat uses fixed IOR 1.5 / F0 0.04 and [ADR-053](053-energy-compensated-ggx.md)'s
compensated correlated Smith GGX. Prepare its directional DFG reflectance once.
Base transmission is `1 - factor * coat_reflectance`. Multiply base diffuse,
specular, emission and transmitted radiance by that value, then add the factor
weighted coat specular. Each direct lobe uses its own normal and light cosine.
Opaque indirect coat lighting also uses the coat normal for GTAO cone
occlusion, decoding bent direction against that normal and using the packed
coat roughness. Deferred, forward and transmission lighting traverse each active punctual and
rectangle list once for the base, coat and sheen. Vulkan has one punctual
helper and one rectangle helper for all three passes; transmission skips the
base diffuse lobe on fully transmissive surfaces outside diagnostic views.
Punctual lights share row loads, geometry and attenuation; a coat whose
receiver-bias normal differs from the base uses its own local-shadow query.
Sheen uses the front base normal; a backlit thin sheet retains its light-facing
diffuse bias and zero front sheen response. Rectangle layers retain their own
LTC frames and integrals, including the sheen frame when an anisotropic base
rotates its axes. Directional lights retain the existing common base-normal
shadow policy. These source changes do not establish measured savings.

Runtime lights, LTC and probe/global IBL share this allocation. The offline
path/photon BSDF uses the same layer and a corresponding mixture PDF. It tracks
the geometric normal separately from both mapped normals so reflection and
refraction agree with real surface crossings. Shading-normal horizon clipping
and split-sum IBL remain approximations for strongly tilted normals.
This retains ADR-053's directional approximation and is not an exact reciprocal
multiple-scattering layered BSDF. Zero factor takes the existing base path.

The coat lobe reads local probes and the global environment like the base
lobe, with the coat normal and roughness. Coat-priority SSR, which traced the
coat lobe on coated pixels, was removed with SSR
([ADR-055](055-screen-space-reflections.md)).

## Ownership and lifetime

Material texture slots grow from eight to eleven. Immutable native material
rows own their enabled texture references until recorded/submitted GPU uses
complete. Native row layouts are independently pinned: Metal 240 bytes,
Vulkan 192 bytes. CPU material loading resolves scalar and texture intent before
publication. The bake texture store retains decoded images for the bake lifetime.

The graph owns a full-resolution `R8G8B8A8_UNORM` PER_IMAGE image only when
the opaque/cutout source-material aggregate contains clearcoat: factor, roughness
and octahedral normal XY. Resolve writes it; deferred lighting reads it. The
plane adds no
resolve pass; deferred lighting adds its layered pass, described below.
At 1280×720 this adds 3.515625 MiB per image, 10.546875 MiB for three images or
28.125 MiB for eight, before native alignment. Vulkan uses one sampled and one
storage descriptor per image within its existing graph descriptor pools.
Graph generation retirement and frame completion govern image reuse. Extraction
aggregates clearcoat, sheen and anisotropy from material scalar factors before
recording; a texture cannot enable a zero scalar factor. External packet producers
without a valid aggregate conservatively retain all planes. Disabled planes have
no graph allocation or uses. Native null/slot-zero bindings return an explicit
zero material value and skip writes; no full-resolution load targets a fallback
image.

Deferred lighting has two kernels. The base kernel compiles the clearcoat,
sheen and anisotropy paths out, so it needs fewer registers; the layered kernel
keeps them. A frame whose aggregate has no material plane runs only the base
kernel over the viewport. When any plane exists, the graph also runs
`Lighting.Deferred.Layered.Fullscreen` or `.Editor`. Both kernels cover the
same 8x8 groups, OR in group memory whether any geometry pixel holds a nonzero
plane value, and shade only their own class of tiles, so every tile is shaded
exactly once. A pixel whose planes are all zero shades identically in either
kernel. On the Metal desktop implementation, removed on 2026-10-06 (M1
development host, Release, 1280x720, Bistro street view, no planes),
`Lighting.Deferred` fell from 9.71 ms to 7.09 ms. In a Bistro fixture with a
layered cube, split output matched a build that sent every tile to the layered
kernel, apart from one-ULP rounding of the recompiled library and pixels that
also vary between identical runs.

## Consequences

Coat highlights can move independently of the base, and one additional coat
lookup/allocation is needed on coated surfaces.
Coat normal/roughness maps are independent and do not use the base map's paired
normal-variance roughness recipe. The baker retains its existing bounded
caustic estimator and first-diffuse-receiver policy.

## Alternatives considered

Reusing the base normal would lose authored coat detail. Per-light Schlick base attenuation would
conflict with the existing integrated-energy allocation.

## Revisit when

Assets require transformed or alternate UV sets, custom samplers, coupled coat
mip filtering, or a reciprocal layered transport model.

## Implementation

[Material contract](../../renderer/src/vkr_render_resources.h),
[shared layer math](../../renderer/src/shaders/shared/clearcoat_kernel.slangh),
[glTF preparation](../../tools/assets/mesh_loader_gltf.c), and
[offline BSDF](../../tools/bake/vkr_bake_bsdf.cpp).

## Verification

The production Release wrapper and actual Vulkan SPIR-V reflection validated all
four resolve variants and deferred lighting, together with the SSR and SSGI
modules since removed.
Material and root offsets match independently pinned native layouts.

On the Metal desktop implementation, a 480×480 unit-environment capture
checked 36 coated dielectric/metal samples across opaque, blend and
transmission paths and six roughnesses. Its maximum absolute RGB error was
0.00048828125. The zero-coat final image was byte-identical to the retained
pre-change image; three HDR pixels differed by one FP16 step (0.00048828125).
Independent CPU integration and sampling check aligned furnace
energy, tilted/opposed normals and thin/thick glass event classification.

On the same implementation, the 512×384 material fixture checked factor and
roughness response, independent coat normals and green light through coated
glass. While SSR was still in the frame, a floor with base roughness 1 and coat
roughness .04 produced 682 SSR hits, including 33 red-emitter hits inside the
floor region; its base was ineligible for SSR.

A coated nested-glass bake completes 20,000 emitted photons with 1,605 caustic
deposits, 100 valid probes and 32 valid cells. All 4,032 stored SH components are
finite. Import checks reject UV1, transforms, custom samplers, embedded images,
unlit combinations and invalid factors before generated-material publication.
A direct material-loader check verifies R/G channel use, independent RG normal,
default/zero normal scale and invalid scalar/texture-intent rejection.

[The material case](../../tools/cases/local/clearcoat_local.case.json) and [the
resize case](../../tools/cases/local/clearcoat_resize_local.case.json), now
pinned to Vulkan, with [its checker and cooking
command](../../tools/checks/check_clearcoat_fixture.py) and [the furnace
checker](../../tools/checks/check_clearcoat_furnace.py), are the native Vulkan
gates for this domain; they have not run on Vulkan. These are local correctness
observations, not matched performance measurements.