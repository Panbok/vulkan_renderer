include_guard(GLOBAL)

# AMD GPU Performance API (GPUPerfAPI, MIT) for the Vulkan backend's
# per-pass hardware counters on AMD GPUs (ADR-130). Windows x86-64 only: the
# pinned release's headers compile into the backend, and its Vulkan DLL sits
# beside the executables, opened only when a recording asks for counters on
# an AMD device. Other hosts and GPUs build and run without it.

# Cache entries, so the editor's and tools' directories see them.
set(VKR_GPA_AVAILABLE OFF CACHE INTERNAL "GPUPerfAPI downloaded")
if(NOT WIN32 OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    return()
endif()

set(VKR_GPA_VERSION "4.4.1.3")
set(_vkr_gpa_root "${CMAKE_BINARY_DIR}/_deps/gpuperfapi-${VKR_GPA_VERSION}")
set(_vkr_gpa_dir "${_vkr_gpa_root}/GPUPerfAPI-${VKR_GPA_VERSION}/4_4")
if(NOT EXISTS "${_vkr_gpa_dir}/include/gpu_performance_api/gpu_perf_api_vk.h")
    set(_vkr_gpa_archive "GPUPerfAPI-${VKR_GPA_VERSION}.zip")
    set(_vkr_gpa_download "${_vkr_gpa_root}/${_vkr_gpa_archive}")
    message(STATUS "GPUPerfAPI: downloading ${VKR_GPA_VERSION}")
    file(DOWNLOAD
         "https://github.com/GPUOpen-Tools/gpu_performance_api/releases/download/v${VKR_GPA_VERSION}/${_vkr_gpa_archive}"
         "${_vkr_gpa_download}"
         EXPECTED_HASH SHA256=2e3eed3651f86f2a629ad7efe5bd28d14a769c2a91deeb26c5e4904eadbe1465
         STATUS _vkr_gpa_status)
    list(GET _vkr_gpa_status 0 _vkr_gpa_code)
    if(NOT _vkr_gpa_code EQUAL 0)
        message(WARNING
            "Could not download GPUPerfAPI ${VKR_GPA_VERSION}: ${_vkr_gpa_status}. "
            "AMD GPU counters are unavailable in this build.")
        return()
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_vkr_gpa_download}"
         DESTINATION "${_vkr_gpa_root}"
         PATTERNS "*/include/*" "*/bin/GPUPerfAPIVK-x64.dll" "*/LICENSE.txt")
    file(REMOVE "${_vkr_gpa_download}")
endif()

set(VKR_GPA_AVAILABLE ON CACHE INTERNAL "GPUPerfAPI downloaded")
set(VKR_GPA_INCLUDE_DIR "${_vkr_gpa_dir}/include" CACHE INTERNAL
    "GPUPerfAPI headers")
set(VKR_GPA_VK_DLL "${_vkr_gpa_dir}/bin/GPUPerfAPIVK-x64.dll" CACHE INTERNAL
    "GPUPerfAPI Vulkan DLL")

# Copies the Vulkan GPUPerfAPI DLL beside `target`'s executable, where the
# backend opens it by name.
function(vkr_gpa_copy_runtime target)
    if(VKR_GPA_AVAILABLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VKR_GPA_VK_DLL}" "$<TARGET_FILE_DIR:${target}>/GPUPerfAPIVK-x64.dll")
    endif()
endfunction()
