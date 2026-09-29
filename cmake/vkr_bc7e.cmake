include_guard(GLOBAL)

# BC7 and BC5 encoders for `bc` derived textures (vendor/bc7enc_rdo.md).
# bc7e is ISPC source: x86-64 hosts compile it with a pinned ISPC, selecting
# SSE2 or SSE4 at run time. Four-lane SSE4 encoded Bistro's colours 1.55 times
# as fast as eight-lane AVX2 on a Ryzen 5 2600 (83 against 53 Mpx/s, PSNR
# 52.276 against 52.275 dB): Zen+ splits 256-bit operations and microcodes
# gathers, and wider gangs diverge more. Without fused multiply-adds every x86
# host also encodes the same bytes (ADR-077). Other architectures build the
# packer without it, and the `bc` encoding is rejected there.
set(VKR_BC7E_AVAILABLE OFF)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    message(STATUS "bc7e: ${CMAKE_SYSTEM_PROCESSOR} host; the bc encoding is unavailable")
    return()
endif()

set(VKR_ISPC_VERSION "1.31.0")
set(VKR_ISPC_EXECUTABLE "" CACHE FILEPATH
    "ISPC ${VKR_ISPC_VERSION} compiler; empty downloads the pinned release")
if(NOT VKR_ISPC_EXECUTABLE)
    set(_vkr_ispc_url
        "https://github.com/ispc/ispc/releases/download/v${VKR_ISPC_VERSION}")
    if(WIN32)
        set(_vkr_ispc_name "ispc-v${VKR_ISPC_VERSION}-windows")
        set(_vkr_ispc_archive "${_vkr_ispc_name}.zip")
        set(_vkr_ispc_sha256
            "9a18793800b91d5be7b851513672cd9a81a985a5a5dfec5611c2318e8ad4140a")
        set(_vkr_ispc_program "ispc.exe")
    elseif(APPLE)
        set(_vkr_ispc_name "ispc-v${VKR_ISPC_VERSION}-macOS.universal")
        set(_vkr_ispc_archive "${_vkr_ispc_name}.tar.gz")
        set(_vkr_ispc_sha256
            "98c47aa9543f9f99f2c6778181d5c2f247c01c82653dd62f0c83e1fba923800e")
        set(_vkr_ispc_program "ispc")
    else()
        set(_vkr_ispc_name "ispc-v${VKR_ISPC_VERSION}-linux")
        set(_vkr_ispc_archive "${_vkr_ispc_name}.tar.gz")
        set(_vkr_ispc_sha256
            "d74089c835e10fd7e2c4b9225ced38b87d1fb53d35c7ceabd48cdf035da11b11")
        set(_vkr_ispc_program "ispc")
    endif()
    set(_vkr_ispc_root "${CMAKE_BINARY_DIR}/_deps/ispc-${VKR_ISPC_VERSION}")
    set(_vkr_ispc_path "${_vkr_ispc_root}/${_vkr_ispc_name}/bin/${_vkr_ispc_program}")
    if(NOT EXISTS "${_vkr_ispc_path}")
        set(_vkr_ispc_download "${_vkr_ispc_root}/${_vkr_ispc_archive}")
        message(STATUS "bc7e: downloading ISPC ${VKR_ISPC_VERSION}")
        file(DOWNLOAD "${_vkr_ispc_url}/${_vkr_ispc_archive}" "${_vkr_ispc_download}"
             EXPECTED_HASH SHA256=${_vkr_ispc_sha256}
             STATUS _vkr_ispc_status)
        list(GET _vkr_ispc_status 0 _vkr_ispc_code)
        if(NOT _vkr_ispc_code EQUAL 0)
            message(FATAL_ERROR
                "Could not download ISPC ${VKR_ISPC_VERSION}: ${_vkr_ispc_status}. "
                "Set VKR_ISPC_EXECUTABLE to an installed ISPC ${VKR_ISPC_VERSION}.")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_vkr_ispc_download}"
             DESTINATION "${_vkr_ispc_root}")
        file(REMOVE "${_vkr_ispc_download}")
    endif()
    set(VKR_ISPC_EXECUTABLE "${_vkr_ispc_path}" CACHE FILEPATH
        "ISPC ${VKR_ISPC_VERSION} compiler; empty downloads the pinned release" FORCE)
endif()

set(_vkr_bc7e_source "${CMAKE_SOURCE_DIR}/vendor/bc7enc_rdo/bc7e.ispc")
set(_vkr_bc7e_dir "${CMAKE_BINARY_DIR}/bc7e")
set(_vkr_bc7e_ext "${CMAKE_CXX_OUTPUT_EXTENSION}")
set(_vkr_bc7e_objects
    "${_vkr_bc7e_dir}/bc7e${_vkr_bc7e_ext}"
    "${_vkr_bc7e_dir}/bc7e_sse2${_vkr_bc7e_ext}"
    "${_vkr_bc7e_dir}/bc7e_sse4${_vkr_bc7e_ext}")
set(_vkr_bc7e_flags -O2 --arch=x86-64 --target=sse2-i32x4,sse4-i32x4
    --opt=disable-assertions --woff)
if(NOT WIN32)
    list(APPEND _vkr_bc7e_flags --pic)
endif()
add_custom_command(
    OUTPUT ${_vkr_bc7e_objects} "${_vkr_bc7e_dir}/bc7e_ispc.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_vkr_bc7e_dir}"
    COMMAND "${VKR_ISPC_EXECUTABLE}" ${_vkr_bc7e_flags} "${_vkr_bc7e_source}"
            -o "${_vkr_bc7e_dir}/bc7e${_vkr_bc7e_ext}"
            -h "${_vkr_bc7e_dir}/bc7e_ispc.h"
    DEPENDS "${_vkr_bc7e_source}"
    COMMENT "Compiling bc7e.ispc"
    VERBATIM)

add_library(vkr_bc7e STATIC
    "${CMAKE_SOURCE_DIR}/vendor/bc7enc_rdo/rgbcx.cpp"
    ${_vkr_bc7e_objects}
    "${_vkr_bc7e_dir}/bc7e_ispc.h")
set_source_files_properties(${_vkr_bc7e_objects} PROPERTIES
    EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_compile_features(vkr_bc7e PRIVATE cxx_std_17)
target_compile_definitions(vkr_bc7e PUBLIC RGBCX_USE_SMALLER_TABLES=1)
target_include_directories(vkr_bc7e SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/vendor/bc7enc_rdo"
    "${_vkr_bc7e_dir}")
set(VKR_BC7E_AVAILABLE ON)
