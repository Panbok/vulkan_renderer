#include "containers/vkr_spsc_ring.h"

#include "core/vkr_byte_io.h"

typedef enum VkrSpscRecordKind {
  VKR_SPSC_RECORD_DATA = 1u,
  VKR_SPSC_RECORD_PADDING = 2u,
} VkrSpscRecordKind;

vkr_internal uint64_t vkr_spsc_record_bytes(uint64_t payload) {
  return AlignPow2(VKR_SPSC_RING_RECORD_HEADER + payload,
                   (uint64_t)VKR_SPSC_RING_RECORD_ALIGNMENT);
}

vkr_internal void vkr_spsc_write_header(VkrSpscRing *ring, uint64_t position,
                                        uint32_t size, VkrSpscRecordKind kind) {
  uint8_t *header = ring->data + (position & (ring->capacity - 1u));
  vkr_store_le_u32(header, size);
  vkr_store_le_u32(header + 4u, (uint32_t)kind);
}

bool8_t vkr_spsc_ring_create(VkrAllocator *allocator, uint64_t capacity,
                             VkrSpscRing *out_ring) {
  assert(allocator != NULL && out_ring != NULL);
  MemZero(out_ring, sizeof(*out_ring));
  if (capacity < 64u || (capacity & (capacity - 1u)) != 0u) {
    return false_v;
  }
  uint8_t *data = vkr_allocator_alloc_aligned(allocator, capacity, 64u,
                                              VKR_ALLOCATOR_MEMORY_TAG_QUEUE);
  if (!data) {
    return false_v;
  }
  out_ring->data = data;
  out_ring->capacity = capacity;
  vkr_atomic_uint64_store(&out_ring->write, 0u, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&out_ring->read, 0u, VKR_MEMORY_ORDER_RELAXED);
  return true_v;
}

void vkr_spsc_ring_destroy(VkrAllocator *allocator, VkrSpscRing *ring) {
  assert(allocator != NULL && ring != NULL);
  if (ring->data) {
    vkr_allocator_free_aligned(allocator, ring->data, ring->capacity, 64u,
                               VKR_ALLOCATOR_MEMORY_TAG_QUEUE);
  }
  MemZero(ring, sizeof(*ring));
}

uint64_t vkr_spsc_ring_max_payload(const VkrSpscRing *ring) {
  /* A record may need padding up to the end of the buffer first, so half the
     capacity is the largest size that always fits in an empty ring. */
  const uint64_t limit = ring->capacity / 2u - VKR_SPSC_RING_RECORD_HEADER;
  return Min(limit, (uint64_t)UINT32_MAX);
}

void *vkr_spsc_ring_reserve(VkrSpscRing *ring, uint32_t size) {
  assert(ring != NULL && ring->data != NULL);
  assert(ring->reserved_bytes == 0u && "commit the open reservation first");
  if ((uint64_t)size > vkr_spsc_ring_max_payload(ring)) {
    return NULL;
  }

  const uint64_t write =
      vkr_atomic_uint64_load(&ring->write, VKR_MEMORY_ORDER_RELAXED);
  const uint64_t record = vkr_spsc_record_bytes(size);
  const uint64_t offset = write & (ring->capacity - 1u);
  const uint64_t to_end = ring->capacity - offset;
  const uint64_t padding = record > to_end ? to_end : 0u;
  const uint64_t needed = padding + record;

  if (ring->capacity - (write - ring->producer_read_cache) < needed) {
    ring->producer_read_cache =
        vkr_atomic_uint64_load(&ring->read, VKR_MEMORY_ORDER_ACQUIRE);
    if (ring->capacity - (write - ring->producer_read_cache) < needed) {
      return NULL;
    }
  }

  if (padding > 0u) {
    vkr_spsc_write_header(ring, write,
                          (uint32_t)(padding - VKR_SPSC_RING_RECORD_HEADER),
                          VKR_SPSC_RECORD_PADDING);
  }
  const uint64_t start = write + padding;
  vkr_spsc_write_header(ring, start, size, VKR_SPSC_RECORD_DATA);
  ring->reserved_bytes = needed;
  return ring->data + (start & (ring->capacity - 1u)) +
         VKR_SPSC_RING_RECORD_HEADER;
}

void vkr_spsc_ring_commit(VkrSpscRing *ring) {
  assert(ring != NULL && ring->reserved_bytes > 0u);
  const uint64_t write =
      vkr_atomic_uint64_load(&ring->write, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&ring->write, write + ring->reserved_bytes,
                          VKR_MEMORY_ORDER_RELEASE);
  ring->reserved_bytes = 0u;
}

bool8_t vkr_spsc_ring_push(VkrSpscRing *ring, const void *payload,
                           uint32_t size) {
  void *destination = vkr_spsc_ring_reserve(ring, size);
  if (!destination) {
    return false_v;
  }
  if (size > 0u) {
    MemCopy(destination, payload, size);
  }
  vkr_spsc_ring_commit(ring);
  return true_v;
}

const void *vkr_spsc_ring_peek(VkrSpscRing *ring, uint32_t *out_size) {
  assert(ring != NULL && ring->data != NULL && out_size != NULL);
  uint64_t read = vkr_atomic_uint64_load(&ring->read, VKR_MEMORY_ORDER_RELAXED);
  for (;;) {
    if (ring->consumer_write_cache == read) {
      ring->consumer_write_cache =
          vkr_atomic_uint64_load(&ring->write, VKR_MEMORY_ORDER_ACQUIRE);
      if (ring->consumer_write_cache == read) {
        ring->peeked_bytes = 0u;
        return NULL;
      }
    }

    const uint8_t *header = ring->data + (read & (ring->capacity - 1u));
    const uint32_t size = vkr_load_le_u32(header);
    const uint32_t kind = vkr_load_le_u32(header + 4u);
    const uint64_t bytes = vkr_spsc_record_bytes(size);
    if (kind == VKR_SPSC_RECORD_PADDING) {
      /* Padding only fills the tail of the buffer; release it at once. */
      read += bytes;
      vkr_atomic_uint64_store(&ring->read, read, VKR_MEMORY_ORDER_RELEASE);
      continue;
    }
    assert(kind == VKR_SPSC_RECORD_DATA);
    ring->peeked_bytes = bytes;
    *out_size = size;
    return header + VKR_SPSC_RING_RECORD_HEADER;
  }
}

void vkr_spsc_ring_pop(VkrSpscRing *ring) {
  assert(ring != NULL && ring->peeked_bytes > 0u && "peek a record first");
  const uint64_t read =
      vkr_atomic_uint64_load(&ring->read, VKR_MEMORY_ORDER_RELAXED);
  vkr_atomic_uint64_store(&ring->read, read + ring->peeked_bytes,
                          VKR_MEMORY_ORDER_RELEASE);
  ring->peeked_bytes = 0u;
}

uint64_t vkr_spsc_ring_used_bytes(VkrSpscRing *ring) {
  const uint64_t read =
      vkr_atomic_uint64_load(&ring->read, VKR_MEMORY_ORDER_ACQUIRE);
  const uint64_t write =
      vkr_atomic_uint64_load(&ring->write, VKR_MEMORY_ORDER_ACQUIRE);
  return write - read;
}
