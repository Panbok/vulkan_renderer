# Discover all source files in tests/src
file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.h)

# Create the test executable
add_executable(opengl_renderer_tester ${TEST_SOURCES})

# Link against the renderer_lib
target_link_libraries(opengl_renderer_tester PRIVATE renderer_lib)

# Include directories
target_include_directories(opengl_renderer_tester
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_SOURCE_DIR}/lib/src
    # Include app source directory to test app-specific code
    PRIVATE ${CMAKE_SOURCE_DIR}/app/src
)

# Add tests if using CTest
# enable_testing()
# add_test(NAME RendererTests COMMAND opengl_renderer_tester) 