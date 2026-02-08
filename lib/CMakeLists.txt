cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

if(APPLE)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h src/*.m)
enable_language(C OBJC)
elseif(WIN32)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h)
endif()
list(APPEND LIB_SOURCES ${CMAKE_SOURCE_DIR}/vendor/spirv_reflect.c)

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})

target_compile_definitions(renderer_lib PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

# Platform-specific optimization flags
if(MSVC)
    target_compile_options(renderer_lib PRIVATE /arch:AVX2)
else()
    target_compile_options(renderer_lib PRIVATE -march=native)
endif()

# Find Vulkan package
find_package(Vulkan REQUIRED)

# KTX-Software (KTX2 + BasisU transcoding)
set(VKR_KTX_SOFTWARE_DIR "${CMAKE_SOURCE_DIR}/vendor/ktx-software")
if(NOT EXISTS "${VKR_KTX_SOFTWARE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "KTX-Software submodule not found. Run: git submodule update --init --recursive")
endif()

set(KTX_FEATURE_DOC OFF CACHE BOOL "Disable KTX docs in renderer build." FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "Disable KTX JNI in renderer build." FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "Disable KTX python bindings in renderer build." FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests in renderer build." FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "Disable KTX tools in renderer build." FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX tools CTS in renderer build." FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "Disable KTX loadtest apps in renderer build." FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "Disable KTX GL upload path in renderer build." FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "Disable KTX Vulkan upload path in renderer build." FORCE)

set(_VKR_BUILD_SHARED_LIBS_PREV "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build KTX dependencies statically." FORCE)
add_subdirectory("${VKR_KTX_SOFTWARE_DIR}" "${CMAKE_BINARY_DIR}/vendor/ktx-software" EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS "${_VKR_BUILD_SHARED_LIBS_PREV}" CACHE BOOL "Restore shared library default after KTX setup." FORCE)

# Locate SPIR-V headers (needed by spirv_reflect.h when using system headers).
find_path(SPIRV_HEADERS_INCLUDE_DIR
    NAMES spirv/unified1/spirv.h
    HINTS
        /usr/local/include
        /opt/homebrew/include
        ${Vulkan_INCLUDE_DIRS}
)

if(NOT SPIRV_HEADERS_INCLUDE_DIR)
    message(FATAL_ERROR "SPIR-V headers not found (missing spirv/unified1/spirv.h). Install SPIRV-Headers or point CMake to the include path.")
endif()

# Set include directories
target_include_directories(renderer_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC ${CMAKE_SOURCE_DIR}/vendor
    PUBLIC ${SPIRV_HEADERS_INCLUDE_DIR}
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

# Link Vulkan - use modern target if available, otherwise use libraries directly
if(TARGET Vulkan::Vulkan)
    target_link_libraries(renderer_lib PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(renderer_lib PRIVATE ${Vulkan_LIBRARIES})
endif()
target_link_libraries(renderer_lib PRIVATE ktx_read)

# Set platform-specific definitions and link libraries
if(APPLE)
    target_compile_definitions(renderer_lib PRIVATE PLATFORM_APPLE=1)
    # Link Cocoa framework if needed for macOS-specific functionality
    find_library(COCOA_LIBRARY Cocoa)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(GAMECONTROLLER_LIBRARY GameController)
    target_link_libraries(renderer_lib PRIVATE ${COCOA_LIBRARY} ${FOUNDATION_LIBRARY} ${QUARTZCORE_LIBRARY} ${GAMECONTROLLER_LIBRARY})
elseif(WIN32)
    target_compile_definitions(renderer_lib PRIVATE PLATFORM_WINDOWS=1)
    target_link_libraries(renderer_lib PRIVATE user32.lib winmm.lib Xinput.lib)
endif()

# Set compile definitions for logging based on build type
target_compile_definitions(renderer_lib PRIVATE
    $<$<CONFIG:Release>:LOG_LEVEL=1>
    $<$<CONFIG:Release>:ASSERT_LOG=0>
    $<$<CONFIG:RelWithDebInfo>:LOG_LEVEL=3>
    $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
    $<$<CONFIG:Debug>:LOG_LEVEL=4>
    $<$<CONFIG:Debug>:ASSERT_LOG=1>
    $<$<CONFIG:Release>:VKR_ALLOCATOR_DISABLE_STATS=1>
    $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_DISABLE_STATS=1>
    $<$<CONFIG:Debug>:VKR_ALLOCATOR_DISABLE_STATS=0>
    $<$<CONFIG:Release>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    $<$<CONFIG:Debug>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
)

target_precompile_headers(renderer_lib PRIVATE src/vkr_pch.h)
