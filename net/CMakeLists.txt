# The network transport and its services (docs/proposals/network-protocol.md):
# the sans-I/O core, crypto, congestion control and drivers. It depends only on
# the foundation and libsodium, so servers and tools link it without the
# renderer.
include("${CMAKE_SOURCE_DIR}/cmake/vkr_libsodium.cmake")
file(GLOB_RECURSE VKR_NET_SOURCES CONFIGURE_DEPENDS src/*.c src/*.h)
add_library(vkr_net STATIC ${VKR_NET_SOURCES})
vkr_configure_library(vkr_net)
target_include_directories(vkr_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(vkr_net PUBLIC vkr_foundation PRIVATE vkr_sodium)
