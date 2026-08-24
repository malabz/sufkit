cmake_minimum_required(VERSION 3.20)

project(sufkit VERSION 0.1.1 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(PROJECT_IS_TOP_LEVEL)
    set(_sufkit_top_level_default ON)
else()
    set(_sufkit_top_level_default OFF)
endif()

option(SUFKIT_BUILD_CLI "Build the sufkit command-line application" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_TESTS "Build sufkit tests" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_BENCHMARKS "Build the benchmark suite" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_EXAMPLES "Build consumer examples" ${_sufkit_top_level_default})
option(SUFKIT_ENABLE_CAPS "Build the bundled CaPS-SA parallel suffix-array backend" ON)
option(SUFKIT_BUILD_DOCS "Build local Doxygen API documentation" OFF)

find_package(ZLIB REQUIRED)
if(SUFKIT_ENABLE_CAPS)
    find_package(Threads REQUIRED)
endif()
if(SUFKIT_BUILD_DOCS)
    find_package(Doxygen REQUIRED)
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

if(SUFKIT_ENABLE_CAPS)
    add_library(sufkit_caps OBJECT
        third_party/caps-sa/src/Suffix_Array.cpp)
    target_compile_features(sufkit_caps PRIVATE cxx_std_17)
    target_include_directories(sufkit_caps SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/caps-sa/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/parlaylib/include)
    target_link_libraries(sufkit_caps PRIVATE Threads::Threads)
    set_target_properties(sufkit_caps PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
    set(_sufkit_caps_objects $<TARGET_OBJECTS:sufkit_caps>)
else()
    set(_sufkit_caps_objects)
endif()

set(_sufkit_library_type STATIC)
if(BUILD_SHARED_LIBS)
    set(_sufkit_library_type SHARED)
endif()

add_library(sufkit ${_sufkit_library_type}
    src/error.cpp
    src/genome_reference.cpp
    src/query.cpp
    src/serialization.cpp
    src/caps_backend.cpp
    src/divsufsort_backend.cpp
    src/suffix_array.cpp
    src/fm_index.cpp
    src/inspect.cpp
    $<TARGET_OBJECTS:sufkit_divsufsort32>
    $<TARGET_OBJECTS:sufkit_divsufsort64>
    ${_sufkit_caps_objects})
add_library(sufkit::sufkit ALIAS sufkit)

target_compile_features(sufkit PUBLIC cxx_std_17)
target_compile_definitions(sufkit PRIVATE
    SUFKIT_BUILDING_LIBRARY
    SUFKIT_HAS_CAPS=$<BOOL:${SUFKIT_ENABLE_CAPS}>)
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
if(SUFKIT_ENABLE_CAPS)
    target_include_directories(sufkit SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/caps-sa/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/parlaylib/include)
    target_link_libraries(sufkit PRIVATE Threads::Threads)
endif()
set_target_properties(sufkit PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    VERSION ${PROJECT_VERSION}
    SOVERSION 0)

if(BUILD_SHARED_LIBS AND UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(sufkit PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/sufkit-private-symbols.map")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sufkit PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(SUFKIT_BUILD_CLI)
    set(_sufkit_cli_sources apps/sufkit.cpp apps/app_support.cpp)
    if(SUFKIT_BUILD_BENCHMARKS)
        list(APPEND _sufkit_cli_sources
            apps/benchmark.cpp
            apps/benchmark_data.cpp
            apps/benchmark_output.cpp
            apps/benchmark_runner.cpp
            apps/right_maximal_benchmark.cpp)
    else()
        list(APPEND _sufkit_cli_sources apps/benchmark_disabled.cpp)
    endif()
    add_executable(sufkit_cli ${_sufkit_cli_sources})
    set_target_properties(sufkit_cli PROPERTIES OUTPUT_NAME sufkit)
    target_link_libraries(sufkit_cli PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_cli PRIVATE cxx_std_17)
    target_include_directories(sufkit_cli SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kseq)
    target_include_directories(sufkit_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_cli PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(SUFKIT_BUILD_BENCHMARKS)
        target_compile_definitions(sufkit_cli PRIVATE
            "SUFKIT_BENCH_CMAKE_VERSION=\"${CMAKE_VERSION}\""
            "SUFKIT_BENCH_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\"")
    endif()
endif()

if(SUFKIT_BUILD_TESTS)
    enable_testing()
    add_executable(sufkit_tests tests/test_sufkit.cpp)
    target_link_libraries(sufkit_tests PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_tests PRIVATE cxx_std_17)
    add_test(NAME sufkit_tests COMMAND sufkit_tests)
    add_executable(sufkit_caps_tests tests/test_caps_backend.cpp)
    target_link_libraries(sufkit_caps_tests PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_caps_tests PRIVATE cxx_std_17)
    target_include_directories(sufkit_caps_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(sufkit_caps_tests PRIVATE
        SUFKIT_TEST_CAPS_ENABLED=$<BOOL:${SUFKIT_ENABLE_CAPS}>)
    add_test(NAME sufkit_caps_tests COMMAND sufkit_caps_tests)
    add_executable(sufkit_sampled_sa_tests tests/test_sampled_sa.cpp)
    target_link_libraries(sufkit_sampled_sa_tests PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_sampled_sa_tests PRIVATE cxx_std_17)
    add_test(NAME sufkit_sampled_sa_tests COMMAND sufkit_sampled_sa_tests)
    if(SUFKIT_BUILD_BENCHMARKS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(NAME sufkit_benchmark_packager_tests
                COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_package_benchmark_results.py -v)
            add_test(NAME sufkit_server_runner_recovery_tests
                COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                    ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_run_server_suite.py -v)
        endif()
    endif()
    if(SUFKIT_BUILD_CLI AND SUFKIT_BUILD_BENCHMARKS)
        add_test(NAME sufkit_benchmark_smoke
            COMMAND ${CMAKE_COMMAND}
                -DSUFKIT_EXECUTABLE=$<TARGET_FILE:sufkit_cli>
                -DSUFKIT_CAPS_ENABLED=$<BOOL:${SUFKIT_ENABLE_CAPS}>
                -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-benchmark
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_benchmark.cmake)
        set_tests_properties(sufkit_benchmark_smoke PROPERTIES TIMEOUT 180)
        add_test(NAME sufkit_right_maximal_benchmark_smoke
            COMMAND ${CMAKE_COMMAND}
                -DSUFKIT_EXECUTABLE=$<TARGET_FILE:sufkit_cli>
                -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-right-maximal-benchmark
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_right_maximal_benchmark.cmake)
        set_tests_properties(sufkit_right_maximal_benchmark_smoke PROPERTIES TIMEOUT 180)
    endif()
endif()

if(SUFKIT_BUILD_EXAMPLES)
    add_executable(sufkit_example examples/basic.cpp)
    target_link_libraries(sufkit_example PRIVATE sufkit::sufkit)
    add_executable(sufkit_example_exact_sa examples/exact_sa.cpp)
    target_link_libraries(sufkit_example_exact_sa PRIVATE sufkit::sufkit)
    add_executable(sufkit_example_fm_batch examples/fm_batch.cpp)
    target_link_libraries(sufkit_example_fm_batch PRIVATE sufkit::sufkit)
    add_executable(sufkit_example_right_maximal_stream examples/right_maximal_stream.cpp)
    target_link_libraries(sufkit_example_right_maximal_stream PRIVATE sufkit::sufkit)
    add_executable(sufkit_example_inspect_error examples/inspect_error.cpp)
    target_link_libraries(sufkit_example_inspect_error PRIVATE sufkit::sufkit)
endif()

if(SUFKIT_BUILD_BENCHMARKS)
    add_executable(sufkit_sa_build_bench benchmarks/sa_build_benchmark.cpp)
    target_link_libraries(sufkit_sa_build_bench PRIVATE sufkit::sufkit)
    target_compile_features(sufkit_sa_build_bench PRIVATE cxx_std_17)
    target_include_directories(sufkit_sa_build_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_sa_build_bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(SUFKIT_BUILD_TESTS)
        add_test(NAME sufkit_sa_build_benchmark_smoke
            COMMAND ${CMAKE_COMMAND}
                -DSUFKIT_SA_BUILD_BENCH=$<TARGET_FILE:sufkit_sa_build_bench>
                -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-sa-build-benchmark
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_sa_build_benchmark.cmake)
        set_tests_properties(sufkit_sa_build_benchmark_smoke PROPERTIES TIMEOUT 180)
    endif()
endif()

if(SUFKIT_BUILD_DOCS)
    configure_file(
        docs/Doxyfile.in
        ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        @ONLY)
    add_custom_target(sufkit_docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating sufkit public API documentation"
        VERBATIM)
endif()

install(TARGETS sufkit
    EXPORT sufkitTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(SUFKIT_BUILD_CLI)
    install(TARGETS sufkit_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(SUFKIT_BUILD_BENCHMARKS)
    install(TARGETS sufkit_sa_build_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/sufkit DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sufkit)
install(FILES README.md README.zh-CN.md CHANGELOG.md CONTRIBUTING.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sufkit)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/sufkit/docs
    FILES_MATCHING PATTERN "*.md")

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
