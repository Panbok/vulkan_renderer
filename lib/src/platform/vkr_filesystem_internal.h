#pragma once

#include "filesystem/filesystem.h"

/* Seam between the portable filesystem code in vkr_filesystem_common.c and
 * each native backend. */

/** Bytes between the handle's current position and the end of its file. */
FileError file_remaining_size(FileHandle *handle, uint64_t *out_size);
