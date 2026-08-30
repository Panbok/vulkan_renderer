# Shared shader sources

This directory owns target-neutral shader source compiled and consumed by both
the Metal and Vulkan implementations. `gpu_draw.slangh` is the shared source of
truth for the geometry, candidate-draw, and visible-draw GPU rows in both Slang
libraries. Metal's native deferred library mirrors those records and the packed
decode in `../metal/msl/common/draw.metalh`; that mirror must stay aligned with
the shared source and is tracked in
`docs/rendering/shader-cross-backend-contract.md`.
`normal_map_kernel.slangh` owns the positive-hemisphere tangent-space decode
used by every Metal and Vulkan material path, including two-channel BC5 and EAC
RG11 sources.

`point_light.slangh` owns the six-vector punctual-light GPU row and the
inclusive world-space influence-bounds test compiled by both Slang libraries.
Metal's native deferred library mirrors that row and test in
`../metal/msl/common/draw.metalh`; the ABI and behavioral mirror are tracked in
`docs/rendering/shader-cross-backend-contract.md`.

`sh_l2_kernel.slangh` owns the L2 diffuse basis, packing, evaluation, and exact
cubemap texel solid angle for ADR-038, mirroring `renderer/vkr_ibl_math.h`. It
is included by the Vulkan Slang library and concatenated into the native Metal
library, so both production backends compile the same math.

Backend ABI records, resource bindings, entry points, and helpers written in a
backend-native language stay under `../metal/` or `../vulkan/`. A helper moves
here only when both production targets consume the same source; similar math in
two backend-specific languages is not shared ownership.
