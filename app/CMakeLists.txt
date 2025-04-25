# Discover all source files in app/src
file(GLOB_RECURSE APP_SOURCES src/*.c src/*.h)

# Create the main application executable
add_executable(vulkan_renderer ${APP_SOURCES})

# Link against the renderer_lib
target_link_libraries(vulkan_renderer PRIVATE renderer_lib)

# Include directories
target_include_directories(vulkan_renderer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE ${CMAKE_SOURCE_DIR}/lib/src
)

# Set up installation rules if needed
# install(TARGETS vulkan_renderer DESTINATION bin) 