#pragma once

#include "memory/vkr_allocator.h"

#include <assert.h>

/* Allocator statistics exist only when the libraries record them. Release and
 * RelWithDebInfo libraries define VKR_ALLOCATOR_DISABLE_STATS, and the tester
 * receives the same setting, so statistics assertions are type-checked but
 * not evaluated there. */
#if VKR_ALLOCATOR_DISABLE_STATS
#define VKR_TEST_ASSERT_STATS(expr) ((void)sizeof((expr) ? 1 : 0))
#else
#define VKR_TEST_ASSERT_STATS(expr) assert(expr)
#endif
