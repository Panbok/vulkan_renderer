/* A project library built four ways for the host's project tests
 * (ADR-079), listing its modules as Bakery's generated module list does:
 * 1 lists ProbeA and ProbeB; 2 changes only ProbeA's code (ten per tick);
 * 3 drops ProbeB and adds ProbeC; 4 changes a ProbeA field and ProbeC's
 * code. Built as a MODULE library from the SDK and foundation headers. */
#include "sdk.h"

#if !defined(PROJECT_VARIANT)
#error "PROJECT_VARIANT selects the build"
#endif

typedef struct ProbeData {
  uint32_t ticks;
} ProbeData;

#if PROJECT_VARIANT == 4
#define PROBE_A_FIELDS                                                         \
  VKR_FIELD(F32, value, "Value", 0.0f) VKR_FIELD(F32, extra, "Extra", 0.0f)
#else
#define PROBE_A_FIELDS VKR_FIELD(F32, value, "Value", 0.0f)
#endif

VKR_COMPONENT(ProbeA, probe_a, "Probe A", PROBE_A_FIELDS)

static void probe_a_fixed_update(VkrCtx *ctx, ProbeData *data) {
  (void)ctx;
  data->ticks += PROJECT_VARIANT == 2 ? 10u : 1u;
}

VKR_MODULE(ProbeA, ProbeData, VKR_EXPORT_COMPONENT(probe_a),
           .fixed_update = probe_a_fixed_update)

#if PROJECT_VARIANT <= 2
VKR_COMPONENT(ProbeB, probe_b, "Probe B", VKR_FIELD(BOOL, on, "On", true_v))
VKR_MODULE(ProbeB, VkrNoData, VKR_EXPORT_COMPONENT(probe_b))
#else
VKR_COMPONENT(ProbeC, probe_c, "Probe C", VKR_FIELD(BOOL, on, "On", true_v))

static void probe_c_fixed_update(VkrCtx *ctx, ProbeData *data) {
  (void)ctx;
  data->ticks += PROJECT_VARIANT == 4 ? 100u : 1u;
}

VKR_MODULE(ProbeC, ProbeData, VKR_EXPORT_COMPONENT(probe_c),
           .fixed_update = probe_c_fixed_update)
#endif

VKR_SDK_EXPORT uint32_t vkr_project_modules(VkrModuleEntry *entries,
                                            uint32_t capacity) {
#if PROJECT_VARIANT <= 2
  static const VkrModuleEntry modules[] = {vkr_module_ProbeA,
                                           vkr_module_ProbeB};
#else
  static const VkrModuleEntry modules[] = {vkr_module_ProbeA,
                                           vkr_module_ProbeC};
#endif
  for (uint32_t i = 0; i < 2u && i < capacity; ++i) {
    entries[i] = modules[i];
  }
  return 2u;
}
