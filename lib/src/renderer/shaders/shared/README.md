# Shared shader sources

This directory owns target-neutral shader source compiled and consumed by both
the Metal and Vulkan implementations. `gpu_draw.slangh` is the shared source of
truth for the geometry, candidate-draw, and visible-draw GPU rows.

Backend ABI records, resource bindings, entry points, and helpers written in a
backend-native language stay under `../metal/` or `../vulkan/`. A helper moves
here only when both production targets consume the same source; similar math in
two backend-specific languages is not shared ownership.
