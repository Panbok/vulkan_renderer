cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

if(APPLE)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h src/*.m)
enable_language(C OBJC)
elseif(WIN32)
file(GLOB_RECURSE LIB_SOURCES src/*.c src/*.h)
endif()
list(APPEND LIB_SOURCES ${CMAKE_SOURCE_DIR}/vendor/spirv_reflect.c)

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})
vkr_require_declared_c_functions(renderer_lib)

# Backend-owned Vulkan sources compile to the stable runtime asset names used by
# .shadercfg manifests. Source organization is independent of that runtime ABI.
find_program(VKR_SLANGC_EXECUTABLE NAMES slangc REQUIRED)
set(VKR_VULKAN_SHADER_SOURCE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/vulkan/shaders")
set(VKR_VULKAN_SHADER_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/assets/shaders")
file(GLOB_RECURSE VKR_VULKAN_SHADER_HEADERS CONFIGURE_DEPENDS
     "${VKR_VULKAN_SHADER_SOURCE_DIR}/*.slangh")
set(VKR_VULKAN_SHADER_OUTPUTS "")
function(vkr_add_vulkan_shader VKR_VULKAN_SHADER_SOURCE_RELATIVE
                               VKR_VULKAN_SHADER_NAME)
    set(VKR_VULKAN_SHADER_SOURCE
        "${VKR_VULKAN_SHADER_SOURCE_DIR}/${VKR_VULKAN_SHADER_SOURCE_RELATIVE}")
    set(VKR_VULKAN_SHADER_OUTPUT
        "${VKR_VULKAN_SHADER_OUTPUT_DIR}/${VKR_VULKAN_SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT "${VKR_VULKAN_SHADER_OUTPUT}"
        COMMAND "${VKR_SLANGC_EXECUTABLE}"
                -target spirv
                -I "${VKR_VULKAN_SHADER_SOURCE_DIR}"
                -o "${VKR_VULKAN_SHADER_OUTPUT}"
                "${VKR_VULKAN_SHADER_SOURCE}"
        DEPENDS "${VKR_VULKAN_SHADER_SOURCE}"
                ${VKR_VULKAN_SHADER_HEADERS}
        COMMENT "Compiling Vulkan shader ${VKR_VULKAN_SHADER_NAME}"
        VERBATIM
    )
    set(VKR_VULKAN_SHADER_OUTPUTS
        ${VKR_VULKAN_SHADER_OUTPUTS} "${VKR_VULKAN_SHADER_OUTPUT}"
        PARENT_SCOPE)
endfunction()

vkr_add_vulkan_shader("skybox/default.slang" "default.skybox")
vkr_add_vulkan_shader("text/default.slang" "default.text")
vkr_add_vulkan_shader("ui/default.slang" "default.ui")
vkr_add_vulkan_shader("viewport/display.slang" "default.viewport_display")
vkr_add_vulkan_shader("world/default.slang" "default.world")
vkr_add_vulkan_shader("world/pbr.slang" "pbr.world")
vkr_add_vulkan_shader("picking/world.slang" "picking")
vkr_add_vulkan_shader("picking/text.slang" "picking_text")
vkr_add_vulkan_shader("post/tonemap.slang" "post.tonemap")
vkr_add_vulkan_shader("shadow/cutout.slang" "shadow")
vkr_add_vulkan_shader("shadow/opaque.slang" "shadow_opaque")
vkr_add_vulkan_shader("ibl/brdf_lut.slang" "ibl.brdf_lut")
vkr_add_vulkan_shader("ibl/diffuse_convolution.slang"
                      "ibl.diffuse_convolution")
vkr_add_vulkan_shader("ibl/equirect_to_cube.slang" "ibl.equirect_to_cube")
vkr_add_vulkan_shader("ibl/specular_prefilter.slang"
                      "ibl.specular_prefilter")
add_custom_target(vkr_vulkan_shaders DEPENDS ${VKR_VULKAN_SHADER_OUTPUTS})
add_dependencies(renderer_lib vkr_vulkan_shaders)

# Production bindless walking shaders stay in the build tree. They are not part
# of the retained Vulkan runtime shader-name ABI under assets/shaders.
set(VKR_BINDLESS_VK_SHADER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/vulkan/bindless/shaders/walking.slang")
set(VKR_BINDLESS_VK_SHADER_OUTPUT_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bindless_vulkan")
set(VKR_BINDLESS_VK_VERTEX_SPV
    "${VKR_BINDLESS_VK_SHADER_OUTPUT_DIR}/walking.vert.spv")
set(VKR_BINDLESS_VK_FRAGMENT_SPV
    "${VKR_BINDLESS_VK_SHADER_OUTPUT_DIR}/walking.frag.spv")
function(vkr_add_bindless_vulkan_shader output entry stage)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${VKR_BINDLESS_VK_SHADER_OUTPUT_DIR}"
        COMMAND "${VKR_SLANGC_EXECUTABLE}"
                -target spirv
                -profile "spirv_1_6+SPV_GOOGLE_user_type+spvDerivativeControl+spvImageQuery+spvImageGatherExtended+spvSparseResidency+spvMinLod+spvFragmentFullyCoveredEXT"
                -fvk-use-entrypoint-name
                -fvk-use-scalar-layout
                -entry "${entry}"
                -stage "${stage}"
                "${VKR_BINDLESS_VK_SHADER_SOURCE}"
                -o "${output}"
        DEPENDS "${VKR_BINDLESS_VK_SHADER_SOURCE}"
        COMMENT "Compiling bindless Vulkan walking ${stage} shader"
        VERBATIM
    )
endfunction()
vkr_add_bindless_vulkan_shader("${VKR_BINDLESS_VK_VERTEX_SPV}"
                               vert_main vertex)
vkr_add_bindless_vulkan_shader("${VKR_BINDLESS_VK_FRAGMENT_SPV}"
                               frag_main fragment)
add_custom_target(vkr_bindless_vulkan_shaders DEPENDS
    "${VKR_BINDLESS_VK_VERTEX_SPV}"
    "${VKR_BINDLESS_VK_FRAGMENT_SPV}")
add_dependencies(renderer_lib vkr_bindless_vulkan_shaders)
target_compile_definitions(renderer_lib PRIVATE
    VKR_BINDLESS_VK_VERTEX_SPV="${VKR_BINDLESS_VK_VERTEX_SPV}"
    VKR_BINDLESS_VK_FRAGMENT_SPV="${VKR_BINDLESS_VK_FRAGMENT_SPV}"
)

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
set(BUILD_SHARED_LIBS "${_VKR_BUILD_SHARED_LIBS_PREV}" CACHE BOOL "Restore shared library default after KTX setup." FORCE)

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

# Set platform-specific definitions and link libraries
if(APPLE)
    set(VKR_METAL_PACKET_SHADER_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/metal/shaders")
    set(VKR_METAL_PACKET_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/metal")
    set(VKR_METAL_PACKET_SLANG_MSL
        "${VKR_METAL_PACKET_GENERATED_DIR}/library.slang.metal")
    set(VKR_METAL_PACKET_FRAGMENT_MSL
        "${VKR_METAL_PACKET_GENERATED_DIR}/library.metal")
    file(GLOB_RECURSE VKR_METAL_PACKET_SLANG_SOURCES CONFIGURE_DEPENDS
         "${VKR_METAL_PACKET_SHADER_DIR}/*.slang"
         "${VKR_METAL_PACKET_SHADER_DIR}/*.slangh")
    set(VKR_METAL_PACKET_MSL_SOURCES
        "${VKR_METAL_PACKET_SHADER_DIR}/common/draw.metalh"
        "${VKR_METAL_PACKET_SHADER_DIR}/world/lighting.metalh"
        "${VKR_METAL_PACKET_SHADER_DIR}/shadow/sampling.metalh"
        "${VKR_METAL_PACKET_SHADER_DIR}/common/fullscreen.metalh"
        "${VKR_METAL_PACKET_SHADER_DIR}/world/default.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/ibl/common.metalh"
        "${VKR_METAL_PACKET_SHADER_DIR}/ibl/equirect_to_cube.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/ibl/diffuse_convolution.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/ibl/specular_prefilter.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/ibl/brdf_lut.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/text/default.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/skybox/default.metal"
        "${VKR_METAL_PACKET_SHADER_DIR}/post/tonemap.metal")
    list(JOIN VKR_METAL_PACKET_MSL_SOURCES "|"
         VKR_METAL_PACKET_MSL_SOURCE_ARGUMENT)
    add_custom_command(
        OUTPUT "${VKR_METAL_PACKET_SLANG_MSL}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${VKR_METAL_PACKET_GENERATED_DIR}"
        COMMAND "${VKR_SLANGC_EXECUTABLE}"
                -target metal
                -line-directive-mode none
                "${VKR_METAL_PACKET_SHADER_DIR}/library.slang"
                -o "${VKR_METAL_PACKET_SLANG_MSL}"
        DEPENDS ${VKR_METAL_PACKET_SLANG_SOURCES}
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
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    find_library(GAMECONTROLLER_LIBRARY GameController)
    target_link_libraries(renderer_lib PRIVATE ${COCOA_LIBRARY} ${FOUNDATION_LIBRARY} ${METAL_LIBRARY} ${QUARTZCORE_LIBRARY} ${GAMECONTROLLER_LIBRARY})
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
