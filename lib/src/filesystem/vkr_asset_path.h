#pragma once
#include "filesystem/filesystem.h"

/** Return owned, null-terminated paths in allocator storage. Bare relative
 * references retain the legacy repository-root interpretation. Explicit ./ or
 * ../ references resolve against the owning file; no cwd/global root changes.
 * Resolution is lexical and preserves query suffixes. The import boundary owns
 * containment and file-format validation; opening rechecks current existence.
 */
String8 vkr_asset_path_resolve(VkrAllocator *allocator, String8 owner,
                               String8 reference);
/** Convert a length-delimited resource path into an absolute FilePath without
 * assuming a trailing NUL. Empty result reports invalid input/allocation. */
FilePath vkr_asset_path_file(VkrAllocator *allocator, String8 path);
