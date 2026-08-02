cmake_minimum_required(VERSION 3.27)

project(vulkan_renderer)

# Set explicit C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}/")

option(VKR_ENABLE_SANITIZERS "Enable ASan/UBSan (Clang/GCC only)" OFF)
option(VKR_METRICS_ENABLED "Compile renderer metrics instrumentation" ON)

if(VKR_METRICS_ENABLED)
    add_compile_definitions(VKR_METRICS_ENABLED=1)
else()
    add_compile_definitions(VKR_METRICS_ENABLED=0)
endif()

if(MSVC)
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Zi -W4 /FC")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} ${SAN_FLAGS}")
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

if(VKR_ENABLE_SANITIZERS AND NOT MSVC)
    set(_VKR_SAN_FLAGS "-fsanitize=address,undefined" "-fno-omit-frame-pointer")
    add_compile_options(${_VKR_SAN_FLAGS})
    add_link_options(-fsanitize=address,undefined)

    # Windows note: ASan generally doesn't work with the MSVC *Debug* CRT.
    # Use RelWithDebInfo (or Release) when VKR_ENABLE_SANITIZERS=ON.
    if(WIN32 AND (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(FATAL_ERROR "VKR_ENABLE_SANITIZERS=ON on Windows+Clang is not supported with CMAKE_BUILD_TYPE=Debug (Debug CRT). Use -DCMAKE_BUILD_TYPE=RelWithDebInfo instead.")
        endif()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
endif()

# TODO: Add optimization flags for release build like -O3, -flto, -ffast-math, etc.

# Add subdirectories for each component
add_subdirectory(lib)
add_subdirectory(tools)
add_subdirectory(app)
add_subdirectory(tests)
