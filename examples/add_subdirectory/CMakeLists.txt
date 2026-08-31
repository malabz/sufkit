cmake_minimum_required(VERSION 3.20)
project(sufkit_add_subdirectory_consumer LANGUAGES C CXX)

add_subdirectory(../.. sufkit-build)
add_executable(sufkit_add_subdirectory_consumer main.cpp)
target_link_libraries(sufkit_add_subdirectory_consumer PRIVATE sufkit::sufkit)
target_compile_features(sufkit_add_subdirectory_consumer PRIVATE cxx_std_17)

add_executable(sufkit_add_subdirectory_smem_mum ../smem_mum.cpp)
target_link_libraries(sufkit_add_subdirectory_smem_mum PRIVATE sufkit::sufkit)
target_compile_features(sufkit_add_subdirectory_smem_mum PRIVATE cxx_std_17)
