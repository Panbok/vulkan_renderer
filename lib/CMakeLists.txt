file(GLOB_RECURSE VKR_FOUNDATION_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h)
add_library(vkr_foundation STATIC ${VKR_FOUNDATION_SOURCES})
vkr_configure_library(vkr_foundation)
target_include_directories(vkr_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(vkr_foundation PUBLIC
    $<$<PLATFORM_ID:Darwin>:PLATFORM_APPLE=1>
    $<$<PLATFORM_ID:Windows>:PLATFORM_WINDOWS=1>)
if(APPLE)
    target_link_libraries(vkr_foundation PRIVATE "-framework CoreGraphics")
elseif(WIN32)
    target_link_libraries(vkr_foundation PRIVATE user32 winmm psapi)
endif()
