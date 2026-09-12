# Cooked formats are shared by offline producers and runtime consumers.
add_library(vkr_asset_formats STATIC
    "${CMAKE_SOURCE_DIR}/runtime/src/assets/vkr_diffuse_volume.c"
    "${CMAKE_SOURCE_DIR}/runtime/src/assets/vkr_font_cooked_decode.c"
    "${CMAKE_SOURCE_DIR}/runtime/src/assets/vkr_mesh_cooked_decode.c"
    "${CMAKE_SOURCE_DIR}/runtime/src/assets/vkr_mesh_decode.cpp")
vkr_configure_library(vkr_asset_formats)
target_include_directories(vkr_asset_formats PUBLIC "${CMAKE_SOURCE_DIR}/runtime/src")
target_link_libraries(vkr_asset_formats PUBLIC vkr_render_contracts)

# KTX-Software (KTX2 + BasisU transcoding)
set(VKR_KTX_SOFTWARE_DIR "${CMAKE_SOURCE_DIR}/vendor/ktx-software")
if(NOT EXISTS "${VKR_KTX_SOFTWARE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "KTX-Software submodule not found. Run: git submodule update --init --recursive")
endif()

set(KTX_FEATURE_DOC OFF CACHE BOOL "Disable KTX docs in renderer build." FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "Disable KTX JNI in renderer build." FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "Disable KTX python bindings in renderer build." FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests in renderer build." FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "Disable KTX tools in renderer build." FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX tools CTS in renderer build." FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "Disable KTX loadtest apps in renderer build." FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "Disable KTX GL upload path in renderer build." FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "Disable KTX Vulkan upload path in renderer build." FORCE)

set(_VKR_BUILD_SHARED_LIBS_PREV "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build KTX dependencies statically." FORCE)
# KTX's floating-point channel flags can set the high bit. Shift them as
# uint32_t; the upstream signed shift triggers UBSan during HDR cube creation.
find_package(Git REQUIRED)
set(VKR_KTX_FLOAT_PATCH "${CMAKE_SOURCE_DIR}/vendor/ktx-float-channel-shift.patch")
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${VKR_KTX_SOFTWARE_DIR}"
    apply --reverse --check "${VKR_KTX_FLOAT_PATCH}"
    RESULT_VARIABLE vkr_ktx_patch_present OUTPUT_QUIET ERROR_QUIET)
if(NOT vkr_ktx_patch_present EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${VKR_KTX_SOFTWARE_DIR}"
        apply --check "${VKR_KTX_FLOAT_PATCH}" RESULT_VARIABLE vkr_ktx_patch_check)
    if(NOT vkr_ktx_patch_check EQUAL 0)
        message(FATAL_ERROR "KTX floating-point channel patch no longer applies")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${VKR_KTX_SOFTWARE_DIR}"
        apply "${VKR_KTX_FLOAT_PATCH}" RESULT_VARIABLE vkr_ktx_patch_result)
    if(NOT vkr_ktx_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not apply KTX floating-point channel patch")
    endif()
endif()
add_subdirectory("${VKR_KTX_SOFTWARE_DIR}" "${CMAKE_BINARY_DIR}/vendor/ktx-software" EXCLUDE_FROM_ALL)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # Clang can select MSVC STL vector-algorithm helpers newer than the
    # installed runtime import library. Keep vendored BasisU self-contained.
    target_compile_definitions(ktx PRIVATE _USE_STD_VECTOR_ALGORITHMS=0)
endif()
vkr_configure_cooker_dependencies(ktx astcenc-avx2-static astcenc-sse4.1-static
    astcenc-sse2-static astcenc-neon-static astcenc-native-static)
set(BUILD_SHARED_LIBS "${_VKR_BUILD_SHARED_LIBS_PREV}" CACHE BOOL "Restore shared library default after KTX setup." FORCE)
include("${CMAKE_SOURCE_DIR}/cmake/vkr_ktx_read.cmake")


# The runtime bridge references codec decoding only. Source optimization and
# encoding are consumed by vkr_asset_cooking below tools/.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
if(VKR_BUILD_TOOLS)
    add_subdirectory("${CMAKE_SOURCE_DIR}/vendor/meshoptimizer"
                     "${CMAKE_BINARY_DIR}/vendor/meshoptimizer" EXCLUDE_FROM_ALL)
    vkr_configure_cooker_dependencies(meshoptimizer)
endif()
add_library(vkr_mesh_codecs STATIC
    "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/indexcodec.cpp"
    "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/vertexcodec.cpp"
    "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/indexanalyzer.cpp"
    "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src/allocator.cpp")
target_include_directories(vkr_mesh_codecs PUBLIC "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer/src")
target_compile_features(vkr_mesh_codecs PRIVATE cxx_std_11)
# Upstream codec translation units also contain encoder entry points. Discard
# unreferenced functions in Release consumers so decoding does not ship them.
if(APPLE)
    target_link_options(vkr_mesh_codecs INTERFACE "$<$<CONFIG:Release>:LINKER:-dead_strip>")
elseif(WIN32)
    if(MSVC)
        target_compile_options(vkr_mesh_codecs PRIVATE "$<$<CONFIG:Release>:/Gy>")
    else()
        target_compile_options(vkr_mesh_codecs PRIVATE "$<$<CONFIG:Release>:-ffunction-sections>")
    endif()
    target_link_options(vkr_mesh_codecs INTERFACE "$<$<CONFIG:Release>:LINKER:/OPT:REF>")
endif()
target_link_libraries(vkr_asset_formats PRIVATE vkr_mesh_codecs)
target_compile_features(vkr_asset_formats PRIVATE cxx_std_11)

add_library(vkr_image_decode STATIC "${CMAKE_SOURCE_DIR}/runtime/src/assets/stb_image_impl.c")
vkr_configure_library(vkr_image_decode)
vkr_configure_cooker_target(vkr_image_decode)
target_include_directories(vkr_image_decode PRIVATE "${CMAKE_SOURCE_DIR}/vendor")
