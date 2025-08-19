cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer_tester)

# Discover all source files in tests/src
# Note: GLOB_RECURSE includes headers, which is fine but unnecessary for compilation
file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.h)

# Create the test executable
add_executable(vulkan_renderer_tester ${TEST_SOURCES})

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_property(TARGET vulkan_renderer_tester PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endif()
if(CMAKE_GENERATOR MATCHES "Xcode")
    set_property(TARGET vulkan_renderer_tester PROPERTY XCODE_ATTRIBUTE_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endif()

if(MSVC)
    target_compile_options(vulkan_renderer_tester PRIVATE /arch:AVX2)
else()
    target_compile_options(vulkan_renderer_tester PRIVATE -march=native)
endif()

# Link against the renderer_lib
target_link_libraries(vulkan_renderer_tester PRIVATE renderer_lib)

# Include directories
target_include_directories(vulkan_renderer_tester
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src # For test headers (e.g., arena_test.h)
    PRIVATE ${CMAKE_SOURCE_DIR}/lib/src     # For library headers (e.g., arena.h)
    # Include app source directory if tests need app-specific code
    # PRIVATE ${CMAKE_SOURCE_DIR}/app/src
)