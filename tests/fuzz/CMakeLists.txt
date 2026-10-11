# One executable per fuzzer (cmake/vkr_fuzzers.cmake). With libFuzzer the
# executable is the fuzzing engine; otherwise fuzz_replay_main.c runs the
# files it is given.
function(vkr_add_fuzzer name source)
    add_executable(${name} ${source})
    vkr_require_declared_c_functions(${name})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE ${ARGN})
    if(VKR_FUZZ_ENGINE STREQUAL "libfuzzer")
        target_link_options(${name} PRIVATE ${VKR_FUZZ_LINK_OPTIONS})
    else()
        target_sources(${name} PRIVATE fuzz_replay_main.c)
    endif()
endfunction()

vkr_add_fuzzer(vkr_fuzz_net_core fuzz_net_core.c vkr_net vkr_sodium vkr_foundation)
vkr_add_fuzzer(vkr_fuzz_net_session fuzz_net_session.c vkr_net vkr_sodium vkr_foundation)
vkr_add_fuzzer(vkr_fuzz_wire fuzz_wire.c vkr_net vkr_foundation)
if(TARGET vkr_depot_lib)
    vkr_add_fuzzer(vkr_fuzz_depot fuzz_depot.c vkr_depot_lib vkr_depot_zstd)
else()
    message(STATUS "VKR fuzzers: vkr_fuzz_depot needs VKR_BUILD_TOOLS")
endif()
