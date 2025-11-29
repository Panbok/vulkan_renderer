#include "core/vkr_atomic.h"

#define VKR_ATOMIC_DEFINE_INT_FUNCS(TYPE, ATOMIC_TYPE, NAME)                  \
  void vkr_atomic_##NAME##_store(ATOMIC_TYPE *obj, TYPE value,                \
                                 VkrMemoryOrder order) {                      \
    atomic_store_explicit(obj, value, order);                                 \
  }                                                                           \
                                                                              \
  TYPE vkr_atomic_##NAME##_load(const ATOMIC_TYPE *obj,                       \
                                VkrMemoryOrder order) {                       \
    return atomic_load_explicit(obj, order);                                  \
  }                                                                           \
                                                                              \
  TYPE vkr_atomic_##NAME##_exchange(ATOMIC_TYPE *obj, TYPE desired,           \
                                    VkrMemoryOrder order) {                   \
    return atomic_exchange_explicit(obj, desired, order);                     \
  }                                                                           \
                                                                              \
  TYPE vkr_atomic_##NAME##_fetch_add(ATOMIC_TYPE *obj, TYPE value,            \
                                     VkrMemoryOrder order) {                  \
    return atomic_fetch_add_explicit(obj, value, order);                      \
  }                                                                           \
                                                                              \
  TYPE vkr_atomic_##NAME##_fetch_sub(ATOMIC_TYPE *obj, TYPE value,            \
                                     VkrMemoryOrder order) {                  \
    return atomic_fetch_sub_explicit(obj, value, order);                      \
  }                                                                           \
                                                                              \
  bool32_t vkr_atomic_##NAME##_compare_exchange(                              \
      ATOMIC_TYPE *obj, TYPE *expected, TYPE desired,                         \
      VkrMemoryOrder success_order, VkrMemoryOrder failure_order) {           \
    return atomic_compare_exchange_strong_explicit(                           \
        obj, expected, desired, success_order, failure_order);                \
  }

void vkr_atomic_bool_store(VkrAtomicBool *obj, bool32_t value,
                           VkrMemoryOrder order) {
  atomic_store_explicit(obj, value, order);
}

bool32_t vkr_atomic_bool_load(const VkrAtomicBool *obj, VkrMemoryOrder order) {
  return atomic_load_explicit(obj, order);
}

bool32_t vkr_atomic_bool_exchange(VkrAtomicBool *obj, bool32_t desired,
                                  VkrMemoryOrder order) {
  return atomic_exchange_explicit(obj, desired, order);
}

bool32_t vkr_atomic_bool_compare_exchange(VkrAtomicBool *obj,
                                          bool32_t *expected,
                                          bool32_t desired,
                                          VkrMemoryOrder success_order,
                                          VkrMemoryOrder failure_order) {
  return atomic_compare_exchange_strong_explicit(
      obj, expected, desired, success_order, failure_order);
}

VKR_ATOMIC_DEFINE_INT_FUNCS(int32_t, VkrAtomicInt32, int32)
VKR_ATOMIC_DEFINE_INT_FUNCS(uint32_t, VkrAtomicUint32, uint32)
VKR_ATOMIC_DEFINE_INT_FUNCS(int64_t, VkrAtomicInt64, int64)
VKR_ATOMIC_DEFINE_INT_FUNCS(uint64_t, VkrAtomicUint64, uint64)

#undef VKR_ATOMIC_DEFINE_INT_FUNCS
