cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

# Discover all source files in lib/src
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h)

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})

# Set include directories
target_include_directories(renderer_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_precompile_headers(renderer_lib PRIVATE src/pch.h)

# Set up installation rules if needed
# install(TARGETS renderer_lib DESTINATION lib)
# install(DIRECTORY src/ DESTINATION include FILES_MATCHING PATTERN "*.h") 