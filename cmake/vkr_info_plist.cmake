# macOS reads an executable's privacy usage strings from its Info.plist. A
# program outside an app bundle carries one in its __TEXT,__info_plist
# section, so the Local Network prompt names it and says why it asks
# (ADR-105). Other platforms ignore the call.
set(VKR_INFO_PLIST_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/vkr_info_plist.in")

function(vkr_embed_info_plist target identifier display_name network_usage)
    if(NOT APPLE)
        return()
    endif()
    set(VKR_PLIST_IDENTIFIER "${identifier}")
    set(VKR_PLIST_NAME "${display_name}")
    set(VKR_PLIST_NETWORK_USAGE "${network_usage}")
    set(plist "${CMAKE_CURRENT_BINARY_DIR}/${target}.Info.plist")
    configure_file("${VKR_INFO_PLIST_TEMPLATE}" "${plist}" @ONLY)
    target_link_options(${target} PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${plist}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${plist}")
endfunction()
