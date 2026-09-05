---
status: proposed
updated: 2026-09-05
authority: proposal
---

# Visibility-buffer MSAA

Portable same-resolution temporal AA is implemented. Visibility-buffer MSAA is
not: although [vkr_renderer.h](../../lib/src/renderer/vkr_renderer.h) defines
sample counts, Vulkan rejects multisampled published textures in
[vkr_vulkan_publisher.c](../../lib/src/renderer/vulkan/vkr_vulkan_publisher.c),
and graph images currently require one sample in
[vkr_vulkan_graph.c](../../lib/src/renderer/vulkan/vkr_vulkan_graph.c).

## Current implementation baseline

The existing temporal state is prepared and committed through
[vkr_temporal.c](../../lib/src/renderer/vkr_temporal.c), while Vulkan records a
temporal-resolve compute pass in
[vkr_vulkan_deferred.c](../../lib/src/renderer/vulkan/vkr_vulkan_deferred.c).
The production deferred topology already includes GPU visibility, G-buffer
resolve, lighting, transmission, HZB, and temporal compute passes; it must not
gain an MSAA path that bypasses those consumers.

## Proposed gap

Measure whether a representative visibility-buffer edge case has a defect that
temporal AA and the final FXAA pass cannot address. Only then design a bounded
two- or four-sample path. The design must represent per-sample visibility,
depth, coverage, and resolve semantics, then account for its attachment memory,
bandwidth, graph barriers, and both backend lowering paths.

## Unsettled decisions

- The measured artifact and target hardware that justify multisampling.
- Whether the resolve shades every covered sample, selects a representative
  visibility sample, or uses a separate edge path.
- Whether alpha-to-coverage is required and how it interacts with cutouts and
  ordered transmission.
- Which sample counts and formats both selected backends can support within a
  defined resource budget.
- Whether temporal history consumes resolved output only or needs sample-aware
  rejection metadata.
