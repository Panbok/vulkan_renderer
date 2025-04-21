cmake_minimum_required(VERSION 3.13)

project(opengl_renderer)

file(GLOB_RECURSE SOURCE_FILES src/*.c src/*.h)

set(CMAKE_C_STANDARD 23) # Enable the C23 standard
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")

add_executable(opengl_renderer ${SOURCE_FILES})

