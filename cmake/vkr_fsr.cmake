include_guard(GLOBAL)

if(NOT WIN32)
    return()
endif()

find_package(Git REQUIRED)

set(VKR_FSR_SDK_TAG "v1.1.4")
set(VKR_FSR_SDK_COMMIT "c6efa6bf7f2027b3ec94f28578bb5965eabb9e55")
set(VKR_FSR_SDK_REPOSITORY
    "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git")
set(VKR_FSR_SDK_SOURCE_DIR
    "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk-1.1.4-src")
set(VKR_FSR_SDK_BINARY_DIR
    "${CMAKE_BINARY_DIR}/_deps/fidelityfx-sdk-1.1.4-build")
set(VKR_FSR_SDK_LUMA_PATCH
    "${CMAKE_SOURCE_DIR}/vendor/fidelityfx-sdk-1.1.4-luma-history-format.patch")

if(NOT EXISTS "${VKR_FSR_SDK_SOURCE_DIR}/sdk/CMakeLists.txt")
    if(EXISTS "${VKR_FSR_SDK_SOURCE_DIR}")
        message(FATAL_ERROR
            "Incomplete FidelityFX SDK checkout at ${VKR_FSR_SDK_SOURCE_DIR}. "
            "Remove that build-directory path and configure again.")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" clone --depth 1 --filter=blob:none
                --sparse --branch "${VKR_FSR_SDK_TAG}"
                "${VKR_FSR_SDK_REPOSITORY}" "${VKR_FSR_SDK_SOURCE_DIR}"
        RESULT_VARIABLE vkr_fsr_clone_result)
    if(NOT vkr_fsr_clone_result EQUAL 0)
        message(FATAL_ERROR "Could not sparse-clone FidelityFX SDK ${VKR_FSR_SDK_TAG}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${VKR_FSR_SDK_SOURCE_DIR}"
                sparse-checkout set sdk
        RESULT_VARIABLE vkr_fsr_sparse_result)
    if(NOT vkr_fsr_sparse_result EQUAL 0)
        message(FATAL_ERROR "Could not select FidelityFX SDK files")
    endif()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${VKR_FSR_SDK_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE vkr_fsr_actual_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE vkr_fsr_revision_result)
if(NOT vkr_fsr_revision_result EQUAL 0 OR
   NOT vkr_fsr_actual_commit STREQUAL VKR_FSR_SDK_COMMIT)
    message(FATAL_ERROR
        "FidelityFX SDK revision must be ${VKR_FSR_SDK_COMMIT}; got ${vkr_fsr_actual_commit}")
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${VKR_FSR_SDK_SOURCE_DIR}" apply --reverse --check
            "${VKR_FSR_SDK_LUMA_PATCH}"
    RESULT_VARIABLE vkr_fsr_patch_present)
if(NOT vkr_fsr_patch_present EQUAL 0)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${VKR_FSR_SDK_SOURCE_DIR}" apply --check
                "${VKR_FSR_SDK_LUMA_PATCH}"
        RESULT_VARIABLE vkr_fsr_patch_check)
    if(NOT vkr_fsr_patch_check EQUAL 0)
        message(FATAL_ERROR "FidelityFX SDK luma-history patch no longer applies")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${VKR_FSR_SDK_SOURCE_DIR}" apply
                "${VKR_FSR_SDK_LUMA_PATCH}"
        RESULT_VARIABLE vkr_fsr_patch_result)
    if(NOT vkr_fsr_patch_result EQUAL 0)
        message(FATAL_ERROR "Could not apply FidelityFX SDK luma-history patch")
    endif()
endif()

# The upstream CMake integration builds all available components unless these
# cache entries are explicit. FSR3UPSCALER provides temporal upscaling only;
# FSR3 also enables frame interpolation and optical flow.
set(FFX_API_BACKEND VK_X64 CACHE STRING "" FORCE)
set(FFX_API_VK ON CACHE BOOL "" FORCE)
set(FFX_FSR3 OFF CACHE BOOL "" FORCE)
set(FFX_FSR3UPSCALER ON CACHE BOOL "" FORCE)
set(FFX_FI OFF CACHE BOOL "" FORCE)
set(FFX_OF OFF CACHE BOOL "" FORCE)
set(FFX_ALL OFF CACHE BOOL "" FORCE)
set(FFX_BUILD_AS_DLL OFF CACHE BOOL "" FORCE)
set(FFX_AUTO_COMPILE_SHADERS ON CACHE BOOL "" FORCE)

# VKR's wrapper already selected compiler, generator and CRT. The standalone
# SDK toolchain must not overwrite the parent's Ninja platform cache.
set(_TOOLCHAIN_ ON)
set(FFX_PLATFORM_NAME x64)
add_subdirectory("${VKR_FSR_SDK_SOURCE_DIR}/sdk" "${VKR_FSR_SDK_BINARY_DIR}"
                 EXCLUDE_FROM_ALL)
unset(_TOOLCHAIN_)
unset(FFX_PLATFORM_NAME)

get_target_property(vkr_fsr_backend_sources ffx_backend_vk_x64 SOURCES)
list(FILTER vkr_fsr_backend_sources EXCLUDE REGEX "FrameInterpolationSwapchain/")
set_property(TARGET ffx_backend_vk_x64 PROPERTY SOURCES "${vkr_fsr_backend_sources}")
target_compile_definitions(ffx_backend_vk_x64 PRIVATE
    VKR_FSR_FP32_ONLY=1 VKR_FSR_UPSCALER_ONLY=1)
foreach(vkr_fsr_target ffx_backend_vk_x64 ffx_fsr3upscaler_x64)
    set_target_properties(${vkr_fsr_target} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${VKR_FSR_SDK_BINARY_DIR}/lib"
        ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${VKR_FSR_SDK_BINARY_DIR}/lib/Debug"
        ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${VKR_FSR_SDK_BINARY_DIR}/lib/Release"
        ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${VKR_FSR_SDK_BINARY_DIR}/lib/RelWithDebInfo")
endforeach()

set(VKR_FSR3_UPSCALER_TARGET ffx_fsr3upscaler_x64)
set(VKR_FSR3_VULKAN_BACKEND_TARGET ffx_backend_vk_x64)
