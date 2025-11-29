/**
 * @file vkr_atomic.h
 * @brief Thin wrappers over C11 atomics for common types.
 */
#pragma once

#include "defines.h"

typedef memory_order VkrMemoryOrder;

#define VKR_MEMORY_ORDER_RELAXED memory_order_relaxed
#define VKR_MEMORY_ORDER_CONSUME memory_order_consume
#define VKR_MEMORY_ORDER_ACQUIRE memory_order_acquire
#define VKR_MEMORY_ORDER_RELEASE memory_order_release
#define VKR_MEMORY_ORDER_ACQ_REL memory_order_acq_rel
#define VKR_MEMORY_ORDER_SEQ_CST memory_order_seq_cst

typedef _Atomic(bool32_t) VkrAtomicBool;
typedef _Atomic(int32_t) VkrAtomicInt32;
typedef _Atomic(uint32_t) VkrAtomicUint32;
typedef _Atomic(int64_t) VkrAtomicInt64;
typedef _Atomic(uint64_t) VkrAtomicUint64;

/**
 * @brief Store a value into an atomic boolean.
 * @param obj The atomic boolean to store the value into.
 * @param value The value to store.
 * @param order The memory order to use.
 */
void vkr_atomic_bool_store(VkrAtomicBool *obj, bool32_t value,
                           VkrMemoryOrder order);

/**
 * @brief Load a value from an atomic boolean.
 * @param obj The atomic boolean to load the value from.
 * @param order The memory order to use.
 * @return The loaded value.
 */
bool32_t vkr_atomic_bool_load(const VkrAtomicBool *obj, VkrMemoryOrder order);

/**
 * @brief Exchange a value in an atomic boolean.
 * @param obj The atomic boolean to exchange the value in.
 * @param desired The desired value.
 * @param order The memory order to use.
 * @return The exchanged value.
 */
bool32_t vkr_atomic_bool_exchange(VkrAtomicBool *obj, bool32_t desired,
                                  VkrMemoryOrder order);

/**
 * @brief Compare and exchange a value in an atomic boolean.
 * @param obj The atomic boolean to compare and exchange the value in.
 * @param expected The expected value.
 * @param desired The desired value.
 * @param success_order The memory order to use for the success case.
 * @param failure_order The memory order to use for the failure case.
 * @return True if the exchange was successful, false otherwise.
 */
bool32_t vkr_atomic_bool_compare_exchange(VkrAtomicBool *obj,
                                          bool32_t *expected, bool32_t desired,
                                          VkrMemoryOrder success_order,
                                          VkrMemoryOrder failure_order);

/**
 * @brief Store a value into an atomic int32.
 * @param obj The atomic int32 to store the value into.
 * @param value The value to store.
 * @param order The memory order to use.
 */
void vkr_atomic_int32_store(VkrAtomicInt32 *obj, int32_t value,
                            VkrMemoryOrder order);

/**
 * @brief Load a value from an atomic int32.
 * @param obj The atomic int32 to load the value from.
 * @param order The memory order to use.
 * @return The loaded value.
 */
int32_t vkr_atomic_int32_load(const VkrAtomicInt32 *obj, VkrMemoryOrder order);

/**
 * @brief Exchange a value in an atomic int32.
 * @param obj The atomic int32 to exchange the value in.
 * @param desired The desired value.
 * @param order The memory order to use.
 * @return The exchanged value.
 */
int32_t vkr_atomic_int32_exchange(VkrAtomicInt32 *obj, int32_t desired,
                                  VkrMemoryOrder order);

/**
 * @brief Fetch and add a value to an atomic int32.
 * @param obj The atomic int32 to fetch and add the value to.
 * @param value The value to add.
 * @param order The memory order to use.
 * @return The previous value.
 */
int32_t vkr_atomic_int32_fetch_add(VkrAtomicInt32 *obj, int32_t value,
                                   VkrMemoryOrder order);

/**
 * @brief Fetch and subtract a value from an atomic int32.
 * @param obj The atomic int32 to fetch and subtract the value from.
 * @param value The value to subtract.
 * @param order The memory order to use.
 * @return The previous value.
 */
int32_t vkr_atomic_int32_fetch_sub(VkrAtomicInt32 *obj, int32_t value,
                                   VkrMemoryOrder order);

/**
 * @brief Compare and exchange a value in an atomic int32.
 * @param obj The atomic int32 to compare and exchange the value in.
 * @param expected The expected value.
 * @param desired The desired value.
 * @param success_order The memory order to use for the success case.
 * @param failure_order The memory order to use for the failure case.
 * @return True if the exchange was successful, false otherwise.
 */
bool32_t vkr_atomic_int32_compare_exchange(VkrAtomicInt32 *obj,
                                           int32_t *expected, int32_t desired,
                                           VkrMemoryOrder success_order,
                                           VkrMemoryOrder failure_order);

/**
 * @brief Store a value into an atomic uint32.
 * @param obj The atomic uint32 to store the value into.
 * @param value The value to store.
 * @param order The memory order to use.
 */
void vkr_atomic_uint32_store(VkrAtomicUint32 *obj, uint32_t value,
                             VkrMemoryOrder order);

/**
 * @brief Load a value from an atomic uint32.
 * @param obj The atomic uint32 to load the value from.
 * @param order The memory order to use.
 * @return The loaded value.
 */
uint32_t vkr_atomic_uint32_load(const VkrAtomicUint32 *obj,
                                VkrMemoryOrder order);

/**
 * @brief Exchange a value in an atomic uint32.
 * @param obj The atomic uint32 to exchange the value in.
 * @param desired The desired value.
 * @param order The memory order to use.
 * @return The exchanged value.
 */
uint32_t vkr_atomic_uint32_exchange(VkrAtomicUint32 *obj, uint32_t desired,
                                    VkrMemoryOrder order);

/**
 * @brief Fetch and add a value to an atomic uint32.
 * @param obj The atomic uint32 to fetch and add the value to.
 * @param value The value to add.
 * @param order The memory order to use.
 * @return The previous value.
 */
uint32_t vkr_atomic_uint32_fetch_add(VkrAtomicUint32 *obj, uint32_t value,
                                     VkrMemoryOrder order);

/**
 * @brief Fetch and subtract a value from an atomic uint32.
 * @param obj The atomic uint32 to fetch and subtract the value from.
 * @param value The value to subtract.
 * @param order The memory order to use.
 * @return The previous value.
 */
uint32_t vkr_atomic_uint32_fetch_sub(VkrAtomicUint32 *obj, uint32_t value,
                                     VkrMemoryOrder order);

/**
 * @brief Compare and exchange a value in an atomic uint32.
 * @param obj The atomic uint32 to compare and exchange the value in.
 * @param expected The expected value.
 * @param desired The desired value.
 * @param success_order The memory order to use for the success case.
 * @param failure_order The memory order to use for the failure case.
 * @return True if the exchange was successful, false otherwise.
 */
bool32_t vkr_atomic_uint32_compare_exchange(VkrAtomicUint32 *obj,
                                            uint32_t *expected,
                                            uint32_t desired,
                                            VkrMemoryOrder success_order,
                                            VkrMemoryOrder failure_order);

/**
 * @brief Store a value into an atomic int64.
 * @param obj The atomic int64 to store the value into.
 * @param value The value to store.
 * @param order The memory order to use.
 */
void vkr_atomic_int64_store(VkrAtomicInt64 *obj, int64_t value,
                            VkrMemoryOrder order);

/**
 * @brief Load a value from an atomic int64.
 * @param obj The atomic int64 to load the value from.
 * @param order The memory order to use.
 * @return The loaded value.
 */
int64_t vkr_atomic_int64_load(const VkrAtomicInt64 *obj, VkrMemoryOrder order);

/**
 * @brief Exchange a value in an atomic int64.
 * @param obj The atomic int64 to exchange the value in.
 * @param desired The desired value.
 * @param order The memory order to use.
 * @return The exchanged value.
 */
int64_t vkr_atomic_int64_exchange(VkrAtomicInt64 *obj, int64_t desired,
                                  VkrMemoryOrder order);

/**
 * @brief Fetch and add a value to an atomic int64.
 * @param obj The atomic int64 to fetch and add the value to.
 * @param value The value to add.
 * @param order The memory order to use.
 * @return The previous value.
 */
int64_t vkr_atomic_int64_fetch_add(VkrAtomicInt64 *obj, int64_t value,
                                   VkrMemoryOrder order);

/**
 * @brief Fetch and subtract a value from an atomic int64.
 * @param obj The atomic int64 to fetch and subtract the value from.
 * @param value The value to subtract.
 * @param order The memory order to use.
 * @return The previous value.
 */
int64_t vkr_atomic_int64_fetch_sub(VkrAtomicInt64 *obj, int64_t value,
                                   VkrMemoryOrder order);

/**
 * @brief Compare and exchange a value in an atomic int64.
 * @param obj The atomic int64 to compare and exchange the value in.
 * @param expected The expected value.
 * @param desired The desired value.
 * @param success_order The memory order to use for the success case.
 * @param failure_order The memory order to use for the failure case.
 * @return True if the exchange was successful, false otherwise.
 */
bool32_t vkr_atomic_int64_compare_exchange(VkrAtomicInt64 *obj,
                                           int64_t *expected, int64_t desired,
                                           VkrMemoryOrder success_order,
                                           VkrMemoryOrder failure_order);

/**
 * @brief Store a value into an atomic uint64.
 * @param obj The atomic uint64 to store the value into.
 * @param value The value to store.
 * @param order The memory order to use.
 */
void vkr_atomic_uint64_store(VkrAtomicUint64 *obj, uint64_t value,
                             VkrMemoryOrder order);

/**
 * @brief Load a value from an atomic uint64.
 * @param obj The atomic uint64 to load the value from.
 * @param order The memory order to use.
 * @return The loaded value.
 */
uint64_t vkr_atomic_uint64_load(const VkrAtomicUint64 *obj,
                                VkrMemoryOrder order);

/**
 * @brief Exchange a value in an atomic uint64.
 * @param obj The atomic uint64 to exchange the value in.
 * @param desired The desired value.
 * @param order The memory order to use.
 * @return The exchanged value.
 */
uint64_t vkr_atomic_uint64_exchange(VkrAtomicUint64 *obj, uint64_t desired,
                                    VkrMemoryOrder order);

/**
 * @brief Fetch and add a value to an atomic uint64.
 * @param obj The atomic uint64 to fetch and add the value to.
 * @param value The value to add.
 * @param order The memory order to use.
 * @return The previous value.
 */
uint64_t vkr_atomic_uint64_fetch_add(VkrAtomicUint64 *obj, uint64_t value,
                                     VkrMemoryOrder order);

/**
 * @brief Fetch and subtract a value from an atomic uint64.
 * @param obj The atomic uint64 to fetch and subtract the value from.
 * @param value The value to subtract.
 * @param order The memory order to use.
 * @return The previous value.
 */
uint64_t vkr_atomic_uint64_fetch_sub(VkrAtomicUint64 *obj, uint64_t value,
                                     VkrMemoryOrder order);

/**
 * @brief Compare and exchange a value in an atomic uint64.
 * @param obj The atomic uint64 to compare and exchange the value in.
 * @param expected The expected value.
 * @param desired The desired value.
 * @param success_order The memory order to use for the success case.
 * @param failure_order The memory order to use for the failure case.
 * @return True if the exchange was successful, false otherwise.
 */
bool32_t vkr_atomic_uint64_compare_exchange(VkrAtomicUint64 *obj,
                                            uint64_t *expected,
                                            uint64_t desired,
                                            VkrMemoryOrder success_order,
                                            VkrMemoryOrder failure_order);
