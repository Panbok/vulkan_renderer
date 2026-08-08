cmake_minimum_required(VERSION 3.27)

project(vkr_tools)

find_package(Vulkan REQUIRED)

add_library(vkr_harness_core STATIC
    harness/vkr_harness_camera.c
    harness/vkr_harness_capture.c
    harness/vkr_harness_common.c
    harness/vkr_harness_compare.c
    harness/vkr_harness_artifact.c
    harness/vkr_harness_baseline.c
    harness/vkr_harness_fingerprint.c
    harness/vkr_harness_json.c
    harness/vkr_harness_manifest.c
    harness/vkr_harness_path.c
    harness/vkr_harness_report.c
    harness/vkr_harness_replay.c
    harness/vkr_harness_scene_manifest.c
    harness/vkr_harness_sha256.c
    harness/vkr_harness_statistics.c
)
vkr_require_declared_c_functions(vkr_harness_core)

target_include_directories(vkr_harness_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/harness
    PRIVATE
        ${CMAKE_SOURCE_DIR}/lib/src
)

target_link_libraries(vkr_harness_core PUBLIC renderer_lib)

if(NOT WIN32)
    target_link_libraries(vkr_harness_core PUBLIC m)
endif()

# The parent orchestrator, the child repetition runtime, and their shared
# sample/provenance seam. Only the child opens a renderer target.
add_executable(vkr_harness
    harness/vkr_harness_main.c
    harness/vkr_harness_autotest.c
    harness/vkr_harness_parent.c
    harness/vkr_harness_child.c
    harness/vkr_harness_provenance.c
    harness/vkr_harness_samples.c
    harness/vkr_harness_snapshot.c
)
vkr_require_declared_c_functions(vkr_harness)

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
vkr_require_declared_c_functions(vkr_vkt_packer)

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

# Stage V0 of the bindless Vulkan design is deliberately independent of
# renderer_lib: building the spike must not rebuild legacy production shaders or
# pull the shipping backend into its capability result.
find_program(VKR_V0_SLANGC_EXECUTABLE NAMES slangc REQUIRED)
find_program(VKR_V0_SPIRV_VAL_EXECUTABLE NAMES spirv-val)
set(VKR_V0_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/bindless_vulkan_v0")
set(VKR_V0_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bindless_vulkan_v0")
set(VKR_V0_VERTEX_SPV "${VKR_V0_OUTPUT_DIR}/v0.vert.spv")
set(VKR_V0_FRAGMENT_SPV "${VKR_V0_OUTPUT_DIR}/v0.frag.spv")

function(vkr_add_v0_shader output entry stage)
    set(_VKR_V0_COMMAND
        "${VKR_V0_SLANGC_EXECUTABLE}"
        -target spirv
        -profile spirv_1_6
        -fvk-use-entrypoint-name
        -fvk-use-scalar-layout
        -entry "${entry}"
        -stage "${stage}"
        "${VKR_V0_SOURCE_DIR}/v0.slang"
        -o "${output}"
    )
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${VKR_V0_OUTPUT_DIR}"
        COMMAND ${_VKR_V0_COMMAND}
        DEPENDS "${VKR_V0_SOURCE_DIR}/v0.slang"
        COMMENT "Compiling bindless Vulkan V0 ${stage} shader"
        VERBATIM
    )
    if(VKR_V0_SPIRV_VAL_EXECUTABLE)
        add_custom_command(
            OUTPUT "${output}.validated"
            COMMAND "${VKR_V0_SPIRV_VAL_EXECUTABLE}"
                    --target-env vulkan1.4
                    --scalar-block-layout
                    "${output}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${output}.validated"
            DEPENDS "${output}"
            COMMENT "Validating bindless Vulkan V0 ${stage} SPIR-V"
            VERBATIM
        )
        set_property(GLOBAL APPEND PROPERTY VKR_V0_VALIDATED_OUTPUTS
                     "${output}.validated")
    endif()
endfunction()

vkr_add_v0_shader("${VKR_V0_VERTEX_SPV}" vert_main vertex)
vkr_add_v0_shader("${VKR_V0_FRAGMENT_SPV}" frag_main fragment)
get_property(VKR_V0_VALIDATED_OUTPUTS GLOBAL PROPERTY VKR_V0_VALIDATED_OUTPUTS)
if(VKR_V0_SPIRV_VAL_EXECUTABLE)
    set(VKR_V0_SHADER_DEPENDENCIES ${VKR_V0_VALIDATED_OUTPUTS})
else()
    set(VKR_V0_SHADER_DEPENDENCIES
        "${VKR_V0_VERTEX_SPV}"
        "${VKR_V0_FRAGMENT_SPV}")
endif()
add_custom_target(vkr_bindless_vulkan_v0_shaders
    DEPENDS ${VKR_V0_SHADER_DEPENDENCIES})

add_executable(vkr_bindless_vulkan_v0
    bindless_vulkan_v0/vkr_bindless_vulkan_v0.c
    ${CMAKE_SOURCE_DIR}/vendor/spirv_reflect.c
)
vkr_require_declared_c_functions(vkr_bindless_vulkan_v0)
add_dependencies(vkr_bindless_vulkan_v0 vkr_bindless_vulkan_v0_shaders)
target_compile_definitions(vkr_bindless_vulkan_v0 PRIVATE
    SPIRV_REFLECT_USE_SYSTEM_SPIRV_H
    _CRT_SECURE_NO_WARNINGS
    VKR_V0_VERTEX_SPV="${VKR_V0_VERTEX_SPV}"
    VKR_V0_FRAGMENT_SPV="${VKR_V0_FRAGMENT_SPV}"
)
if(WIN32)
    target_compile_definitions(vkr_bindless_vulkan_v0 PRIVATE
        VK_USE_PLATFORM_WIN32_KHR)
endif()
target_include_directories(vkr_bindless_vulkan_v0 PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${Vulkan_INCLUDE_DIRS}
)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(vkr_bindless_vulkan_v0 PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(vkr_bindless_vulkan_v0 PRIVATE ${Vulkan_LIBRARIES})
endif()
