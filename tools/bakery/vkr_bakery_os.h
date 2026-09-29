#pragma once

#include "defines.h"

/* Host file-system and process helpers for vkr_bakery. Every path is a
 * UTF-8 host path (ADR-070); Windows converts to the extended native form
 * immediately before each call. Paths written into documents and events are
 * converted to portable '/' form by the caller. */

#define VKR_BAKERY_PATH_CAPACITY 4096u
#define VKR_BAKERY_SHA256_HEX 65u

typedef struct VkrBakeryStat {
  bool8_t exists;
  bool8_t is_directory;
  uint64_t size;
  int64_t mtime_ns;
  uint64_t device;
  uint64_t file_id;
} VkrBakeryStat;

bool8_t vkr_bakery_stat(const char *path, VkrBakeryStat *out_stat);
bool8_t vkr_bakery_is_file(const char *path);
bool8_t vkr_bakery_is_directory(const char *path);

/** Calls `visit` for each entry except "." and "..", in byte order of names.
 * Returning false from `visit` stops the listing and returns false. */
typedef bool8_t (*VkrBakeryDirectoryVisitor)(void *context, const char *name,
                                             bool8_t is_directory);
bool8_t vkr_bakery_list_directory(const char *path,
                                  VkrBakeryDirectoryVisitor visit,
                                  void *context);

bool8_t vkr_bakery_make_directories(const char *path);
/** Reads a whole file into malloc storage the caller frees; NUL-appended. */
bool8_t vkr_bakery_read_file(const char *path, uint64_t max_bytes,
                             uint8_t **out_data, uint64_t *out_length);
/** Writes a sibling temporary, flushes it, then replaces `path`. */
bool8_t vkr_bakery_write_file_atomic(const char *path, const void *data,
                                     uint64_t length);
bool8_t vkr_bakery_append_file(const char *path, const void *data,
                               uint64_t length);
bool8_t vkr_bakery_remove_file(const char *path);
/** Removes a file or a directory tree. A missing path succeeds. */
bool8_t vkr_bakery_remove_tree(const char *path);
bool8_t vkr_bakery_rename(const char *source, const char *destination,
                          bool8_t overwrite);
/** Replaces `destination` with a copy-on-write clone of `source` where the
 * volume supports it, otherwise a byte copy; the replacement is atomic. */
bool8_t vkr_bakery_clone_or_copy(const char *source, const char *destination);
/** As vkr_bakery_clone_or_copy, but where the volume cannot clone, hard links
 * `source` before falling back to a byte copy. Only for sources that are never
 * rewritten in place and are replaced only by rename (see file_link), since
 * both names then share one file. */
bool8_t vkr_bakery_clone_link_or_copy(const char *source,
                                      const char *destination);

bool8_t vkr_bakery_hash_file(const char *path,
                             char out_hex[VKR_BAKERY_SHA256_HEX],
                             uint64_t *out_size);
void vkr_bakery_hash_bytes(const void *data, uint64_t length,
                           char out_hex[VKR_BAKERY_SHA256_HEX]);

/** Joins with '/', dropping a duplicate separator. Returns false on overflow.
 */
bool8_t vkr_bakery_path_join(char *out, uint32_t capacity, const char *lhs,
                             const char *rhs);
/** Directory part of `path` without its trailing separator. */
void vkr_bakery_path_parent(char *out, uint32_t capacity, const char *path);
const char *vkr_bakery_path_name(const char *path);
/** Extension including the dot, lower-cased into `out`; empty when none. */
void vkr_bakery_path_extension(const char *path, char *out, uint32_t capacity);
/** Rewrites Windows '\\' separators as '/' in place; POSIX paths keep
 * backslashes, which are filename bytes there. */
void vkr_bakery_path_portable(char *path);
bool8_t vkr_bakery_path_is_absolute(const char *path);
/** Makes `path` absolute against the current directory, lexically. */
bool8_t vkr_bakery_path_absolute(const char *path, char *out,
                                 uint32_t capacity);
/** Relative form of `path` under `root`, or false when outside it. */
bool8_t vkr_bakery_path_relative(const char *root, const char *path, char *out,
                                 uint32_t capacity);

/** Unique sibling name for staging writes into `directory`. */
void vkr_bakery_temp_path(char *out, uint32_t capacity, const char *directory,
                          const char *label);

bool8_t vkr_bakery_executable_path(char *out, uint32_t capacity);
bool8_t vkr_bakery_user_cache_directory(char *out, uint32_t capacity);
uint64_t vkr_bakery_physical_memory_bytes(void);
uint32_t vkr_bakery_logical_cores(void);

/** CPU time and peak resident memory of this process so far. */
bool8_t vkr_bakery_self_usage(uint64_t *out_cpu_ms,
                              uint64_t *out_peak_rss_bytes);
/** Same for reaped descendants of this process, cumulatively. */
bool8_t vkr_bakery_children_usage(uint64_t *out_cpu_ms,
                                  uint64_t *out_peak_rss_bytes);

/** UTC wall time as "YYYY-MM-DDTHH:MM:SSZ". */
void vkr_bakery_utc_timestamp(char out[32]);
int64_t vkr_bakery_unix_seconds(void);
float64_t vkr_bakery_monotonic_seconds(void);
