cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer_tester)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS src/*.c)
add_executable(vulkan_renderer_tester ${TEST_SOURCES})
target_sources(vulkan_renderer_tester PRIVATE
    src/atmosphere_bake_dark_tests.cpp
    src/lightmap_bake_tests.cpp
    "${CMAKE_SOURCE_DIR}/editor/src/editor_project_store.c"
    "${CMAKE_SOURCE_DIR}/editor/src/editor_label_sight.c"
    "${CMAKE_SOURCE_DIR}/app/src/app_ui.c"
    "${CMAKE_SOURCE_DIR}/app/src/debug_overlay.c"
)
vkr_require_declared_c_functions(vulkan_renderer_tester)

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_property(TARGET vulkan_renderer_tester PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endif()
if(CMAKE_GENERATOR MATCHES "Xcode")
    set_property(TARGET vulkan_renderer_tester PROPERTY XCODE_GENERATE_SCHEME TRUE)
    set_property(TARGET vulkan_renderer_tester PROPERTY XCODE_SCHEME_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# Test assertions are the oracle, so they stay active in every configuration.
# Tests that check release-only library behavior use VKR_TEST_LIBRARIES_NDEBUG,
# which records whether the linked libraries were compiled with NDEBUG.
if(MSVC)
    target_compile_options(vulkan_renderer_tester PRIVATE /UNDEBUG)
else()
    target_compile_options(vulkan_renderer_tester PRIVATE -UNDEBUG)
endif()
# Allocator statistics assertions must see the libraries' setting: only Debug
# libraries record statistics (vkr_configure_library).
target_compile_definitions(vulkan_renderer_tester PRIVATE
    $<IF:$<CONFIG:Debug>,VKR_TEST_LIBRARIES_NDEBUG=0,VKR_TEST_LIBRARIES_NDEBUG=1>
    $<IF:$<CONFIG:Debug>,VKR_ALLOCATOR_DISABLE_STATS=0,VKR_ALLOCATOR_DISABLE_STATS=1>)

# The harness core exports renderer_lib transitively.
find_package(Vulkan REQUIRED)
# The editor project store writes reflection probes through the Bakery JSON
# tree (ADR-103).
target_link_libraries(vulkan_renderer_tester PRIVATE vkr_sample_runtime vkr_script_fps vkr_harness_core vkr_collision_cooking vkr_bake_core vkr_bakery_json ktx Vulkan::Vulkan)
# The network transport tests also call libsodium to check Noise vectors.
target_link_libraries(vulkan_renderer_tester PRIVATE vkr_net vkr_sodium)
# The depot tests use libktx's zstd through the depot codec adapter.
target_link_libraries(vulkan_renderer_tester PRIVATE vkr_depot_lib)
target_sources(vulkan_renderer_tester PRIVATE
    "${CMAKE_SOURCE_DIR}/tools/depot/vkr_depot_zstd.c")
target_include_directories(vulkan_renderer_tester PRIVATE
    "${CMAKE_SOURCE_DIR}/vendor/ktx-software/external/basisu/zstd")

# Script modules the host reload tests load (ADR-079): one source, four
# variants, linked as MODULE libraries so an engine symbol fails the link.
foreach(variant 1 2 3 4)
    add_library(vkr_test_reload_probe_${variant} MODULE scripts/reload_probe.c)
    target_compile_definitions(vkr_test_reload_probe_${variant} PRIVATE
        RELOAD_VARIANT=${variant})
    # Only the staged script SDK, as Bakery builds them: a header missing
    # from its list fails this build.
    target_include_directories(vkr_test_reload_probe_${variant} PRIVATE
        "${VKR_SCRIPT_SDK_DIR}")
    add_dependencies(vkr_test_reload_probe_${variant} vkr_script_sdk)
    set_target_properties(vkr_test_reload_probe_${variant} PROPERTIES
        C_VISIBILITY_PRESET hidden)
    # Reload closes these modules and opens their next generation. With
    # ASan-instrumented images, AppleClang's runtime faulted inside its global
    # lookup (GetGlobalsForAddress) after a reload and aborted the suite. The
    # host under test stays instrumented.
    if(APPLE)
        target_compile_options(vkr_test_reload_probe_${variant} PRIVATE
            -fno-sanitize=address)
    endif()
    add_dependencies(vulkan_renderer_tester vkr_test_reload_probe_${variant})
    target_compile_definitions(vulkan_renderer_tester PRIVATE
        VKR_TEST_RELOAD_PROBE_${variant}="$<TARGET_FILE:vkr_test_reload_probe_${variant}>")
endforeach()

# A project library the host's project tests load: one source, four
# variants listing their modules through vkr_project_modules.
foreach(variant 1 2 3 4)
    add_library(vkr_test_project_probe_${variant} MODULE scripts/project_probe.c)
    target_compile_definitions(vkr_test_project_probe_${variant} PRIVATE
        PROJECT_VARIANT=${variant})
    target_include_directories(vkr_test_project_probe_${variant} PRIVATE
        "${VKR_SCRIPT_SDK_DIR}")
    add_dependencies(vkr_test_project_probe_${variant} vkr_script_sdk)
    set_target_properties(vkr_test_project_probe_${variant} PROPERTIES
        C_VISIBILITY_PRESET hidden)
    if(APPLE)
        target_compile_options(vkr_test_project_probe_${variant} PRIVATE
            -fno-sanitize=address)
    endif()
    add_dependencies(vulkan_renderer_tester vkr_test_project_probe_${variant})
    target_compile_definitions(vulkan_renderer_tester PRIVATE
        VKR_TEST_PROJECT_PROBE_${variant}="$<TARGET_FILE:vkr_test_project_probe_${variant}>")
endforeach()

target_include_directories(vulkan_renderer_tester
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/app/src
        ${CMAKE_SOURCE_DIR}/runtime/src
        ${CMAKE_SOURCE_DIR}/vendor
        ${CMAKE_SOURCE_DIR}/tools
)

# Material tests read fixture textures the build cooks on this host (ADR-012).
include("${CMAKE_SOURCE_DIR}/cmake/vkr_engine_content.cmake")
add_dependencies(vulkan_renderer_tester vkr_engine_textures)
