#pragma once

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(PLATFORM_APPLE)
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define SIMD_ARM_NEON 1
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_IX86_FP)
#define SIMD_X86_SSE 1
#include <emmintrin.h>
#if defined(__SSE4_1__)
#define SIMD_X86_SSE4 1
#include <smmintrin.h>
#endif
#endif