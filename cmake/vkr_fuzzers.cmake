# Fuzzers of the network transport and the asset depot (tests/fuzz, ADR-105
# "Security"). Enable them only in a separate build tree configured by
# tools/checks/run_fuzzers.py: every target of that tree compiles with
# ASan, UBSan and coverage feedback, so the libraries the fuzzers link carry
# the same instrumentation.
#
# The engine is the compiler's own libFuzzer (-fsanitize=fuzzer) when it has
# one. Apple clang has none, so it instruments with -fsanitize-coverage and
# links the libFuzzer archive of an LLVM install (VKR_FUZZ_LIBFUZZER_LIBRARY,
# found under Homebrew by default); on macOS 26 Homebrew LLVM 20's own ASan
# runtime deadlocks at start-up, while Apple's works. Without either, each
# fuzzer builds as a replay driver that runs input files once.
option(VKR_BUILD_FUZZERS "Build the transport and depot fuzzers (separate tree)" OFF)
if(NOT VKR_BUILD_FUZZERS)
    return()
endif()

if(MSVC OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "VKR_BUILD_FUZZERS needs the Clang driver with ASan and UBSan runtimes.")
endif()

include(CheckCSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer,address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer,address,undefined")
check_c_source_compiles("
#include <stddef.h>
#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  return data == 0 ? (int)size : 0;
}" VKR_FUZZ_HAVE_LIBFUZZER)
cmake_pop_check_state()

set(VKR_FUZZ_LINK_OPTIONS "")
if(VKR_FUZZ_HAVE_LIBFUZZER)
    set(VKR_FUZZ_ENGINE libfuzzer)
    add_compile_options(-fsanitize=fuzzer-no-link)
    set(VKR_FUZZ_LINK_OPTIONS -fsanitize=fuzzer)
else()
    if(APPLE AND NOT VKR_FUZZ_LIBFUZZER_LIBRARY)
        file(GLOB _vkr_fuzz_archives
             "/opt/homebrew/opt/llvm*/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a"
             "/usr/local/opt/llvm*/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a")
        if(_vkr_fuzz_archives)
            list(SORT _vkr_fuzz_archives COMPARE NATURAL ORDER DESCENDING)
            list(GET _vkr_fuzz_archives 0 _vkr_fuzz_archive)
            set(VKR_FUZZ_LIBFUZZER_LIBRARY "${_vkr_fuzz_archive}" CACHE FILEPATH
                "libFuzzer archive for a compiler without -fsanitize=fuzzer")
        endif()
        unset(_vkr_fuzz_archives)
        unset(_vkr_fuzz_archive)
    endif()
    if(VKR_FUZZ_LIBFUZZER_LIBRARY)
        set(VKR_FUZZ_ENGINE libfuzzer)
        add_compile_options(
            -fsanitize-coverage=inline-8bit-counters,pc-table,trace-cmp)
        # libFuzzer finds LLVMFuzzerInitialize with dlsym on Darwin; keep
        # ThinLTO from internalizing it.
        set(VKR_FUZZ_LINK_OPTIONS "${VKR_FUZZ_LIBFUZZER_LIBRARY}" -lc++
            -Wl,-export_dynamic)
    else()
        set(VKR_FUZZ_ENGINE replay)
    endif()
endif()
# Every target: an undefined-behaviour report must stop the run like a crash.
add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                    -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=address,undefined)
message(STATUS "VKR fuzzers: ${VKR_FUZZ_ENGINE} engine with ASan and UBSan "
               "${VKR_FUZZ_LIBFUZZER_LIBRARY}")
