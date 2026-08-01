cmake_minimum_required(VERSION 3.27)

project(vkr_tools)

find_package(Vulkan REQUIRED)

add_library(vkr_harness_core STATIC
    harness/vkr_harness_camera.c
    harness/vkr_harness_common.c
    harness/vkr_harness_artifact.c
    harness/vkr_harness_fingerprint.c
    harness/vkr_harness_json.c
    harness/vkr_harness_manifest.c
    harness/vkr_harness_path.c
    harness/vkr_harness_report.c
    harness/vkr_harness_sha256.c
    harness/vkr_harness_statistics.c
)

target_include_directories(vkr_harness_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/harness
    PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/src
)

target_link_libraries(vkr_harness_core PUBLIC renderer_lib)

if(NOT MSVC)
    target_link_libraries(vkr_harness_core PUBLIC m)
endif()

# The parent orchestrator, the child repetition runtime, and their shared
# sample/provenance seam. Only the child links the renderer.
add_executable(vkr_harness
    harness/vkr_harness_main.c
    harness/vkr_harness_parent.c
    harness/vkr_harness_child.c
    harness/vkr_harness_provenance.c
    harness/vkr_harness_samples.c
)

target_link_libraries(vkr_harness PRIVATE vkr_harness_core)
target_compile_definitions(vkr_harness PRIVATE
    VKR_HARNESS_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    VKR_HARNESS_COMPILER="${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}"
    $<$<CONFIG:Release>:LOG_LEVEL=1>
    $<$<CONFIG:Release>:ASSERT_LOG=0>
    $<$<CONFIG:RelWithDebInfo>:LOG_LEVEL=3>
    $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
    $<$<CONFIG:Debug>:LOG_LEVEL=4>
    $<$<CONFIG:Debug>:ASSERT_LOG=1>
)

add_executable(vkr_vkt_packer
    vkr_vkt_packer.cpp
)

target_compile_features(vkr_vkt_packer PRIVATE cxx_std_17)

target_include_directories(vkr_vkt_packer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor
        ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(vkr_vkt_packer
    PRIVATE
        ktx
)
