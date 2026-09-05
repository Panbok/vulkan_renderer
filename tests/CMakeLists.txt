cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer_tester)

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS src/*.c)
add_executable(vulkan_renderer_tester ${TEST_SOURCES})
target_sources(vulkan_renderer_tester PRIVATE
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

if(MSVC)
    target_compile_options(vulkan_renderer_tester PRIVATE /arch:AVX2)
else()
    target_compile_options(vulkan_renderer_tester PRIVATE -march=native)
endif()

# The harness core exports renderer_lib transitively.
target_link_libraries(vulkan_renderer_tester PRIVATE vkr_harness_core ktx)
if(TARGET Vulkan::Vulkan)
    target_link_libraries(vulkan_renderer_tester PRIVATE Vulkan::Vulkan)
endif()

target_include_directories(vulkan_renderer_tester
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/app/src
        ${CMAKE_SOURCE_DIR}/runtime/src
        ${CMAKE_SOURCE_DIR}/tools
)
