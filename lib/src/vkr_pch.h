#pragma once

/* libc and operating-system headers for the lib's own sources and for
 * consumers outside the renderer. Renderer translation units define
 * VKR_LIBC_BOUNDARY and see only the freestanding headers from defines.h. */
#if defined(VKR_LIBC_BOUNDARY)
#error "vkr_pch.h is outside the renderer's libc boundary"
#endif

#if defined(_MSC_VER) && !defined(NDEBUG)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#if !defined(__cplusplus)
// C11 atomic declarations conflict with the Windows C++ <atomic> declarations.
#include <stdatomic.h>
#endif
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

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
// windows.h defines `min`/`max` function-like macros that break every C++ use
// of std::min, std::max, and std::numeric_limits<T>::min(). This header is the
// project's only include site for windows.h, so suppress them here.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <io.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif
// clang-format on
