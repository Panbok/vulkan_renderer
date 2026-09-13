#pragma once
#include <cgltf.h>

/* glTF file bytes use the supplied cgltf allocator and are released by cgltf.
 * Paths remain UTF-8 through the shared native filesystem boundary. */
cgltf_file_options vkr_cgltf_file_options(void);
