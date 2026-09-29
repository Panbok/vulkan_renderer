#pragma once

#include "defines.h"

/* Compiled shaders published by `vkr_bakery shaders` (ADR-044 owns shader
 * semantics; docs/proposals/asset-build-system.md section 9.7 the catalog).
 * A catalog root holds `vulkan/<name>.spv`, `metal/<library>.metal`, optional
 * `metal/<library>.metallib` and one `shader_manifest.json` per backend.
 *
 * The root is, in order: the process override, $VKR_SHADER_CATALOG, a
 * `shaders` directory beside the executable or in `../Resources` (bundled
 * games, macOS application bundles), then the build tree catalog compiled
 * into repository builds. */

/** Copies `path` as the process-wide catalog root; NULL clears it. Set once
 * before creating a renderer. */
void vkr_shader_catalog_set_root(const char *path);
bool8_t vkr_shader_catalog_root(char *out, uint32_t capacity);
/** `<root>/<backend>/<file>`. */
bool8_t vkr_shader_catalog_path(const char *backend, const char *file,
                                char *out, uint32_t capacity);
/** Finds the precompiled metallib of `library` ("library" or
 * "library.slang") and returns true only when the Metal manifest records a
 * metallib built from the source file currently in the catalog. `out_source`
 * always receives the MSL path for the source-compile fallback. */
bool8_t vkr_shader_catalog_metallib(const char *library, char *out_metallib,
                                    char *out_source, uint32_t capacity);
