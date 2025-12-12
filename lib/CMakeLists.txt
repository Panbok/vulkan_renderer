cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

if(APPLE)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h src/*.m)
enable_language(C OBJC)
elseif(WIN32)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h)
endif()

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})

# Platform-specific optimization flags
if(MSVC)
    target_compile_options(renderer_lib PRIVATE /arch:AVX2)
else()
    target_compile_options(renderer_lib PRIVATE -march=native)
endif()

# Find Vulkan package
find_package(Vulkan REQUIRED)

# Set include directories
target_include_directories(renderer_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC ${CMAKE_SOURCE_DIR}/vendor
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

# Link Vulkan - use modern target if available, otherwise use libraries directly
if(TARGET Vulkan::Vulkan)
    target_link_libraries(renderer_lib PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(renderer_lib PRIVATE ${Vulkan_LIBRARIES})
endif()

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
