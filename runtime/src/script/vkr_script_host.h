/**
 * @file vkr_script_host.h
 * @brief Runs C script modules on one simulated scene (ADR-079).
 *
 * The host owns the VkrScriptApi table, the registered modules and one
 * session: a started scene with per-module state. It is the scene's only
 * simulation callback client and input observer, and calls each active
 * module's hooks in registration order.
 *
 * Modules either link into the executable or load from a shared library.
 * A library module can be reloaded: the host swaps its code between frames,
 * keeps the running session's state when the state shape is unchanged, and
 * keeps superseded libraries loaded until the session stops.
 */
#pragma once

#include "memory/vkr_allocator.h"
#include "platform/vkr_platform.h"
#include "script/vkr_script.h"

#define VKR_SCRIPT_MODULE_MAX 16u
#define VKR_SCRIPT_MODULE_TYPE_MAX 8u
#define VKR_SCRIPT_MODULE_NAME_CAPACITY 64u
#define VKR_SCRIPT_RETIRED_LIBRARY_MAX 32u
#define VKR_SCRIPT_PATH_CAPACITY 1024u

typedef struct VkrScriptModule {
  const VkrScriptModuleDesc *desc;
  char name[VKR_SCRIPT_MODULE_NAME_CAPACITY];
  /* Host-owned copies of the module's component types; scenes and editors
     hold these pointers, so they outlive every library generation. */
  VkrTypeDesc *types[VKR_SCRIPT_MODULE_TYPE_MAX];
  uint32_t type_count;
  void *state;
  uint32_t state_capacity;
  /* Library modules: the loaded copy of the build. */
  VkrPlatformLibrary library;
  char loaded_path[VKR_SCRIPT_PATH_CAPACITY];
  bool8_t dynamic;
  /* A retired module keeps its registered types and receives no calls. */
  bool8_t retired;
  /* Counts successful loads and reloads. */
  uint32_t generation;
} VkrScriptModule;

typedef struct VkrScriptRetiredLibrary {
  VkrPlatformLibrary library;
  char path[VKR_SCRIPT_PATH_CAPACITY];
} VkrScriptRetiredLibrary;

typedef enum VkrScriptReload {
  VKR_SCRIPT_RELOAD_FAILED = 0,
  /* A module new to the host registered. */
  VKR_SCRIPT_RELOAD_LOADED,
  /* The new code runs with the kept state, or no session was running. */
  VKR_SCRIPT_RELOAD_KEPT_STATE,
  /* The state shape changed: the session restarted with the new code. */
  VKR_SCRIPT_RELOAD_RESTARTED,
} VkrScriptReload;

typedef struct VkrScriptHost {
  VkrScriptApi api;
  VkrAllocator *allocator;
  VkrScriptModule modules[VKR_SCRIPT_MODULE_MAX];
  uint32_t module_count;
  VkrScriptSession session;
  bool8_t active[VKR_SCRIPT_MODULE_MAX];
  bool8_t started;
  /* Libraries replaced while a session ran; closed when it stops. */
  VkrScriptRetiredLibrary retired[VKR_SCRIPT_RETIRED_LIBRARY_MAX];
  uint32_t retired_count;
  uint64_t last_instance_id;
  uint32_t load_serial;
  char error[256];
} VkrScriptHost;

/** Fills the API table. The allocator backs the module state, type copies
 * and names. Registered scene types have no removal, so it must live for the
 * process: every later scene reads those copies. Keep the host at a stable
 * address: modules keep its table, and the scene and input borrow it as
 * callback context while started. */
void vkr_script_host_init(VkrScriptHost *host, VkrAllocator *allocator);

/** Stops the session, closes every library and removes their loaded copies.
 */
void vkr_script_host_shutdown(VkrScriptHost *host);

/** Validates a linked module's description and registers copies of its
 * component types as scene world types. Call before any scene initializes. */
bool8_t vkr_script_host_add_module(VkrScriptHost *host,
                                   VkrScriptModuleEntry entry,
                                   const char **error);

/**
 * Loads module `name` from the shared library `library_path` through its
 * `vkr_script_module_<name>` entry, copying the file first so the build can
 * be replaced. A new module registers its component types, which must come
 * before any scene that uses them initializes. A module already known by
 * that name reloads: its component types must keep their layout, and the
 * running state stays when its shape is unchanged. Call between frames,
 * never from a hook. A failure keeps the previous code running.
 */
VkrScriptReload vkr_script_host_load_library(VkrScriptHost *host,
                                             const char *name,
                                             const char *library_path,
                                             const char **error);

/** Retires every library module, as when a project closes: the session
 * stops, their component types stay registered without hooks, and a later
 * load of the same name adopts them. */
void vkr_script_host_retire_libraries(VkrScriptHost *host);

/** Module by name, or NULL. */
const VkrScriptModule *vkr_script_host_module(const VkrScriptHost *host,
                                              const char *name);

/** Pauses the scene, resets a simulation that already advanced, starts every
 * module and installs the host as the scene's simulation callbacks and the
 * input's observer. Returns false with `*error` when a module fails; nothing
 * stays started then. A host whose modules are all idle still counts as
 * started. */
bool8_t vkr_script_host_start(VkrScriptHost *host, VkrScene *scene,
                              InputState *input, struct VkrRenderAssets *assets,
                              uint32_t flags, const char **error);

/** Pauses the scene, detaches the host, stops the modules in reverse order
 * and closes superseded libraries. The caller resets the scene's simulation
 * afterwards to restore bodies. */
void vkr_script_host_stop(VkrScriptHost *host);

/** Started with at least one active module. */
bool8_t vkr_script_host_active(const VkrScriptHost *host);

/** Before the scene advances; `frame->scene_delta` may change. */
void vkr_script_host_frame(VkrScriptHost *host, VkrScriptFrame *frame);

/** After the scene advanced; later modules overwrite earlier camera poses
 * and append their HUD lines. */
void vkr_script_host_present(VkrScriptHost *host, const VkrScriptFrame *frame,
                             VkrScriptView *view);
