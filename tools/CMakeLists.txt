cmake_minimum_required(VERSION 3.27)

project(vkr_tools)

if(APPLE)
    enable_language(OBJC)
endif()

# Offline generator for the checked-in, shared immutable BRDF coefficients.
add_executable(vkr_dfg_cooker vkr_dfg_cooker.cpp)
target_compile_features(vkr_dfg_cooker PRIVATE cxx_std_17)
target_include_directories(vkr_dfg_cooker PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src")
set(VKR_DFG_DATA "${CMAKE_SOURCE_DIR}/renderer/src/vkr_dfg_lut_data.inc")
add_custom_command(OUTPUT "${VKR_DFG_DATA}"
    COMMAND $<TARGET_FILE:vkr_dfg_cooker> "${VKR_DFG_DATA}"
    DEPENDS vkr_dfg_cooker vkr_dfg_cooker.cpp
            "${CMAKE_SOURCE_DIR}/renderer/src/vkr_dfg_lut.h"
            "${CMAKE_SOURCE_DIR}/renderer/src/shaders/shared/dfg_contract.slangh"
    COMMENT "Cooking shared immutable GGX DFG coefficients"
    VERBATIM)
add_custom_target(vkr_dfg_data DEPENDS "${VKR_DFG_DATA}")
add_dependencies(renderer_lib vkr_dfg_data)

# Charlie energy and rectangle fits share one immutable payload in both backends.
add_executable(vkr_sheen_cooker vkr_sheen_cooker.cpp)
target_compile_features(vkr_sheen_cooker PRIVATE cxx_std_17)
target_include_directories(vkr_sheen_cooker PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src")
set(VKR_SHEEN_DATA "${CMAKE_SOURCE_DIR}/renderer/src/vkr_sheen_lut_data.inc")
add_custom_command(OUTPUT "${VKR_SHEEN_DATA}"
    COMMAND $<TARGET_FILE:vkr_sheen_cooker> "${VKR_SHEEN_DATA}"
    DEPENDS vkr_sheen_cooker vkr_sheen_cooker.cpp
            "${CMAKE_SOURCE_DIR}/renderer/src/vkr_sheen_lut.h"
    COMMENT "Cooking shared Charlie energy and rectangle coefficients"
    VERBATIM)
add_custom_target(vkr_sheen_data DEPENDS "${VKR_SHEEN_DATA}")
add_dependencies(renderer_lib vkr_sheen_data)

add_executable(vkr_anisotropy_cooker vkr_anisotropy_cooker.cpp
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_ltc_lut.c")
target_compile_features(vkr_anisotropy_cooker PRIVATE cxx_std_17)
target_include_directories(vkr_anisotropy_cooker PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src")
set(VKR_ANISOTROPY_DATA "${CMAKE_SOURCE_DIR}/renderer/src/vkr_anisotropy_lut_data.inc")
add_custom_command(OUTPUT "${VKR_ANISOTROPY_DATA}"
    COMMAND $<TARGET_FILE:vkr_anisotropy_cooker> "${VKR_ANISOTROPY_DATA}"
    DEPENDS vkr_anisotropy_cooker vkr_anisotropy_cooker.cpp
            assets/anisotropy_ltc_seed.inc
            "${CMAKE_SOURCE_DIR}/renderer/src/vkr_ltc_lut.c"
            "${CMAKE_SOURCE_DIR}/renderer/src/vkr_anisotropy_lut.h"
    COMMENT "Cooking anisotropic GGX LTC and DFG tables"
    VERBATIM)
add_custom_target(vkr_anisotropy_data DEPENDS "${VKR_ANISOTROPY_DATA}")
add_dependencies(renderer_lib vkr_anisotropy_data)

find_package(Vulkan REQUIRED)

# Capture/export consumers and asset importers share one image writer object.
add_library(vkr_image_write STATIC assets/stb_image_write_impl.c)
target_include_directories(vkr_image_write PRIVATE "${CMAKE_SOURCE_DIR}/vendor")


if(VKR_BUILD_HARNESS)
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
        ${CMAKE_SOURCE_DIR}/renderer/src
        ${CMAKE_SOURCE_DIR}/vendor
)

target_link_libraries(vkr_harness_core PUBLIC vkr_runtime PRIVATE vkr_image_decode vkr_image_write)

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

endif()

if(VKR_BUILD_TOOLS)
add_library(vkr_asset_cooking STATIC
    assets/vkr_font_encode.c
    assets/vkr_mesh_encode.c
    assets/vkr_mesh_cook_source.c
    assets/vkr_scene_light_bake.c
    assets/vkr_mesh_deduplicate.c
    assets/vkr_meshoptimizer_bridge.cpp
    assets/mesh_loader_gltf.c
    assets/vkr_gltf_material_conversion.c
    assets/cgltf_impl.c)
vkr_configure_library(vkr_asset_cooking)
target_include_directories(vkr_asset_cooking PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/vendor")
target_link_libraries(vkr_asset_cooking PUBLIC vkr_asset_formats PRIVATE meshoptimizer vkr_image_decode vkr_image_write)
target_compile_features(vkr_asset_cooking PRIVATE cxx_std_11)

add_library(vkr_vkt_cooking STATIC
    vkr_vkt_packer.cpp
)
target_compile_features(vkr_vkt_cooking PRIVATE cxx_std_17)

target_include_directories(vkr_vkt_cooking
    PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor
        ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(vkr_vkt_cooking
    PRIVATE
        ktx
        vkr_image_decode
)

target_link_libraries(vkr_asset_cooking PRIVATE vkr_vkt_cooking)

add_executable(vkr_vkt_packer
    vkr_vkt_packer_main.cpp
)
vkr_require_declared_c_functions(vkr_vkt_packer)
target_compile_features(vkr_vkt_packer PRIVATE cxx_std_17)
target_link_libraries(vkr_vkt_packer PRIVATE vkr_vkt_cooking)

add_executable(vkr_hdr_cube_packer vkr_hdr_cube_packer.cpp)
target_compile_features(vkr_hdr_cube_packer PRIVATE cxx_std_17)
target_include_directories(vkr_hdr_cube_packer PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(vkr_hdr_cube_packer PRIVATE ktx)
if(TARGET vkr_harness)
    add_dependencies(vkr_harness vkr_hdr_cube_packer)
endif()

# CPU-only indirect-lighting preparation and portable baked assets.
add_library(vkr_bake_core STATIC
    bake/vkr_bake_atmosphere.cpp
    bake/vkr_bake_scene.cpp
    bake/vkr_bake_mesh_decode.c
    bake/vkr_bake_material.cpp
    bake/vkr_bake_bsdf.cpp
    bake/vkr_bake_bvh.cpp
    bake/vkr_bake_voxels.cpp
    bake/vkr_bake_sh.cpp
    bake/vkr_bake_integrator.cpp
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_dfg_lut.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_sheen_lut.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_anisotropy_lut.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_atmosphere.c")
target_compile_features(vkr_bake_core PRIVATE cxx_std_17)
target_include_directories(vkr_bake_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src" "${CMAKE_SOURCE_DIR}/vendor" ${Vulkan_INCLUDE_DIRS})
target_link_libraries(vkr_bake_core PUBLIC vkr_asset_formats PRIVATE ktx vkr_image_decode)
add_dependencies(vkr_bake_core vkr_dfg_data vkr_sheen_data vkr_anisotropy_data)
add_executable(vkr_diffuse_baker vkr_diffuse_baker.cpp)
target_compile_features(vkr_diffuse_baker PRIVATE cxx_std_17)
target_link_libraries(vkr_diffuse_baker PRIVATE vkr_bake_core)

add_executable(vkr_mesh_cooker
    vkr_mesh_cooker.c
)
vkr_require_declared_c_functions(vkr_mesh_cooker)
target_link_libraries(vkr_mesh_cooker PRIVATE vkr_asset_cooking)

set(VKR_MSDF_ATLAS_DIR "${CMAKE_SOURCE_DIR}/vendor/msdf-atlas-gen")
set(VKR_MSDF_ATLAS_COMMIT
    "2ede254314a2512252a225fa6c975948d6af559a")
set(VKR_MSDFGEN_COMMIT
    "1874bcf7d9624ccc85b4bc9a85d78116f690f35b")
if(NOT EXISTS "${VKR_MSDF_ATLAS_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${VKR_MSDF_ATLAS_DIR}/msdfgen/CMakeLists.txt")
    message(FATAL_ERROR
        "vendor/msdf-atlas-gen is missing. Run: git submodule update --init --recursive")
endif()

function(vkr_require_clean_pinned_checkout label directory expected_commit)
    execute_process(
        COMMAND git -C "${directory}" rev-parse HEAD
        OUTPUT_VARIABLE actual_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result
        ERROR_QUIET)
    execute_process(
        COMMAND git -C "${directory}" status --porcelain=v1
                --untracked-files=all
        OUTPUT_VARIABLE dirty_status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE status_result
        ERROR_QUIET)
    if(NOT git_result EQUAL 0 OR NOT status_result EQUAL 0 OR
       NOT actual_commit STREQUAL expected_commit OR NOT dirty_status STREQUAL "")
        message(FATAL_ERROR
            "${label} must be clean and pinned to ${expected_commit}. "
            "Run: git submodule update --init --recursive")
    endif()
endfunction()

vkr_require_clean_pinned_checkout(
    "vendor/msdf-atlas-gen" "${VKR_MSDF_ATLAS_DIR}"
    "${VKR_MSDF_ATLAS_COMMIT}")
vkr_require_clean_pinned_checkout(
    "vendor/msdf-atlas-gen/msdfgen" "${VKR_MSDF_ATLAS_DIR}/msdfgen"
    "${VKR_MSDFGEN_COMMIT}")

set(MSDF_ATLAS_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_USE_SKIA OFF CACHE BOOL "" FORCE)
set(MSDF_ATLAS_NO_ARTERY_FONT ON CACHE BOOL "" FORCE)
set(MSDF_ATLAS_INSTALL OFF CACHE BOOL "" FORCE)
if(WIN32 AND (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY OR
              CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL"))
    set(MSDF_ATLAS_DYNAMIC_RUNTIME ON CACHE BOOL "" FORCE)
else()
    set(MSDF_ATLAS_DYNAMIC_RUNTIME OFF CACHE BOOL "" FORCE)
endif()
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_SVG ON CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL OFF CACHE BOOL "" FORCE)
find_package(Freetype REQUIRED)
set(VKR_TOOLS_BUILD_SHARED_LIBS_WAS_DEFINED FALSE)
if(DEFINED BUILD_SHARED_LIBS)
    set(VKR_TOOLS_BUILD_SHARED_LIBS_WAS_DEFINED TRUE)
    set(VKR_TOOLS_BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
endif()
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${VKR_MSDF_ATLAS_DIR}" "${CMAKE_BINARY_DIR}/msdf-atlas-gen"
                 EXCLUDE_FROM_ALL)
if(VKR_TOOLS_BUILD_SHARED_LIBS_WAS_DEFINED)
    set(BUILD_SHARED_LIBS "${VKR_TOOLS_BUILD_SHARED_LIBS_SAVED}")
else()
    unset(BUILD_SHARED_LIBS)
endif()
unset(VKR_TOOLS_BUILD_SHARED_LIBS_WAS_DEFINED)
unset(VKR_TOOLS_BUILD_SHARED_LIBS_SAVED)

add_executable(vkr_font_cooker
    vkr_font_cooker.cpp
)
target_compile_features(vkr_font_cooker PRIVATE cxx_std_17)
target_compile_definitions(vkr_font_cooker PRIVATE
    VKR_FONT_COOKER_MSDF_ATLAS_COMMIT="${VKR_MSDF_ATLAS_COMMIT}"
    VKR_FONT_COOKER_MSDFGEN_COMMIT="${VKR_MSDFGEN_COMMIT}")
target_include_directories(vkr_font_cooker PRIVATE
    ${CMAKE_SOURCE_DIR}/renderer/src
    ${VKR_MSDF_ATLAS_DIR}
    ${VKR_MSDF_ATLAS_DIR}/msdfgen
    ${FREETYPE_INCLUDE_DIRS}
)
target_link_libraries(vkr_font_cooker PRIVATE
    vkr_asset_cooking
    msdf-atlas-gen::msdf-atlas-gen
    Freetype::Freetype
)

if(APPLE)
    add_executable(vkr_metal_timestamp_diagnostic
        metal/vkr_metal_timestamp_diagnostic.m
    )
    find_library(VKR_DIAGNOSTIC_FOUNDATION_LIBRARY Foundation)
    find_library(VKR_DIAGNOSTIC_METAL_LIBRARY Metal)
    target_link_libraries(vkr_metal_timestamp_diagnostic PRIVATE
        ${VKR_DIAGNOSTIC_FOUNDATION_LIBRARY}
        ${VKR_DIAGNOSTIC_METAL_LIBRARY}
    )
    add_executable(vkr_metal_texture_array_diagnostic
        metal/vkr_metal_texture_array_diagnostic.m
    )
    target_link_libraries(vkr_metal_texture_array_diagnostic PRIVATE
        ${VKR_DIAGNOSTIC_FOUNDATION_LIBRARY}
        ${VKR_DIAGNOSTIC_METAL_LIBRARY}
    )
endif()

endif()
