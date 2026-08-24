cmake_minimum_required(VERSION 3.20)

project(sufkit VERSION 0.2.0 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(cmake/SufkitCpuBaseline.cmake)

if(PROJECT_IS_TOP_LEVEL)
    set(_sufkit_top_level_default ON)
else()
    set(_sufkit_top_level_default OFF)
endif()

option(SUFKIT_BUILD_CLI "Build the sufkit command-line application" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_TESTS "Build sufkit tests" ${_sufkit_top_level_default})
option(SUFKIT_BUILD_BENCHMARKS
    "Build the benchmark suite and developer microbenchmarks"
    ${_sufkit_top_level_default})
option(SUFKIT_BUILD_EXAMPLES "Build consumer examples" ${_sufkit_top_level_default})
option(SUFKIT_ENABLE_CAPS "Build the bundled CaPS-SA parallel suffix-array backend" ON)
option(SUFKIT_BUILD_DOCS "Build local Doxygen API documentation" OFF)
option(SUFKIT_ENABLE_DEVELOPER_TOOLS
    "Enable local clang-format and clang-tidy targets" OFF)
option(SUFKIT_ENABLE_INTERNAL_BENCH_DIAGNOSTICS
    "Instrument suffix-link scans in a developer-only static benchmark build"
    OFF)
mark_as_advanced(SUFKIT_ENABLE_INTERNAL_BENCH_DIAGNOSTICS)
set(SUFKIT_IO_BUFFER_KIB "1024" CACHE STRING
    "Private serialization stream buffer size in KiB (64, 256, or 1024)")
set_property(CACHE SUFKIT_IO_BUFFER_KIB PROPERTY STRINGS 64 256 1024)
if(NOT SUFKIT_IO_BUFFER_KIB MATCHES "^(64|256|1024)$")
    message(FATAL_ERROR
        "SUFKIT_IO_BUFFER_KIB must be one of: 64, 256, 1024")
endif()
mark_as_advanced(SUFKIT_IO_BUFFER_KIB)

if(SUFKIT_ENABLE_DEVELOPER_TOOLS)
    include(cmake/SufkitDeveloperTools.cmake)
endif()

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
    set_target_properties(${_target} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        C_VISIBILITY_PRESET hidden)
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
sufkit_enable_x86_64_sse42(sufkit)

target_compile_features(sufkit PUBLIC cxx_std_17)
target_compile_definitions(sufkit PRIVATE
    SUFKIT_BUILDING_LIBRARY
    SUFKIT_HAS_CAPS=$<BOOL:${SUFKIT_ENABLE_CAPS}>
    SUFKIT_IO_BUFFER_KIB=${SUFKIT_IO_BUFFER_KIB})
set(_sufkit_suffix_link_scan_diagnostics OFF)
if(SUFKIT_ENABLE_INTERNAL_BENCH_DIAGNOSTICS)
    if(NOT SUFKIT_BUILD_BENCHMARKS OR NOT SUFKIT_BUILD_CLI OR BUILD_SHARED_LIBS)
        message(FATAL_ERROR
            "SUFKIT_ENABLE_INTERNAL_BENCH_DIAGNOSTICS requires a static "
            "build with SUFKIT_BUILD_CLI=ON and SUFKIT_BUILD_BENCHMARKS=ON")
    endif()
    set(_sufkit_suffix_link_scan_diagnostics ON)
    target_compile_definitions(sufkit PRIVATE
        SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS=1)
endif()
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
            apps/benchmark_provenance.cpp
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
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_cli PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(SUFKIT_BUILD_BENCHMARKS)
        target_include_directories(sufkit_cli PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src)
        if(_sufkit_suffix_link_scan_diagnostics)
            target_compile_definitions(sufkit_cli PRIVATE
                SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS=1)
        endif()
        set(_sufkit_bench_git_commit "unknown")
        set(_sufkit_bench_git_dirty "unknown")
        find_package(Git QUIET)
        if(GIT_FOUND)
            execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}
                    rev-parse HEAD
                OUTPUT_VARIABLE _sufkit_bench_git_commit
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE _sufkit_bench_git_status)
            if(NOT _sufkit_bench_git_status EQUAL 0)
                set(_sufkit_bench_git_commit "unknown")
            endif()
            execute_process(
                COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR}
                    status --porcelain --untracked-files=normal
                OUTPUT_VARIABLE _sufkit_bench_git_changes
                ERROR_QUIET
                RESULT_VARIABLE _sufkit_bench_dirty_status)
            if(_sufkit_bench_dirty_status EQUAL 0)
                if(_sufkit_bench_git_changes STREQUAL "")
                    set(_sufkit_bench_git_dirty "false")
                else()
                    set(_sufkit_bench_git_dirty "true")
                endif()
            endif()
        endif()

        string(TOUPPER "${CMAKE_BUILD_TYPE}" _sufkit_config_upper)
        set(_sufkit_config_flags_name
            "CMAKE_CXX_FLAGS_${_sufkit_config_upper}")
        set(_sufkit_config_flags "${${_sufkit_config_flags_name}}")
        get_target_property(_sufkit_library_options sufkit COMPILE_OPTIONS)
        get_target_property(_sufkit_cli_options sufkit_cli COMPILE_OPTIONS)
        if(NOT _sufkit_library_options)
            set(_sufkit_library_options)
        endif()
        if(NOT _sufkit_cli_options)
            set(_sufkit_cli_options)
        endif()
        string(JOIN " " _sufkit_library_flags ${_sufkit_library_options})
        string(JOIN " " _sufkit_cli_flags ${_sufkit_cli_options})
        set(_sufkit_bench_compile_flags
            "global=${CMAKE_CXX_FLAGS} config=${_sufkit_config_flags} sufkit-private=${_sufkit_library_flags} cli-private=${_sufkit_cli_flags}")
        string(REPLACE "\\" "\\\\" _sufkit_bench_compile_flags
            "${_sufkit_bench_compile_flags}")
        string(REPLACE "\"" "\\\"" _sufkit_bench_compile_flags
            "${_sufkit_bench_compile_flags}")

        set(_sufkit_bench_sse42_compiled 0)
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sufkit_processor)
        if(_sufkit_processor MATCHES "^(amd64|x86_64|x64)$" AND
           CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND
           SUFKIT_COMPILER_HAS_MSSE42)
            set(_sufkit_bench_sse42_compiled 1)
        endif()
        target_compile_definitions(sufkit_cli PRIVATE
            "SUFKIT_BENCH_CMAKE_VERSION=\"${CMAKE_VERSION}\""
            "SUFKIT_BENCH_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}\""
            "SUFKIT_BENCH_GIT_COMMIT=\"${_sufkit_bench_git_commit}\""
            "SUFKIT_BENCH_GIT_DIRTY=\"${_sufkit_bench_git_dirty}\""
            "SUFKIT_BENCH_COMPILE_FLAGS=\"${_sufkit_bench_compile_flags}\""
            SUFKIT_BENCH_SSE42_COMPILED=${_sufkit_bench_sse42_compiled})
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
    if(SUFKIT_BUILD_CLI AND SUFKIT_BUILD_BENCHMARKS)
        add_test(NAME sufkit_benchmark_smoke
            COMMAND ${CMAKE_COMMAND}
                -DSUFKIT_EXECUTABLE=$<TARGET_FILE:sufkit_cli>
                -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-benchmark
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_benchmark.cmake)
        set_tests_properties(sufkit_benchmark_smoke PROPERTIES TIMEOUT 180)
        add_test(NAME sufkit_right_maximal_benchmark_smoke
            COMMAND ${CMAKE_COMMAND}
                -DSUFKIT_EXECUTABLE=$<TARGET_FILE:sufkit_cli>
                -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-right-maximal-benchmark
                -DSUFKIT_SUFFIX_LINK_SCAN_DIAGNOSTICS=${_sufkit_suffix_link_scan_diagnostics}
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
    target_include_directories(sufkit_sa_build_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_sa_build_bench PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_executable(sufkit_low_level_bench
        benchmarks/low_level_benchmark.cpp)
    target_compile_features(sufkit_low_level_bench PRIVATE cxx_std_17)
    target_include_directories(sufkit_low_level_bench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
    sufkit_enable_x86_64_sse42(sufkit_low_level_bench)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_low_level_bench PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_executable(sufkit_query_allocation_bench
        benchmarks/query_allocation_benchmark.cpp)
    target_link_libraries(sufkit_query_allocation_bench PRIVATE
        sufkit::sufkit)
    target_compile_features(sufkit_query_allocation_bench PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_query_allocation_bench PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_executable(sufkit_query_memory_bench
        benchmarks/query_memory_benchmark.cpp)
    target_link_libraries(sufkit_query_memory_bench PRIVATE
        sufkit::sufkit)
    target_compile_features(sufkit_query_memory_bench PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(sufkit_query_memory_bench PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    if(SUFKIT_BUILD_TESTS)
        add_test(NAME sufkit_low_level_kernel_verify
            COMMAND sufkit_low_level_bench --profile smoke --verify-only)
        set_tests_properties(sufkit_low_level_kernel_verify PROPERTIES
            TIMEOUT 60)
        add_test(NAME sufkit_low_level_kernel_verify_quick
            COMMAND sufkit_low_level_bench --profile quick --verify-only)
        set_tests_properties(sufkit_low_level_kernel_verify_quick PROPERTIES
            TIMEOUT 60)
        add_test(NAME sufkit_query_allocation_verify
            COMMAND sufkit_query_allocation_bench --verify-only)
        set_tests_properties(sufkit_query_allocation_verify PROPERTIES
            TIMEOUT 60)
        add_test(NAME sufkit_query_memory_verify
            COMMAND ${CMAKE_COMMAND}
                -DSUFKIT_QUERY_MEMORY_BENCH=$<TARGET_FILE:sufkit_query_memory_bench>
                -DOUTPUT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/test-query-memory
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_query_memory_benchmark.cmake)
        set_tests_properties(sufkit_query_memory_verify PROPERTIES
            TIMEOUT 60)
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
    FILES_MATCHING
    PATTERN "*.md"
    PATTERN "benchmarks/results/artifacts" EXCLUDE)

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
