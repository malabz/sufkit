cmake_minimum_required(VERSION 3.20)

project(sufkit VERSION 0.1.0 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(PROJECT_IS_TOP_LEVEL)
    set(_sufkit_top_level_default ON)
else()
    set(_sufkit_top_level_default OFF)
endif()

option(SUFKIT_BUILD_CLI "Build the sufkit command-line application" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_TESTS "Build sufkit tests" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_BENCHMARKS "Build the quick benchmark" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_EXAMPLES "Build consumer examples" ${_sufkit_top_level_default})
option(SUFKIT_ENABLE_CAPS "Enable the future CaPS suffix-array backend" OFF)
option(SUFKIT_USE_BUNDLED_DIVSUFSORT "Use the pinned bundled libdivsufsort" ON)
option(SUFKIT_USE_BUNDLED_SDSL "Use the pinned bundled SDSL headers" ON)

if(SUFKIT_ENABLE_CAPS)
    message(FATAL_ERROR "SUFKIT_ENABLE_CAPS is reserved for V1.1 and is not implemented in sufkit 0.1.0")
endif()

find_package(ZLIB REQUIRED)

if(NOT SUFKIT_USE_BUNDLED_DIVSUFSORT)
    message(FATAL_ERROR "System libdivsufsort discovery is not implemented in sufkit 0.1.0")
endif()

if(NOT SUFKIT_USE_BUNDLED_SDSL)
    message(FATAL_ERROR "System SDSL discovery is not implemented in sufkit 0.1.0; use the pinned bundled SDSL 3.0.3")
endif()

set(_sufkit_divsufsort_sources
    third_party/libdivsufsort/lib/divsufsort.c
    third_party/libdivsufsort/lib/sssort.c
    third_party/libdivsufsort/lib/trsort.c
    third_party/libdivsufsort/lib/utils.c)

add_library(sufkit_divsufsort32 OBJECT ${_sufkit_divsufsort_sources})
add_library(sufkit_divsufsort64 OBJECT ${_sufkit_divsufsort_sources})
foreach(_target IN ITEMS sufkit_divsufsort32 sufkit_divsufsort64)
    target_include_directories(${_target} PRIVATE third_party/libdivsufsort/include)
    target_compile_definitions(${_target} PRIVATE HAVE_CONFIG_H=1 __STDC_LIMIT_MACROS __STDC_CONSTANT_MACROS __STDC_FORMAT_MACROS)
    set_target_properties(${_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()
target_compile_definitions(sufkit_divsufsort64 PRIVATE BUILD_DIVSUFSORT64)

set(_sufkit_library_type STATIC)
if(BUILD_SHARED_LIBS)
    set(_sufkit_library_type SHARED)
endif()

add_library(sufkit ${_sufkit_library_type}
    src/error.cpp
    src/genome_reference.cpp
    src/query.cpp
    src/serialization.cpp
    src/suffix_array.cpp
    src/fm_index.cpp
    src/inspect.cpp
    $<TARGET_OBJECTS:sufkit_divsufsort32>
    $<TARGET_OBJECTS:sufkit_divsufsort64>)
add_library(sufkit::sufkit ALIAS sufkit)

target_compile_features(sufkit PUBLIC cxx_std_17)
target_compile_definitions(sufkit PRIVATE SUFKIT_BUILDING_LIBRARY)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(sufkit PUBLIC SUFKIT_STATIC)
endif()
target_include_directories(sufkit
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdivsufsort/include)
target_include_directories(sufkit SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kseq
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sdsl/include)
target_link_libraries(sufkit PUBLIC ZLIB::ZLIB)
set_target_properties(sufkit PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sufkit PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(SUFKIT_BUILD_CLI)
    set(_sufkit_cli_sources apps/sufkit.cpp apps/app_support.cpp)
    if(SUFKIT_BUILD_BENCHMARKS)
        list(APPEND _sufkit_cli_sources apps/benchmark.cpp)
    else()
        list(APPEND _sufkit_cli_sources apps/benchmark_disabled.cpp)
    endif()
    add_executable(sufkit_cli ${_sufkit_cli_sources})
    set_target_properties(sufkit_cli PROPERTIES OUTPUT_NAME sufkit)
    target_link_libraries(sufkit_cli PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_cli PRIVATE cxx_std_17)
    target_include_directories(sufkit_cli SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kseq)
endif()

if(SUFKIT_BUILD_TESTS)
    enable_testing()
    add_executable(sufkit_tests tests/test_sufkit.cpp)
    target_link_libraries(sufkit_tests PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_tests PRIVATE cxx_std_17)
    add_test(NAME sufkit_tests COMMAND sufkit_tests)
endif()

if(SUFKIT_BUILD_EXAMPLES)
    add_executable(sufkit_example examples/basic.cpp)
    target_link_libraries(sufkit_example PRIVATE sufkit::sufkit)
endif()

install(TARGETS sufkit
    EXPORT sufkitTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(SUFKIT_BUILD_CLI)
    install(TARGETS sufkit_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/sufkit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/sufkitConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sufkitConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sufkit)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/sufkitConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/sufkitConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/sufkitConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sufkit)
install(EXPORT sufkitTargets
    FILE sufkitTargets.cmake
    NAMESPACE sufkit::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sufkit)
