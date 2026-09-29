# vkr_bakery: the asset and shader build system. It is always built because the
# renderer compiles its shader catalog through it; cooker producers and the
# `tool` entries join only when VKR_BUILD_TOOLS provides the cooker libraries.

add_executable(vkr_bakery
    vkr_bakery_main.c
    vkr_bakery_commands.c
    vkr_bakery_buffer.c
    vkr_bakery_cache.c
    vkr_bakery_diag.c
    vkr_bakery_events.c
    vkr_bakery_graph.c
    vkr_bakery_index.c
    vkr_bakery_json.c
    vkr_bakery_os.c
    vkr_bakery_registry.c
    vkr_bakery_script.c
    vkr_bakery_serve.c
    vkr_bakery_shaders.c
    vkr_bakery_tools.c
)
vkr_require_declared_c_functions(vkr_bakery)
target_include_directories(vkr_bakery PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_SOURCE_DIR}/lib/src")
target_link_libraries(vkr_bakery PRIVATE vkr_foundation)
if(APPLE)
    # FSEvents watches files for the daemon.
    target_link_libraries(vkr_bakery PRIVATE "-framework CoreServices")
endif()
if(NOT WIN32)
    target_link_libraries(vkr_bakery PRIVATE m)
else()
    # BCryptGenRandom seeds project identifiers.
    target_link_libraries(vkr_bakery PRIVATE psapi bcrypt)
endif()
target_compile_definitions(vkr_bakery PRIVATE
    VKR_BAKERY_SLANGC_DEFAULT="${VKR_SLANGC_EXECUTABLE}")

# Producer identities: each group lists the sources whose change can alter that
# producer's output bytes. Toolchain versions for shaders live in recipes.
set(VKR_BAKERY_IDENTITY_GROUPS SHADER SCRIPT)
set(VKR_BAKERY_IDENTITY_SHADER_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/vkr_bakery_shaders.c")
set(VKR_BAKERY_IDENTITY_SHADER_EXTRA "shader")
set(VKR_BAKERY_IDENTITY_SCRIPT_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/vkr_bakery_script.c")
set(VKR_BAKERY_IDENTITY_SCRIPT_EXTRA "script")

if(VKR_BUILD_TOOLS)
    target_sources(vkr_bakery PRIVATE
        vkr_bakery_assets.c
        vkr_bakery_bake.c
        vkr_bakery_bundle.c
        vkr_bakery_package.c
        project/vkr_project_bake.c
        project/vkr_project_gc.c
        project/vkr_project_import.c
        project/vkr_project_lower.c
        project/vkr_project_main.c
        project/vkr_project_scene.c
        project/vkr_project_util.c)
    # The archive writer compresses chunks with the zstd libktx carries.
    target_include_directories(vkr_bakery PRIVATE "${CMAKE_SOURCE_DIR}/tools"
        "${CMAKE_SOURCE_DIR}/vendor/ktx-software/external/basisu/zstd")
    target_compile_definitions(vkr_bakery PRIVATE VKR_BAKERY_WITH_COOKERS=1)
    target_link_libraries(vkr_bakery PRIVATE ktx
        vkr_mesh_cooker vkr_animation_cooker vkr_collision_cooker
        vkr_font_cooker vkr_vkt_packer vkr_hdr_cube_packer
        vkr_dfg_cooker vkr_sheen_cooker vkr_anisotropy_cooker
        vkr_diffuse_baker)
    # The cookers are C++ libraries; link with the C++ runtime.
    set_target_properties(vkr_bakery PROPERTIES LINKER_LANGUAGE CXX)

    file(GLOB VKR_BAKERY_COOKING_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/tools/assets/*.c"
        "${CMAKE_SOURCE_DIR}/tools/assets/*.cpp"
        "${CMAKE_SOURCE_DIR}/tools/assets/*.h"
        "${CMAKE_SOURCE_DIR}/tools/assets/*.inc"
        "${CMAKE_SOURCE_DIR}/runtime/src/assets/*.c"
        "${CMAKE_SOURCE_DIR}/runtime/src/assets/*.cpp"
        "${CMAKE_SOURCE_DIR}/runtime/src/assets/*.h")
    set(VKR_BAKERY_PRODUCER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/vkr_bakery_assets.c")
    set(VKR_BAKERY_TEXTURE_SOURCES
        "${CMAKE_SOURCE_DIR}/tools/vkr_vkt_packer.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vkr_vkt_packer.h"
        "${CMAKE_SOURCE_DIR}/tools/vkr_vkt_packer_main.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vkr_vkt_mips.h"
        "${CMAKE_SOURCE_DIR}/tools/vkr_vkt_normal_roughness.h"
        "${CMAKE_SOURCE_DIR}/tools/vkr_vkt_pack_contract.h"
        "${CMAKE_SOURCE_DIR}/vendor/ktx-float-channel-shift.patch"
        "${CMAKE_SOURCE_DIR}/vendor/ktx-basisu-job-pool-shutdown.patch"
        "${CMAKE_SOURCE_DIR}/runtime/src/assets/stb_image_impl.c"
        "${CMAKE_SOURCE_DIR}/vendor/stb_image.h")
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/vendor/ktx-software"
        rev-parse HEAD OUTPUT_VARIABLE VKR_BAKERY_KTX_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/vendor/meshoptimizer"
        rev-parse HEAD OUTPUT_VARIABLE VKR_BAKERY_MESHOPT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git -C "${CMAKE_SOURCE_DIR}/vendor/JoltPhysics"
        rev-parse HEAD OUTPUT_VARIABLE VKR_BAKERY_JOLT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)

    list(APPEND VKR_BAKERY_IDENTITY_GROUPS
        TEXTURE MESH ANIMATION COLLISION FONT TABLE)
    set(VKR_BAKERY_IDENTITY_TEXTURE_FILES
        ${VKR_BAKERY_TEXTURE_SOURCES} "${VKR_BAKERY_PRODUCER_SOURCE}")
    set(VKR_BAKERY_IDENTITY_TEXTURE_EXTRA "ktx ${VKR_BAKERY_KTX_COMMIT}")
    set(VKR_BAKERY_IDENTITY_MESH_FILES
        "${CMAKE_SOURCE_DIR}/tools/vkr_mesh_cooker.c"
        ${VKR_BAKERY_COOKING_SOURCES} ${VKR_BAKERY_TEXTURE_SOURCES}
        "${VKR_BAKERY_PRODUCER_SOURCE}")
    set(VKR_BAKERY_IDENTITY_MESH_EXTRA
        "ktx ${VKR_BAKERY_KTX_COMMIT} meshoptimizer ${VKR_BAKERY_MESHOPT_COMMIT}")
    set(VKR_BAKERY_IDENTITY_ANIMATION_FILES
        "${CMAKE_SOURCE_DIR}/tools/vkr_animation_cooker.c"
        ${VKR_BAKERY_COOKING_SOURCES} "${VKR_BAKERY_PRODUCER_SOURCE}")
    set(VKR_BAKERY_IDENTITY_ANIMATION_EXTRA "animation")
    set(VKR_BAKERY_IDENTITY_COLLISION_FILES
        "${CMAKE_SOURCE_DIR}/tools/vkr_collision_cooker.c"
        ${VKR_BAKERY_COOKING_SOURCES} "${VKR_BAKERY_PRODUCER_SOURCE}")
    set(VKR_BAKERY_IDENTITY_COLLISION_EXTRA "jolt ${VKR_BAKERY_JOLT_COMMIT}")
    set(VKR_BAKERY_IDENTITY_FONT_FILES
        "${CMAKE_SOURCE_DIR}/tools/vkr_font_cooker.cpp"
        "${CMAKE_SOURCE_DIR}/tools/assets/vkr_font_encode.c"
        "${CMAKE_SOURCE_DIR}/tools/assets/vkr_font_encode.h"
        "${VKR_BAKERY_PRODUCER_SOURCE}")
    set(VKR_BAKERY_IDENTITY_FONT_EXTRA
        "msdf ${VKR_MSDF_ATLAS_COMMIT} ${VKR_MSDFGEN_COMMIT}")
    set(VKR_BAKERY_IDENTITY_TABLE_FILES
        "${CMAKE_SOURCE_DIR}/tools/vkr_dfg_cooker.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vkr_sheen_cooker.cpp"
        "${CMAKE_SOURCE_DIR}/tools/vkr_anisotropy_cooker.cpp"
        "${CMAKE_SOURCE_DIR}/tools/assets/anisotropy_ltc_seed.inc"
        "${CMAKE_SOURCE_DIR}/renderer/src/vkr_ltc_lut.c"
        "${VKR_BAKERY_PRODUCER_SOURCE}")
    set(VKR_BAKERY_IDENTITY_TABLE_EXTRA "table")
endif()

set(VKR_BAKERY_IDENTITY_SPEC "${CMAKE_CURRENT_BINARY_DIR}/vkr_bakery_identity_spec.cmake")
set(VKR_BAKERY_IDENTITY_HEADER "${CMAKE_CURRENT_BINARY_DIR}/vkr_bakery_identity.h")
set(vkr_identity_spec "set(VKR_IDENTITY_GROUPS \"${VKR_BAKERY_IDENTITY_GROUPS}\")\n")
set(vkr_identity_depends "")
foreach(group IN LISTS VKR_BAKERY_IDENTITY_GROUPS)
    string(APPEND vkr_identity_spec
        "set(VKR_IDENTITY_${group}_FILES \"${VKR_BAKERY_IDENTITY_${group}_FILES}\")\n"
        "set(VKR_IDENTITY_${group}_EXTRA \"${VKR_BAKERY_IDENTITY_${group}_EXTRA}\")\n")
    list(APPEND vkr_identity_depends ${VKR_BAKERY_IDENTITY_${group}_FILES})
endforeach()
file(GENERATE OUTPUT "${VKR_BAKERY_IDENTITY_SPEC}" CONTENT "${vkr_identity_spec}")
list(REMOVE_DUPLICATES vkr_identity_depends)
add_custom_command(
    OUTPUT "${VKR_BAKERY_IDENTITY_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DVKR_IDENTITY_SPEC=${VKR_BAKERY_IDENTITY_SPEC}"
            "-DVKR_IDENTITY_ROOT=${CMAKE_SOURCE_DIR}"
            "-DVKR_IDENTITY_OUTPUT=${VKR_BAKERY_IDENTITY_HEADER}"
            -P "${CMAKE_SOURCE_DIR}/cmake/vkr_bakery_identity.cmake"
    DEPENDS ${vkr_identity_depends} "${VKR_BAKERY_IDENTITY_SPEC}"
            "${CMAKE_SOURCE_DIR}/cmake/vkr_bakery_identity.cmake"
    COMMENT "Hashing vkr_bakery producer identities"
    VERBATIM)
target_sources(vkr_bakery PRIVATE "${VKR_BAKERY_IDENTITY_HEADER}")
