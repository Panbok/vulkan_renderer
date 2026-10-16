#pragma once

#include "core/vkr_atomic.h"
#include "defines.h"
#include "memory/vkr_allocator.h"

/* Bytes in front of every record: its payload size and kind. */
#define VKR_SPSC_RING_RECORD_HEADER 8u
/* Records start on this boundary, so a payload can hold aligned scalars. */
#define VKR_SPSC_RING_RECORD_ALIGNMENT 8u

/**
 * A lock-free byte ring between exactly one producer thread and exactly one
 * consumer thread. Each record is contiguous: a record that does not fit
 * before the end of the buffer leaves a padding record and starts at offset
 * zero, so the consumer reads every payload in place.
 *
 * The producer reserves, writes and commits; the consumer peeks and pops.
 * Neither side blocks: a reservation fails when the ring is full and a peek
 * fails when it is empty. Positions are monotonic byte counts; the producer
 * owns `write`, the consumer owns `read`, and each side caches the other's
 * position to avoid reading a shared line on every call.
 */
typedef struct VkrSpscRing {
  uint8_t *data;
  /* A power of two of at least 64 bytes. */
  uint64_t capacity;

  /* Producer-owned. */
  VkrAtomicUint64 write;
  uint64_t producer_read_cache;
  /* Bytes of the open reservation, padding included; zero when none. */
  uint64_t reserved_bytes;
  uint8_t producer_padding[64];

  /* Consumer-owned. */
  VkrAtomicUint64 read;
  uint64_t consumer_write_cache;
  /* Bytes of the record the last peek returned; zero when none. */
  uint64_t peeked_bytes;
  uint8_t consumer_padding[64];
} VkrSpscRing;

/**
 * @brief Allocates a ring of `capacity` bytes.
 * @param capacity A power of two of at least 64 bytes.
 * @return false_v when the capacity is invalid or the allocation fails.
 */
bool8_t vkr_spsc_ring_create(VkrAllocator *allocator, uint64_t capacity,
                             VkrSpscRing *out_ring);

/** Releases the ring's storage. Both threads must have stopped using it. */
void vkr_spsc_ring_destroy(VkrAllocator *allocator, VkrSpscRing *ring);

/** The largest payload one record can hold in this ring. */
uint64_t vkr_spsc_ring_max_payload(const VkrSpscRing *ring);

/**
 * @brief Producer: reserves a contiguous payload of `size` bytes.
 * @return The payload, valid until vkr_spsc_ring_commit, or NULL when the
 * ring is full or the record is larger than vkr_spsc_ring_max_payload.
 * A failed reservation leaves the ring unchanged.
 */
void *vkr_spsc_ring_reserve(VkrSpscRing *ring, uint32_t size);

/** Producer: publishes the open reservation to the consumer. */
void vkr_spsc_ring_commit(VkrSpscRing *ring);

/** Producer: copies one record in. Returns false_v when the ring is full. */
bool8_t vkr_spsc_ring_push(VkrSpscRing *ring, const void *payload,
                           uint32_t size);

/**
 * @brief Consumer: returns the oldest record without removing it.
 * @param out_size Receives the payload size.
 * @return The payload, valid until vkr_spsc_ring_pop, or NULL when empty.
 */
const void *vkr_spsc_ring_peek(VkrSpscRing *ring, uint32_t *out_size);

/** Consumer: removes the record the last peek returned. */
void vkr_spsc_ring_pop(VkrSpscRing *ring);

/** Bytes in use, padding included. Exact only on the consumer thread. */
uint64_t vkr_spsc_ring_used_bytes(VkrSpscRing *ring);
