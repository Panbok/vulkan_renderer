# Shared shader sources

This directory owns target-neutral shader source compiled and consumed by both
the Metal and Vulkan implementations. `gpu_draw.slangh` is the shared source of
truth for the geometry, candidate-draw, and visible-draw GPU rows in both Slang
libraries. Metal's native deferred library mirrors those records and the packed
decode in `../metal/msl/common/draw.metalh`; that mirror must stay aligned with
the shared source and is tracked in
`docs/adr/044-shader-cross-backend-contract.md`.
`normal_map_kernel.slangh` owns the positive-hemisphere tangent-space decode
used by every Metal and Vulkan material path, including two-channel BC5 and EAC
RG11 sources.

`point_light.slangh` owns the four-vector punctual-light GPU row compiled by
both Slang libraries. Metal's native deferred library mirrors that row in
`../metal/msl/common/draw.metalh`; host and shader reflection validate both
representations.

`sh_l2_kernel.slangh` owns the L2 diffuse basis, packing, evaluation, and exact
cubemap texel solid angle for ADR-038. `renderer/vkr_ibl_math.h` defines the host
coefficient layout and slot constants. The kernel is included by the Vulkan
Slang library and concatenated into the native Metal library, so both production
backends compile the same math.

Backend ABI records, resource bindings, entry points, and helpers written in a
backend-native language stay under `../metal/` or `../vulkan/`. A helper moves
here only when both production targets consume the same source; similar math in
two backend-specific languages is not shared ownership.
