/* GPU telemetry on macOS (docs/proposals/profiling-and-diagnostics.md, P1):
   the Apple GPU driver's statistics from the `PerformanceStatistics`
   dictionary of its IOAccelerator service (class AGXAccelerator...). One read
   of that property takes 80 to 450 us on an M1 Pro, too long for a frame, so
   a sampler thread of its own reads it and a frame takes the result with
   atomic loads. The keys are undocumented driver properties; a missing key
   leaves its value unavailable. */
#include "vkr_platform.h"

#if defined(PLATFORM_APPLE)
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "core/vkr_threads.h"
#include "memory/vkr_allocator.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

/* One read of the driver's statistics per period. The driver's own update
   period of its utilization figures is unknown; values that change less often
   repeat over several reads. */
#define VKR_GPU_MONITOR_PERIOD_MS 250u
/* The sampler checks its stop request this often while it waits, so
   destroying the monitor joins it within about this long. */
#define VKR_GPU_MONITOR_STOP_POLL_MS 50u
/* A published value before the first read, and after a read that lacked
   it. */
#define VKR_GPU_MONITOR_VALUE_UNAVAILABLE UINT64_MAX

/* The driver statistics in the order of their dictionary keys
   (vkr_gpu_monitor_read). */
typedef enum VkrGpuMonitorStatistic {
  VKR_GPU_MONITOR_DEVICE_UTIL,
  VKR_GPU_MONITOR_RENDERER_UTIL,
  VKR_GPU_MONITOR_TILER_UTIL,
  VKR_GPU_MONITOR_TILED_SCENE_BYTES,
  VKR_GPU_MONITOR_SPLIT_SCENE_COUNT,
  VKR_GPU_MONITOR_PARAMETER_BUFFER_BYTES,
  VKR_GPU_MONITOR_RECOVERY_COUNT,
  VKR_GPU_MONITOR_MEMORY_IN_USE,
  VKR_GPU_MONITOR_DRIVER_MEMORY_IN_USE,
  VKR_GPU_MONITOR_MEMORY_ALLOCATED,
  VKR_GPU_MONITOR_STATISTIC_COUNT,
} VkrGpuMonitorStatistic;

struct s_VkrPlatformGpuMonitor {
  VkrAllocator *allocator;
  /* Created by the first sample and joined by destroy, the only two users;
     both run on the owner's side. */
  VkrThread sampler;
  bool8_t sampler_started;
  /* Shared with the sampler thread. */
  VkrAtomicBool stop;
  /* Set by the sampler when no IOAccelerator service has statistics. */
  VkrAtomicBool source_missing;
  VkrAtomicUint64 values[VKR_GPU_MONITOR_STATISTIC_COUNT];
};

/*
 * Finds the GPU driver's service: the first IOAccelerator that publishes
 * `PerformanceStatistics`. An Apple silicon Mac has one. Returns a reference
 * the caller releases with IOObjectRelease, or IO_OBJECT_NULL.
 */
vkr_internal io_registry_entry_t vkr_gpu_monitor_find_service(void) {
  io_iterator_t iterator = IO_OBJECT_NULL;
  /* The call consumes the matching dictionary. */
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOAccelerator"),
                                   &iterator) != KERN_SUCCESS) {
    return IO_OBJECT_NULL;
  }

  io_registry_entry_t found = IO_OBJECT_NULL;
  io_registry_entry_t entry = IO_OBJECT_NULL;
  while ((entry = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
    if (found == IO_OBJECT_NULL) {
      CFTypeRef statistics = IORegistryEntryCreateCFProperty(
          entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0u);
      if (statistics) {
        CFRelease(statistics);
        found = entry;
        continue;
      }
    }
    IOObjectRelease(entry);
  }
  IOObjectRelease(iterator);
  return found;
}

/* A non-negative integer of the dictionary, or
   VKR_GPU_MONITOR_VALUE_UNAVAILABLE when the key is missing or holds
   something else. The dictionary keeps ownership of the value. */
vkr_internal uint64_t vkr_gpu_monitor_number(CFDictionaryRef dictionary,
                                             CFStringRef key) {
  CFTypeRef value = CFDictionaryGetValue(dictionary, key);
  if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) {
    return VKR_GPU_MONITOR_VALUE_UNAVAILABLE;
  }
  int64_t number = 0;
  if (!CFNumberGetValue((CFNumberRef)value, kCFNumberSInt64Type, &number) ||
      number < 0) {
    return VKR_GPU_MONITOR_VALUE_UNAVAILABLE;
  }
  return (uint64_t)number;
}

/* Reads the driver's statistics once and publishes every value; a failed
   read publishes every value unavailable. */
vkr_internal void vkr_gpu_monitor_read(struct s_VkrPlatformGpuMonitor *monitor,
                                       io_registry_entry_t service) {
  const CFStringRef keys[VKR_GPU_MONITOR_STATISTIC_COUNT] = {
      [VKR_GPU_MONITOR_DEVICE_UTIL] = CFSTR("Device Utilization %"),
      [VKR_GPU_MONITOR_RENDERER_UTIL] = CFSTR("Renderer Utilization %"),
      [VKR_GPU_MONITOR_TILER_UTIL] = CFSTR("Tiler Utilization %"),
      [VKR_GPU_MONITOR_TILED_SCENE_BYTES] = CFSTR("TiledSceneBytes"),
      [VKR_GPU_MONITOR_SPLIT_SCENE_COUNT] = CFSTR("SplitSceneCount"),
      [VKR_GPU_MONITOR_PARAMETER_BUFFER_BYTES] = CFSTR("Allocated PB Size"),
      [VKR_GPU_MONITOR_RECOVERY_COUNT] = CFSTR("recoveryCount"),
      [VKR_GPU_MONITOR_MEMORY_IN_USE] = CFSTR("In use system memory"),
      [VKR_GPU_MONITOR_DRIVER_MEMORY_IN_USE] =
          CFSTR("In use system memory (driver)"),
      [VKR_GPU_MONITOR_MEMORY_ALLOCATED] = CFSTR("Alloc system memory"),
  };

  CFTypeRef statistics = IORegistryEntryCreateCFProperty(
      service, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0u);
  const bool8_t is_dictionary =
      statistics && CFGetTypeID(statistics) == CFDictionaryGetTypeID();

  for (uint32_t i = 0u; i < VKR_GPU_MONITOR_STATISTIC_COUNT; ++i) {
    uint64_t value = VKR_GPU_MONITOR_VALUE_UNAVAILABLE;
    if (is_dictionary) {
      value = vkr_gpu_monitor_number((CFDictionaryRef)statistics, keys[i]);
    }
    vkr_atomic_uint64_store(&monitor->values[i], value,
                            VKR_MEMORY_ORDER_RELEASE);
  }

  if (statistics) {
    CFRelease(statistics);
  }
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
  (void)vkr_thread_set_name("VKR GPU Monitor");

  /* The service lookup walks the registry, so it runs here rather than on
     the thread that creates the monitor. */
  const io_registry_entry_t service = vkr_gpu_monitor_find_service();
  if (service == IO_OBJECT_NULL) {
    log_warn("No GPU driver statistics found; GPU utilization is not "
             "sampled");
    vkr_atomic_bool_store(&monitor->source_missing, true_v,
                          VKR_MEMORY_ORDER_RELEASE);
    return NULL;
  }

  /* The driver's statistics are absolute, so the first read is complete. */
  do {
    vkr_gpu_monitor_read(monitor, service);
  } while (vkr_gpu_monitor_wait(monitor));

  IOObjectRelease(service);
  return NULL;
}

bool8_t vkr_platform_gpu_monitor_create(VkrAllocator *allocator,
                                        const uint8_t *adapter_luid,
                                        VkrPlatformGpuMonitor *out_monitor) {
  /* Metal names no adapter LUID, and an Apple silicon Mac has one GPU. */
  (void)adapter_luid;
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
  vkr_atomic_bool_store(&monitor->stop, false_v, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_bool_store(&monitor->source_missing, false_v,
                        VKR_MEMORY_ORDER_RELAXED);
  for (uint32_t i = 0u; i < VKR_GPU_MONITOR_STATISTIC_COUNT; ++i) {
    vkr_atomic_uint64_store(&monitor->values[i],
                            VKR_GPU_MONITOR_VALUE_UNAVAILABLE,
                            VKR_MEMORY_ORDER_RELAXED);
  }
  *out_monitor = monitor;
  return true_v;
}

/* Loads one published value; false while it is unavailable. */
vkr_internal bool8_t
vkr_gpu_monitor_load(const struct s_VkrPlatformGpuMonitor *monitor,
                     VkrGpuMonitorStatistic statistic, uint64_t *out_value) {
  const uint64_t value = vkr_atomic_uint64_load(&monitor->values[statistic],
                                                VKR_MEMORY_ORDER_ACQUIRE);
  if (value == VKR_GPU_MONITOR_VALUE_UNAVAILABLE) {
    return false_v;
  }
  *out_value = value;
  return true_v;
}

/* Loads a utilization, clamped to 100 percent. */
vkr_internal bool8_t vkr_gpu_monitor_load_percent(
    const struct s_VkrPlatformGpuMonitor *monitor,
    VkrGpuMonitorStatistic statistic, uint32_t *out_percent) {
  uint64_t value = 0u;
  if (!vkr_gpu_monitor_load(monitor, statistic, &value)) {
    return false_v;
  }
  *out_percent = (uint32_t)Min(value, 100u);
  return true_v;
}

/* macOS reports no OS budget of GPU memory and no GPU use of other
   processes; Metal's own memory figures stay in the renderer's metrics. */
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
    /* A sampler that cannot start leaves every value unavailable for the
       monitor's life. */
    monitor->sampler_started = true_v;
    if (!vkr_thread_create(monitor->allocator, &monitor->sampler,
                           vkr_gpu_monitor_sampler_main, monitor)) {
      monitor->sampler = NULL;
      vkr_atomic_bool_store(&monitor->source_missing, true_v,
                            VKR_MEMORY_ORDER_RELEASE);
      log_warn("Failed to start the GPU driver statistics sampler thread");
    }
  }

  telemetry.driver_statistics_supported =
      !vkr_atomic_bool_load(&monitor->source_missing, VKR_MEMORY_ORDER_ACQUIRE);
  telemetry.device_util_valid = vkr_gpu_monitor_load_percent(
      monitor, VKR_GPU_MONITOR_DEVICE_UTIL, &telemetry.device_util_percent);
  telemetry.renderer_util_valid = vkr_gpu_monitor_load_percent(
      monitor, VKR_GPU_MONITOR_RENDERER_UTIL, &telemetry.renderer_util_percent);
  telemetry.tiler_util_valid = vkr_gpu_monitor_load_percent(
      monitor, VKR_GPU_MONITOR_TILER_UTIL, &telemetry.tiler_util_percent);
  telemetry.tiled_scene_bytes_valid = vkr_gpu_monitor_load(
      monitor, VKR_GPU_MONITOR_TILED_SCENE_BYTES, &telemetry.tiled_scene_bytes);
  telemetry.split_scene_count_valid = vkr_gpu_monitor_load(
      monitor, VKR_GPU_MONITOR_SPLIT_SCENE_COUNT, &telemetry.split_scene_count);
  telemetry.parameter_buffer_bytes_valid =
      vkr_gpu_monitor_load(monitor, VKR_GPU_MONITOR_PARAMETER_BUFFER_BYTES,
                           &telemetry.parameter_buffer_bytes);
  telemetry.recovery_count_valid = vkr_gpu_monitor_load(
      monitor, VKR_GPU_MONITOR_RECOVERY_COUNT, &telemetry.recovery_count);
  telemetry.memory_in_use_valid = vkr_gpu_monitor_load(
      monitor, VKR_GPU_MONITOR_MEMORY_IN_USE, &telemetry.memory_in_use_bytes);
  telemetry.driver_memory_in_use_valid =
      vkr_gpu_monitor_load(monitor, VKR_GPU_MONITOR_DRIVER_MEMORY_IN_USE,
                           &telemetry.driver_memory_in_use_bytes);
  telemetry.memory_allocated_valid =
      vkr_gpu_monitor_load(monitor, VKR_GPU_MONITOR_MEMORY_ALLOCATED,
                           &telemetry.memory_allocated_bytes);

  *out_telemetry = telemetry;
  return telemetry.device_util_valid || telemetry.renderer_util_valid ||
         telemetry.tiler_util_valid || telemetry.tiled_scene_bytes_valid ||
         telemetry.split_scene_count_valid ||
         telemetry.parameter_buffer_bytes_valid ||
         telemetry.recovery_count_valid || telemetry.memory_in_use_valid ||
         telemetry.driver_memory_in_use_valid ||
         telemetry.memory_allocated_valid;
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
  vkr_allocator_free(allocator, owned, sizeof(*owned),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  *monitor = NULL;
}

bool8_t vkr_platform_get_gpu_info(VkrPlatformGpuInfo *out_info) {
  if (!out_info) {
    return false_v;
  }
  VkrPlatformGpuInfo info = {0};
  const io_registry_entry_t service = vkr_gpu_monitor_find_service();
  if (service == IO_OBJECT_NULL) {
    *out_info = info;
    return false_v;
  }

  bool8_t model_found = false_v;
  CFTypeRef model = IORegistryEntryCreateCFProperty(service, CFSTR("model"),
                                                    kCFAllocatorDefault, 0u);
  if (model) {
    /* A name longer than the array leaves it empty. */
    if (CFGetTypeID(model) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)model, info.model, sizeof(info.model),
                           kCFStringEncodingUTF8)) {
      model_found = info.model[0] != '\0';
    } else {
      info.model[0] = '\0';
    }
    CFRelease(model);
  }

  CFTypeRef cores = IORegistryEntryCreateCFProperty(
      service, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0u);
  if (cores) {
    int64_t count = 0;
    if (CFGetTypeID(cores) == CFNumberGetTypeID() &&
        CFNumberGetValue((CFNumberRef)cores, kCFNumberSInt64Type, &count) &&
        count > 0 && count <= (int64_t)UINT32_MAX) {
      info.core_count = (uint32_t)count;
      info.core_count_valid = true_v;
    }
    CFRelease(cores);
  }

  IOObjectRelease(service);
  *out_info = info;
  return model_found || info.core_count_valid;
}
#endif
