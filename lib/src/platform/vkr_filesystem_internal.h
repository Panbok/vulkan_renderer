#pragma once

#include "filesystem/filesystem.h"

/* Seam between the portable filesystem code in vkr_filesystem_common.c and
 * each native backend. */

/* Content mounts (filesystem/vkr_vfs.c). A read-only open, stat or fopen of a
 * path a mounted archive holds is served from the archive's mapped bytes; the
 * native backends ask here first and fall through to the disk otherwise. */
bool8_t fs_vfs_open(const FilePath *path, FileMode mode,
                    FileHandle *out_handle);
bool8_t fs_vfs_stats(const FilePath *path, FileStats *out_stats);
FILE *fs_vfs_fopen(const char *path, const char *mode, bool8_t *out_served);
/* The workspace overlay (vkr_vfs_set_overlay_root). When a read of `path`
 * should use the overlay's copy and that file exists, writes its path into
 * `buffer`, points `out_path` at it and returns true; the native backends
 * then read `out_path` instead. Callers decide that the access only reads. */
bool8_t fs_vfs_overlay(const FilePath *path, FilePath *out_path, char *buffer,
                       uint64_t capacity);
#define FS_VFS_OVERLAY_PATH_MAX 4096u
/* The remote content directory (vkr_vfs_set_remote): gives a path below it
 * the chance to arrive before a native read-only access. Callers decide that
 * the access only reads. */
void fs_vfs_remote(const FilePath *path);
FileError fs_memory_read_into(FileHandle *handle, void *buffer, uint64_t size,
                              uint64_t *bytes_read);
FileError fs_memory_remaining(FileHandle *handle, uint64_t *out_size);
FileError fs_memory_read_line(FileHandle *handle, VkrAllocator *allocator,
                              VkrAllocator *line_allocator,
                              uint64_t max_line_length, String8 *out_line);
