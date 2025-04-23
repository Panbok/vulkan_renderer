cmake_minimum_required(VERSION 3.13)

project(opengl_renderer)

# Discover all source files in src
file(GLOB_RECURSE ALL_SOURCES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
# Add -g for debug symbols, essential for ASan
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")

# Create source list for the main application (all sources except *_test.c files)
set(APP_SOURCES ${ALL_SOURCES})
list(FILTER APP_SOURCES EXCLUDE REGEX "_test\\.c$") # Exclude files ending in _test.c

# Create source list for the test executable (all sources except the main app file)
set(TEST_SOURCES ${ALL_SOURCES})
list(FILTER TEST_SOURCES EXCLUDE REGEX "/main\\.c$") # Exclude main.c

# Main application executable
add_executable(opengl_renderer ${APP_SOURCES})

# Test executable
add_executable(opengl_renderer_tester ${TEST_SOURCES})
