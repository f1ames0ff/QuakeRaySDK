cmake_minimum_required(VERSION 3.20)

project(qray_package_consumer LANGUAGES CXX)

if(NOT DEFINED QR_NVRHI_SOURCE_DIR)
    message(FATAL_ERROR "Set QR_NVRHI_SOURCE_DIR to the NVRHI source checkout")
endif()

find_package(Vulkan REQUIRED)
add_subdirectory("${QR_NVRHI_SOURCE_DIR}" nvrhi)
find_package(qray_renderer CONFIG REQUIRED)

add_executable(qray_package_consumer main.cpp)
target_compile_features(qray_package_consumer PRIVATE cxx_std_20)
target_link_libraries(qray_package_consumer PRIVATE qray::renderer)
