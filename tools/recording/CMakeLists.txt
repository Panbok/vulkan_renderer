# vkr_rec: reads recordings without the editor or the GPU
# (docs/proposals/recordings.md). It decodes zstd chunks with the copy the
# depot command carries.
add_executable(vkr_rec vkr_rec_main.c)
vkr_require_declared_c_functions(vkr_rec)
target_link_libraries(vkr_rec PRIVATE vkr_asset_formats vkr_depot_zstd)
