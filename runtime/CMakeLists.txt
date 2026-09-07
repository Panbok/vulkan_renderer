file(GLOB_RECURSE VKR_RUNTIME_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h)
# Shared reader targets also serve the offline tools without linking runtime.
list(FILTER VKR_RUNTIME_SOURCES EXCLUDE REGEX "/src/assets/")
list(REMOVE_ITEM VKR_RUNTIME_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vkr_sample_runtime.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vkr_sample_runtime.h")
if(APPLE)
    enable_language(OBJC)
    file(GLOB VKR_RUNTIME_OBJC CONFIGURE_DEPENDS src/platform/*.m)
    list(APPEND VKR_RUNTIME_SOURCES ${VKR_RUNTIME_OBJC})
endif()
add_library(vkr_runtime STATIC ${VKR_RUNTIME_SOURCES})
vkr_configure_library(vkr_runtime)
target_include_directories(vkr_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(vkr_runtime PRIVATE "${CMAKE_SOURCE_DIR}/vendor")
target_link_libraries(vkr_runtime PUBLIC renderer_lib vkr_asset_formats PRIVATE vkr_ktx_read vkr_image_decode)
if(APPLE)
    target_link_libraries(vkr_runtime PRIVATE "-framework Cocoa" "-framework GameController")
elseif(WIN32)
    target_link_libraries(vkr_runtime PRIVATE Xinput)
endif()

# Demo controls and presentation policy are optional consumers of the runtime.
if(VKR_BUILD_APP OR VKR_BUILD_EDITOR OR VKR_BUILD_TESTS)
    add_library(vkr_sample_runtime STATIC src/vkr_sample_runtime.c)
    vkr_configure_library(vkr_sample_runtime)
    target_link_libraries(vkr_sample_runtime PUBLIC vkr_runtime)
endif()
