cmake_minimum_required(VERSION 3.20)
project(sufkit_find_package_consumer LANGUAGES CXX)

find_package(sufkit CONFIG REQUIRED)
add_executable(sufkit_find_package_consumer main.cpp)
target_link_libraries(sufkit_find_package_consumer PRIVATE sufkit::sufkit)
target_compile_features(sufkit_find_package_consumer PRIVATE cxx_std_17)

