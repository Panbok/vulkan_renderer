cmake_minimum_required(VERSION 3.27)

project(vkr_tools)

if(APPLE)
    enable_language(OBJC)
endif()

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

add_executable(vkr_mesh_cooker
    vkr_mesh_cooker.c
)
vkr_require_declared_c_functions(vkr_mesh_cooker)
target_link_libraries(vkr_mesh_cooker PRIVATE renderer_lib)

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
    ${CMAKE_SOURCE_DIR}/lib/src
    ${VKR_MSDF_ATLAS_DIR}
    ${VKR_MSDF_ATLAS_DIR}/msdfgen
    ${FREETYPE_INCLUDE_DIRS}
)
target_link_libraries(vkr_font_cooker PRIVATE
    renderer_lib
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
