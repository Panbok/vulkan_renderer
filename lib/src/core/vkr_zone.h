#pragma once

#include "core/vkr_atomic.h"
#include "defines.h"

/*
 * CPU zones: named spans of a thread's time, recorded for a timeline
 * (docs/proposals/profiling-and-diagnostics.md, P2). A zone costs one atomic
 * load while nothing records; with a sink installed it reads the monotonic
 * clock twice and hands one fixed record to the sink, which copies it into
 * the calling thread's own ring and never blocks.
 *
 * Use one zone per scope, closed on every path out of it:
 *
 *   VKR_ZONE_BEGIN(zone, "Physics.Step");
 *   ...
 *   VKR_ZONE_END(zone);
 *
 * Only threads that named their slot with vkr_zone_thread_set record; zones
 * on other threads cost the same atomic load and record nothing.
 *
 * With signposts on, the same zones also open and close native profiler
 * intervals (vkr_platform_signpost_begin: Instruments on macOS), whether or
 * not a sink records them.
 */

#define VKR_ZONE_SITE_MAX 1024u
/* Thread slots: the frame loop, the render thread, then job workers. */
#define VKR_ZONE_THREAD_MAIN 0u
#define VKR_ZONE_THREAD_RENDER 1u
#define VKR_ZONE_THREAD_WORKER_FIRST 2u
#define VKR_ZONE_THREAD_MAX 18u
#define VKR_ZONE_THREAD_NONE 0xffffu

/** One call site. `id` is 0 until the first recorded zone registers the
 * site, then its zone id plus one. */
typedef struct VkrZoneSite {
  const char *name;
  VkrAtomicUint32 id;
} VkrZoneSite;

/** One finished zone, in the layout of the recording's `zone` stream. Times
 * are vkr_platform_get_monotonic_ns. */
typedef struct VkrZoneRecord {
  uint16_t zone;
  uint16_t thread;
  uint32_t depth;
  uint64_t begin_ns;
  uint64_t end_ns;
} VkrZoneRecord;

_Static_assert(sizeof(VkrZoneRecord) == 24u, "zone record layout");

/** Receives finished zones on the thread that ran them. */
typedef void (*VkrZoneWrite)(void *context, const VkrZoneRecord *record);

typedef struct VkrZoneSink {
  VkrZoneWrite write;
  void *context;
} VkrZoneSink;

typedef struct VkrZoneScope {
  VkrZoneSite *site;
  uint64_t begin_ns;
  uint32_t depth;
  bool8_t open;
  /* The native interval's id, or 0 without one. */
  uint64_t signpost;
} VkrZoneScope;

/**
 * Installs the sink zones go to, or stops recording with NULL. A stop waits
 * until no thread is inside a write, so the sink's context may be released
 * when it returns. `sink` is copied.
 */
void vkr_zone_sink_set(const VkrZoneSink *sink);

/** Turns the zones' native profiler intervals on or off, and reads it. */
void vkr_zone_signposts_set(bool8_t on);
bool8_t vkr_zone_signposts(void);

/** Names the calling thread's slot; VKR_ZONE_THREAD_NONE stops its zones. */
void vkr_zone_thread_set(uint16_t slot);

/** Sites registered so far, and one site's name; safe from any thread. */
uint32_t vkr_zone_site_count(void);
const char *vkr_zone_site_name(uint32_t zone);

VkrZoneScope vkr_zone_begin(VkrZoneSite *site);
void vkr_zone_end(VkrZoneScope *scope);

#define VKR_ZONE_BEGIN(scope, label)                                           \
  vkr_local_persist VkrZoneSite scope##_site = {.name = (label)};              \
  VkrZoneScope scope = vkr_zone_begin(&scope##_site)

#define VKR_ZONE_END(scope) vkr_zone_end(&(scope))
