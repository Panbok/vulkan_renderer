#pragma once

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <stb_image.h>

#if defined(PLATFORM_APPLE)
#include <CoreGraphics/CoreGraphics.h>
#include <TargetConditionals.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

// clang-format off
#if defined(PLATFORM_WINDOWS)
#include <windows.h>
#include <windowsx.h>
#include <io.h>
#include <timeapi.h>
#include <Xinput.h>
#pragma comment(lib, "winmm.lib")
#endif
// clang-format on

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define SIMD_ARM_NEON 1
#include <arm_neon.h>
#elif defined(__AVX2__) || defined(__AVX__)
#define SIMD_X86_AVX 1
#include <immintrin.h>
#endif