cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer)

# Set explicit C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/")

option(VKR_ENABLE_SANITIZERS
       "Enable ASan/UBSan in every configuration (Clang/GCC only)" OFF)
set(_VKR_DEBUG_SANITIZERS_DEFAULT ON)
if(WIN32)
    # Clang ASan is incompatible with the Windows Debug CRT, and MSVC does not
    # provide the requested ASan+UBSan pair.
    set(_VKR_DEBUG_SANITIZERS_DEFAULT OFF)
endif()
option(VKR_ENABLE_DEBUG_SANITIZERS
       "Enable ASan/UBSan in Debug configurations (Clang/GCC only)"
       ${_VKR_DEBUG_SANITIZERS_DEFAULT})
unset(_VKR_DEBUG_SANITIZERS_DEFAULT)
option(VKR_METRICS_ENABLED "Compile renderer metrics instrumentation" ON)

if(VKR_METRICS_ENABLED)
    add_compile_definitions(VKR_METRICS_ENABLED=1)
else()
    add_compile_definitions(VKR_METRICS_ENABLED=0)
endif()

if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Zi -W4 /FC")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /arch:AVX2")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
endif()

# An undeclared C function is an ABI bug, not a tolerable warning: the compiler
# otherwise assumes a signature that can bind to an unrelated same-named symbol
# and fail only at runtime. Apply this to VKR-owned targets explicitly so the
# policy does not leak into third-party subdirectories.
function(vkr_require_declared_c_functions target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/we4013>
        )
    elseif(CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang|GNU)$")
        target_compile_options(${target} PRIVATE
            $<$<COMPILE_LANGUAGE:C,OBJC>:-Werror=implicit-function-declaration>
        )
    endif()
endfunction()

if((VKR_ENABLE_SANITIZERS OR VKR_ENABLE_DEBUG_SANITIZERS) AND NOT MSVC)
    set(_VKR_SAN_FLAGS "-fsanitize=address,undefined" "-fno-omit-frame-pointer")

    if(VKR_ENABLE_SANITIZERS)
        add_compile_options(${_VKR_SAN_FLAGS})
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "ASan/UBSan enabled for every configuration")
    else()
        add_compile_options(
            "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
            "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        )
        add_link_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
        if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(STATUS "ASan/UBSan enabled for Debug configurations")
        else()
            message(STATUS "ASan/UBSan inactive for ${CMAKE_BUILD_TYPE}; enabled by default for Debug")
        endif()
    endif()

    # Windows note: ASan generally doesn't work with the MSVC *Debug* CRT.
    # Use RelWithDebInfo (or Release) when VKR_ENABLE_SANITIZERS=ON.
    if(WIN32 AND (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
        if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(FATAL_ERROR "ASan/UBSan on Windows+Clang is not supported with the Debug CRT. Disable VKR_ENABLE_DEBUG_SANITIZERS or use VKR_ENABLE_SANITIZERS=ON with RelWithDebInfo.")
        endif()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
elseif((VKR_ENABLE_SANITIZERS OR VKR_ENABLE_DEBUG_SANITIZERS) AND MSVC)
    message(WARNING "ASan/UBSan requested but this project only enables the pair with Clang/GCC")
endif()
unset(_VKR_SAN_FLAGS)

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(runtime)
add_subdirectory(app)
add_subdirectory(editor)
add_subdirectory(tests)
