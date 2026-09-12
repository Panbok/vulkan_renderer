---
status: implemented
updated: 2026-09-12
authority: adr
---

# ADR-063: Charlie sheen below clearcoat

## Status

Accepted. Material import, graph storage, native lighting and offline transport
are implemented. Metal material, rectangle, furnace, zero-color equivalence,
editor and API-validation resize checks pass. Native Vulkan execution and
same-revision pixel comparison are unavailable on the current Metal host.

## Context

Fabric needs a grazing reflection lobe independent of the existing GGX base.
The accepted first implementation must keep the current reflection-ray budget
and use the existing environment assets.

## Decision

Add `sheen_color` and `sheen_roughness`, both defaulting to zero. Color components
and roughness must be finite and within [0,1]. Optional color maps multiply
linear color by decoded sRGB RGB; roughness maps multiply roughness by linear
alpha. Effective roughness is at least 0.04. Sheen uses the base mapped normal.
The glTF importer accepts the same external-image, untransformed UV0 and
repeat/linear/mip-linear subset as [clearcoat](062-layered-clearcoat.md), rejecting
unsupported views or unlit/specular-glossiness combinations before publication.
The authored parameters follow
[KHR_materials_sheen](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_sheen).

The layer uses Charlie distribution with perceptual roughness squared and the
Estevez/Kulla fitted correlated visibility. The fit has a finite Lambda at the
horizon, so unrestricted division by the geometric cosine would diverge.
Evaluate visibility with both positive cosines floored to 0.0001; use the same
view domain for table lookup and generation. Keep actual geometric directions
for the half vector and light cosine. Apply one shared roughness-dependent
normalization to direct lighting, integrated energy, rectangle amplitudes and
the offline BSDF. Independent integration must verify the normalized lobe's
energy between table samples, including below the cosine floor.

For directional sheen energy E and maximum color component C, allocate
`max(0, 1 - C * min(1, E + min(E * relative_reserve, absolute_reserve)))` to the base.
Relative and absolute reserves cover table integration
and filtering error without adding sheen radiance. The reserve vanishes with
E and has an absolute cap; IBL uses the raw E. Scale base diffuse, GGX
specular, emission and transmitted radiance by this residual, then add colored
sheen. Clearcoat is the outer layer and scales that entire substrate. Zero sheen color preserves the
existing paths and skips sheen material maps and lighting tables.

Environment sheen reuses the current GGX-prefiltered cubemaps and multiplies by
the integrated Charlie energy. This is an explicit filtering approximation;
there is no dedicated Charlie environment capture or prefilter. Rectangle lights
use a two-component LTC fit to the same normalized Charlie lobe, with independent area quadrature used to characterize error. The initial
single-component prototype did not meet the rectangle quadrature checks, so
the accepted budget adds a second integral and 64 KiB of tables. Each table weight
represents a fraction of upper-hemisphere sheen energy. Normalize the interpolated
fractions, divide by each transformed cosine lobe's exact upper-hemisphere mass,
and multiply by raw directional energy E once per surface before the light loop.
This prevents large full-sphere amplitudes from producing energy gain after
matrix interpolation. Clip each rectangle to the receiver horizon before
integrating the transformed lobes, so lights below the surface contribute zero.
The light loop still evaluates two integrals per rectangle.
Existing one-sided, unshadowed runtime rectangle policy remains in force.

SSR continues tracing base GGX or clearcoat under its existing selection policy.
Sheen has no SSR ray or history. For base SSR, sheen attenuates the replaced base
specular; coat SSR retains the outer-layer policy. SSGI source lighting includes
layered direct diffuse and emission, excluding camera-directed sheen highlights; the receiver attenuates base diffuse and adds no
screen-space sheen lobe. Offline path and photon transport use the matching
layered BSDF, with geometric normals governing real medium crossings.

## Ownership and lifetime

Prepared material slots grow to thirteen. Native immutable rows retain enabled
material textures through GPU completion: Metal rows are 288 bytes and Vulkan
rows are 224 bytes. The bake texture store owns decoded source texels for one
bake. Input loaders validate scalar ranges and texture intent before publication.

The graph owns a full-resolution `R8G8B8A8_UNORM` PER_IMAGE image containing
linear sheen RGB and roughness only when the opaque/cutout material aggregate
contains nonzero sheen color. The coarse feature and absent-binding contract
follows [ADR-062](062-layered-clearcoat.md). Resolve writes it; deferred lighting and SSGI/SSR
composites read it. No pass is added. At 1280×720 the additional payload is
10.546875 MiB for three images or 28.125 MiB for eight, before native alignment.
Generation retirement and frame completion govern reuse.

The native renderer owns one immutable 256² R16F directional-energy texture
(128 KiB) and four 64² RGBA16F rectangle textures (128 KiB total). Table 0 stores
the first inverse transform as `(log2(s), log2(k), h, beta)`; table 1 stores
its energy fraction in R, the relative
and absolute allocation reserves in G/B and the common normalization in A.
Table 2 stores the second encoded transform; table 3 stores its energy fraction
in R. Bilinear filtering precedes decoding. For `a=s*cos(beta)`, `c=s*sin(beta)`,
`b=h*a-k*sin(beta)` and `d=h*c+k*cos(beta)`, the inverse matrix is
`[[a,0,b],[0,1,0],[c,0,d]]`. Its determinant is `s*k>0`, and its physical
upper-hemisphere mass is `(1+cos(beta))/2`. Log scales and shear are bounded
to [-6,6], and beta to [-3.10,3.10] radians. This representation preserves
orientation between entries. The fitter restricts beta to [-1.4,1.4] before
half conversion to avoid nearly unsupported lobes. It preserves an axial
normal-view endpoint, matches component labels by interpolated response, and
copies the active shape into negligible-weight entries before filtering.
The combined immutable payload is 256 KiB.
They reuse the clamp-linear sampler and are released after renderer GPU work
completes. Vulkan uses five permanent sampled descriptors. Native frame roots point to separate table blocks (Metal 48 bytes,
Vulkan 32 bytes); existing LTC fields retain their layout. The public frame-input handle contract is unchanged.
Metal uploads the 528-byte frame root in two contiguous 512-byte cells, with
capacity reserved before frame preparation; ordinary draw cells remain 512 bytes.

## Consequences

Active sheen adds table and BRDF work, plus graph bandwidth for the additional
material image. The directional allocation and reused environment filtering are
approximations; this is not a reciprocal multiple-scattering layered solution.
The cosine floor defines a finite numerical domain for the fitted visibility.
Table generation and transport must change together when that domain changes.

## Verification and approximation limits

Release app/editor builds and reflection of nine production SPIR-V modules
pass. A 65×65 Metal rectangle capture, checked with
`tools/checks/check_sheen_rectangle.py`, measures 0.0821533 against independent
Charlie area quadrature of 0.0788004: 4.25% error. The reference uses the
captured, quantized surface normal. Mixed rectangle/material, glass and SSR
checks pass; the floor retains 682 SSR hits, including 33 red hits.

For 100 checked roughness/view/rectangle combinations, the cooked fit has maximum
absolute error 0.002359 at unit emitted radiance; maximum relative error where
reference response is at least 0.01 is 11.56%. For 192 additional tilted
rectangles, median, 90th-percentile and maximum absolute errors are 0.000191,
0.002291 and 0.010974. The reported errors describe only the checked samples.
Relative error remains large for some dim tilted lights: one reference response
of 0.011186 is fitted as 0.019150. Independent 512×512 area quadrature confirms
that discrepancy. The rectangle approximation needs revisiting if those cases
matter for authored fabric lighting.

Every stored record and 35,721 filtered cell-interior samples pass encoded-domain
and support checks. The normalized mixture has no measured energy excess over
its allocated directional albedo. Directional-energy and allocation-reserve data
remain byte-identical through rectangle fitting, preserving the earlier furnace,
BSDF and glass/photon-bake evidence. Final cooker regeneration reports unchanged
output. Native Vulkan remains an evidence gap in [ADR-044](044-shader-cross-backend-contract.md).

## Alternatives considered

Dedicated Charlie environment prefilters would add approximately 4 MiB per
source plus another bake. Replacing Charlie with a different fitted volumetric
sheen model would change the accepted material model. Additional SSR rays would
exceed the accepted reflection budget.

## Revisit when

Assets require alternate UVs, texture transforms or custom samplers; sheen needs
its own environment filtering or SSR; or the rectangle fit and directional
allocation are insufficient for the intended fabric.

## Implementation

- [Prepared materials](../../renderer/src/vkr_render_resources.h)
- [Runtime loading](../../runtime/src/renderer/resources/loaders/material_loader.c)
- [glTF material cooking](../../tools/assets/mesh_loader_gltf.c)
- [Graph storage](../../assets/render_graphs/main.rendergraph.json)
- [Shared shader math](../../renderer/src/shaders/shared/sheen_kernel.slangh)
- [Table cooker](../../tools/vkr_sheen_cooker.cpp)
- [Offline material sampling](../../tools/bake/vkr_bake_material.cpp)
