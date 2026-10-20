#pragma once

#include "core/vkr_threads.h"
#include "defines.h"

/* What each backend reports about the pipelines it creates, for the
 * Performance window's HW tab and recordings (ADR-130). The renderer owns
 * one registry for its lifetime and hands it to its backend; the backend
 * adds an entry after each pipeline it creates, on any thread. Entries are
 * never removed: a pipeline created again (a variant rebuilt, a format
 * change) adds another. A value the API does not report stays zero.
 *
 * Metal reports compute and tile pipelines' maxTotalThreadsPerThreadgroup,
 * which an Apple GPU lowers below the device's 1024 when a kernel holds
 * many registers: the only public sign of register pressure there. Vulkan
 * reports VK_KHR_pipeline_executable_properties statistics (registers,
 * spills, instructions) when the device has it and capture is on. */

/* Entries a registry keeps; later ones are counted in `dropped`. */
#define VKR_PIPELINE_REGISTRY_MAX 4096u
/* Statistics it keeps over all entries; later ones are left out. */
#define VKR_PIPELINE_REGISTRY_STATS_MAX 32768u
#define VKR_PIPELINE_NAME_MAX 63u
#define VKR_PIPELINE_STAT_NAME_MAX 47u
/* Statistics one entry carries at most. */
#define VKR_PIPELINE_STATS_PER_ENTRY 64u

typedef enum VkrPipelineKind {
  VKR_PIPELINE_KIND_RENDER = 0,
  VKR_PIPELINE_KIND_COMPUTE,
  VKR_PIPELINE_KIND_TILE,
  VKR_PIPELINE_KIND_COUNT,
} VkrPipelineKind;

/** One named statistic a driver reports, such as Vulkan's "VGPRs" of a
 * fragment executable ("Fragment Shader: VGPRs"). */
typedef struct VkrPipelineStat {
  char name[VKR_PIPELINE_STAT_NAME_MAX + 1u];
  float64_t value;
} VkrPipelineStat;

typedef struct VkrPipelineInfo {
  char name[VKR_PIPELINE_NAME_MAX + 1u];
  VkrPipelineKind kind;
  /** Wall time of the create call; negative when it was not timed. */
  float32_t compile_ms;
  /** Most threads one threadgroup may hold (Metal compute and tile). */
  uint32_t max_threads;
  /** Threads that run in lockstep: Metal's threadExecutionWidth. */
  uint32_t execution_width;
  /** Threadgroup or workgroup memory the shader declares, in bytes. */
  uint32_t threadgroup_memory;
  /** The most registers one of its stages uses (Vulkan VGPRs or the
   * driver's register count). */
  uint32_t registers;
  /** Bytes of scratch or spilled registers its stages use. */
  uint32_t spill_bytes;
  /** Instructions of all its stages. */
  uint32_t instructions;
} VkrPipelineInfo;

typedef struct VkrPipelineRegistryEntry {
  VkrPipelineInfo info;
  uint32_t stat_first;
  uint32_t stat_count;
} VkrPipelineRegistryEntry;

typedef struct VkrPipelineRegistry {
  VkrAllocator *allocator;
  VkrMutex lock;
  /* Heap blocks that grow under the lock. */
  VkrPipelineRegistryEntry *entries;
  uint32_t count;
  uint32_t capacity;
  VkrPipelineStat *stats;
  uint32_t stat_count;
  uint32_t stat_capacity;
  /* Entries left out past VKR_PIPELINE_REGISTRY_MAX. */
  uint32_t dropped;
} VkrPipelineRegistry;

/** Receives entry `index` and its statistics while the registry is locked;
 * neither pointer outlives the call. */
typedef void (*VkrPipelineRegistryVisit)(void *context, uint32_t index,
                                         const VkrPipelineInfo *info,
                                         const VkrPipelineStat *stats,
                                         uint32_t stat_count);

/** `allocator` holds the lock; the entries live on the heap. */
bool8_t vkr_pipeline_registry_create(VkrAllocator *allocator,
                                     VkrPipelineRegistry *out_registry);
void vkr_pipeline_registry_destroy(VkrPipelineRegistry *registry);

/** Adds a pipeline and up to VKR_PIPELINE_STATS_PER_ENTRY statistics; a
 * NULL registry ignores it. Safe from any thread. */
void vkr_pipeline_registry_add(VkrPipelineRegistry *registry,
                               const VkrPipelineInfo *info,
                               const VkrPipelineStat *stats,
                               uint32_t stat_count);

/** Visits the entries from `first` in the order they were added and returns
 * how many there are in all. */
uint32_t vkr_pipeline_registry_visit(VkrPipelineRegistry *registry,
                                     uint32_t first,
                                     VkrPipelineRegistryVisit visit,
                                     void *context);

/** Copies `name` into an entry or statistic name, cut to `capacity`. */
void vkr_pipeline_registry_name(char *out, uint32_t capacity, const char *name);
