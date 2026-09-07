# Runtime KTX/Basis decoding uses upstream reader sources and compile settings.
# The upstream Apple ktx_read POST_BUILD command merges TARGET_FILE:ktx into
# ktx_read, replacing it with the writer and adding the full ASTC encoder.
# Keep that packaging command on the upstream tool target only.
include_guard(GLOBAL)

if(NOT TARGET ktx_read)
    message(FATAL_ERROR "Include vkr_ktx_read.cmake after KTX-Software setup")
endif()

get_target_property(_vkr_ktx_source_dir ktx_read SOURCE_DIR)
get_target_property(_vkr_ktx_reader_sources ktx_read SOURCES)
set(_vkr_ktx_runtime_sources)
set(_vkr_ktx_astc_utility_found FALSE)
foreach(_vkr_ktx_source IN LISTS _vkr_ktx_reader_sources)
    # Static readers need compilation units, not upstream public-header or
    # conditional DLL export-definition entries.
    if(NOT _vkr_ktx_source MATCHES "\\.(c|cc|cpp|cxx)$")
        continue()
    endif()
    if(_vkr_ktx_source MATCHES "\\$<")
        message(FATAL_ERROR
            "KTX reader gained a conditional compilation unit; review runtime source selection: ${_vkr_ktx_source}")
    endif()
    get_filename_component(_vkr_ktx_source "${_vkr_ktx_source}" ABSOLUTE
        BASE_DIR "${_vkr_ktx_source_dir}")
    if(_vkr_ktx_source STREQUAL "${_vkr_ktx_source_dir}/lib/astc_codec.cpp")
        # Standalone ASTC encode/decode utilities have no runtime caller and
        # require astcenc. Basis-to-ASTC transcoding stays in basisu_transcoder;
        # native ASTC KTX payloads are loaded and uploaded without this utility.
        set(_vkr_ktx_astc_utility_found TRUE)
        continue()
    endif()
    list(APPEND _vkr_ktx_runtime_sources "${_vkr_ktx_source}")
endforeach()
if(NOT _vkr_ktx_astc_utility_found OR NOT _vkr_ktx_runtime_sources)
    message(FATAL_ERROR "KTX reader source layout changed; review runtime source selection")
endif()

add_library(vkr_ktx_read STATIC ${_vkr_ktx_runtime_sources})
# Copy values at configuration time. Linking ktx_read or referring to its output
# would restore the upstream archive-merging dependency. Preserve its feature,
# platform, SIMD and floating-point compile policy without its build commands.
foreach(_vkr_ktx_property IN ITEMS
        COMPILE_DEFINITIONS COMPILE_OPTIONS COMPILE_FEATURES INCLUDE_DIRECTORIES
        INTERFACE_COMPILE_DEFINITIONS INTERFACE_COMPILE_OPTIONS
        INTERFACE_COMPILE_FEATURES INTERFACE_INCLUDE_DIRECTORIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES POSITION_INDEPENDENT_CODE
        C_STANDARD C_STANDARD_REQUIRED C_EXTENSIONS
        CXX_STANDARD CXX_STANDARD_REQUIRED CXX_EXTENSIONS MSVC_RUNTIME_LIBRARY)
    get_target_property(_vkr_ktx_value ktx_read ${_vkr_ktx_property})
    if(NOT _vkr_ktx_value MATCHES "-NOTFOUND$")
        set_property(TARGET vkr_ktx_read PROPERTY ${_vkr_ktx_property}
            "${_vkr_ktx_value}")
    endif()
endforeach()
# Reader sources must never compile their guarded writer paths.
target_compile_definitions(vkr_ktx_read PRIVATE KTX_FEATURE_WRITE=0)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_definitions(vkr_ktx_read PRIVATE _USE_STD_VECTOR_ALGORITHMS=0)
endif()
find_package(Threads REQUIRED)
target_link_libraries(vkr_ktx_read PRIVATE Threads::Threads)

unset(_vkr_ktx_source_dir)
unset(_vkr_ktx_reader_sources)
unset(_vkr_ktx_runtime_sources)
unset(_vkr_ktx_astc_utility_found)
unset(_vkr_ktx_source)
unset(_vkr_ktx_property)
unset(_vkr_ktx_value)
