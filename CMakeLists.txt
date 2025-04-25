cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer)

# Set explicit C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Add sanitizer flags for debug builds
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=address -fno-omit-frame-pointer -g")
# Disable implicit function declaration warnings
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-implicit-function-declaration")

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(app)
add_subdirectory(tests)
