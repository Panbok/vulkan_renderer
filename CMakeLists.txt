cmake_minimum_required(VERSION 3.27)

# The Windows wrappers use the installed vcpkg dependencies. The toolchain must
# be selected before project() so package wrappers supply transitive libraries.
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND
   NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE} AND DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VKR_VCPKG_ROOT)
    set(CMAKE_TOOLCHAIN_FILE "${VKR_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "CMake toolchain file")
    if(NOT DEFINED VCPKG_TARGET_TRIPLET)
        set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg target triplet")
    endif()
    unset(VKR_VCPKG_ROOT)
endif()

project(vulkan_renderer)

# Match the CRT used by the selected vcpkg Windows triplet. CMake otherwise
# defaults to the DLL CRT, which cannot link static-CRT font dependencies.
if(WIN32 AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND
   VCPKG_TARGET_TRIPLET MATCHES "-windows-static$")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Set explicit C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/")

option(VKR_ENABLE_SANITIZERS
       "Enable ASan/UBSan in every configuration (Clang/GCC only)" OFF)
set(_VKR_DEBUG_SANITIZERS_DEFAULT ON)
if(WIN32)
    # Clang ASan is incompatible with the Windows Debug CRT, and MSVC does not
    # provide the requested ASan+UBSan pair.
    set(_VKR_DEBUG_SANITIZERS_DEFAULT OFF)
endif()
option(VKR_ENABLE_DEBUG_SANITIZERS
       "Enable ASan/UBSan in Debug configurations (Clang/GCC only)"
       ${_VKR_DEBUG_SANITIZERS_DEFAULT})
unset(_VKR_DEBUG_SANITIZERS_DEFAULT)
option(VKR_EDITOR_LOGGING "Capture all editor log levels with runtime filtering" OFF)
option(VKR_METRICS_ENABLED "Compile renderer metrics instrumentation" ON)

if(VKR_METRICS_ENABLED)
    add_compile_definitions(VKR_METRICS_ENABLED=1)
else()
    add_compile_definitions(VKR_METRICS_ENABLED=0)
endif()

if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Zi -W4 /FC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /arch:AVX2")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
endif()

# An undeclared C function is an ABI bug, not a tolerable warning: the compiler
# otherwise assumes a signature that can bind to an unrelated same-named symbol
# and fail only at runtime. Apply this to VKR-owned targets explicitly so the
# policy does not leak into third-party subdirectories.
function(vkr_require_declared_c_functions target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/we4013>
        )
    elseif(CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C,OBJC>:-Werror=implicit-function-declaration>
        )
    endif()
endfunction()

# Cooking is batch work that produces an artifact; it is diagnosed from that
# artifact, not by single-stepping an encoder, so a Debug tree loses nothing by
# optimizing it, and an unoptimized cooker is unusable -- an unoptimized Charlie
# fit runs for tens of minutes and an unoptimized Basis encode costs 5x. Apply
# this to every cooking target and to the third-party codecs those targets
# drive, so one unoptimized library cannot dominate a Bakery job.
#
# Optimization level alone cannot change an IEEE result, so this leaves every
# cooked artifact byte-identical to what an unoptimized tree produced.
function(vkr_configure_cooker_target target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /O2)
    else()
        target_compile_options(${target} PRIVATE -O2)
    endif()
endfunction()

# The immutable BRDF tables are checked into the tree and read by both backends,
# so their cookers additionally pin floating point to unfused, source-order
# evaluation: no optimization level may move a rounding boundary in a committed
# table. Asset cooking deliberately does not take this -- suppressing FMA there
# would rewrite every cooked mesh and generated texture without making the
# result more correct.
function(vkr_configure_table_cooker_target target)
    vkr_configure_cooker_target(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /fp:precise)
    else()
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
endfunction()

# Same policy for third-party cooking dependencies, which own their warning and
# language settings and must not inherit VKR's.
function(vkr_configure_cooker_dependencies)
    foreach(target ${ARGV})
        if(TARGET ${target})
            get_target_property(imported ${target} IMPORTED)
            get_target_property(aliased ${target} ALIASED_TARGET)
            if(NOT imported AND NOT aliased)
                vkr_configure_cooker_target(${target})
            endif()
        endif()
    endforeach()
endfunction()

# Windows reserves 1 MiB of main-thread stack by default. An unoptimized
# FidelityFX dispatch alone needs over 300 KiB in two frames
# (fsr3upscalerDispatch reserves 232 KiB for its by-value job state and
# scheduleDispatch a further 77 KiB), which overflows that default once the
# renderer's own Debug frames sit below it. Reserve the 8 MiB the macOS main
# thread already provides, so Debug stack headroom does not depend on the host.
# This reserves address space; pages still commit on demand.
function(vkr_set_main_stack_reserve target)
    if(NOT WIN32)
        return()
    endif()
    if(MSVC)
        target_link_options(${target} PRIVATE "/STACK:8388608")
    else()
        target_link_options(${target} PRIVATE "-Xlinker" "/STACK:8388608")
    endif()
endfunction()

if((VKR_ENABLE_SANITIZERS OR VKR_ENABLE_DEBUG_SANITIZERS) AND NOT MSVC)
    set(_VKR_SAN_FLAGS "-fsanitize=address,undefined" "-fno-omit-frame-pointer")

    if(VKR_ENABLE_SANITIZERS)
        add_compile_options(${_VKR_SAN_FLAGS})
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "ASan/UBSan enabled for every configuration")
    else()
        add_compile_options(
            "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        )
        add_link_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
        if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(STATUS "ASan/UBSan enabled for Debug configurations")
        else()
            message(STATUS "ASan/UBSan inactive for ${CMAKE_BUILD_TYPE}; enabled by default for Debug")
        endif()
    endif()

    # Windows note: ASan generally doesn't work with the MSVC *Debug* CRT.
    # Use RelWithDebInfo (or Release) when VKR_ENABLE_SANITIZERS=ON.
    if(WIN32 AND (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
        if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(FATAL_ERROR "ASan/UBSan on Windows+Clang is not supported with the Debug CRT. Disable VKR_ENABLE_DEBUG_SANITIZERS or use VKR_ENABLE_SANITIZERS=ON with RelWithDebInfo.")
        endif()
        if(VCPKG_TARGET_TRIPLET MATCHES "-windows-static$")
            message(FATAL_ERROR "Windows Clang sanitizers require the DLL CRT. Use a vcpkg triplet with the DLL CRT, such as x64-windows-static-md, and install FreeType for that triplet.")
        endif()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
elseif((VKR_ENABLE_SANITIZERS OR VKR_ENABLE_DEBUG_SANITIZERS) AND MSVC)
    message(WARNING "ASan/UBSan requested but this project only enables the pair with Clang/GCC")
endif()
unset(_VKR_SAN_FLAGS)

# Applications and authoring tools are optional consumers of the libraries.
option(VKR_BUILD_EXAMPLES "Build public library client examples" OFF)
option(VKR_BUILD_RUNTIME "Build application and scene runtime libraries" ON)
option(VKR_BUILD_TOOLS "Build offline asset cookers and diagnostics" ON)
option(VKR_BUILD_APP "Build the sample application" ${VKR_BUILD_RUNTIME})
option(VKR_BUILD_EDITOR "Build the editor and Bakery" ${VKR_BUILD_RUNTIME})
option(VKR_BUILD_HARNESS "Build the renderer harness" ${VKR_BUILD_RUNTIME})
option(VKR_BUILD_TESTS "Build the CPU test executable" ${VKR_BUILD_HARNESS})
if((VKR_BUILD_APP OR VKR_BUILD_EDITOR OR VKR_BUILD_HARNESS OR VKR_BUILD_TESTS)
   AND NOT VKR_BUILD_RUNTIME)
    message(FATAL_ERROR "App, editor, harness and tests require VKR_BUILD_RUNTIME")
endif()
if(VKR_BUILD_EDITOR AND (NOT VKR_BUILD_TOOLS OR NOT VKR_BUILD_HARNESS))
    message(FATAL_ERROR "The editor's Bakery requires VKR_BUILD_TOOLS and VKR_BUILD_HARNESS; disable VKR_BUILD_EDITOR for a game build")
endif()
if(VKR_BUILD_TESTS AND (NOT VKR_BUILD_HARNESS OR NOT VKR_BUILD_TOOLS))
    message(FATAL_ERROR "CPU tests require VKR_BUILD_HARNESS and VKR_BUILD_TOOLS")
endif()
include(cmake/vkr_library_target.cmake)
add_subdirectory(lib)
add_subdirectory(renderer)
if(VKR_BUILD_RUNTIME OR VKR_BUILD_TOOLS)
    include(cmake/vkr_asset_targets.cmake)
endif()
if(VKR_BUILD_RUNTIME)
    add_subdirectory(runtime)
endif()
if(VKR_BUILD_TOOLS OR VKR_BUILD_HARNESS)
    add_subdirectory(tools)
endif()
if(VKR_BUILD_APP)
    add_subdirectory(app)
endif()
if(VKR_BUILD_EDITOR)
    add_subdirectory(editor)
endif()
if(VKR_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(VKR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
