# Developer-only formatting and static-analysis targets. Keep this source list
# explicit so that vendored dependencies and benchmark implementations can
# never be rewritten accidentally.

find_program(SUFKIT_CLANG_FORMAT_EXECUTABLE
    NAMES clang-format-18
    DOC "clang-format 18 executable used by sufkit developer targets")
if(NOT SUFKIT_CLANG_FORMAT_EXECUTABLE)
    message(FATAL_ERROR
        "SUFKIT_ENABLE_DEVELOPER_TOOLS requires clang-format-18")
endif()

find_program(SUFKIT_CLANG_TIDY_EXECUTABLE
    NAMES clang-tidy-18
    DOC "clang-tidy 18 executable used by sufkit developer targets")
if(NOT SUFKIT_CLANG_TIDY_EXECUTABLE)
    message(FATAL_ERROR
        "SUFKIT_ENABLE_DEVELOPER_TOOLS requires clang-tidy-18")
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(_sufkit_format_files
    include/sufkit/export.hpp
    include/sufkit/fm_index.hpp
    include/sufkit/genome_reference.hpp
    include/sufkit/inspect.hpp
    include/sufkit/suffix_array.hpp
    include/sufkit/sufkit.hpp
    include/sufkit/types.hpp
    include/sufkit/version.hpp
    src/caps_backend.cpp
    src/caps_backend.hpp
    src/divsufsort_backend.cpp
    src/divsufsort_backend.hpp
    src/error.cpp
    src/fm_index.cpp
    src/genome_reference.cpp
    src/genome_reference_internal.hpp
    src/inspect.cpp
    src/query.cpp
    src/query.hpp
    src/reference_data.hpp
    src/serialization.cpp
    src/serialization.hpp
    src/suffix_array.cpp
    apps/app_support.cpp
    apps/app_support.hpp
    apps/sufkit.cpp
    tests/test_caps_backend.cpp
    tests/test_sampled_sa.cpp
    tests/test_sufkit.cpp
    examples/add_subdirectory/main.cpp
    examples/basic.cpp
    examples/exact_sa.cpp
    examples/find_package/main.cpp
    examples/fm_batch.cpp
    examples/inspect_error.cpp
    examples/right_maximal_stream.cpp)

list(TRANSFORM _sufkit_format_files
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_custom_target(sufkit-format
    COMMAND ${SUFKIT_CLANG_FORMAT_EXECUTABLE}
        --style=file
        -i
        ${_sufkit_format_files}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Formatting first-party sufkit C++ sources with clang-format 18"
    COMMAND_EXPAND_LISTS
    VERBATIM)

add_custom_target(sufkit-format-check
    COMMAND ${SUFKIT_CLANG_FORMAT_EXECUTABLE}
        --style=file
        --dry-run
        --Werror
        ${_sufkit_format_files}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking first-party sufkit C++ formatting with clang-format 18"
    COMMAND_EXPAND_LISTS
    VERBATIM)

# Only translation units enabled by this configure invocation are sent to
# clang-tidy, ensuring that each file has a matching compilation database entry.
set(_sufkit_tidy_files
    src/caps_backend.cpp
    src/divsufsort_backend.cpp
    src/error.cpp
    src/fm_index.cpp
    src/genome_reference.cpp
    src/inspect.cpp
    src/query.cpp
    src/serialization.cpp
    src/suffix_array.cpp)
if(SUFKIT_BUILD_CLI)
    list(APPEND _sufkit_tidy_files
        apps/app_support.cpp
        apps/sufkit.cpp)
endif()
if(SUFKIT_BUILD_TESTS)
    list(APPEND _sufkit_tidy_files
        tests/test_caps_backend.cpp
        tests/test_sampled_sa.cpp
        tests/test_sufkit.cpp)
endif()
if(SUFKIT_BUILD_EXAMPLES)
    list(APPEND _sufkit_tidy_files
        examples/basic.cpp
        examples/exact_sa.cpp
        examples/fm_batch.cpp
        examples/inspect_error.cpp
        examples/right_maximal_stream.cpp)
endif()
list(TRANSFORM _sufkit_tidy_files
    PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

add_custom_target(sufkit-tidy-check
    COMMAND ${SUFKIT_CLANG_TIDY_EXECUTABLE}
        --quiet
        -p=${CMAKE_CURRENT_BINARY_DIR}
        --config-file=${CMAKE_CURRENT_SOURCE_DIR}/.clang-tidy
        ${_sufkit_tidy_files}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking first-party sufkit C++ style with clang-tidy 18"
    COMMAND_EXPAND_LISTS
    VERBATIM)

unset(_sufkit_format_files)
unset(_sufkit_tidy_files)
