cmake_minimum_required(VERSION 3.27)

project(vkr_sample_runtime)

add_library(vkr_sample_runtime STATIC
    src/vkr_sample_runtime.c
)
vkr_require_declared_c_functions(vkr_sample_runtime)

if(MSVC)
    target_compile_options(vkr_sample_runtime PRIVATE /arch:AVX2)
else()
    target_compile_options(vkr_sample_runtime PRIVATE -march=native)
endif()

target_link_libraries(vkr_sample_runtime PUBLIC renderer_lib)
target_include_directories(vkr_sample_runtime PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_definitions(vkr_sample_runtime PRIVATE
    $<$<CONFIG:Release>:LOG_LEVEL=1>
    $<$<CONFIG:Release>:ASSERT_LOG=0>
    $<$<CONFIG:RelWithDebInfo>:LOG_LEVEL=3>
    $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
    $<$<CONFIG:Debug>:LOG_LEVEL=4>
    $<$<CONFIG:Debug>:ASSERT_LOG=1>
)
