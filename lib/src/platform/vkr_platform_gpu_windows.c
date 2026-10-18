/* GPU adapter telemetry on Windows
   (docs/proposals/profiling-and-diagnostics.md, P1): the OS's video memory
   budget and usage through DXGI, and the engine use of other processes through
   PDH. The PDH sampler is a thread of its own because one collection of every
   process's engine counters takes milliseconds; a frame reads its result with
   an atomic load. */
#include "vkr_platform.h"

#if defined(PLATFORM_WINDOWS)
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "memory/vkr_allocator.h"
#include "memory/vkr_heap.h"

#define COBJMACROS
#include <dxgi1_4.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wchar.h>

_Static_assert(sizeof(LUID) == VKR_PLATFORM_ADAPTER_LUID_SIZE,
               "an adapter LUID is the Windows LUID");

/* One engine counter collection per period. */
#define VKR_GPU_MONITOR_PERIOD_MS 1000u
/* The sampler checks its stop request this often while it waits, so
   destroying the monitor joins it within about this long. */
#define VKR_GPU_MONITOR_STOP_POLL_MS 50u
/* Distinct engines one collection sums apart. An adapter has about 16
   (3D, Copy, Compute 0, Video Decode 1, ...); engines past the table, only
   possible without an adapter filter, are left out of the maximum. */
#define VKR_GPU_MONITOR_ENGINE_MAX 64u
/* The published utilization before the first complete sample, and after a
   failed one. */
#define VKR_GPU_MONITOR_UTIL_UNAVAILABLE UINT32_MAX

struct s_VkrPlatformGpuMonitor {
  VkrAllocator *allocator;
  /* NULL without an adapter LUID or without DXGI 1.4. */
  IDXGIAdapter3 *adapter;
  LUID luid;
  bool8_t luid_valid;
  DWORD process_id;
  /* Created by the first sample and joined by destroy, the only two users;
     both run on the owner's side. */
  VkrThread sampler;
  bool8_t sampler_started;
  /* Shared with the sampler thread. */
  VkrAtomicBool stop;
  VkrAtomicUint32 other_process_util;
};

/* The engines of one collection, keyed by the instance name after its
   process id, `luid_0x.._0x.._phys_N_eng_N_engtype_<type>`, so each engine of
   each adapter sums apart. The keys borrow the collection's item buffer. */
typedef struct VkrGpuMonitorEngines {
  const wchar_t *keys[VKR_GPU_MONITOR_ENGINE_MAX];
  float64_t sums[VKR_GPU_MONITOR_ENGINE_MAX];
  uint32_t count;
} VkrGpuMonitorEngines;

vkr_internal IDXGIAdapter3 *vkr_gpu_monitor_open_adapter(LUID luid) {
  IDXGIFactory4 *factory = NULL;
  IDXGIAdapter3 *adapter = NULL;
  if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory))) {
    return NULL;
  }
  if (FAILED(IDXGIFactory4_EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter3,
                                             (void **)&adapter))) {
    adapter = NULL;
  }
  /* The adapter keeps its factory alive. */
  IDXGIFactory4_Release(factory);
  return adapter;
}

/* Advances `*at` past `literal` when the text starts with it. */
vkr_internal bool8_t vkr_gpu_monitor_skip(const wchar_t **at,
                                          const wchar_t *literal) {
  const wchar_t *cursor = *at;
  while (*literal != L'\0') {
    if (*cursor != *literal) {
      return false_v;
    }
    ++cursor;
    ++literal;
  }
  *at = cursor;
  return true_v;
}

/* Reads a 32-bit decimal or hexadecimal number at `*at` and advances past
   it. */
vkr_internal bool8_t vkr_gpu_monitor_number(const wchar_t **at, uint32_t base,
                                            uint32_t *out_value) {
  const wchar_t *cursor = *at;
  uint64_t value = 0u;
  for (;; ++cursor) {
    uint32_t digit = 0u;
    if (*cursor >= L'0' && *cursor <= L'9') {
      digit = (uint32_t)(*cursor - L'0');
    } else if (base == 16u && *cursor >= L'a' && *cursor <= L'f') {
      digit = 10u + (uint32_t)(*cursor - L'a');
    } else if (base == 16u && *cursor >= L'A' && *cursor <= L'F') {
      digit = 10u + (uint32_t)(*cursor - L'A');
    } else {
      break;
    }
    value = value * base + digit;
    if (value > UINT32_MAX) {
      return false_v;
    }
  }
  if (cursor == *at) {
    return false_v;
  }
  *at = cursor;
  *out_value = (uint32_t)value;
  return true_v;
}

/* Splits a `GPU Engine` instance name,
   `pid_1148_luid_0x00000000_0x0000D398_phys_0_eng_0_engtype_3D`, into the
   process, the adapter (high part first) and the engine key that starts at
   `luid_`. */
vkr_internal bool8_t
vkr_gpu_monitor_parse_instance(const wchar_t *name, DWORD *out_process_id,
                               LUID *out_luid, const wchar_t **out_engine) {
  const wchar_t *at = name;
  uint32_t process_id = 0u;
  uint32_t high = 0u;
  uint32_t low = 0u;
  if (!vkr_gpu_monitor_skip(&at, L"pid_") ||
      !vkr_gpu_monitor_number(&at, 10u, &process_id) ||
      !vkr_gpu_monitor_skip(&at, L"_")) {
    return false_v;
  }
  const wchar_t *engine = at;
  if (!vkr_gpu_monitor_skip(&at, L"luid_0x") ||
      !vkr_gpu_monitor_number(&at, 16u, &high) ||
      !vkr_gpu_monitor_skip(&at, L"_0x") ||
      !vkr_gpu_monitor_number(&at, 16u, &low)) {
    return false_v;
  }
  *out_process_id = (DWORD)process_id;
  out_luid->HighPart = (LONG)high;
  out_luid->LowPart = (DWORD)low;
  *out_engine = engine;
  return true_v;
}

vkr_internal void vkr_gpu_monitor_engine_add(VkrGpuMonitorEngines *engines,
                                             const wchar_t *key,
                                             float64_t value) {
  for (uint32_t i = 0u; i < engines->count; ++i) {
    if (wcscmp(engines->keys[i], key) == 0) {
      engines->sums[i] += value;
      return;
    }
  }
  if (engines->count < VKR_GPU_MONITOR_ENGINE_MAX) {
    engines->keys[engines->count] = key;
    engines->sums[engines->count] = value;
    engines->count++;
  }
}

/*
 * Folds one collection: every other process's utilization summed per engine
 * of the monitored adapter, and the busiest engine's sum, 0 to 100. A known
 * adapter that no instance names, not even this process's, reads unavailable
 * rather than idle.
 */
vkr_internal uint32_t vkr_gpu_monitor_fold(
    const struct s_VkrPlatformGpuMonitor *monitor,
    const PDH_FMT_COUNTERVALUE_ITEM_W *items, DWORD item_count) {
  VkrGpuMonitorEngines engines = {0};
  bool8_t adapter_seen = !monitor->luid_valid;
  for (DWORD i = 0u; i < item_count; ++i) {
    DWORD process_id = 0u;
    LUID luid = {0};
    const wchar_t *engine = NULL;
    if (!items[i].szName || !vkr_gpu_monitor_parse_instance(
                                items[i].szName, &process_id, &luid, &engine)) {
      continue;
    }
    if (monitor->luid_valid && (luid.LowPart != monitor->luid.LowPart ||
                                luid.HighPart != monitor->luid.HighPart)) {
      continue;
    }
    adapter_seen = true_v;
    const DWORD status = items[i].FmtValue.CStatus;
    if (process_id == monitor->process_id ||
        (status != PDH_CSTATUS_VALID_DATA && status != PDH_CSTATUS_NEW_DATA)) {
      continue;
    }
    vkr_gpu_monitor_engine_add(&engines, engine, items[i].FmtValue.doubleValue);
  }
  if (!adapter_seen) {
    return VKR_GPU_MONITOR_UTIL_UNAVAILABLE;
  }
  float64_t busiest = 0.0;
  for (uint32_t i = 0u; i < engines.count; ++i) {
    busiest = Max(busiest, engines.sums[i]);
  }
  return (uint32_t)(Clamp(busiest, 0.0, 100.0) + 0.5);
}

/* Reads the last collection into `*items`, growing it as instances come and
   go; the sampler thread owns the buffer. */
vkr_internal uint32_t vkr_gpu_monitor_read(
    const struct s_VkrPlatformGpuMonitor *monitor, PDH_HCOUNTER counter,
    PDH_FMT_COUNTERVALUE_ITEM_W **items, DWORD *capacity_bytes) {
  DWORD bytes = *capacity_bytes;
  DWORD count = 0u;
  PDH_STATUS status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE,
                                                   &bytes, &count, *items);
  if (status == PDH_MORE_DATA) {
    PDH_FMT_COUNTERVALUE_ITEM_W *grown = vkr_heap_realloc(*items, bytes);
    if (!grown) {
      return VKR_GPU_MONITOR_UTIL_UNAVAILABLE;
    }
    *items = grown;
    *capacity_bytes = bytes;
    count = 0u;
    status = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes,
                                          &count, *items);
  }
  if (status != ERROR_SUCCESS) {
    return VKR_GPU_MONITOR_UTIL_UNAVAILABLE;
  }
  return vkr_gpu_monitor_fold(monitor, *items, count);
}

/* Waits one period; false once destroy asked the sampler to stop. */
vkr_internal bool8_t
vkr_gpu_monitor_wait(struct s_VkrPlatformGpuMonitor *monitor) {
  for (uint32_t waited = 0u; waited < VKR_GPU_MONITOR_PERIOD_MS;
       waited += VKR_GPU_MONITOR_STOP_POLL_MS) {
    if (vkr_atomic_bool_load(&monitor->stop, VKR_MEMORY_ORDER_ACQUIRE)) {
      return false_v;
    }
    vkr_thread_sleep(VKR_GPU_MONITOR_STOP_POLL_MS);
  }
  return !vkr_atomic_bool_load(&monitor->stop, VKR_MEMORY_ORDER_ACQUIRE);
}

vkr_internal void *vkr_gpu_monitor_sampler_main(void *arg) {
  struct s_VkrPlatformGpuMonitor *monitor = arg;
  (void)vkr_thread_set_utility();
  PDH_HQUERY query = NULL;
  PDH_HCOUNTER counter = NULL;
  PDH_FMT_COUNTERVALUE_ITEM_W *items = NULL;
  DWORD capacity_bytes = 0u;

  /* The English path works on every display language. Utilization is a rate,
     so the first collection only sets its base. */
  if (PdhOpenQueryW(NULL, 0u, &query) != ERROR_SUCCESS) {
    query = NULL;
    goto cleanup;
  }
  if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage",
                            0u, &counter) != ERROR_SUCCESS ||
      PdhCollectQueryData(query) != ERROR_SUCCESS) {
    log_warn("GPU engine counters are unavailable; other processes' GPU use "
             "is not sampled");
    goto cleanup;
  }
  while (vkr_gpu_monitor_wait(monitor)) {
    uint32_t util = VKR_GPU_MONITOR_UTIL_UNAVAILABLE;
    if (PdhCollectQueryData(query) == ERROR_SUCCESS) {
      util = vkr_gpu_monitor_read(monitor, counter, &items, &capacity_bytes);
    }
    vkr_atomic_uint32_store(&monitor->other_process_util, util,
                            VKR_MEMORY_ORDER_RELEASE);
  }

cleanup:
  vkr_atomic_uint32_store(&monitor->other_process_util,
                          VKR_GPU_MONITOR_UTIL_UNAVAILABLE,
                          VKR_MEMORY_ORDER_RELEASE);
  vkr_heap_free(items);
  if (query) {
    PdhCloseQuery(query);
  }
  return NULL;
}

bool8_t vkr_platform_gpu_monitor_create(VkrAllocator *allocator,
                                        const uint8_t *adapter_luid,
                                        VkrPlatformGpuMonitor *out_monitor) {
  if (!out_monitor) {
    return false_v;
  }
  *out_monitor = NULL;
  if (!allocator) {
    return false_v;
  }
  VkrPlatformGpuMonitor monitor = vkr_allocator_alloc(
      allocator, sizeof(*monitor), VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!monitor) {
    return false_v;
  }
  MemZero(monitor, sizeof(*monitor));
  monitor->allocator = allocator;
  monitor->process_id = GetCurrentProcessId();
  vkr_atomic_bool_store(&monitor->stop, false_v, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&monitor->other_process_util,
                          VKR_GPU_MONITOR_UTIL_UNAVAILABLE,
                          VKR_MEMORY_ORDER_RELAXED);
  if (adapter_luid) {
    MemCopy(&monitor->luid, adapter_luid, sizeof(monitor->luid));
    monitor->luid_valid = true_v;
    monitor->adapter = vkr_gpu_monitor_open_adapter(monitor->luid);
  }
  *out_monitor = monitor;
  return true_v;
}

/* QueryVideoMemoryInfo is a kernel query of a few microseconds, so memory is
   read on the frame; node 0 is the adapter of a single-GPU device. */
bool8_t
vkr_platform_gpu_monitor_sample(VkrPlatformGpuMonitor monitor,
                                VkrPlatformGpuTelemetry *out_telemetry) {
  if (!out_telemetry) {
    return false_v;
  }
  VkrPlatformGpuTelemetry telemetry = {0};
  if (!monitor) {
    *out_telemetry = telemetry;
    return false_v;
  }
  if (!monitor->sampler_started) {
    /* A sampler that cannot start leaves the value unavailable for the
       monitor's life. */
    monitor->sampler_started = true_v;
    if (!vkr_thread_create(monitor->allocator, &monitor->sampler,
                           vkr_gpu_monitor_sampler_main, monitor)) {
      monitor->sampler = NULL;
      log_warn("Failed to start the GPU engine sampler thread");
    }
  }

  if (monitor->adapter) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {0};
    if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(
            monitor->adapter, 0u, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
      telemetry.memory_budget_bytes = info.Budget;
      telemetry.memory_usage_bytes = info.CurrentUsage;
      telemetry.memory_valid = true_v;
    }
  }
  const uint32_t util = vkr_atomic_uint32_load(&monitor->other_process_util,
                                               VKR_MEMORY_ORDER_ACQUIRE);
  if (util != VKR_GPU_MONITOR_UTIL_UNAVAILABLE) {
    telemetry.other_process_util_percent = util;
    telemetry.other_process_util_valid = true_v;
  }
  *out_telemetry = telemetry;
  return telemetry.memory_valid || telemetry.other_process_util_valid;
}

void vkr_platform_gpu_monitor_destroy(VkrAllocator *allocator,
                                      VkrPlatformGpuMonitor *monitor) {
  if (!allocator || !monitor || !*monitor) {
    return;
  }
  VkrPlatformGpuMonitor owned = *monitor;
  if (owned->sampler) {
    vkr_atomic_bool_store(&owned->stop, true_v, VKR_MEMORY_ORDER_RELEASE);
    (void)vkr_thread_join(owned->sampler);
    (void)vkr_thread_destroy(allocator, &owned->sampler);
  }
  if (owned->adapter) {
    IDXGIAdapter3_Release(owned->adapter);
  }
  vkr_allocator_free(allocator, owned, sizeof(*owned),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *monitor = NULL;
}
#endif
