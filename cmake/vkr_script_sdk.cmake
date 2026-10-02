# The script SDK (ADR-079): sdk.h and the foundation headers it includes,
# staged flat into one include root. Bakery compiles project scripts against
# it, the script test probes build from it alone, the Script editor completes
# from it, and the editor distribution installs it as `sdk/`. The list must
# stay closed under #include: a missing header fails the probes' build.
set(VKR_SCRIPT_SDK_DIR "${CMAKE_BINARY_DIR}/script_sdk")
set(VKR_SCRIPT_SDK_HEADERS
    "sdk/sdk.h|sdk.h"
    "lib/src/defines.h|defines.h"
    "lib/src/vkr_pch.h|vkr_pch.h"
    "lib/src/math/mat.h|math/mat.h"
    "lib/src/math/vec.h|math/vec.h"
    "lib/src/math/vkr_math.h|math/vkr_math.h"
    "lib/src/math/vkr_quat.h|math/vkr_quat.h"
    "lib/src/math/vkr_simd.h|math/vkr_simd.h")

set(VKR_SCRIPT_SDK_OUTPUTS)
foreach(entry IN LISTS VKR_SCRIPT_SDK_HEADERS)
    string(REPLACE "|" ";" pair "${entry}")
    list(GET pair 0 source)
    list(GET pair 1 target)
    get_filename_component(directory "${VKR_SCRIPT_SDK_DIR}/${target}" DIRECTORY)
    add_custom_command(
        OUTPUT "${VKR_SCRIPT_SDK_DIR}/${target}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${directory}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/${source}" "${VKR_SCRIPT_SDK_DIR}/${target}"
        DEPENDS "${CMAKE_SOURCE_DIR}/${source}"
        COMMENT "Staging script SDK header ${target}"
        VERBATIM)
    list(APPEND VKR_SCRIPT_SDK_OUTPUTS "${VKR_SCRIPT_SDK_DIR}/${target}")
endforeach()
add_custom_target(vkr_script_sdk DEPENDS ${VKR_SCRIPT_SDK_OUTPUTS})
