# libsodium for the network transport (vendor/libsodium.md). libsodium ships
# autotools, MSVC and Zig builds but no CMake build, so this target compiles
# the pinned submodule's sources with the configuration its build.zig uses
# for each host. The submodule stays unmodified: the version header comes
# from builds/msvc/version.h, written into the build tree.
if(TARGET vkr_sodium)
    return()
endif()

set(_vkr_sodium_root "${CMAKE_CURRENT_LIST_DIR}/../vendor/libsodium")
set(_vkr_sodium_src "${_vkr_sodium_root}/src/libsodium")
if(NOT EXISTS "${_vkr_sodium_src}/include/sodium.h")
    message(FATAL_ERROR "vendor/libsodium is missing; run "
                        "`git submodule update --init vendor/libsodium`")
endif()

# Every C source; the assembly files serve x86-64 Unix hosts only and are
# not built (HAVE_AMD64_ASM stays undefined), as on Windows in build.zig.
file(GLOB_RECURSE _vkr_sodium_sources CONFIGURE_DEPENDS "${_vkr_sodium_src}/*.c")
add_library(vkr_sodium STATIC ${_vkr_sodium_sources})

# sodium.h includes "sodium/version.h"; the prebuilt header includes
# "export.h" relative to itself, which names the source tree's copy here.
file(READ "${_vkr_sodium_root}/builds/msvc/version.h" _vkr_sodium_version)
string(REPLACE "#include \"export.h\"" "#include \"sodium/export.h\""
       _vkr_sodium_version "${_vkr_sodium_version}")
set(_vkr_sodium_generated "${CMAKE_BINARY_DIR}/vendor/libsodium/include")
file(CONFIGURE OUTPUT "${_vkr_sodium_generated}/sodium/version.h"
     CONTENT "${_vkr_sodium_version}" @ONLY)

target_include_directories(vkr_sodium
    PUBLIC "${_vkr_sodium_generated}" "${_vkr_sodium_src}/include"
    PRIVATE "${_vkr_sodium_generated}/sodium" "${_vkr_sodium_src}/include/sodium")
target_compile_definitions(vkr_sodium
    PUBLIC SODIUM_STATIC=1
    PRIVATE CONFIGURED=1 NATIVE_LITTLE_ENDIAN=1 HAVE_STDINT_H=1
            HAVE_INTTYPES_H=1)

if(WIN32)
    # clang and cl both define _MSC_VER here, and libsodium's common.h then
    # selects the intrinsics headers itself. RtlGenRandom needs advapi32.
    target_compile_definitions(vkr_sodium PRIVATE HAVE_RAISE=1
        _CRT_SECURE_NO_WARNINGS=1)
    target_link_libraries(vkr_sodium PUBLIC advapi32)
elseif(APPLE)
    target_compile_definitions(vkr_sodium PRIVATE
        ASM_HIDE_SYMBOL=.private_extern TLS=_Thread_local
        HAVE_ATOMIC_OPS=1 HAVE_C11_MEMORY_FENCES=1 HAVE_GCC_MEMORY_FENCES=1
        HAVE_INLINE_ASM=1 HAVE_TI_MODE=1
        HAVE_ARC4RANDOM=1 HAVE_ARC4RANDOM_BUF=1 HAVE_CATCHABLE_ABRT=1
        HAVE_CATCHABLE_SEGV=1 HAVE_CLOCK_GETTIME=1 HAVE_GETENTROPY=1
        HAVE_GETPID=1 HAVE_MADVISE=1 HAVE_MEMSET_S=1 HAVE_MLOCK=1 HAVE_MMAP=1
        HAVE_MPROTECT=1 HAVE_NANOSLEEP=1 HAVE_POSIX_MEMALIGN=1 HAVE_PTHREAD=1
        HAVE_PTHREAD_PRIO_INHERIT=1 HAVE_RAISE=1 HAVE_SYSCONF=1
        HAVE_SYS_MMAN_H=1 HAVE_SYS_PARAM_H=1 HAVE_SYS_RANDOM_H=1
        HAVE_WEAK_SYMBOLS=1)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    target_compile_definitions(vkr_sodium PRIVATE HAVE_ARMCRYPTO=1)
elseif(NOT MSVC AND NOT WIN32)
    target_compile_definitions(vkr_sodium PRIVATE HAVE_CPUID=1
        HAVE_MMINTRIN_H=1 HAVE_EMMINTRIN_H=1 HAVE_PMMINTRIN_H=1
        HAVE_TMMINTRIN_H=1 HAVE_SMMINTRIN_H=1 HAVE_AVXINTRIN_H=1
        HAVE_AVX2INTRIN_H=1 HAVE_AVX512FINTRIN_H=1 HAVE_WMMINTRIN_H=1
        HAVE_RDRAND=1)
endif()

# libsodium selects AES, AVX and ARM crypto code per function at run time
# (`#pragma clang attribute` or target attributes), so it builds without the
# project's -march baseline. It relies on wrapping signed arithmetic.
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(vkr_sodium PRIVATE /O2 /w)
else()
    target_compile_options(vkr_sodium PRIVATE -O2 -w -fno-strict-aliasing
        -fwrapv -fvisibility=hidden)
endif()
set_target_properties(vkr_sodium PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

unset(_vkr_sodium_sources)
unset(_vkr_sodium_version)
unset(_vkr_sodium_generated)
unset(_vkr_sodium_src)
unset(_vkr_sodium_root)
