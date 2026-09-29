cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer_tester)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS src/*.c)
add_executable(vulkan_renderer_tester ${TEST_SOURCES})
target_sources(vulkan_renderer_tester PRIVATE
    "${CMAKE_SOURCE_DIR}/editor/src/editor_project_store.c"
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
target_link_libraries(vulkan_renderer_tester PRIVATE vkr_sample_runtime vkr_script_fps vkr_harness_core vkr_collision_cooking ktx Vulkan::Vulkan)

# Script modules the host reload tests load (ADR-079): one source, four
# variants, linked as MODULE libraries so an engine symbol fails the link.
foreach(variant 1 2 3 4)
    add_library(vkr_test_reload_probe_${variant} MODULE scripts/reload_probe.c)
    target_compile_definitions(vkr_test_reload_probe_${variant} PRIVATE
        RELOAD_VARIANT=${variant})
    target_include_directories(vkr_test_reload_probe_${variant} PRIVATE
        "${CMAKE_SOURCE_DIR}/runtime/src" "${CMAKE_SOURCE_DIR}/lib/src"
        "${CMAKE_SOURCE_DIR}/renderer/src")
    set_target_properties(vkr_test_reload_probe_${variant} PROPERTIES
        C_VISIBILITY_PRESET hidden)
    add_dependencies(vulkan_renderer_tester vkr_test_reload_probe_${variant})
    target_compile_definitions(vulkan_renderer_tester PRIVATE
        VKR_TEST_RELOAD_PROBE_${variant}="$<TARGET_FILE:vkr_test_reload_probe_${variant}>")
endforeach()

target_include_directories(vulkan_renderer_tester
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/app/src
        ${CMAKE_SOURCE_DIR}/runtime/src
        ${CMAKE_SOURCE_DIR}/vendor
        ${CMAKE_SOURCE_DIR}/tools
)
