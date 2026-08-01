#include "containers/vkr_sort.h"

void vkr_sort(void *records, uint64_t count, uint64_t record_size,
              VkrSortCompare compare) {
  if (!records || count < 2u || record_size == 0u || !compare) {
    return;
  }
  qsort(records, (size_t)count, (size_t)record_size,
        (int (*)(const void *, const void *))compare);
}
