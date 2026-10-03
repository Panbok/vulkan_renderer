#pragma once
#include <cgltf.h>

/* glTF file bytes use the supplied cgltf allocator and are released by cgltf.
 * Paths remain UTF-8 through the shared native filesystem boundary. */
cgltf_file_options vkr_cgltf_file_options(void);

/* FNV-1a over the JSON text and the loaded buffers: the source identity that a
 * cooked mesh and its animation asset share. Line endings do not change it. */
uint64_t vkr_cgltf_source_fingerprint(const cgltf_data *data);
