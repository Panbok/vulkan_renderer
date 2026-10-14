#pragma once

#include "defines.h"

typedef int32_t (*VkrSortCompare)(const void *lhs, const void *rhs);

/**
 * Sorts `count` fixed-size records in ascending comparator order.
 *
 * This is the renderer-library boundary over the platform C runtime sort.
 * It performs no allocation; the comparator must establish a total order.
 */
void vkr_sort(void *records, uint64_t count, uint64_t record_size,
              VkrSortCompare compare);

/** Finds a record equal to `key` in `records`, sorted by `compare`; NULL when
 * none is. `compare` receives `key` first, as bsearch passes it. */
void *vkr_binary_search(const void *key, const void *records, uint64_t count,
                        uint64_t record_size, VkrSortCompare compare);
