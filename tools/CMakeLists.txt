cmake_minimum_required(VERSION 3.27)

project(vkr_tools)

find_package(Vulkan REQUIRED)

add_executable(vkr_vkt_packer
    vkr_vkt_packer.cpp
)

target_compile_features(vkr_vkt_packer PRIVATE cxx_std_17)

target_include_directories(vkr_vkt_packer
    PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor
        ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(vkr_vkt_packer
    PRIVATE
        ktx
)
