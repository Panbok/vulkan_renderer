#include "vulkan/vkr_vulkan_amd_counters.h"

#if VKR_HAS_GPA

#include "containers/str.h"
#include "core/logger.h"
#include "core/vkr_atomic.h"
#include "math/vkr_math.h"
#include "platform/vkr_platform.h"

/* GPUPerfAPI's headers name Windows types for its library handle and UUIDs,
   and assert their layout with C11's static_assert spelling. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#if !defined(static_assert)
#define static_assert _Static_assert
#endif
#include "gpu_performance_api/gpu_perf_api.h"
#include "gpu_performance_api/gpu_perf_api_vk.h"

#define VKR_AMD_COUNTERS_LIBRARY "GPUPerfAPIVK-x64.dll"
/* Frames that may still run a session's command lists once its frame was
   recorded: the frame slots in flight and one more. A session that failed
   after its frame recorded GPUPerfAPI's commands is deleted only after this
   many later frames, so the GPU never writes into a deleted session. */
#define VKR_AMD_COUNTERS_RETIRE_FRAMES 4u
/* Failed sessions awaiting deletion at most; sampling pauses while full. */
#define VKR_AMD_COUNTERS_RETIRED_MAX 4u
/* A session whose frames never finished, such as a frame that was
   recorded but cancelled, is retired after this long. */
#define VKR_AMD_COUNTERS_WAIT_NS 2000000000ull
/* Frames one session samples at most. Few long sessions, since AMD's
   Windows driver before Adrenalin 26.7.1 hangs after a few dozen
   consecutive ones (GPUPerfAPI's known issues); a hang on 26.6.3 stopped
   the GPU in GpaBeginCommandList's packet after about 28 one-frame
   sessions. */
#define VKR_AMD_COUNTERS_SESSION_FRAMES 30u
/* Samples read per frame once a session finished, so its reads spread
   over frames instead of stalling one. */
#define VKR_AMD_COUNTERS_READS_PER_FRAME 1024u
/* Sample k * VKR_RENDERER_IMPL_MAX_PASS_TIMINGS + p is frame k's pass p. */
#define VKR_AMD_COUNTERS_SESSION_SAMPLES                                       \
  ((uint64_t)VKR_AMD_COUNTERS_SESSION_FRAMES *                                 \
   (uint64_t)VKR_RENDERER_IMPL_MAX_PASS_TIMINGS)
_Static_assert(VKR_AMD_COUNTERS_SESSION_SAMPLES < UINT32_MAX,
               "sample ids fit GpaUInt32");

/* The counters sampled, most useful first: the units each pass keeps busy,
   the memory path's stalls and hit rates, and compute efficiency. Names a
   GPU generation lacks are skipped. They form groups the hardware reads in
   one replay, which successive sessions take in turn. */
static const char *const VKR_AMD_COUNTER_NAMES[] = {
    "GPUBusy",
    "VsGsBusy",
    "PSBusy",
    "CSBusy",
    "TexUnitBusy",
    "DepthStencilTestBusy",
    "MemUnitBusy",
    "MemUnitStalled",
    "WriteUnitStalled",
    "L0CacheHit",
    "L1CacheHit",
    "L2CacheHit",
    "CSVALUUtilization",
    "CSLDSBankConflict",
    "PSExportStalls",
    "PrimitiveAssemblyBusy",
};
#define VKR_AMD_COUNTER_NAME_COUNT ArrayCount(VKR_AMD_COUNTER_NAMES)
_Static_assert(VKR_AMD_COUNTER_NAME_COUNT <= VKR_RENDERER_IMPL_GPU_COUNTER_MAX,
               "a session holds every requested counter");

typedef enum VkrAmdCounterState {
  VKR_AMD_COUNTERS_IDLE = 0,
  /* Frames record the session's samples. */
  VKR_AMD_COUNTERS_SAMPLING,
  /* Every frame is recorded; the GPU has yet to finish them. */
  VKR_AMD_COUNTERS_WAITING,
  /* The GPU finished; the samples are read a batch per frame. */
  VKR_AMD_COUNTERS_READING,
} VkrAmdCounterState;

struct s_VkrVulkanAmdCounters {
  VkrPlatformLibrary library;
  GpaFunctionTable gpa;
  bool8_t initialized;
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  GpaContextId context;
  /* The context or the counter plan failed; no session starts again. */
  bool8_t disabled;
  /* Requests from any thread: sample frames until this monotonic time. */
  VkrAtomicUint64 deadline_ns;

  /* Counter groups, each read in one replay: group g is
     group_names[group_start[g], group_start[g] + group_size[g]). */
  bool8_t planned;
  const char *group_names[VKR_AMD_COUNTER_NAME_COUNT];
  uint32_t group_start[VKR_AMD_COUNTER_NAME_COUNT];
  uint32_t group_size[VKR_AMD_COUNTER_NAME_COUNT];
  uint32_t group_count;
  uint32_t next_group;

  /* The session, on the render thread. */
  VkrAmdCounterState state;
  GpaSessionId session;
  /* Non-NULL while the frame being recorded samples. */
  GpaCommandListId command_list;
  bool8_t sample_open;
  /* Frames sampled so far and the first, last and current packet frame
     indices. */
  uint32_t frame_count;
  uint64_t first_frame_index;
  uint64_t last_frame_index;
  uint64_t frame_index;
  uint64_t waiting_since_ns;
  /* The passes the session's frames ran, by name: a graph that changes
     between frames adds entries, and a pass's values average over the
     frames that ran it. */
  uint32_t pass_count;
  char pass_names[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS]
                 [VKR_RENDERER_IMPL_TIMING_NAME_CAPACITY];
  /* Each sampled frame's passes as entries, UINT16_MAX past the table; the
     last frame's pass signature reuses its row while the graph holds. */
  uint16_t frame_entries[VKR_AMD_COUNTERS_SESSION_FRAMES]
                        [VKR_RENDERER_IMPL_MAX_PASS_TIMINGS];
  uint16_t frame_pass_counts[VKR_AMD_COUNTERS_SESSION_FRAMES];
  uint64_t last_signature;
  /* Reading: the next sample, and each entry's sums and sample counts. */
  uint32_t read_frame;
  uint32_t read_pass;
  float64_t sums[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS]
                [VKR_RENDERER_IMPL_GPU_COUNTER_MAX];
  uint16_t counts[VKR_RENDERER_IMPL_MAX_PASS_TIMINGS]
                 [VKR_RENDERER_IMPL_GPU_COUNTER_MAX];
  /* The session's counters in result order. */
  uint32_t counter_count;
  char counter_names[VKR_RENDERER_IMPL_GPU_COUNTER_MAX]
                    [VKR_RENDERER_IMPL_GPU_COUNTER_NAME_CAPACITY];
  uint8_t counter_units[VKR_RENDERER_IMPL_GPU_COUNTER_MAX];
  GpaDataType counter_types[VKR_RENDERER_IMPL_GPU_COUNTER_MAX];
  /* Failed sessions and the frames recorded since, deleted once the GPU
     is past them. */
  GpaSessionId retired[VKR_AMD_COUNTERS_RETIRED_MAX];
  uint32_t retired_frames[VKR_AMD_COUNTERS_RETIRED_MAX];
  uint32_t retired_count;

  /* The latest finished session; take() copies it out and empties it. */
  VkrAtomicUint32 published_full;
  VkrRendererImplGpuCounters published;
};

vkr_internal void vkr_amd_counters_log(GpaLoggingType type,
                                       const char *message) {
  if (type & kGpaLoggingError) {
    log_warn("GPUPerfAPI: %s", message);
  }
}

vkr_internal const char *vkr_amd_counters_status(VkrVulkanAmdCounters *c,
                                                 GpaStatus status) {
  const char *text =
      c->gpa.GpaGetStatusAsStr ? c->gpa.GpaGetStatusAsStr(status) : NULL;
  return text ? text : "unknown status";
}

bool8_t vkr_vulkan_amd_counters_create(VkrAllocator *allocator,
                                       VkrVulkanAmdCounters **out_counters) {
  if (!allocator || !out_counters) {
    return false_v;
  }
  *out_counters = NULL;
  VkrVulkanAmdCounters *c = vkr_allocator_alloc(
      allocator, sizeof(*c), VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
  if (!c) {
    return false_v;
  }
  MemZero(c, sizeof(*c));

  char error[256];
  if (!vkr_platform_library_open(VKR_AMD_COUNTERS_LIBRARY, &c->library, error,
                                 sizeof(error))) {
    log_info("AMD GPU counters unavailable: %s did not load (%s)",
             VKR_AMD_COUNTERS_LIBRARY, error);
    goto cleanup;
  }
  const GpaGetFuncTablePtrType get_function_table =
      (GpaGetFuncTablePtrType)vkr_platform_library_symbol(&c->library,
                                                          "GpaGetFuncTable");
  c->gpa.major_version = GPA_FUNCTION_TABLE_MAJOR_VERSION_NUMBER;
  c->gpa.minor_version = GPA_FUNCTION_TABLE_MINOR_VERSION_NUMBER;
  if (!get_function_table || get_function_table(&c->gpa) != kGpaStatusOk) {
    log_warn("AMD GPU counters unavailable: %s has no matching function table",
             VKR_AMD_COUNTERS_LIBRARY);
    goto cleanup;
  }
  (void)c->gpa.GpaRegisterLoggingCallback(kGpaLoggingError,
                                          vkr_amd_counters_log);
  const GpaStatus status = c->gpa.GpaInitialize(kGpaInitializeDefaultBit);
  if (status != kGpaStatusOk) {
    log_warn("AMD GPU counters unavailable: GpaInitialize failed (%s)",
             vkr_amd_counters_status(c, status));
    goto cleanup;
  }
  c->initialized = true_v;
  vkr_atomic_uint64_store(&c->deadline_ns, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint32_store(&c->published_full, 0u, VKR_MEMORY_ORDER_RELAXED);
  *out_counters = c;
  return true_v;

cleanup:
  if (c->library.handle) {
    vkr_platform_library_close(&c->library);
  }
  vkr_allocator_free(allocator, c, sizeof(*c), VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
  return false_v;
}

/* Keeps a session whose frame recorded GPUPerfAPI's commands until the GPU
   is past that frame. */
vkr_internal void vkr_amd_counters_retire(VkrVulkanAmdCounters *c,
                                          const char *reason) {
  log_warn("AMD GPU counters: dropped a session of %u frames: %s",
           c->frame_count, reason);
  if (c->session && c->retired_count < VKR_AMD_COUNTERS_RETIRED_MAX) {
    c->retired[c->retired_count] = c->session;
    c->retired_frames[c->retired_count] = 0u;
    ++c->retired_count;
  }
  c->session = NULL;
  c->command_list = NULL;
  c->sample_open = false_v;
  c->state = VKR_AMD_COUNTERS_IDLE;
}

void vkr_vulkan_amd_counters_destroy(VkrAllocator *allocator,
                                     VkrVulkanAmdCounters **counters) {
  if (!allocator || !counters || !*counters) {
    return;
  }
  VkrVulkanAmdCounters *c = *counters;
  /* The device is idle before it is destroyed, so no frame still runs a
     session's command lists. */
  for (uint32_t i = 0u; i < c->retired_count; ++i) {
    (void)c->gpa.GpaDeleteSession(c->retired[i]);
  }
  if (c->session) {
    (void)c->gpa.GpaDeleteSession(c->session);
  }
  if (c->context) {
    (void)c->gpa.GpaCloseContext(c->context);
  }
  if (c->initialized) {
    (void)c->gpa.GpaDestroy();
  }
  vkr_platform_library_close(&c->library);
  vkr_allocator_free(allocator, c, sizeof(*c), VKR_ALLOCATOR_MEMORY_TAG_VULKAN);
  *counters = NULL;
}

void vkr_vulkan_amd_counters_bind(VkrVulkanAmdCounters *counters,
                                  VkInstance instance,
                                  VkPhysicalDevice physical_device,
                                  VkDevice device) {
  if (!counters) {
    return;
  }
  counters->instance = instance;
  counters->physical_device = physical_device;
  counters->device = device;
}

void vkr_vulkan_amd_counters_request(VkrVulkanAmdCounters *counters,
                                     float64_t seconds) {
  if (!counters) {
    return;
  }
  const uint64_t now = vkr_platform_get_monotonic_ns();
  const uint64_t deadline =
      seconds > 0.0 ? now + (uint64_t)(seconds * 1.0e9) : 0u;
  log_info("AMD GPU counters: sampling for %.1f s", seconds);
  vkr_atomic_uint64_store(&counters->deadline_ns, deadline,
                          VKR_MEMORY_ORDER_RELEASE);
}

/* Opens the counter context with the driver's own clocks. */
vkr_internal bool8_t vkr_amd_counters_open(VkrVulkanAmdCounters *c) {
  if (c->context) {
    return true_v;
  }
  if (!c->device) {
    return false_v;
  }
  GpaVkContextOpenInfo info = {
      .instance = c->instance,
      .physical_device = c->physical_device,
      .device = c->device,
  };
  GpaStatus status = c->gpa.GpaOpenContext(
      &info, kGpaOpenContextClockModeNoneBit, &c->context);
  GpaContextSampleTypeFlags sample_types = 0;
  if (status == kGpaStatusOk) {
    status = c->gpa.GpaGetSupportedSampleTypes(c->context, &sample_types);
  }
  if (status != kGpaStatusOk ||
      !(sample_types & kGpaContextSampleTypeDiscreteCounter)) {
    log_warn("AMD GPU counters unavailable: the counter context did not "
             "open (%s)",
             vkr_amd_counters_status(c, status));
    if (c->context) {
      (void)c->gpa.GpaCloseContext(c->context);
      c->context = NULL;
    }
    return false_v;
  }
  return true_v;
}

/* Splits the counters into groups the hardware reads in one replay, with
   sessions that never begin. A counter that needs more than one replay on
   its own, or that this GPU lacks, is left out. */
vkr_internal bool8_t vkr_amd_counters_plan(VkrVulkanAmdCounters *c) {
  bool8_t placed[VKR_AMD_COUNTER_NAME_COUNT] = {0};
  uint32_t placed_count = 0u;
  uint32_t named = 0u;
  c->group_count = 0u;
  while (placed_count < VKR_AMD_COUNTER_NAME_COUNT) {
    GpaSessionId probe = NULL;
    if (c->gpa.GpaCreateSession(c->context,
                                kGpaSessionSampleTypeDiscreteCounter,
                                &probe) != kGpaStatusOk) {
      return false_v;
    }
    uint32_t grouped = 0u;
    for (uint32_t i = 0u; i < VKR_AMD_COUNTER_NAME_COUNT; ++i) {
      if (placed[i]) {
        continue;
      }
      const char *name = VKR_AMD_COUNTER_NAMES[i];
      GpaUInt32 replays = 0u;
      const bool8_t enabled =
          c->gpa.GpaEnableCounterByName(probe, name) == kGpaStatusOk;
      if (enabled && c->gpa.GpaGetPassCount(probe, &replays) == kGpaStatusOk &&
          replays == 1u) {
        c->group_names[named++] = name;
        ++grouped;
        placed[i] = true_v;
        ++placed_count;
        continue;
      }
      if (enabled) {
        (void)c->gpa.GpaDisableCounterByName(probe, name);
      }
      /* Absent, or too much for a replay even alone. */
      if (!enabled || grouped == 0u) {
        placed[i] = true_v;
        ++placed_count;
      }
    }
    (void)c->gpa.GpaDeleteSession(probe);
    if (grouped > 0u) {
      c->group_start[c->group_count] = named - grouped;
      c->group_size[c->group_count] = grouped;
      ++c->group_count;
    }
  }
  return c->group_count > 0u;
}

/* Creates and begins a session reading counter group `group`. Nothing is
   recorded yet, so a failure deletes the session at once. */
vkr_internal bool8_t vkr_amd_counters_start_session(VkrVulkanAmdCounters *c,
                                                    uint32_t group) {
  GpaStatus status = c->gpa.GpaCreateSession(
      c->context, kGpaSessionSampleTypeDiscreteCounter, &c->session);
  if (status != kGpaStatusOk) {
    log_warn("AMD GPU counters: GpaCreateSession failed (%s)",
             vkr_amd_counters_status(c, status));
    c->session = NULL;
    return false_v;
  }
  for (uint32_t i = 0u; i < c->group_size[group]; ++i) {
    (void)c->gpa.GpaEnableCounterByName(
        c->session, c->group_names[c->group_start[group] + i]);
  }
  GpaUInt32 replays = 0u;
  GpaUInt32 count = 0u;
  status = c->gpa.GpaGetPassCount(c->session, &replays);
  if (status == kGpaStatusOk) {
    status = c->gpa.GpaGetNumEnabledCounters(c->session, &count);
  }
  if (status == kGpaStatusOk && (replays != 1u || count == 0u)) {
    status = kGpaStatusErrorFailed;
  }
  if (status == kGpaStatusOk) {
    c->counter_count = Min(count, (GpaUInt32)VKR_RENDERER_IMPL_GPU_COUNTER_MAX);
    for (uint32_t e = 0u; e < c->counter_count; ++e) {
      GpaUInt32 index = 0u;
      const char *name = NULL;
      GpaUsageType usage = kGpaUsageTypeItems;
      GpaDataType type = kGpaDataTypeUint64;
      (void)c->gpa.GpaGetEnabledIndex(c->session, e, &index);
      (void)c->gpa.GpaGetCounterName(c->session, index, &name);
      (void)c->gpa.GpaGetCounterUsageType(c->session, index, &usage);
      (void)c->gpa.GpaGetCounterDataType(c->session, index, &type);
      if (!vkr_string_copy_bounded(c->counter_names[e],
                                   sizeof(c->counter_names[e]),
                                   name ? name : "")) {
        string_format(c->counter_names[e], sizeof(c->counter_names[e]),
                      "counter %u", index);
      }
      c->counter_units[e] = usage == kGpaUsageTypePercentage
                                ? VKR_RENDERER_IMPL_GPU_COUNTER_PERCENT
                                : VKR_RENDERER_IMPL_GPU_COUNTER_COUNT;
      c->counter_types[e] = type;
    }
    status = c->gpa.GpaBeginSession(c->session);
  }
  if (status != kGpaStatusOk) {
    log_warn("AMD GPU counters: counter group %u did not begin (%s)", group,
             vkr_amd_counters_status(c, status));
    (void)c->gpa.GpaDeleteSession(c->session);
    c->session = NULL;
    return false_v;
  }
  c->frame_count = 0u;
  c->pass_count = 0u;
  c->last_signature = 0u;
  c->state = VKR_AMD_COUNTERS_SAMPLING;
  return true_v;
}

/* Ends the session after its last sampled frame; the GPU then finishes
   the frames in flight. */
vkr_internal void vkr_amd_counters_end_session(VkrVulkanAmdCounters *c) {
  if (c->frame_count == 0u) {
    vkr_amd_counters_retire(c, "no frame was sampled");
    return;
  }
  const GpaStatus status = c->gpa.GpaEndSession(c->session);
  if (status != kGpaStatusOk) {
    vkr_amd_counters_retire(c, vkr_amd_counters_status(c, status));
    return;
  }
  c->state = VKR_AMD_COUNTERS_WAITING;
  c->waiting_since_ns = vkr_platform_get_monotonic_ns();
}

/* A hash of the frame's pass names in order. */
vkr_internal uint64_t vkr_amd_counters_signature(
    const VkrRendererImplPassTiming *passes, uint32_t pass_count) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t p = 0u; p < pass_count; ++p) {
    for (const char *at = passes[p].name; *at; ++at) {
      hash = (hash ^ (uint8_t)*at) * 1099511628211ull;
    }
    hash = (hash ^ 0xffu) * 1099511628211ull;
  }
  return hash;
}

/* The session's entry for pass `name`, added when new; UINT16_MAX when the
   table is full. */
vkr_internal uint16_t vkr_amd_counters_entry(VkrVulkanAmdCounters *c,
                                             const char *name) {
  for (uint32_t i = 0u; i < c->pass_count; ++i) {
    if (string_equals(c->pass_names[i], name)) {
      return (uint16_t)i;
    }
  }
  if (c->pass_count == VKR_RENDERER_IMPL_MAX_PASS_TIMINGS) {
    return UINT16_MAX;
  }
  MemCopy(c->pass_names[c->pass_count], name, sizeof(c->pass_names[0]));
  c->pass_names[c->pass_count][sizeof(c->pass_names[0]) - 1u] = '\0';
  return (uint16_t)c->pass_count++;
}

void vkr_vulkan_amd_counters_begin_frame(
    VkrVulkanAmdCounters *counters, VkCommandBuffer command,
    uint64_t frame_index, const VkrRendererImplPassTiming *passes,
    uint32_t pass_count) {
  VkrVulkanAmdCounters *c = counters;
  if (!c) {
    return;
  }
  c->command_list = NULL;
  c->sample_open = false_v;
  const bool8_t wanted =
      !c->disabled &&
      vkr_platform_get_monotonic_ns() <
          vkr_atomic_uint64_load(&c->deadline_ns, VKR_MEMORY_ORDER_ACQUIRE);
  if (c->state == VKR_AMD_COUNTERS_SAMPLING && !wanted) {
    vkr_amd_counters_end_session(c);
    return;
  }
  if (c->state == VKR_AMD_COUNTERS_IDLE) {
    if (!wanted || c->retired_count == VKR_AMD_COUNTERS_RETIRED_MAX) {
      return;
    }
    if (!c->planned) {
      c->planned = true_v;
      if (!vkr_amd_counters_open(c) || !vkr_amd_counters_plan(c)) {
        log_warn("AMD GPU counters unavailable: no counter group fits one "
                 "replay");
        c->disabled = true_v;
        return;
      }
      log_info("AMD GPU counters: %u counters in %u groups of one replay",
               c->group_start[c->group_count - 1u] +
                   c->group_size[c->group_count - 1u],
               c->group_count);
    }
    const uint32_t group = c->next_group;
    if (!vkr_amd_counters_start_session(c, group)) {
      c->disabled = true_v;
      return;
    }
    c->next_group = (group + 1u) % c->group_count;
  }
  if (c->state != VKR_AMD_COUNTERS_SAMPLING || pass_count == 0u ||
      pass_count > VKR_RENDERER_IMPL_MAX_PASS_TIMINGS) {
    return;
  }
  const uint32_t k = c->frame_count;
  const uint64_t signature = vkr_amd_counters_signature(passes, pass_count);
  if (k > 0u && signature == c->last_signature &&
      pass_count == c->frame_pass_counts[k - 1u]) {
    MemCopy(c->frame_entries[k], c->frame_entries[k - 1u],
            sizeof(uint16_t) * pass_count);
  } else {
    for (uint32_t p = 0u; p < pass_count; ++p) {
      c->frame_entries[k][p] = vkr_amd_counters_entry(c, passes[p].name);
    }
  }
  c->frame_pass_counts[k] = (uint16_t)pass_count;
  c->last_signature = signature;
  const GpaStatus status = c->gpa.GpaBeginCommandList(
      c->session, 0u, command, kGpaCommandListPrimary, &c->command_list);
  if (status != kGpaStatusOk) {
    log_warn("AMD GPU counters: GpaBeginCommandList failed (%s)",
             vkr_amd_counters_status(c, status));
    c->command_list = NULL;
    /* The frames already sampled still finish on the GPU. */
    vkr_amd_counters_end_session(c);
    return;
  }
  if (k == 0u) {
    c->first_frame_index = frame_index;
  }
  c->frame_index = frame_index;
}

void vkr_vulkan_amd_counters_begin_pass(VkrVulkanAmdCounters *counters,
                                        uint32_t order) {
  VkrVulkanAmdCounters *c = counters;
  if (c && c->command_list) {
    const GpaUInt32 sample =
        c->frame_count * (GpaUInt32)VKR_RENDERER_IMPL_MAX_PASS_TIMINGS + order;
    c->sample_open =
        c->gpa.GpaBeginSample(sample, c->command_list) == kGpaStatusOk;
  }
}

void vkr_vulkan_amd_counters_end_pass(VkrVulkanAmdCounters *counters) {
  if (counters && counters->sample_open) {
    (void)counters->gpa.GpaEndSample(counters->command_list);
    counters->sample_open = false_v;
  }
}

void vkr_vulkan_amd_counters_end_frame(VkrVulkanAmdCounters *counters) {
  VkrVulkanAmdCounters *c = counters;
  if (!c || !c->command_list) {
    return;
  }
  const GpaStatus status = c->gpa.GpaEndCommandList(c->command_list);
  c->command_list = NULL;
  if (status != kGpaStatusOk) {
    vkr_amd_counters_retire(c, vkr_amd_counters_status(c, status));
    return;
  }
  c->last_frame_index = c->frame_index;
  if (++c->frame_count == VKR_AMD_COUNTERS_SESSION_FRAMES) {
    vkr_amd_counters_end_session(c);
  }
}

/* One sample's results as float32 values in counter order, NaN where the
   sample has none. */
vkr_internal void vkr_amd_counters_read_sample(VkrVulkanAmdCounters *c,
                                               GpaUInt32 sample,
                                               float32_t *out) {
  for (uint32_t e = 0u; e < c->counter_count; ++e) {
    out[e] = VKR_FLOAT_NAN;
  }
  size_t size = 0u;
  uint64_t raw[VKR_RENDERER_IMPL_GPU_COUNTER_MAX];
  if (c->gpa.GpaGetSampleResultSize(c->session, sample, &size) !=
          kGpaStatusOk ||
      size != sizeof(uint64_t) * c->counter_count ||
      c->gpa.GpaGetSampleResult(c->session, sample, size, raw) !=
          kGpaStatusOk) {
    return;
  }
  for (uint32_t e = 0u; e < c->counter_count; ++e) {
    if (c->counter_types[e] == kGpaDataTypeFloat64) {
      float64_t value = 0.0;
      MemCopy(&value, &raw[e], sizeof(value));
      out[e] = (float32_t)value;
    } else {
      out[e] = (float32_t)raw[e];
    }
  }
}

/* Publishes the read session when the slot is empty: each pass entry's
   mean over the frames that ran it. */
vkr_internal void vkr_amd_counters_publish(VkrVulkanAmdCounters *c) {
  if (vkr_atomic_uint32_load(&c->published_full, VKR_MEMORY_ORDER_ACQUIRE) !=
      0u) {
    return;
  }
  VkrRendererImplGpuCounters *out = &c->published;
  out->first_frame_index = c->first_frame_index;
  out->last_frame_index = c->last_frame_index;
  out->counter_count = c->counter_count;
  MemCopy(out->counter_names, c->counter_names, sizeof(out->counter_names));
  MemCopy(out->counter_units, c->counter_units, sizeof(out->counter_units));
  out->pass_count = c->pass_count;
  for (uint32_t p = 0u; p < c->pass_count; ++p) {
    MemCopy(out->pass_names[p], c->pass_names[p], sizeof(out->pass_names[p]));
    for (uint32_t e = 0u; e < c->counter_count; ++e) {
      out->values[p][e] = c->counts[p][e]
                              ? (float32_t)(c->sums[p][e] / c->counts[p][e])
                              : VKR_FLOAT_NAN;
    }
  }
  vkr_atomic_uint32_store(&c->published_full, 1u, VKR_MEMORY_ORDER_RELEASE);
}

void vkr_vulkan_amd_counters_collect(VkrVulkanAmdCounters *counters) {
  VkrVulkanAmdCounters *c = counters;
  if (!c) {
    return;
  }
  /* Called once per frame recorded: a retired session is deleted once the
     frames in flight since it cannot hold its command lists any more. */
  for (uint32_t i = 0u; i < c->retired_count;) {
    if (++c->retired_frames[i] <= VKR_AMD_COUNTERS_RETIRE_FRAMES) {
      ++i;
      continue;
    }
    (void)c->gpa.GpaDeleteSession(c->retired[i]);
    c->retired[i] = c->retired[c->retired_count - 1u];
    c->retired_frames[i] = c->retired_frames[c->retired_count - 1u];
    --c->retired_count;
  }
  if (c->state == VKR_AMD_COUNTERS_WAITING) {
    const GpaStatus status = c->gpa.GpaIsSessionComplete(c->session);
    if (status == kGpaStatusResultNotReady) {
      if (vkr_platform_get_monotonic_ns() - c->waiting_since_ns >
          VKR_AMD_COUNTERS_WAIT_NS) {
        vkr_amd_counters_retire(c, "its frames never finished");
      }
      return;
    }
    if (status != kGpaStatusOk) {
      vkr_amd_counters_retire(c, vkr_amd_counters_status(c, status));
      return;
    }
    c->state = VKR_AMD_COUNTERS_READING;
    c->read_frame = 0u;
    c->read_pass = 0u;
    MemZero(c->sums, sizeof(c->sums[0]) * c->pass_count);
    MemZero(c->counts, sizeof(c->counts[0]) * c->pass_count);
  }
  if (c->state != VKR_AMD_COUNTERS_READING) {
    return;
  }
  /* The GPU finished every frame, so the reads do not wait. */
  float32_t values[VKR_RENDERER_IMPL_GPU_COUNTER_MAX];
  for (uint32_t budget = VKR_AMD_COUNTERS_READS_PER_FRAME;
       budget > 0u && c->read_frame < c->frame_count; --budget) {
    const uint32_t k = c->read_frame;
    const uint32_t p = c->read_pass;
    const uint16_t entry = c->frame_entries[k][p];
    if (entry != UINT16_MAX) {
      vkr_amd_counters_read_sample(
          c, k * (GpaUInt32)VKR_RENDERER_IMPL_MAX_PASS_TIMINGS + p, values);
      for (uint32_t e = 0u; e < c->counter_count; ++e) {
        if (values[e] == values[e]) {
          c->sums[entry][e] += values[e];
          ++c->counts[entry][e];
        }
      }
    }
    if (++c->read_pass == c->frame_pass_counts[k]) {
      c->read_pass = 0u;
      ++c->read_frame;
    }
  }
  if (c->read_frame < c->frame_count) {
    return;
  }
  log_info("AMD GPU counters: read frames %llu to %llu, %u passes, %u "
           "counters",
           (unsigned long long)c->first_frame_index,
           (unsigned long long)c->last_frame_index, c->pass_count,
           c->counter_count);
  vkr_amd_counters_publish(c);
  (void)c->gpa.GpaDeleteSession(c->session);
  c->session = NULL;
  c->state = VKR_AMD_COUNTERS_IDLE;
}

bool8_t vkr_vulkan_amd_counters_take(VkrVulkanAmdCounters *counters,
                                     VkrRendererImplGpuCounters *out) {
  if (!counters || !out ||
      vkr_atomic_uint32_load(&counters->published_full,
                             VKR_MEMORY_ORDER_ACQUIRE) == 0u) {
    return false_v;
  }
  MemCopy(out, &counters->published, sizeof(*out));
  vkr_atomic_uint32_store(&counters->published_full, 0u,
                          VKR_MEMORY_ORDER_RELEASE);
  return true_v;
}

#else

/* Builds without GPUPerfAPI (other hosts) have no AMD counters. */
bool8_t vkr_vulkan_amd_counters_create(VkrAllocator *allocator,
                                       VkrVulkanAmdCounters **out_counters) {
  (void)allocator;
  if (out_counters) {
    *out_counters = NULL;
  }
  return false_v;
}

void vkr_vulkan_amd_counters_destroy(VkrAllocator *allocator,
                                     VkrVulkanAmdCounters **counters) {
  (void)allocator;
  (void)counters;
}

void vkr_vulkan_amd_counters_bind(VkrVulkanAmdCounters *counters,
                                  VkInstance instance,
                                  VkPhysicalDevice physical_device,
                                  VkDevice device) {
  (void)counters;
  (void)instance;
  (void)physical_device;
  (void)device;
}

void vkr_vulkan_amd_counters_request(VkrVulkanAmdCounters *counters,
                                     float64_t seconds) {
  (void)counters;
  (void)seconds;
}

void vkr_vulkan_amd_counters_begin_frame(
    VkrVulkanAmdCounters *counters, VkCommandBuffer command,
    uint64_t frame_index, const VkrRendererImplPassTiming *passes,
    uint32_t pass_count) {
  (void)counters;
  (void)command;
  (void)frame_index;
  (void)passes;
  (void)pass_count;
}

void vkr_vulkan_amd_counters_begin_pass(VkrVulkanAmdCounters *counters,
                                        uint32_t order) {
  (void)counters;
  (void)order;
}

void vkr_vulkan_amd_counters_end_pass(VkrVulkanAmdCounters *counters) {
  (void)counters;
}

void vkr_vulkan_amd_counters_end_frame(VkrVulkanAmdCounters *counters) {
  (void)counters;
}

void vkr_vulkan_amd_counters_collect(VkrVulkanAmdCounters *counters) {
  (void)counters;
}

bool8_t vkr_vulkan_amd_counters_take(VkrVulkanAmdCounters *counters,
                                     VkrRendererImplGpuCounters *out) {
  (void)counters;
  (void)out;
  return false_v;
}

#endif
