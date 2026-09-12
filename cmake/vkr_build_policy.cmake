# Apply final options after upstream directory and target options. In particular,
# KTX adds -O0 in Debug and FidelityFX adds /Od; neither should win over this
# project's optimized dependency policy. Configuration names still select VKR's
# debugging, assertions and diagnostics.
include(CheckIPOSupported)
option(VKR_ENABLE_IPO "Use supported interprocedural optimization in Release" ON)
if(VKR_ENABLE_IPO)
    # Avoid compiling CMake's probe projects again on every wrapper invocation.
    string(SHA256 ipo_signature
        "${CMAKE_C_COMPILER};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_GENERATOR};${CMAKE_TOOLCHAIN_FILE};${CMAKE_MSVC_RUNTIME_LIBRARY};${CMAKE_C_FLAGS};${CMAKE_CXX_FLAGS};${CMAKE_C_FLAGS_RELEASE};${CMAKE_CXX_FLAGS_RELEASE};${CMAKE_EXE_LINKER_FLAGS};${CMAKE_OSX_ARCHITECTURES};${CMAKE_OSX_DEPLOYMENT_TARGET}")
    if(NOT VKR_IPO_SIGNATURE STREQUAL ipo_signature)
        check_ipo_supported(RESULT supported OUTPUT error LANGUAGES C CXX)
        string(REPLACE "\n" " " error "${error}")
        set(VKR_IPO_SUPPORTED "${supported}" CACHE INTERNAL "IPO probe result" FORCE)
        set(VKR_IPO_ERROR "${error}" CACHE INTERNAL "IPO probe diagnostic" FORCE)
        set(VKR_IPO_SIGNATURE "${ipo_signature}" CACHE INTERNAL "IPO probe inputs" FORCE)
    endif()
    if(NOT VKR_IPO_SUPPORTED)
        message(STATUS "Release IPO unavailable: ${VKR_IPO_ERROR}")
    endif()
endif()

function(vkr_apply_build_policy directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if(NOT type MATCHES "^(STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|EXECUTABLE)$")
            continue()
        endif()
        get_target_property(source_dir ${target} SOURCE_DIR)
        set(vendor FALSE)
        if(source_dir MATCHES "/vendor/|/_deps/" OR
           target MATCHES "^vkr_(ktx_read|mesh_codecs)$")
            set(vendor TRUE)
        endif()
        if(WIN32)
            # msdfgen explicitly resets this property in its own CMake files.
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY
                         "${CMAKE_MSVC_RUNTIME_LIBRARY}")
        endif()
        if(vendor)
            # KTX publishes Debug CRT-selection macros to its consumers. Its
            # optimized implementation and all callers use the Release CRT.
            foreach(property COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS)
                get_target_property(definitions ${target} ${property})
                if(definitions)
                    string(REPLACE "$<$<CONFIG:Debug>:_DEBUG;DEBUG>" ""
                        definitions "${definitions}")
                    set_property(TARGET ${target} PROPERTY ${property} "${definitions}")
                endif()
            endforeach()
            get_target_property(options ${target} COMPILE_OPTIONS)
            if(options AND NOT VKR_SANITIZE_DEPENDENCIES)
                # Debug instrumentation belongs to VKR, not external codecs.
                list(FILTER options EXCLUDE REGEX "fsanitize=|fno-omit-frame-pointer")
                set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "${options}")
            endif()
            target_compile_definitions(${target} PRIVATE NDEBUG)
            if(MSVC)
                target_compile_options(${target} PRIVATE /O2 /U_DEBUG /UDEBUG)
            else()
                target_compile_options(${target} PRIVATE -O3 -U_DEBUG -UDEBUG)
            endif()
        else()
            # A few single-file dependencies are compiled directly into VKR
            # targets. Keep the surrounding VKR translation units debuggable.
            get_target_property(sources ${target} SOURCES)
            foreach(source IN LISTS sources)
                if(source MATCHES "(/vendor/|stb_.*_impl\\.c$|cgltf_impl\\.c$)" AND
                   source MATCHES "\\.(c|cc|cpp|cxx)$")
                    get_filename_component(vendor_source "${source}" ABSOLUTE
                        BASE_DIR "${source_dir}")
                    set_property(SOURCE "${vendor_source}" TARGET_DIRECTORY ${target}
                        PROPERTY SKIP_PRECOMPILE_HEADERS ON)
                    set_property(SOURCE "${vendor_source}" TARGET_DIRECTORY ${target}
                        APPEND PROPERTY COMPILE_DEFINITIONS NDEBUG)
                    if(MSVC)
                        set_property(SOURCE "${vendor_source}" TARGET_DIRECTORY ${target}
                            APPEND PROPERTY COMPILE_OPTIONS /O2 /U_DEBUG /UDEBUG)
                    else()
                        set_property(SOURCE "${vendor_source}" TARGET_DIRECTORY ${target}
                            APPEND PROPERTY COMPILE_OPTIONS -O3 -U_DEBUG -UDEBUG)
                        if(NOT VKR_SANITIZE_DEPENDENCIES)
                            set_property(SOURCE "${vendor_source}" TARGET_DIRECTORY ${target}
                                APPEND PROPERTY COMPILE_OPTIONS -fno-sanitize=all)
                        endif()
                    endif()
                endif()
            endforeach()
            if(MSVC)
                target_compile_options(${target} PRIVATE
                    "$<$<CONFIG:Release,RelWithDebInfo>:/O2>"
                    "$<$<CONFIG:Release,RelWithDebInfo>:/Ob2>"
                    "$<$<CONFIG:Release,RelWithDebInfo>:/Gy>"
                    "$<$<CONFIG:Release,RelWithDebInfo>:/Gw>")
            else()
                target_compile_options(${target} PRIVATE
                    "$<$<CONFIG:Release,RelWithDebInfo>:-O3>"
                    "$<$<CONFIG:Release,RelWithDebInfo>:-ffunction-sections>"
                    "$<$<CONFIG:Release,RelWithDebInfo>:-fdata-sections>")
            endif()
            if(VKR_ENABLE_IPO AND VKR_IPO_SUPPORTED)
                set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
            endif()
        endif()
        if(type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            if(WIN32)
                target_link_options(${target} PRIVATE
                    "$<$<CONFIG:Release,RelWithDebInfo>:LINKER:/OPT:REF,/OPT:ICF>")
            elseif(APPLE)
                target_link_options(${target} PRIVATE
                    "$<$<CONFIG:Release,RelWithDebInfo>:LINKER:-dead_strip>")
            else()
                target_link_options(${target} PRIVATE
                    "$<$<CONFIG:Release,RelWithDebInfo>:LINKER:--gc-sections>")
            endif()
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        vkr_apply_build_policy("${child}")
    endforeach()
endfunction()
