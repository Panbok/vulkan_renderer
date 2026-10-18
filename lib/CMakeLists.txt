file(GLOB_RECURSE VKR_FOUNDATION_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h)
add_library(vkr_foundation STATIC ${VKR_FOUNDATION_SOURCES})
vkr_configure_library(vkr_foundation)
target_include_directories(vkr_foundation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(vkr_foundation PUBLIC
    $<$<PLATFORM_ID:Darwin>:PLATFORM_APPLE=1>
    $<$<PLATFORM_ID:Windows>:PLATFORM_WINDOWS=1>)
if(APPLE)
    # Telemetry reads the power source through IOKit and low-power mode
    # through the Objective-C runtime (vkr_platform_macos.c).
    target_link_libraries(vkr_foundation PRIVATE "-framework CoreGraphics"
        "-framework IOKit" "-framework CoreFoundation" objc)
elseif(WIN32)
    # Local sockets (vkr_local_socket_windows.c) use Winsock and file DACLs;
    # UDP broadcast lists interfaces through the IP helper.
    target_link_libraries(vkr_foundation PRIVATE user32 winmm psapi ws2_32
        advapi32 iphlpapi bcrypt)
endif()
