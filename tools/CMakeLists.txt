cmake_minimum_required(VERSION 3.27)

project(vkr_tools)

# Shared UTF-8 CRT entry adapter also serves standalone table cookers.
include_directories("${CMAKE_SOURCE_DIR}/lib/src")

if(APPLE)
    enable_language(OBJC)
endif()

# Every cooker below is a static library holding its original command line as
# one entry function; tools/bakery links them into vkr_bakery, which runs them
# as `vkr_bakery tool <name>` and through its producers.
#
# Offline generators for checked-in, shared immutable BRDF coefficients. Only
# the explicit `table` producer runs them; configuring or building a renderer
# must never rewrite its static table sources.
add_library(vkr_dfg_cooker STATIC vkr_dfg_cooker.cpp)
target_compile_features(vkr_dfg_cooker PRIVATE cxx_std_17)
vkr_configure_table_cooker_target(vkr_dfg_cooker)
target_link_libraries(vkr_dfg_cooker PRIVATE vkr_foundation)
target_include_directories(vkr_dfg_cooker PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src")

# Charlie energy and rectangle fits share one immutable payload in both backends.
add_library(vkr_sheen_cooker STATIC vkr_sheen_cooker.cpp)
target_compile_features(vkr_sheen_cooker PRIVATE cxx_std_17)
vkr_configure_table_cooker_target(vkr_sheen_cooker)
target_link_libraries(vkr_sheen_cooker PRIVATE vkr_foundation)
target_include_directories(vkr_sheen_cooker PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src")

add_library(vkr_anisotropy_cooker STATIC vkr_anisotropy_cooker.cpp
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_ltc_lut.c")
target_compile_features(vkr_anisotropy_cooker PRIVATE cxx_std_17)
vkr_configure_table_cooker_target(vkr_anisotropy_cooker)
target_link_libraries(vkr_anisotropy_cooker PRIVATE vkr_foundation)
target_include_directories(vkr_anisotropy_cooker PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src")

find_package(Vulkan REQUIRED)

# Capture/export consumers and asset importers share one image writer object.
add_library(vkr_image_write STATIC assets/stb_image_write_impl.c)
vkr_configure_cooker_target(vkr_image_write)
target_include_directories(vkr_image_write PRIVATE "${CMAKE_SOURCE_DIR}/vendor")


if(VKR_BUILD_HARNESS)
add_executable(vkr_asset_preview vkr_asset_preview.c)
vkr_configure_cooker_target(vkr_asset_preview)
target_include_directories(vkr_asset_preview PRIVATE "${CMAKE_SOURCE_DIR}/vendor")
target_link_libraries(vkr_asset_preview PRIVATE vkr_foundation vkr_image_decode vkr_image_write vkr_ktx_read)
if(NOT WIN32)
    target_link_libraries(vkr_asset_preview PRIVATE m)
endif()

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

include("${CMAKE_SOURCE_DIR}/cmake/vkr_application_target.cmake")

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
# Harness children run the sample runtime, which spawns the engine mannequin.
include("${CMAKE_SOURCE_DIR}/cmake/vkr_engine_content.cmake")
add_dependencies(vkr_harness vkr_engine_textures)
vkr_require_declared_c_functions(vkr_harness)
# The child opens a renderer target and reaches the same unoptimized dispatch.
vkr_set_main_stack_reserve(vkr_harness)
vkr_configure_installed_rpath(vkr_harness)

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
    assets/vkr_animation_import.c
    assets/vkr_animation_encode.c
    assets/vkr_font_encode.c
    assets/vkr_mesh_encode.c
    assets/vkr_mesh_cook_source.c
    assets/vkr_scene_light_bake.c
    assets/vkr_mesh_deduplicate.c
    assets/vkr_meshoptimizer_bridge.cpp
    assets/vkr_mesh_lightmap_uv.cpp
    assets/mesh_loader_gltf.c
    assets/vkr_gltf_material_conversion.c
    assets/cgltf_impl.c)
vkr_configure_library(vkr_asset_cooking)
vkr_configure_cooker_target(vkr_asset_cooking)
target_include_directories(vkr_asset_cooking PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/vendor")
target_link_libraries(vkr_asset_cooking PUBLIC vkr_asset_formats PRIVATE meshoptimizer vkr_xatlas vkr_image_decode vkr_image_write)
target_compile_features(vkr_asset_cooking PRIVATE cxx_std_11)

add_library(vkr_vkt_cooking STATIC
    vkr_vkt_packer.cpp
)
vkr_configure_cooker_target(vkr_vkt_cooking)
target_compile_features(vkr_vkt_cooking PRIVATE cxx_std_17)

target_include_directories(vkr_vkt_cooking
    PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor
        ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(vkr_vkt_cooking
    PRIVATE
        ktx
        vkr_foundation
        vkr_image_decode
)
if(APPLE)
    # AppleTextureEncoder (libate) encodes the "astc-fast" encoding.
    target_link_libraries(vkr_vkt_cooking PRIVATE ate)
endif()
# bc7e and rgbcx encode the "bc" and "bc-fast" encodings on x86-64.
include("${CMAKE_SOURCE_DIR}/cmake/vkr_bc7e.cmake")
if(VKR_BC7E_AVAILABLE)
    target_link_libraries(vkr_vkt_cooking PRIVATE vkr_bc7e)
    target_compile_definitions(vkr_vkt_cooking PRIVATE VKR_VKT_HAS_BC7E=1)
endif()

target_link_libraries(vkr_asset_cooking PRIVATE vkr_vkt_cooking)

add_library(vkr_vkt_packer STATIC
    vkr_vkt_packer_main.cpp
)
vkr_require_declared_c_functions(vkr_vkt_packer)
vkr_configure_cooker_target(vkr_vkt_packer)
target_compile_features(vkr_vkt_packer PRIVATE cxx_std_17)
target_link_libraries(vkr_vkt_packer PRIVATE vkr_vkt_cooking)

add_library(vkr_hdr_cube_packer STATIC vkr_hdr_cube_packer.cpp)
vkr_configure_cooker_target(vkr_hdr_cube_packer)
target_compile_features(vkr_hdr_cube_packer PRIVATE cxx_std_17)
target_include_directories(vkr_hdr_cube_packer PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(vkr_hdr_cube_packer PRIVATE ktx vkr_foundation)

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
    bake/vkr_bake_layers.cpp
    bake/vkr_bake_lightmap.cpp
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_dfg_lut.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_sheen_lut.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_anisotropy_lut.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_atmosphere.c"
    "${CMAKE_SOURCE_DIR}/renderer/src/vkr_clouds.c"
    "${CMAKE_SOURCE_DIR}/runtime/src/level/vkr_blockout.c"
    "${CMAKE_SOURCE_DIR}/runtime/src/level/vkr_brush.c")
vkr_configure_cooker_target(vkr_bake_core)
target_compile_features(vkr_bake_core PRIVATE cxx_std_17)
target_include_directories(vkr_bake_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/renderer/src" "${CMAKE_SOURCE_DIR}/vendor" ${Vulkan_INCLUDE_DIRS})
target_link_libraries(vkr_bake_core PUBLIC vkr_asset_formats PRIVATE ktx vkr_image_decode)
# Metal ray-traced lightmap transport on Apple hosts; others bake on the CPU.
if(APPLE)
    target_sources(vkr_bake_core PRIVATE bake/vkr_bake_metal.mm)
    set_source_files_properties(bake/vkr_bake_metal.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(vkr_bake_core PUBLIC "-framework Metal"
        "-framework Foundation")
else()
    target_sources(vkr_bake_core PRIVATE bake/vkr_bake_metal_stub.cpp)
endif()
add_library(vkr_diffuse_baker STATIC vkr_diffuse_baker.cpp)
vkr_configure_cooker_target(vkr_diffuse_baker)
target_compile_features(vkr_diffuse_baker PRIVATE cxx_std_17)
target_link_libraries(vkr_diffuse_baker PRIVATE vkr_bake_core)
add_library(vkr_lightmap_baker STATIC vkr_lightmap_baker.cpp)
vkr_configure_cooker_target(vkr_lightmap_baker)
target_compile_features(vkr_lightmap_baker PRIVATE cxx_std_17)
target_link_libraries(vkr_lightmap_baker PRIVATE vkr_bake_core)

add_library(vkr_mesh_cooker STATIC
    vkr_mesh_cooker.c
)
vkr_require_declared_c_functions(vkr_mesh_cooker)
vkr_configure_cooker_target(vkr_mesh_cooker)
target_link_libraries(vkr_mesh_cooker PRIVATE vkr_asset_cooking)

if(NOT TARGET Jolt)
    include("${CMAKE_SOURCE_DIR}/cmake/vkr_physics.cmake")
endif()
add_library(vkr_collision_cooking STATIC
    assets/vkr_collision_import.c assets/vkr_collision_hull.cpp)
vkr_configure_library(vkr_collision_cooking)
vkr_configure_cooker_target(vkr_collision_cooking)
target_include_directories(vkr_collision_cooking PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/vendor")
target_compile_features(vkr_collision_cooking PRIVATE cxx_std_17)
target_link_libraries(vkr_collision_cooking PUBLIC vkr_asset_cooking PRIVATE Jolt)
add_library(vkr_collision_cooker STATIC vkr_collision_cooker.c)
vkr_require_declared_c_functions(vkr_collision_cooker)
vkr_configure_cooker_target(vkr_collision_cooker)
target_link_libraries(vkr_collision_cooker PRIVATE vkr_collision_cooking)

add_library(vkr_animation_cooker STATIC vkr_animation_cooker.c)
vkr_require_declared_c_functions(vkr_animation_cooker)
vkr_configure_cooker_target(vkr_animation_cooker)
target_link_libraries(vkr_animation_cooker PRIVATE vkr_asset_cooking)

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
# On macOS FreeType and its PNG decoder link from their static archives, so
# a distributed vkr_bakery loads no dylib from the machine that built it.
if(APPLE)
    get_filename_component(VKR_FREETYPE_DIRECTORY "${FREETYPE_LIBRARY}" DIRECTORY)
    find_library(VKR_FREETYPE_ARCHIVE NAMES libfreetype.a
        HINTS "${VKR_FREETYPE_DIRECTORY}" NO_DEFAULT_PATH)
    find_library(VKR_PNG_ARCHIVE NAMES libpng16.a
        HINTS "${VKR_FREETYPE_DIRECTORY}")
    if(NOT VKR_FREETYPE_ARCHIVE OR NOT VKR_PNG_ARCHIVE)
        message(FATAL_ERROR "The macOS build links FreeType and libpng statically; "
            "install their archives (Homebrew freetype and libpng)")
    endif()
    set_target_properties(Freetype::Freetype PROPERTIES
        IMPORTED_LOCATION "${VKR_FREETYPE_ARCHIVE}"
        IMPORTED_LOCATION_RELEASE "${VKR_FREETYPE_ARCHIVE}"
        IMPORTED_LOCATION_DEBUG "${VKR_FREETYPE_ARCHIVE}"
        INTERFACE_LINK_LIBRARIES "${VKR_PNG_ARCHIVE};z;bz2")
endif()
# vcpkg's FreeType export and compatibility wrapper embed raw Debug archive
# paths in link-interface generator expressions. Imported-location mapping does
# not rewrite those expressions, so select their Release branches explicitly.
# The whole Windows graph uses the Release CRT, including Debug VKR consumers.
if(WIN32)
    foreach(target freetype Freetype::Freetype)
        if(TARGET ${target})
            get_target_property(links ${target} INTERFACE_LINK_LIBRARIES)
            if(links)
                string(REGEX REPLACE "\\$<CONFIG:[Dd][Ee][Bb][Uu][Gg]>"
                    "$<BOOL:0>" links "${links}")
                set_property(TARGET ${target} PROPERTY INTERFACE_LINK_LIBRARIES "${links}")
            endif()
        endif()
    endforeach()
endif()
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

vkr_configure_cooker_dependencies(msdf-atlas-gen msdfgen-core msdfgen-ext)

add_library(vkr_font_cooker STATIC
    vkr_font_cooker.cpp
)
vkr_configure_cooker_target(vkr_font_cooker)
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

# The build system itself; always present because renderer builds compile
# their shader catalog through it.
add_subdirectory(bakery)

# The MCP server for the editor's agent channel (ADR-084).
add_executable(vkr_mcp agent/vkr_mcp.c)
vkr_require_declared_c_functions(vkr_mcp)
target_link_libraries(vkr_mcp PRIVATE vkr_bakery_json)

# The bakery finds this build tree's harness for package reflection bakes
# unless one sits beside it, as in an editor distribution.
if(TARGET vkr_harness)
    target_compile_definitions(vkr_bakery PRIVATE
        VKR_BAKERY_HARNESS_DEFAULT="$<TARGET_FILE:vkr_harness>")
endif()
