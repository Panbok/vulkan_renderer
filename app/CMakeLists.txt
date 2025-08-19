cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer_app)

# Discover all source files in app/src
file(GLOB_RECURSE APP_SOURCES src/*.c src/*.h)

# Create the main application executable
add_executable(vulkan_renderer ${APP_SOURCES})

if(CMAKE_GENERATOR MATCHES "Visual Studio")
    set_property(TARGET vulkan_renderer PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/")
endif()
if(CMAKE_GENERATOR MATCHES "Xcode")
    set_property(TARGET vulkan_renderer PROPERTY XCODE_GENERATE_SCHEME TRUE)
    set_property(TARGET vulkan_renderer PROPERTY XCODE_SCHEME_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()

# Platform-specific optimization flags
if(MSVC)
    target_compile_options(vulkan_renderer PRIVATE /arch:AVX2)
else()
    target_compile_options(vulkan_renderer PRIVATE -march=native)
endif()
# Link against the renderer_lib
target_link_libraries(vulkan_renderer PRIVATE renderer_lib)

# Include directories
target_include_directories(vulkan_renderer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_SOURCE_DIR}/lib/src
)

# Set compile definitions for logging based on build type
target_compile_definitions(vulkan_renderer PRIVATE
    $<$<CONFIG:Release>:LOG_LEVEL=1>
    $<$<CONFIG:Release>:ASSERT_LOG=0>
    $<$<CONFIG:RelWithDebInfo>:LOG_LEVEL=3>
    $<$<CONFIG:RelWithDebInfo>:ASSERT_LOG=0>
    $<$<CONFIG:Debug>:LOG_LEVEL=4>
    $<$<CONFIG:Debug>:ASSERT_LOG=1>
)

# Set up installation rules if needed
# install(TARGETS vulkan_renderer DESTINATION bin) 