/* A script module built four ways for the host's reload tests (ADR-079):
 * 1 counts one per tick; 2 changes only code (ten per tick); 3 changes the
 * state version; 4 changes its component's fields. Built as a MODULE library,
 * so any engine symbol it used would fail to link. */
#include "script/vkr_script.h"

#include <stddef.h>

#if !defined(RELOAD_VARIANT)
#error "RELOAD_VARIANT selects the build"
#endif

typedef struct ReloadProbe {
  float32_t value;
#if RELOAD_VARIANT == 4
  float32_t extra;
#endif
} ReloadProbe;

static const VkrPropertyDesc s_properties[] = {
    {.name = "value",
     .label = "Value",
     .offset = (uint32_t)offsetof(ReloadProbe, value),
     .kind = VKR_PROPERTY_F32},
#if RELOAD_VARIANT == 4
    {.name = "extra",
     .label = "Extra",
     .offset = (uint32_t)offsetof(ReloadProbe, extra),
     .kind = VKR_PROPERTY_F32},
#endif
};

static const VkrTypeDesc s_type = {
    .name = "reload_probe",
    .label = "Reload probe",
    .category = "Scripts",
    .properties = s_properties,
    .property_count = sizeof(s_properties) / sizeof(s_properties[0]),
    .size = sizeof(ReloadProbe),
    .align = _Alignof(ReloadProbe),
};

static const VkrTypeDesc *const s_types[] = {&s_type};

/* The test reads `ticks` through the host's state pointer. */
typedef struct ReloadState {
  uint32_t ticks;
  uint32_t starts;
  uint32_t reloads;
} ReloadState;

static VkrScriptStart probe_start(const VkrScriptSession *session, void *state,
                                  const char **error) {
  (void)session;
  (void)error;
  ((ReloadState *)state)->starts++;
  return VKR_SCRIPT_START_ACTIVE;
}

static void probe_stop(const VkrScriptSession *session, void *state) {
  (void)session;
  (void)state;
}

static bool8_t probe_before_physics(const VkrScriptSession *session,
                                    void *state, uint64_t tick,
                                    const char **error) {
  (void)session;
  (void)tick;
  (void)error;
  ((ReloadState *)state)->ticks += RELOAD_VARIANT == 2 ? 10u : 1u;
  return true_v;
}

static void probe_reload(const VkrScriptSession *session, void *state) {
  (void)session;
  ((ReloadState *)state)->reloads++;
}

static const VkrScriptModuleDesc s_module = {
    .abi_version = VKR_SCRIPT_ABI_VERSION,
    .size = sizeof(VkrScriptModuleDesc),
    .name = "reload_probe",
    .types = s_types,
    .type_count = 1,
    .state_size = sizeof(ReloadState),
    .state_align = _Alignof(ReloadState),
    .state_version = RELOAD_VARIANT == 3 ? 2u : 1u,
    .start = probe_start,
    .stop = probe_stop,
    .before_physics = probe_before_physics,
    .reload = probe_reload,
};

VKR_SCRIPT_EXPORT const VkrScriptModuleDesc *
vkr_script_module_reload_probe(const VkrScriptApi *api) {
  return api && api->version == VKR_SCRIPT_ABI_VERSION ? &s_module : NULL;
}
