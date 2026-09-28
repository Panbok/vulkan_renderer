#pragma once

#include "defines.h"
#include "memory/vkr_allocator.h"

/* The editor's `vkr_bakery serve` daemon (ADR-077): one child per editor
 * process, started on the first watch, restarted after a crash with a
 * Console warning, and shut down with the editor. Watches survive restarts;
 * each reports changed files and, with argv, the result of the background
 * rebuild the daemon ran for them. UI-thread owner. */

#define EDITOR_BAKERY_SERVICE_PATH 1024u
#define EDITOR_BAKERY_SERVICE_NONE 0u

typedef struct EditorBakeryService EditorBakeryService;

/** A finished background rebuild of one watch. */
typedef struct EditorBakeryRebuild {
  int32_t exit_code;
  uint32_t actions;
  uint32_t cached;
  uint32_t failed;
} EditorBakeryRebuild;

EditorBakeryService *editor_bakery_service_create(VkrAllocator *allocator);
/** Asks the daemon to shut down, then joins its supervising thread. */
void editor_bakery_service_destroy(EditorBakeryService *service);
/** Poll every frame: supervises the child and reads its events without
 * blocking. */
void editor_bakery_service_update(EditorBakeryService *service);
/** Registers a watch of up to 16 absolute paths. `argv` (a vkr_bakery
 * subcommand and options) may be NULL to report changes only. Returns a
 * handle, or EDITOR_BAKERY_SERVICE_NONE when the daemon is unavailable on
 * this platform. */
uint32_t editor_bakery_service_watch(EditorBakeryService *service,
                                     const char *const *paths,
                                     uint32_t path_count,
                                     const char *const *argv,
                                     uint32_t argument_count);
void editor_bakery_service_unwatch(EditorBakeryService *service,
                                   uint32_t handle);
/** Moves up to `capacity` changed paths reported since the last call into
 * `out`; returns their count. */
uint32_t editor_bakery_service_take_changes(
    EditorBakeryService *service, uint32_t handle,
    char (*out)[EDITOR_BAKERY_SERVICE_PATH], uint32_t capacity);
/** Returns true once per finished background rebuild. */
bool8_t editor_bakery_service_take_rebuild(EditorBakeryService *service,
                                           uint32_t handle,
                                           EditorBakeryRebuild *out);
