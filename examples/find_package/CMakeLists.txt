cmake_minimum_required(VERSION 3.20)
project(sufkit_find_package_consumer LANGUAGES CXX)

find_package(sufkit 0.3 CONFIG REQUIRED)
add_executable(sufkit_find_package_consumer main.cpp)
target_link_libraries(sufkit_find_package_consumer PRIVATE sufkit::sufkit)
target_compile_features(sufkit_find_package_consumer PRIVATE cxx_std_17)

add_executable(sufkit_find_package_smem_mum ../smem_mum.cpp)
target_link_libraries(sufkit_find_package_smem_mum PRIVATE sufkit::sufkit)
target_compile_features(sufkit_find_package_smem_mum PRIVATE cxx_std_17)
