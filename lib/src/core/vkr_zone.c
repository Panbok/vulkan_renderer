#include "core/vkr_zone.h"

#include "core/vkr_threads.h"
#include "platform/vkr_platform.h"

/* The installed sink. A writer counts itself in `g_zone_writers` before it
   reads `g_zone_active` and out after its write; a stop clears the flag,
   then waits for the count to reach zero before the sink's context goes
   away. The count and the flag are each written by one side and read by the
   other, so both sides use sequentially consistent order: with weaker order
   a stop could read no writers while a writer still reads the flag set. */
vkr_global VkrZoneSink g_zone_sink = {0};
vkr_global VkrAtomicBool g_zone_active = false_v;
vkr_global VkrAtomicUint32 g_zone_writers = 0u;

/* Registered site names by zone id; a name is published before the count
   that covers it. */
vkr_global VkrAtomicUint64 g_zone_names[VKR_ZONE_SITE_MAX];
vkr_global VkrAtomicUint32 g_zone_site_count = 0u;

vkr_global VKR_THREAD_LOCAL uint16_t g_zone_thread = VKR_ZONE_THREAD_NONE;
vkr_global VKR_THREAD_LOCAL uint32_t g_zone_depth = 0u;

void vkr_zone_sink_set(const VkrZoneSink *sink) {
  if (sink && sink->write) {
    g_zone_sink = *sink;
    vkr_atomic_bool_store(&g_zone_active, true_v, VKR_MEMORY_ORDER_RELEASE);
    return;
  }
  vkr_atomic_bool_store(&g_zone_active, false_v, VKR_MEMORY_ORDER_SEQ_CST);
  /* A writer that saw the sink active finishes its write first. */
  while (vkr_atomic_uint32_load(&g_zone_writers, VKR_MEMORY_ORDER_SEQ_CST) !=
         0u) {
    vkr_thread_yield();
  }
  g_zone_sink = (VkrZoneSink){0};
}

void vkr_zone_thread_set(uint16_t slot) {
  g_zone_thread = slot < VKR_ZONE_THREAD_MAX ? slot : VKR_ZONE_THREAD_NONE;
  g_zone_depth = 0u;
}

uint32_t vkr_zone_site_count(void) {
  return Min(
      vkr_atomic_uint32_load(&g_zone_site_count, VKR_MEMORY_ORDER_ACQUIRE),
      VKR_ZONE_SITE_MAX);
}

const char *vkr_zone_site_name(uint32_t zone) {
  if (zone >= vkr_zone_site_count()) {
    return NULL;
  }
  return (const char *)(uintptr_t)vkr_atomic_uint64_load(
      &g_zone_names[zone], VKR_MEMORY_ORDER_ACQUIRE);
}

/* The site's zone id, registering it on first use; UINT32_MAX when every id
   is taken. Two threads may race to register one site: the loser's id
   stays a duplicate name, which the timeline reads the same. */
vkr_internal uint32_t vkr_zone_site_id(VkrZoneSite *site) {
  const uint32_t known =
      vkr_atomic_uint32_load(&site->id, VKR_MEMORY_ORDER_ACQUIRE);
  if (known != 0u) {
    return known - 1u;
  }
  const uint32_t zone = vkr_atomic_uint32_fetch_add(&g_zone_site_count, 1u,
                                                    VKR_MEMORY_ORDER_ACQ_REL);
  if (zone >= VKR_ZONE_SITE_MAX) {
    return UINT32_MAX;
  }
  vkr_atomic_uint64_store(&g_zone_names[zone], (uint64_t)(uintptr_t)site->name,
                          VKR_MEMORY_ORDER_RELEASE);
  uint32_t expected = 0u;
  if (!vkr_atomic_uint32_compare_exchange(&site->id, &expected, zone + 1u,
                                          VKR_MEMORY_ORDER_ACQ_REL,
                                          VKR_MEMORY_ORDER_ACQUIRE)) {
    return expected - 1u;
  }
  return zone;
}

VkrZoneScope vkr_zone_begin(VkrZoneSite *site) {
  VkrZoneScope scope = {.site = site};
  if (g_zone_thread == VKR_ZONE_THREAD_NONE ||
      !vkr_atomic_bool_load(&g_zone_active, VKR_MEMORY_ORDER_RELAXED)) {
    return scope;
  }
  scope.open = true_v;
  scope.depth = g_zone_depth++;
  scope.begin_ns = vkr_platform_get_monotonic_ns();
  return scope;
}

void vkr_zone_end(VkrZoneScope *scope) {
  if (!scope->open) {
    return;
  }
  const uint64_t end_ns = vkr_platform_get_monotonic_ns();
  scope->open = false_v;
  g_zone_depth = scope->depth;
  vkr_atomic_uint32_fetch_add(&g_zone_writers, 1u, VKR_MEMORY_ORDER_SEQ_CST);
  if (vkr_atomic_bool_load(&g_zone_active, VKR_MEMORY_ORDER_SEQ_CST)) {
    const uint32_t zone = vkr_zone_site_id(scope->site);
    if (zone != UINT32_MAX) {
      const VkrZoneRecord record = {
          .zone = (uint16_t)zone,
          .thread = g_zone_thread,
          .depth = scope->depth,
          .begin_ns = scope->begin_ns,
          .end_ns = end_ns,
      };
      g_zone_sink.write(g_zone_sink.context, &record);
    }
  }
  vkr_atomic_uint32_fetch_sub(&g_zone_writers, 1u, VKR_MEMORY_ORDER_RELEASE);
}
