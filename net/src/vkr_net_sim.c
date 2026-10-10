#include "vkr_net_sim.h"

#include <string.h>

#define SIM_ENDPOINT_MAX 16u
#define SIM_DATAGRAM_BYTES VKR_NET_DATAGRAM_JUMBO

typedef struct SimLinkState {
  VkrNetSimLink link;
  /* When the bottleneck finishes serializing the queued datagrams. */
  uint64_t busy_until;
  /* The latest arrival so far; in-order datagrams arrive no earlier. */
  uint64_t last_arrival;
  VkrNetSimStats stats;
} SimLinkState;

typedef struct SimEndpoint {
  VkrNetCore *core;
  VkrNetAddress address;
} SimEndpoint;

typedef struct SimDatagram {
  uint64_t arrival;
  uint64_t order;
  uint32_t from;
  uint32_t to;
  VkrNetAddress source;
  uint32_t size;
  uint8_t *bytes;
} SimDatagram;

struct VkrNetSim {
  VkrAllocator *allocator;
  uint64_t state;
  uint64_t order;
  SimEndpoint endpoints[SIM_ENDPOINT_MAX];
  uint32_t endpoint_count;
  SimLinkState links[SIM_ENDPOINT_MAX][SIM_ENDPOINT_MAX];
  SimDatagram *flight;
  uint32_t flight_count;
  uint32_t flight_capacity;
  uint8_t buffer[SIM_DATAGRAM_BYTES];
};

uint64_t vkr_net_sim_random(VkrNetSim *sim) {
  /* splitmix64 */
  uint64_t z = (sim->state += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

static float64_t sim_unit(VkrNetSim *sim) {
  return (float64_t)(vkr_net_sim_random(sim) >> 11) / 9007199254740992.0;
}

VkrNetSim *vkr_net_sim_create(VkrAllocator *allocator, uint64_t seed) {
  VkrNetSim *sim = vkr_allocator_alloc(allocator, sizeof(VkrNetSim),
                                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  if (!sim) {
    return NULL;
  }
  MemZero(sim, sizeof(*sim));
  sim->allocator = allocator;
  sim->state = seed;
  return sim;
}

void vkr_net_sim_destroy(VkrNetSim *sim) {
  if (!sim) {
    return;
  }
  for (uint32_t i = 0u; i < sim->flight_count; ++i) {
    vkr_allocator_free(sim->allocator, sim->flight[i].bytes,
                       sim->flight[i].size, VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
  if (sim->flight) {
    vkr_allocator_free(sim->allocator, sim->flight,
                       (uint64_t)sim->flight_capacity * sizeof(SimDatagram),
                       VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
  }
  vkr_allocator_free(sim->allocator, sim, sizeof(VkrNetSim),
                     VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
}

uint32_t vkr_net_sim_add(VkrNetSim *sim, VkrNetCore *core,
                         const VkrNetAddress *address) {
  if (sim->endpoint_count == SIM_ENDPOINT_MAX) {
    return UINT32_MAX;
  }
  const uint32_t index = sim->endpoint_count++;
  sim->endpoints[index] = (SimEndpoint){.core = core, .address = *address};
  return index;
}

void vkr_net_sim_set_address(VkrNetSim *sim, uint32_t endpoint,
                             const VkrNetAddress *address) {
  if (endpoint < sim->endpoint_count) {
    sim->endpoints[endpoint].address = *address;
  }
}

void vkr_net_sim_set_link(VkrNetSim *sim, uint32_t from, uint32_t to,
                          const VkrNetSimLink *link) {
  if (from < SIM_ENDPOINT_MAX && to < SIM_ENDPOINT_MAX) {
    sim->links[from][to].link = *link;
  }
}

static uint32_t sim_find(const VkrNetSim *sim, const VkrNetAddress *address) {
  for (uint32_t i = 0u; i < sim->endpoint_count; ++i) {
    if (vkr_net_address_equal(&sim->endpoints[i].address, address)) {
      return i;
    }
  }
  return UINT32_MAX;
}

static void sim_enqueue(VkrNetSim *sim, uint32_t from, uint32_t to,
                        const uint8_t *bytes, uint32_t size, uint64_t arrival) {
  if (sim->flight_count == sim->flight_capacity) {
    const uint32_t capacity =
        sim->flight_capacity ? sim->flight_capacity * 2u : 256u;
    SimDatagram *flight = vkr_allocator_alloc(
        sim->allocator, (uint64_t)capacity * sizeof(SimDatagram),
        VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    if (!flight) {
      return;
    }
    if (sim->flight) {
      MemCopy(flight, sim->flight,
              (uint64_t)sim->flight_count * sizeof(SimDatagram));
      vkr_allocator_free(sim->allocator, sim->flight,
                         (uint64_t)sim->flight_capacity * sizeof(SimDatagram),
                         VKR_ALLOCATOR_MEMORY_TAG_STRUCT);
    }
    sim->flight = flight;
    sim->flight_capacity = capacity;
  }
  uint8_t *copy = vkr_allocator_alloc(sim->allocator, size,
                                      VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  if (!copy) {
    return;
  }
  MemCopy(copy, bytes, size);
  sim->flight[sim->flight_count++] = (SimDatagram){
      .arrival = arrival,
      .order = sim->order++,
      .from = from,
      .to = to,
      .source = sim->endpoints[from].address,
      .size = size,
      .bytes = copy,
  };
}

/* Applies the link to one sent datagram. */
static void sim_send(VkrNetSim *sim, uint32_t from, const VkrNetAddress *to,
                     const uint8_t *bytes, uint32_t size, uint64_t now) {
  const uint32_t target = sim_find(sim, to);
  if (target == UINT32_MAX) {
    return;
  }
  SimLinkState *state = &sim->links[from][target];
  const VkrNetSimLink *link = &state->link;
  state->stats.datagrams_sent += 1u;
  state->stats.bytes_sent += size;

  const uint32_t mtu = link->mtu ? link->mtu : VKR_UDP_DATAGRAM_MAX;
  if (size > mtu || (link->loss > 0.0f && sim_unit(sim) < link->loss)) {
    state->stats.datagrams_dropped += 1u;
    return;
  }

  uint64_t depart = now;
  if (link->bandwidth > 0u) {
    const uint64_t start = Max(now, state->busy_until);
    uint64_t queue_limit = link->queue_bytes;
    if (queue_limit == 0u) {
      queue_limit =
          Max(link->bandwidth * Max(link->latency_us, 1000u) * 2u / 1000000u,
              (uint64_t)64u * 1024u);
    }
    const uint64_t queued = (start - now) * link->bandwidth / 1000000u;
    if (queued + size > queue_limit) {
      state->stats.datagrams_dropped += 1u;
      return;
    }
    const uint64_t serialize =
        Max((uint64_t)size * 1000000u / link->bandwidth, 1u);
    state->busy_until = start + serialize;
    depart = state->busy_until;
  }
  uint64_t arrival = depart + link->latency_us;
  if (link->jitter_us > 0u) {
    arrival += vkr_net_sim_random(sim) % (link->jitter_us + 1u);
  }
  const bool8_t overtakes =
      link->reorder > 0.0f && sim_unit(sim) < link->reorder;
  if (!overtakes) {
    arrival = Max(arrival, state->last_arrival);
  }
  state->last_arrival = Max(state->last_arrival, arrival);
  sim_enqueue(sim, from, target, bytes, size, arrival);
  if (link->duplicate > 0.0f && sim_unit(sim) < link->duplicate) {
    uint64_t again = arrival;
    if (link->jitter_us > 0u) {
      again += vkr_net_sim_random(sim) % (link->jitter_us + 1u);
    }
    sim_enqueue(sim, from, target, bytes, size, again);
  }
}

static void sim_deliver_due(VkrNetSim *sim, uint64_t now) {
  for (;;) {
    uint32_t best = UINT32_MAX;
    for (uint32_t i = 0u; i < sim->flight_count; ++i) {
      const SimDatagram *d = &sim->flight[i];
      if (d->arrival > now) {
        continue;
      }
      if (best == UINT32_MAX || d->arrival < sim->flight[best].arrival ||
          (d->arrival == sim->flight[best].arrival &&
           d->order < sim->flight[best].order)) {
        best = i;
      }
    }
    if (best == UINT32_MAX) {
      return;
    }
    SimDatagram datagram = sim->flight[best];
    sim->flight[best] = sim->flight[--sim->flight_count];
    SimLinkState *state = &sim->links[datagram.from][datagram.to];
    state->stats.datagrams_delivered += 1u;
    state->stats.bytes_delivered += datagram.size;
    vkr_net_core_receive(sim->endpoints[datagram.to].core, &datagram.source,
                         datagram.bytes, datagram.size, now);
    vkr_allocator_free(sim->allocator, datagram.bytes, datagram.size,
                       VKR_ALLOCATOR_MEMORY_TAG_BUFFER);
  }
}

void vkr_net_sim_step(VkrNetSim *sim, uint64_t now_us) {
  sim_deliver_due(sim, now_us);
  for (uint32_t i = 0u; i < sim->endpoint_count; ++i) {
    VkrNetCore *core = sim->endpoints[i].core;
    vkr_net_core_update(core, now_us);
    VkrNetAddress to;
    uint32_t size = 0u;
    while ((size = vkr_net_core_transmit(core, now_us, sim->buffer,
                                         sizeof(sim->buffer), &to)) > 0u) {
      sim_send(sim, i, &to, sim->buffer, size, now_us);
    }
  }
  sim_deliver_due(sim, now_us);
}

uint64_t vkr_net_sim_next_time(const VkrNetSim *sim) {
  uint64_t next = UINT64_MAX;
  for (uint32_t i = 0u; i < sim->endpoint_count; ++i) {
    next = Min(next, vkr_net_core_next_deadline(sim->endpoints[i].core));
  }
  for (uint32_t i = 0u; i < sim->flight_count; ++i) {
    next = Min(next, sim->flight[i].arrival);
  }
  return next;
}

VkrNetSimStats vkr_net_sim_stats(const VkrNetSim *sim, uint32_t from,
                                 uint32_t to) {
  if (from >= SIM_ENDPOINT_MAX || to >= SIM_ENDPOINT_MAX) {
    return (VkrNetSimStats){0};
  }
  return sim->links[from][to].stats;
}
