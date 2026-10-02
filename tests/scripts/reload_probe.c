/* A script module built four ways for the host's reload tests (ADR-079):
 * 1 counts one per tick; 2 changes only code (ten per tick); 3 changes the
 * data version; 4 changes its component's fields. Built as a MODULE library
 * with only the SDK and foundation headers, so any engine symbol it used
 * would fail to link. */
#include "sdk.h"

#if !defined(RELOAD_VARIANT)
#error "RELOAD_VARIANT selects the build"
#endif

#if RELOAD_VARIANT == 4
#define RELOAD_PROBE_FIELDS                                                    \
  VKR_FIELD(F32, value, "Value", 0.0f)                                         \
  VKR_FIELD(F32, extra, "Extra", 0.0f)
#else
#define RELOAD_PROBE_FIELDS VKR_FIELD(F32, value, "Value", 0.0f)
#endif

VKR_COMPONENT(ReloadProbe, reload_probe, "Reload probe", RELOAD_PROBE_FIELDS)

/* The test reads `ticks` through the host's instance data. */
typedef struct ReloadData {
  uint32_t ticks;
  uint32_t starts;
} ReloadData;

static void probe_start(VkrCtx *ctx, ReloadData *data) {
  (void)ctx;
  data->starts++;
}

static void probe_fixed_update(VkrCtx *ctx, ReloadData *data) {
  (void)ctx;
  data->ticks += RELOAD_VARIANT == 2 ? 10u : 1u;
}

VKR_MODULE(reload_probe, ReloadData, VKR_EXPORT_COMPONENT(reload_probe),
           .data_version = RELOAD_VARIANT == 3 ? 2u : 1u, .start = probe_start,
           .fixed_update = probe_fixed_update)
