#pragma once

#include "filesystem/filesystem.h"

/* Seam between the portable filesystem code in vkr_filesystem_common.c and
 * each native backend. */

/** Bytes between the handle's current position and the end of its file. */
FileError file_remaining_size(FileHandle *handle, uint64_t *out_size);

/* Content mounts (filesystem/vkr_vfs.c). A read-only open, stat or fopen of a
 * path a mounted archive holds is served from the archive's mapped bytes; the
 * native backends ask here first and fall through to the disk otherwise. */
bool8_t fs_vfs_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle);
bool8_t fs_vfs_stats(const FilePath *path, FileStats *out_stats);
FILE *fs_vfs_fopen(const char *path, const char *mode, bool8_t *out_served);
FileError fs_memory_read_into(FileHandle *handle, void *buffer, uint64_t size,
                              uint64_t *bytes_read);
FileError fs_memory_remaining(FileHandle *handle, uint64_t *out_size);
FileError fs_memory_read_line(FileHandle *handle, VkrAllocator *allocator,
                              VkrAllocator *line_allocator,
                              uint64_t max_line_length, String8 *out_line);
