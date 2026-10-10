# The asset depot (docs/proposals/network-protocol.md, "Depot"): objects,
# the store, working copies, the depot service and its client, and the
# vkr_depot command. The library takes a codec instead of linking zstd, so
# processes that already carry zstd (libktx) do not link a second copy.
add_library(vkr_depot_lib STATIC
    vkr_depot_object.c
    vkr_depot_store.c
    vkr_depot_workspace.c
    vkr_depot_protocol.c
    vkr_depot_server.c
    vkr_depot_client.c)
vkr_configure_library(vkr_depot_lib)
target_include_directories(vkr_depot_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkr_depot_lib PUBLIC vkr_net vkr_bakery_os)

# zstd for the command, from the copy libktx carries; the command does not
# link libktx, so the symbols appear once.
set(_vkr_depot_zstd_dir "${CMAKE_SOURCE_DIR}/vendor/ktx-software/external/basisu/zstd")
add_library(vkr_depot_zstd STATIC vkr_depot_zstd.c "${_vkr_depot_zstd_dir}/zstd.c")
target_include_directories(vkr_depot_zstd PUBLIC "${_vkr_depot_zstd_dir}"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vkr_depot_zstd PUBLIC vkr_foundation)
if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties("${_vkr_depot_zstd_dir}/zstd.c" PROPERTIES
        COMPILE_OPTIONS "/O2;/w")
else()
    set_source_files_properties("${_vkr_depot_zstd_dir}/zstd.c" PROPERTIES
        COMPILE_OPTIONS "-O2;-w")
endif()
unset(_vkr_depot_zstd_dir)

add_executable(vkr_depot vkr_depot_main.c)
vkr_require_declared_c_functions(vkr_depot)
target_link_libraries(vkr_depot PRIVATE vkr_depot_lib vkr_depot_zstd)
