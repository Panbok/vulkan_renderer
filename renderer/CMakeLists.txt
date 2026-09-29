cmake_minimum_required(VERSION 3.27)

project(renderer_lib)

# Asset codecs and offline tools share rendering values and CPU preparation
# helpers without linking a GPU backend or runtime systems.
add_library(vkr_render_contracts STATIC src/vkr_packed_geometry.c
    src/vkr_geometry_data.c
    src/vkr_color_transfer.c
    src/vkr_subsurface.c)
vkr_configure_library(vkr_render_contracts)
target_include_directories(vkr_render_contracts PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vkr_render_contracts PUBLIC vkr_foundation)

find_program(VKR_SLANGC_EXECUTABLE NAMES slangc REQUIRED)

if(APPLE)
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h src/*.m)
enable_language(C OBJC)
elseif(WIN32)
enable_language(CXX)
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h)
list(APPEND LIB_SOURCES src/vulkan/vkr_vulkan_fsr_sdk.cpp)
endif()
list(APPEND LIB_SOURCES ${CMAKE_SOURCE_DIR}/vendor/spirv_reflect.c)

list(REMOVE_ITEM LIB_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vkr_packed_geometry.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vkr_geometry_data.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vkr_color_transfer.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vkr_subsurface.c")

# Create a static library
add_library(renderer_lib STATIC ${LIB_SOURCES})
vkr_require_declared_c_functions(renderer_lib)


# Shaders are compiled by vkr_bakery into one catalog per build tree. Entry
# tables and ordered Metal sources live in each backend's library.recipe.json;
# the action cache makes an unchanged rebuild a hash check. The renderer loads
# the catalog at runtime (src/vkr_shader_catalog.c), so no shader path is
# compiled into the library.
set(VKR_SHADER_CATALOG_DIR "${CMAKE_BINARY_DIR}/shaders")
# The player template names this catalog (player/CMakeLists.txt).
set(VKR_SHADER_CATALOG_DIR "${VKR_SHADER_CATALOG_DIR}" PARENT_SCOPE)
if(APPLE)
    set(VKR_SHADER_BACKENDS all)
else()
    set(VKR_SHADER_BACKENDS vulkan)
endif()
add_custom_target(vkr_shaders
    COMMAND vkr_bakery shaders --backend ${VKR_SHADER_BACKENDS}
            --root "${CMAKE_SOURCE_DIR}" --out "${VKR_SHADER_CATALOG_DIR}"
            --slangc "${VKR_SLANGC_EXECUTABLE}" --quiet
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Compiling shaders with vkr_bakery"
    VERBATIM)
add_dependencies(vkr_shaders vkr_bakery)
add_dependencies(renderer_lib vkr_shaders)
target_compile_definitions(renderer_lib PRIVATE
    VKR_SHADER_CATALOG_DEFAULT="${VKR_SHADER_CATALOG_DIR}")

target_compile_definitions(renderer_lib PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H)

# Find Vulkan package
find_package(Vulkan REQUIRED)
# macOS programs load the Vulkan loader through @rpath/libvulkan.1.dylib, its
# install name. Installed programs and game packages ship this file under
# that name, since the SDK directory exists only on the building machine.
if(APPLE)
    get_filename_component(VKR_VULKAN_LOADER "${Vulkan_LIBRARY}" REALPATH)
    set(VKR_VULKAN_LOADER "${VKR_VULKAN_LOADER}" CACHE INTERNAL
        "Vulkan loader that installed macOS programs ship")
endif()

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
    PRIVATE ${CMAKE_SOURCE_DIR}/vendor
    PRIVATE ${SPIRV_HEADERS_INCLUDE_DIR}
    PRIVATE ${Vulkan_INCLUDE_DIRS}
)

# Link Vulkan - use modern target if available, otherwise use libraries directly
if(TARGET Vulkan::Vulkan)
    target_link_libraries(renderer_lib PRIVATE Vulkan::Vulkan)
else()
    target_link_libraries(renderer_lib PRIVATE ${Vulkan_LIBRARIES})
endif()
target_link_libraries(renderer_lib PUBLIC vkr_render_contracts)

if(WIN32)
    include("${CMAKE_SOURCE_DIR}/cmake/vkr_fsr.cmake")
    target_compile_features(renderer_lib PRIVATE cxx_std_17)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/vulkan/vkr_vulkan_fsr_sdk.cpp"
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_link_libraries(renderer_lib PRIVATE
        ${VKR_FSR3_UPSCALER_TARGET}
        ${VKR_FSR3_VULKAN_BACKEND_TARGET})
    target_compile_definitions(renderer_lib PRIVATE VKR_HAS_FSR3_UPSCALER=1)
else()
    target_compile_definitions(renderer_lib PRIVATE VKR_HAS_FSR3_UPSCALER=0)
endif()

# Set platform-specific definitions and link libraries
if(APPLE)
    target_compile_definitions(renderer_lib PRIVATE PLATFORM_APPLE=1)
    # Link Cocoa framework if needed for macOS-specific functionality
    find_library(COCOA_LIBRARY Cocoa)
    find_library(FOUNDATION_LIBRARY Foundation)
    find_library(METAL_LIBRARY Metal)
    find_library(METALFX_LIBRARY MetalFX)
    find_library(QUARTZCORE_LIBRARY QuartzCore)
    target_link_libraries(renderer_lib PRIVATE
        ${COCOA_LIBRARY}
        ${FOUNDATION_LIBRARY}
        ${METAL_LIBRARY}
        ${METALFX_LIBRARY}
        ${QUARTZCORE_LIBRARY}
        )
elseif(WIN32)
    target_compile_definitions(renderer_lib PRIVATE PLATFORM_WINDOWS=1)
endif()

# Renderer resource, independent of editor workspaces and process CWD. Keep its
# installed build location beside the already configured native shader inputs.
set(VKR_RENDER_GRAPH_PATH "${CMAKE_CURRENT_BINARY_DIR}/resources/main.rendergraph.json")
configure_file("${CMAKE_SOURCE_DIR}/assets/render_graphs/main.rendergraph.json"
               "${VKR_RENDER_GRAPH_PATH}" COPYONLY)
target_compile_definitions(renderer_lib PRIVATE
    VKR_RENDER_GRAPH_PATH="${VKR_RENDER_GRAPH_PATH}")

# Set compile definitions for logging based on build type
target_compile_definitions(renderer_lib PRIVATE
    $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=3>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:VKR_LOG_DEFAULT_LEVEL=1>
    $<$<BOOL:${VKR_EDITOR_LOGGING}>:LOG_LEVEL=5>
    $<$<BOOL:${VKR_EDITOR_LOGGING}>:VKR_EDITOR_LOGGING=1>
    $<$<CONFIG:Release>:ASSERT_LOG=0>
    $<$<AND:$<CONFIG:RelWithDebInfo>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=3>
    $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
    $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${VKR_EDITOR_LOGGING}>>>:LOG_LEVEL=4>
    $<$<CONFIG:Debug>:ASSERT_LOG=1>
    $<$<CONFIG:Release>:VKR_ALLOCATOR_DISABLE_STATS=1>
    $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_DISABLE_STATS=1>
    $<$<CONFIG:Debug>:VKR_ALLOCATOR_DISABLE_STATS=0>
    $<$<CONFIG:Release>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    $<$<CONFIG:RelWithDebInfo>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
    $<$<CONFIG:Debug>:VKR_ALLOCATOR_ENABLE_LOGGING=0>
)

target_precompile_headers(renderer_lib PRIVATE "${CMAKE_SOURCE_DIR}/lib/src/vkr_pch.h")
