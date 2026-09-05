cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

find_program(VKR_SLANGC_EXECUTABLE NAMES slangc REQUIRED)

if(APPLE)
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h src/*.m)
enable_language(C OBJC)
elseif(WIN32)
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h)
endif()
list(APPEND LIB_SOURCES ${CMAKE_SOURCE_DIR}/vendor/spirv_reflect.c)

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})
vkr_require_declared_c_functions(renderer_lib)


set(VKR_RENDERER_SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/shaders")
file(GLOB_RECURSE VKR_SHARED_SHADER_SOURCES CONFIGURE_DEPENDS
     "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/*.slang"
     "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/*.slangh")
set(VKR_VULKAN_SHADER_OUTPUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
set(VKR_VULKAN_PACKET_SHADER_DIR
    "${VKR_RENDERER_SHADER_SOURCE_DIR}/vulkan/slang")
set(VKR_VULKAN_PACKET_SHADER_SOURCE
    "${VKR_VULKAN_PACKET_SHADER_DIR}/library.slang")
file(GLOB_RECURSE VKR_VULKAN_PACKET_SHADER_SOURCES CONFIGURE_DEPENDS
     "${VKR_VULKAN_PACKET_SHADER_DIR}/*.slang"
     "${VKR_VULKAN_PACKET_SHADER_DIR}/*.slangh")
function(vkr_add_vulkan_packet_shader output_name entry stage)
    set(output "${VKR_VULKAN_SHADER_OUTPUT_DIR}/${output_name}.spv")
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${VKR_VULKAN_SHADER_OUTPUT_DIR}"
        COMMAND "${VKR_SLANGC_EXECUTABLE}"
                -target spirv
                -profile "spirv_1_6+SPV_GOOGLE_user_type+spvDerivativeControl+spvImageQuery+spvImageGatherExtended+spvSparseResidency+spvMinLod+spvFragmentFullyCoveredEXT+spvGroupNonUniformArithmetic"
                -fvk-use-entrypoint-name
                -fvk-use-scalar-layout
                # Shader-audit setting. -O3 produced byte-identical modules for
                # the current entry points.
                -O2
                ${ARGN}
                -entry "${entry}"
                -stage "${stage}"
                "${VKR_VULKAN_PACKET_SHADER_SOURCE}"
                -o "${output}"
        DEPENDS ${VKR_VULKAN_PACKET_SHADER_SOURCES}
                ${VKR_SHARED_SHADER_SOURCES}
        COMMENT "Compiling Vulkan packet ${entry} shader"
        VERBATIM)
    set(VKR_VULKAN_PACKET_SHADER_OUTPUTS
        ${VKR_VULKAN_PACKET_SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
    string(TOUPPER "${output_name}" macro_name)
    string(REPLACE "." "_" macro_name "${macro_name}")
    target_compile_definitions(renderer_lib PRIVATE
        "VKR_VULKAN_${macro_name}_SPV=\"${output}\"")
endfunction()
vkr_add_vulkan_packet_shader("packet.world.vert" world_vertex vertex)
vkr_add_vulkan_packet_shader(
    "packet.world.temporal.vert" world_temporal_vertex vertex)
vkr_add_vulkan_packet_shader("packet.world.frag" world_fragment fragment)
vkr_add_vulkan_packet_shader(
    "packet.world.temporal.frag" world_temporal_fragment fragment)
vkr_add_vulkan_packet_shader("packet.picking.frag" picking_fragment fragment)
vkr_add_vulkan_packet_shader("packet.fullscreen.vert" fullscreen_vertex vertex)
vkr_add_vulkan_packet_shader("packet.fullscreen.frag" fullscreen_fragment fragment)
vkr_add_vulkan_packet_shader("packet.text.vert" text_vertex vertex)
vkr_add_vulkan_packet_shader("packet.text.frag" text_fragment fragment)
vkr_add_vulkan_packet_shader("packet.text_picking.frag" text_picking_fragment fragment)
vkr_add_vulkan_packet_shader("packet.ui.vert" ui_vertex vertex)
vkr_add_vulkan_packet_shader("packet.ui.frag" ui_fragment fragment)
vkr_add_vulkan_packet_shader("packet.ui_rect.vert" ui_rect_vertex vertex)
vkr_add_vulkan_packet_shader("packet.ui_rect.frag" ui_rect_fragment fragment)
vkr_add_vulkan_packet_shader("packet.ibl_equirect.comp" ibl_equirect compute)
vkr_add_vulkan_packet_shader("packet.ibl_prefilter.comp" ibl_prefilter compute)
vkr_add_vulkan_packet_shader(
    "packet.ibl_sh.comp" ibl_sh compute -DVKR_VULKAN_IBL_SH_ROOT=1)
vkr_add_vulkan_packet_shader("packet.gpu_draw_classify.comp" vk_gpu_draw_classify compute)
vkr_add_vulkan_packet_shader("packet.gpu_draw_prefix.comp" vk_gpu_draw_prefix compute)
vkr_add_vulkan_packet_shader("packet.gpu_draw_encode.comp" vk_gpu_draw_encode compute)
vkr_add_vulkan_packet_shader("packet.temporal_transform.comp" vk_temporal_transform compute)
vkr_add_vulkan_packet_shader("packet.visibility.vert" vk_visibility_vertex vertex)
vkr_add_vulkan_packet_shader("packet.visibility.frag" vk_visibility_fragment fragment)
vkr_add_vulkan_packet_shader("packet.visibility_opaque.frag" vk_visibility_opaque_fragment fragment)
vkr_add_vulkan_packet_shader("packet.visibility_shadow.frag" vk_visibility_shadow_fragment fragment)
vkr_add_vulkan_packet_shader("packet.gbuffer_resolve.comp" vk_gbuffer_resolve compute)
vkr_add_vulkan_packet_shader("packet.deferred_lighting.comp" vk_deferred_lighting compute)
vkr_add_vulkan_packet_shader("packet.temporal_resolve.comp" vk_temporal_resolve compute)
vkr_add_vulkan_packet_shader("packet.hzb_build.comp" vk_hzb_build compute)
vkr_add_vulkan_packet_shader("packet.sdsm_reduce.comp" vk_sdsm_reduce compute)
vkr_add_vulkan_packet_shader("packet.exposure_clear.comp" vk_exposure_clear compute)
vkr_add_vulkan_packet_shader("packet.exposure_histogram.comp" vk_exposure_histogram compute)
vkr_add_vulkan_packet_shader("packet.exposure_resolve.comp" vk_exposure_resolve compute)
vkr_add_vulkan_packet_shader("packet.bloom_prefilter.comp" vk_bloom_prefilter compute)
vkr_add_vulkan_packet_shader("packet.bloom_downsample_tent13.comp" vk_bloom_downsample_tent13 compute)
vkr_add_vulkan_packet_shader("packet.bloom_downsample_box4.comp" vk_bloom_downsample_box4 compute)
vkr_add_vulkan_packet_shader("packet.bloom_upsample.comp" vk_bloom_upsample compute)
vkr_add_vulkan_packet_shader("packet.gtao_depth_prefilter.comp" vk_gtao_depth_prefilter compute)
vkr_add_vulkan_packet_shader("packet.gtao_depth_mip.comp" vk_gtao_depth_mip compute)
vkr_add_vulkan_packet_shader("packet.gtao_evaluate.comp" vk_gtao_evaluate compute)
vkr_add_vulkan_packet_shader("packet.gtao_denoise.comp" vk_gtao_denoise compute)
vkr_add_vulkan_packet_shader("packet.bloom_combine.comp" vk_bloom_combine compute)
vkr_add_vulkan_packet_shader("packet.picking_resolve.comp" vk_picking_resolve compute)
vkr_add_vulkan_packet_shader("packet.transmission_shade.comp" vk_transmission_shade compute)
vkr_add_vulkan_packet_shader("packet.transmission_shade_partitioned.comp" vk_transmission_shade_partitioned compute)
vkr_add_vulkan_packet_shader("packet.transmission_shade_production.comp" vk_transmission_shade_production compute)
vkr_add_vulkan_packet_shader("packet.transmission_shade_production_temporal.comp" vk_transmission_shade_production_temporal compute)
vkr_add_vulkan_packet_shader("packet.transmission_shade_partitioned_production.comp" vk_transmission_shade_partitioned_production compute)
vkr_add_vulkan_packet_shader("packet.transmission_shade_partitioned_production_temporal.comp" vk_transmission_shade_partitioned_production_temporal compute)
vkr_add_vulkan_packet_shader("packet.transmission_compact_clear.comp" vk_transmission_compact_clear compute)
vkr_add_vulkan_packet_shader("packet.transmission_compact.comp" vk_transmission_compact compute)
vkr_add_vulkan_packet_shader("packet.transmission_compact_finalize.comp" vk_transmission_compact_finalize compute)
vkr_add_vulkan_packet_shader("packet.transmission_coverage.comp" vk_transmission_coverage compute)
add_custom_target(vkr_vulkan_packet_shaders DEPENDS
    ${VKR_VULKAN_PACKET_SHADER_OUTPUTS})
add_dependencies(renderer_lib vkr_vulkan_packet_shaders)

target_compile_definitions(renderer_lib PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

# Platform-specific optimization flags
if(MSVC)
    target_compile_options(renderer_lib PRIVATE /arch:AVX2)
else()
    target_compile_options(renderer_lib PRIVATE -march=native)
endif()

# Find Vulkan package
find_package(Vulkan REQUIRED)

# KTX-Software (KTX2 + BasisU transcoding)
set(VKR_KTX_SOFTWARE_DIR "${CMAKE_SOURCE_DIR}/vendor/ktx-software")
if(NOT EXISTS "${VKR_KTX_SOFTWARE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "KTX-Software submodule not found. Run: git submodule update --init --recursive")
endif()

set(KTX_FEATURE_DOC OFF CACHE BOOL "Disable KTX docs in renderer build." FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "Disable KTX JNI in renderer build." FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "Disable KTX python bindings in renderer build." FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests in renderer build." FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "Disable KTX tools in renderer build." FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX tools CTS in renderer build." FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "Disable KTX loadtest apps in renderer build." FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "Disable KTX GL upload path in renderer build." FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "Disable KTX Vulkan upload path in renderer build." FORCE)

set(_VKR_BUILD_SHARED_LIBS_PREV "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build KTX dependencies statically." FORCE)
add_subdirectory("${VKR_KTX_SOFTWARE_DIR}" "${CMAKE_BINARY_DIR}/vendor/ktx-software" EXCLUDE_FROM_ALL)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang can select MSVC STL vector-algorithm helpers newer than the
    # installed runtime import library. Keep vendored BasisU self-contained.
    target_compile_definitions(ktx PRIVATE _USE_STD_VECTOR_ALGORITHMS=0)
endif()
set(BUILD_SHARED_LIBS "${_VKR_BUILD_SHARED_LIBS_PREV}" CACHE BOOL "Restore shared library default after KTX setup." FORCE)

# meshoptimizer geometry codecs and offline range optimization.
set(VKR_MESHOPTIMIZER_DIR "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer")
if(NOT EXISTS "${VKR_MESHOPTIMIZER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "meshoptimizer submodule not found. Run: git submodule update --init --recursive")
endif()
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "Disable meshoptimizer demo." FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "Disable meshoptimizer gltfpack." FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "Build meshoptimizer statically." FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "Disable meshoptimizer install targets." FORCE)
add_subdirectory("${VKR_MESHOPTIMIZER_DIR}"
                 "${CMAKE_BINARY_DIR}/vendor/meshoptimizer"
                 EXCLUDE_FROM_ALL)
add_library(vkr_meshoptimizer_bridge STATIC
    src/renderer/resources/loaders/vkr_meshoptimizer_bridge.cpp)
target_compile_features(vkr_meshoptimizer_bridge PRIVATE cxx_std_11)
target_include_directories(vkr_meshoptimizer_bridge
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vkr_meshoptimizer_bridge PRIVATE meshoptimizer)

# Locate SPIR-V headers (needed by spirv_reflect.h when using system headers).
set(_VKR_VULKAN_SDK_INCLUDE "")
if(DEFINED ENV{VULKAN_SDK})
    # Vulkan SDK layout on Windows/macOS/Linux: <VULKAN_SDK>/Include
    set(_VKR_VULKAN_SDK_INCLUDE "$ENV{VULKAN_SDK}/Include")
endif()

set(_VKR_VULKAN_TARGET_INCLUDES "")
if(TARGET Vulkan::Vulkan)
    get_target_property(_VKR_VULKAN_TARGET_INCLUDES Vulkan::Vulkan INTERFACE_INCLUDE_DIRECTORIES)
endif()

# Prefer the canonical SPIRV-Headers layout: spirv/unified1/spirv.h
find_path(_VKR_SPIRV_UNIFIED1_INCLUDE_DIR
    NAMES spirv/unified1/spirv.h
    HINTS
        ${_VKR_VULKAN_SDK_INCLUDE}
        ${_VKR_VULKAN_TARGET_INCLUDES}
        /usr/local/include
        /opt/homebrew/include
        ${Vulkan_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIR}
)

# Newer Vulkan SDKs on Windows may ship a flattened header at:
#   <VULKAN_SDK>/Include/spirv-headers/spirv.h
find_path(_VKR_SPIRV_FLAT_INCLUDE_DIR
    NAMES spirv-headers/spirv.h
    HINTS
        ${_VKR_VULKAN_SDK_INCLUDE}
        ${_VKR_VULKAN_TARGET_INCLUDES}
        /usr/local/include
        /opt/homebrew/include
        ${Vulkan_INCLUDE_DIRS}
        ${Vulkan_INCLUDE_DIR}
)

set(_VKR_ADDITIONAL_SPIRV_HEADER_INCLUDE_DIR "")
if(_VKR_SPIRV_UNIFIED1_INCLUDE_DIR)
    set(SPIRV_HEADERS_INCLUDE_DIR "${_VKR_SPIRV_UNIFIED1_INCLUDE_DIR}")
elseif(_VKR_SPIRV_FLAT_INCLUDE_DIR)
    # Vulkan SDK flattened SPIR-V header layout.
    # `vendor/spirv_reflect.h` will include <spirv-headers/spirv.h> when present.
    set(SPIRV_HEADERS_INCLUDE_DIR "${_VKR_SPIRV_FLAT_INCLUDE_DIR}")
endif()

if(NOT SPIRV_HEADERS_INCLUDE_DIR)
    message(FATAL_ERROR
        "SPIR-V headers not found (missing spirv/unified1/spirv.h).\n"
        "Fix options:\n"
        "  - Install SPIRV-Headers (system/vcpkg)\n"
        "  - Ensure your Vulkan SDK install includes SPIR-V headers\n"
        "  - Or configure with: -DSPIRV_HEADERS_INCLUDE_DIR=<path-to-include>\n"
    )
endif()

# Set include directories
target_include_directories(renderer_lib
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC ${CMAKE_SOURCE_DIR}/vendor
    PUBLIC ${SPIRV_HEADERS_INCLUDE_DIR}
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

# Link Vulkan - use modern target if available, otherwise use libraries directly
if(TARGET Vulkan::Vulkan)
    target_link_libraries(renderer_lib PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(renderer_lib PRIVATE ${Vulkan_LIBRARIES})
endif()
target_link_libraries(renderer_lib PRIVATE ktx_read)
target_link_libraries(renderer_lib PRIVATE vkr_meshoptimizer_bridge)

# Set platform-specific definitions and link libraries
if(APPLE)
    set(VKR_METAL_PACKET_SLANG_SHADER_DIR
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/metal/slang")
    set(VKR_METAL_PACKET_MSL_SHADER_DIR
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/metal/msl")
    set(VKR_METAL_PACKET_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal")
    set(VKR_METAL_PACKET_SLANG_MSL
        "${VKR_METAL_PACKET_GENERATED_DIR}/library.slang.metal")
    set(VKR_METAL_PACKET_FRAGMENT_MSL
        "${VKR_METAL_PACKET_GENERATED_DIR}/library.metal")
    file(GLOB_RECURSE VKR_METAL_PACKET_SLANG_SOURCES CONFIGURE_DEPENDS
         "${VKR_METAL_PACKET_SLANG_SHADER_DIR}/*.slang"
         "${VKR_METAL_PACKET_SLANG_SHADER_DIR}/*.slangh")
    # The shared receiver kernel must precede shadow/sampling.metalh: the MSL
    # library is concatenated, not include-resolved, so order is the only
    # declaration order there is.
    set(VKR_METAL_PACKET_MSL_SOURCES
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/exposure_kernel.slangh"
        # The bloom kernel reuses the exposure kernel's Rec. 709 luminance, so
        # it must follow it here for the same reason as below.
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/bloom_kernel.slangh"
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/gtao_kernel.slangh"
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/ggx_kernel.slangh"
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/normal_map_kernel.slangh"
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/transmission_kernel.slangh"
        # VkrShL2Packed and its evaluation are referenced by the deferred
        # lighting path and by the IBL projection kernel, so the shared header
        # precedes both.
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/sh_l2_kernel.slangh"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/common/draw.metalh"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/world/lighting.metalh"
        "${VKR_RENDERER_SHADER_SOURCE_DIR}/shared/shadow_kernel.slangh"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/shadow/sampling.metalh"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/common/fullscreen.metalh"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/world/gpu_draws.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/world/default.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/ibl/common.metalh"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/ibl/equirect_to_cube.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/ibl/sh_projection.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/ibl/specular_prefilter.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/text/default.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/ui/default.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/post/exposure.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/post/bloom.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/post/gtao.metal"
        "${VKR_METAL_PACKET_MSL_SHADER_DIR}/post/tonemap.metal")
    list(JOIN VKR_METAL_PACKET_MSL_SOURCES "|"
         VKR_METAL_PACKET_MSL_SOURCE_ARGUMENT)
    add_custom_command(
        OUTPUT "${VKR_METAL_PACKET_SLANG_MSL}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${VKR_METAL_PACKET_GENERATED_DIR}"
        COMMAND "${VKR_SLANGC_EXECUTABLE}"
                -target metal
                -line-directive-mode none
                "${VKR_METAL_PACKET_SLANG_SHADER_DIR}/library.slang"
                -o "${VKR_METAL_PACKET_SLANG_MSL}"
        DEPENDS ${VKR_METAL_PACKET_SLANG_SOURCES}
                ${VKR_SHARED_SHADER_SOURCES}
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${VKR_METAL_PACKET_FRAGMENT_MSL}"
        COMMAND "${CMAKE_COMMAND}"
                "-DVKR_SHADER_OUTPUT=${VKR_METAL_PACKET_FRAGMENT_MSL}"
                "-DVKR_SHADER_INPUTS=${VKR_METAL_PACKET_MSL_SOURCE_ARGUMENT}"
                -P "${CMAKE_SOURCE_DIR}/cmake/vkr_concat_shader_sources.cmake"
        DEPENDS ${VKR_METAL_PACKET_MSL_SOURCES}
                "${CMAKE_SOURCE_DIR}/cmake/vkr_concat_shader_sources.cmake"
        VERBATIM
    )
    set_source_files_properties("${VKR_METAL_PACKET_SLANG_MSL}"
                                "${VKR_METAL_PACKET_FRAGMENT_MSL}"
                                PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(renderer_lib PRIVATE "${VKR_METAL_PACKET_SLANG_MSL}"
                                        "${VKR_METAL_PACKET_FRAGMENT_MSL}")
    target_compile_definitions(renderer_lib PRIVATE
        PLATFORM_APPLE=1
        VKR_METAL_PACKET_SLANG_MSL="${VKR_METAL_PACKET_SLANG_MSL}"
        VKR_METAL_PACKET_FRAGMENT_MSL="${VKR_METAL_PACKET_FRAGMENT_MSL}"
        VKR_METAL_PACKET_ARCHIVE_PATH="${VKR_METAL_PACKET_GENERATED_DIR}/vkr_application.mtlarchive"
    )
    # Link Cocoa framework if needed for macOS-specific functionality
    find_library(COCOA_LIBRARY Cocoa)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(METAL_LIBRARY Metal)
    find_library(METALFX_LIBRARY MetalFX)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(GAMECONTROLLER_LIBRARY GameController)
    target_link_libraries(renderer_lib PRIVATE
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${METAL_LIBRARY}
        ${METALFX_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        ${GAMECONTROLLER_LIBRARY})
elseif(WIN32)
    target_compile_definitions(renderer_lib PRIVATE PLATFORM_WINDOWS=1)
    target_link_libraries(renderer_lib PRIVATE user32.lib winmm.lib Xinput.lib)
endif()

# Set compile definitions for logging based on build type
target_compile_definitions(renderer_lib PRIVATE
    $<$<CONFIG:Release>:LOG_LEVEL=1>
    $<$<CONFIG:Release>:ASSERT_LOG=0>
    $<$<CONFIG:RelWithDebInfo>:LOG_LEVEL=3>
    $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
    $<$<CONFIG:Debug>:LOG_LEVEL=4>
    $<$<CONFIG:Debug>:ASSERT_LOG=1>
    $<$<CONFIG:Release>:VKR_ALLOCATOR_DISABLE_STATS=1>
    $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_DISABLE_STATS=1>
    $<$<CONFIG:Debug>:VKR_ALLOCATOR_DISABLE_STATS=0>
    $<$<CONFIG:Release>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    $<$<CONFIG:Debug>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
)

target_precompile_headers(renderer_lib PRIVATE src/vkr_pch.h)
