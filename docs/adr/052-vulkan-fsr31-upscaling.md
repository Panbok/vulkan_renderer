---
status: declined
updated: 2026-10-12
authority: adr
---

# ADR-052: Vulkan FSR 3.1 temporal upscaling

## Status

Declined (2026-10-12). FSR 3.1 temporal upscaling shipped as an opt-in mode
on the desktop pipeline, which Vulkan implements, and was removed on
2026-10-12 by the owner's decision in
[Short-history rendering](../proposals/short-history-rendering.md), section 1.
The tiled pipeline never had a temporal upscaler
([ADR-087](087-gpu-class-graphics-pipelines.md)). This record keeps the
reasons, so the design is not tried again without addressing them.

## Context

Vulkan had only same-resolution portable TAA
([ADR-037](037-portable-same-resolution-temporal-antialiasing.md)) and
rejected non-unit spatial scale. FSR 3.1 was added to render the Scene at a
scale in `[1/3, 1]` and reconstruct it at output resolution.

## Decision (removed)

The `fsr31` upscale mode ran FidelityFX SDK `v1.1.4` (FSR 3.1.4) behind a C
bridge, off by default (`temporal_upscaling`). A preparation pass wrote FSR
depth, reactive and composition masks from raw HDR, temporal validity and
transmission depth. The SDK upscaled scene-linear HDR before exposure,
bloom and tonemap. A VKR static convergence pass averaged 128 stationary
output samples per pixel to remove the SDK's residual edge variation, and
with SSR enabled a settling counter kept normal masking for 128 frames
first. The SDK kept its own resources, descriptors and pipelines; a
FidelityFX patch changed its luma history format.

## Why it was removed

FSR is a long-history temporal reconstruction. Its static images needed a
VKR convergence pass and SSR settling rules on top of the SDK, and in a
matched Bistro camera rotation at native and two-thirds scale it did not
remove the variation of thin detail that moving TAA showed. It added a
vendored SDK, a patch and a private resource model to the graph while it
was off by default. The owner decided that the renderer must not hide
stochastic shading behind long temporal history, and removed FSR with SSR
and SSGI.

## Consequences

Vulkan renders the Scene at native resolution: render scale 1 is the
default on both backends, and Vulkan rejects any other scale. Only the
tiled pipeline scales spatially, with its dynamic-resolution controller
([ADR-039](039-metal-internal-render-scale.md),
[ADR-087](087-gpu-class-graphics-pipelines.md)). If a desktop budget needs
it, a spatial render scale like the tiled pipeline's comes first.

`VkrUpscaleMode`, the FSR sources, shaders, CMake module and SDK patch, the
graph passes and the FSR cases were removed with the feature. The graphics
setting `temporal_upscaling` is a retired key that still loads and has no
effect. Former FSR cases that test other features
(`sharpness_bistro_*`, `specular_aa_bistro_static`,
`vulkan_texture_stream_features`) run at unit scale. Git keeps the old
record and its evidence.

## Alternatives considered

A spatial upscaler at a reduced render scale needs no history and matches
the tiled pipeline's approach. A vendor upscaler (FSR, DLSS or XeSS) can
return when a release needs one.

## Revisit when

A PC release expects vendor upscalers, or the desktop pipeline misses its
budget at native resolution.
