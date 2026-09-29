/**
 * @file vkr_script_host.h
 * @brief Runs C script modules on one simulated scene (ADR-079).
 *
 * The host owns the VkrScriptApi table, the registered modules and one
 * session: a started scene with per-module state. It is the scene's only
 * simulation callback client and calls each active module's hooks in
 * registration order.
 */
#pragma once

#include "memory/vkr_allocator.h"
#include "script/vkr_script.h"

#define VKR_SCRIPT_MODULE_MAX 8u

typedef struct VkrScriptHost {
  VkrScriptApi api;
  VkrAllocator *allocator;
  const VkrScriptModuleDesc *modules[VKR_SCRIPT_MODULE_MAX];
  uint32_t module_count;
  VkrScriptSession session;
  void *states[VKR_SCRIPT_MODULE_MAX];
  bool8_t active[VKR_SCRIPT_MODULE_MAX];
  bool8_t started;
  uint64_t last_instance_id;
  char error[192];
} VkrScriptHost;

/** Fills the API table. The allocator backs one state block per module for
 * the process, reused by every session; it must outlive the host. Keep the
 * host at a stable address: modules keep its table, and the scene borrows it
 * as callback context while started. */
void vkr_script_host_init(VkrScriptHost *host, VkrAllocator *allocator);

/** Validates a module's description and registers its component types as
 * scene world types. Call before any scene initializes. */
bool8_t vkr_script_host_add_module(VkrScriptHost *host,
                                   VkrScriptModuleEntry entry,
                                   const char **error);

/** Pauses the scene, resets a simulation that already advanced, starts every
 * module and installs the host as the scene's simulation callbacks. Returns
 * false with `*error` when a module fails; nothing stays started then. A
 * host whose modules are all idle still counts as started. */
bool8_t vkr_script_host_start(VkrScriptHost *host, VkrScene *scene,
                              InputState *input, struct VkrRenderAssets *assets,
                              uint32_t flags, const char **error);

/** Pauses the scene, detaches the host and stops every started module. The
 * caller resets the scene's simulation afterwards to restore bodies. */
void vkr_script_host_stop(VkrScriptHost *host);

/** Started with at least one active module. */
bool8_t vkr_script_host_active(const VkrScriptHost *host);

/** Before the scene advances; `frame->scene_delta` may change. */
void vkr_script_host_frame(VkrScriptHost *host, VkrScriptFrame *frame);

/** After the scene advanced; later modules overwrite earlier camera poses
 * and append their HUD lines. */
void vkr_script_host_present(VkrScriptHost *host, const VkrScriptFrame *frame,
                             VkrScriptView *view);
