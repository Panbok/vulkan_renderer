cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

# Discover all source files in lib/src (including Objective-C .m files)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h src/*.m)

# Enable Objective-C for the project
enable_language(C OBJC)

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})

# Set include directories
target_include_directories(renderer_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Set platform-specific definitions and link libraries
if(APPLE)
    target_compile_definitions(renderer_lib PRIVATE PLATFORM_APPLE=1)
    # Link Cocoa framework if needed for macOS-specific functionality
    find_library(COCOA_LIBRARY Cocoa)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    target_link_libraries(renderer_lib PRIVATE ${COCOA_LIBRARY} ${FOUNDATION_LIBRARY} ${QUARTZCORE_LIBRARY})
endif()

target_precompile_headers(renderer_lib PRIVATE src/pch.h)

# Set up installation rules if needed
# install(TARGETS renderer_lib DESTINATION lib)
# install(DIRECTORY src/ DESTINATION include FILES_MATCHING PATTERN "*.h") 