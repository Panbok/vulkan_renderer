# Discover all source files in tests/src
# Note: GLOB_RECURSE includes headers, which is fine but unnecessary for compilation
file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.h)

# Create the test executable
add_executable(vulkan_renderer_tester ${TEST_SOURCES})

# Link against the renderer_lib
target_link_libraries(vulkan_renderer_tester PRIVATE renderer_lib)

# Include directories
target_include_directories(vulkan_renderer_tester
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src # For test headers (e.g., arena_test.h)
    PRIVATE ${CMAKE_SOURCE_DIR}/lib/src     # For library headers (e.g., arena.h)
    # Include app source directory if tests need app-specific code
    # PRIVATE ${CMAKE_SOURCE_DIR}/app/src
)