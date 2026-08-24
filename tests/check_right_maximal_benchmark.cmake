cmake_policy(SET CMP0007 NEW)

if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile smoke
        --methods right-maximal-baseline,right-maximal-suffix-link-binary,right-maximal-suffix-link-sapling,right-maximal-full
        --min-lengths 20,50
        --strands forward,reverse-complement,both
        --query-repetitions 2
        --output-dir "${OUTPUT_ROOT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "right-maximal exact match smoke benchmark failed (${status}):\n${stdout}\n${stderr}")
endif()

foreach(name IN ITEMS run_metadata.tsv build_results.tsv query_results.tsv raw_repetitions.tsv)
    set(path "${OUTPUT_ROOT}/${name}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "missing right-maximal exact match benchmark output: ${name}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "empty right-maximal exact match benchmark output: ${name}")
    endif()
endforeach()

file(READ "${OUTPUT_ROOT}/query_results.tsv" query_results)
file(READ "${OUTPUT_ROOT}/raw_repetitions.tsv" raw_results)
file(READ "${OUTPUT_ROOT}/run_metadata.tsv" run_metadata)
if(NOT run_metadata MATCHES "learned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits")
    message(FATAL_ERROR "right-maximal exact match run_metadata.tsv does not record learned-index parameters")
endif()
foreach(field IN ITEMS query_repetitions git_commit git_dirty compile_flags cpu_flags
                       executable_sha256 cpu_affinity sse42_compiled sse42_runtime
                       command_line_redacted peak_rss_scope strands)
    if(NOT run_metadata MATCHES "${field}")
        message(FATAL_ERROR "right-maximal run_metadata.tsv is missing ${field}")
    endif()
endforeach()
file(STRINGS "${OUTPUT_ROOT}/run_metadata.tsv" metadata_lines)
list(GET metadata_lines 1 metadata_row)
string(REPLACE "\t" ";" metadata_fields "${metadata_row}")
list(GET metadata_fields 20 recorded_repetitions)
list(GET metadata_fields 25 executable_hash)
list(GET metadata_fields 27 sse42_compiled)
list(GET metadata_fields 28 sse42_runtime)
list(GET metadata_fields 29 recorded_command)
list(GET metadata_fields 30 peak_rss_scope)
list(GET metadata_fields 31 recorded_strands)
string(LENGTH "${executable_hash}" executable_hash_length)
if(NOT recorded_repetitions EQUAL 2)
    message(FATAL_ERROR "right-maximal metadata ignored --query-repetitions")
endif()
if(NOT executable_hash_length EQUAL 64)
    message(FATAL_ERROR "right-maximal executable SHA-256 is invalid")
endif()
if(NOT sse42_compiled EQUAL 1 OR NOT sse42_runtime EQUAL 1)
    message(FATAL_ERROR "right-maximal metadata does not record active SSE4.2 support")
endif()
if(NOT peak_rss_scope STREQUAL "method_process_lifetime")
    message(FATAL_ERROR "right-maximal metadata has an incorrect peak RSS scope")
endif()
if(NOT recorded_strands STREQUAL "forward,reverse-complement,both")
    message(FATAL_ERROR "right-maximal metadata has incorrect strands")
endif()
string(FIND "${recorded_command}" "<path>" redacted_position)
string(FIND "${recorded_command}" "${OUTPUT_ROOT}" leaked_path_position)
if(redacted_position EQUAL -1 OR NOT leaked_path_position EQUAL -1)
    message(FATAL_ERROR "right-maximal command path redaction failed")
endif()
foreach(token IN ITEMS "right-maximal-baseline" "right-maximal-suffix-link-binary" "right-maximal-suffix-link-sapling" "right-maximal-full"
                       "result_checksum" "learned_lookup_calls" "suffix_link_success_rate"
                       "prediction_error_mean" "full_binary_fallbacks"
                       "measurement_iterations" "suffix_link_scan_attempts"
                       "suffix_link_left_scanned_rows"
                       "suffix_link_right_scanned_rows"
                       "suffix_link_scanned_rows_p50"
                       "suffix_link_scanned_rows_p95"
                       "suffix_link_scanned_rows_p99"
                       "suffix_link_scanned_rows_max"
                       "suffix_link_scan_seconds"
                       "suffix_link_instrumented_wall_seconds"
                       "suffix_link_scan_instrumented_wall_fraction"
                       "suffix_link_scan_diagnostics_available")
    string(FIND "${query_results}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "query_results.tsv is missing ${token}")
    endif()
endforeach()
if(NOT raw_results MATCHES "status\tmeasurement_iterations")
    message(FATAL_ERROR "raw_repetitions.tsv is missing measurement iteration metadata")
endif()
if(NOT raw_results MATCHES
       "suffix_link_scan_attempts\tsuffix_link_left_scanned_rows")
    message(FATAL_ERROR
        "raw_repetitions.tsv is missing suffix-link scan diagnostics")
endif()
if(NOT query_results MATCHES
       "suffix_link_scan_diagnostics_available\tstrand" OR
   NOT raw_results MATCHES
       "suffix_link_scan_diagnostics_available\tstrand")
    message(FATAL_ERROR "right-maximal result schemas do not append strand")
endif()

file(STRINGS "${OUTPUT_ROOT}/query_results.tsv" strand_lines)
list(REMOVE_AT strand_lines 0)
set(observed_strand_rows)
foreach(line IN LISTS strand_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 1 method)
    list(GET fields 4 min_length)
    list(GET fields 43 strand)
    list(APPEND observed_strand_rows "${method}|${min_length}|${strand}")
endforeach()
foreach(method IN ITEMS right-maximal-baseline
                        right-maximal-suffix-link-binary
                        right-maximal-suffix-link-sapling
                        right-maximal-full)
    foreach(min_length IN ITEMS 20 50)
        foreach(strand IN ITEMS forward reverse-complement both)
            list(FIND observed_strand_rows
                "${method}|${min_length}|${strand}" strand_row_index)
            if(strand_row_index EQUAL -1)
                message(FATAL_ERROR
                    "missing ${method} min=${min_length} strand=${strand}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(SUFKIT_SUFFIX_LINK_SCAN_DIAGNOSTICS)
    file(STRINGS "${OUTPUT_ROOT}/query_results.tsv" diagnostic_lines)
    list(REMOVE_AT diagnostic_lines 0)
    set(found_suffix_link_diagnostics FALSE)
    set(found_nonzero_suffix_link_diagnostics FALSE)
    set(found_baseline_diagnostics FALSE)
    foreach(line IN LISTS diagnostic_lines)
        string(REPLACE "\t" ";" fields "${line}")
        list(GET fields 1 method)
        list(GET fields 17 public_attempts)
        list(GET fields 32 scan_attempts)
        list(GET fields 33 left_rows)
        list(GET fields 34 right_rows)
        list(GET fields 35 rows_p50)
        list(GET fields 36 rows_p95)
        list(GET fields 37 rows_p99)
        list(GET fields 38 rows_max)
        list(GET fields 39 scan_seconds)
        list(GET fields 40 instrumented_wall_seconds)
        list(GET fields 41 scan_fraction)
        list(GET fields 42 diagnostics_available)
        list(GET fields 43 strand)
        if(NOT diagnostics_available EQUAL 1)
            message(FATAL_ERROR
                "static benchmark did not enable suffix-link diagnostics")
        endif()
        if(method MATCHES "suffix-link" OR method STREQUAL "right-maximal-full")
            set(found_suffix_link_diagnostics TRUE)
            if(NOT scan_attempts EQUAL public_attempts)
                message(FATAL_ERROR
                    "suffix-link scan attempt count is missing or inconsistent")
            endif()
            if(scan_attempts GREATER 0)
                set(found_nonzero_suffix_link_diagnostics TRUE)
            endif()
            if(rows_p50 GREATER rows_p95 OR rows_p95 GREATER rows_p99 OR
               rows_p99 GREATER rows_max)
                message(FATAL_ERROR
                    "suffix-link scan percentiles are not monotonic")
            endif()
            math(EXPR total_rows "${left_rows} + ${right_rows}")
            if(rows_max GREATER total_rows)
                message(FATAL_ERROR
                    "suffix-link scan maximum exceeds total scanned rows")
            endif()
            if(scan_seconds GREATER instrumented_wall_seconds)
                message(FATAL_ERROR
                    "suffix-link scan time exceeds instrumented wall time")
            endif()
            if(scan_fraction LESS 0 OR scan_fraction GREATER 1.0)
                message(FATAL_ERROR
                    "suffix-link scan wall fraction is outside [0,1]")
            endif()
        elseif(method STREQUAL "right-maximal-baseline")
            set(found_baseline_diagnostics TRUE)
            if(NOT scan_attempts EQUAL 0 OR NOT left_rows EQUAL 0 OR
               NOT right_rows EQUAL 0)
                message(FATAL_ERROR
                    "baseline unexpectedly recorded suffix-link scan work")
            endif()
        endif()
    endforeach()
    if(NOT found_suffix_link_diagnostics OR
       NOT found_nonzero_suffix_link_diagnostics OR
       NOT found_baseline_diagnostics)
        message(FATAL_ERROR
            "right-maximal smoke did not cover suffix-link diagnostics")
    endif()
endif()
file(STRINGS "${OUTPUT_ROOT}/raw_repetitions.tsv" raw_lines)
list(REMOVE_AT raw_lines 0)
list(LENGTH raw_lines raw_row_count)
if(NOT raw_row_count EQUAL 48)
    message(FATAL_ERROR
        "right-maximal strand/repetition matrix produced ${raw_row_count} raw rows instead of 48")
endif()
set(observed_raw_strands)
foreach(line IN LISTS raw_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 24 iterations)
    list(GET fields 36 strand)
    list(APPEND observed_raw_strands "${strand}")
    if(NOT iterations EQUAL 1)
        message(FATAL_ERROR "right-maximal smoke unexpectedly amplified its workload")
    endif()
endforeach()
foreach(strand IN ITEMS forward reverse-complement both)
    list(FIND observed_raw_strands "${strand}" raw_strand_index)
    if(raw_strand_index EQUAL -1)
        message(FATAL_ERROR "right-maximal raw TSV is missing ${strand}")
    endif()
endforeach()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile smoke
        --query-repetitions 0
        --output-dir "${OUTPUT_ROOT}-invalid-repetitions"
    RESULT_VARIABLE invalid_repetitions_status
    OUTPUT_QUIET ERROR_QUIET)
if(invalid_repetitions_status EQUAL 0)
    message(FATAL_ERROR "right-maximal accepted --query-repetitions 0")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile smoke
        --strands forward,invalid
        --output-dir "${OUTPUT_ROOT}-invalid-strands"
    RESULT_VARIABLE invalid_strands_status
    OUTPUT_QUIET ERROR_QUIET)
if(invalid_strands_status EQUAL 0)
    message(FATAL_ERROR "right-maximal accepted an invalid strand")
endif()

set(user_dir "${OUTPUT_ROOT}-user")
file(REMOVE_RECURSE "${user_dir}")
string(REPEAT "ACGT" 64 user_reference)
string(REPEAT "ACGT" 5 user_query)
file(WRITE "${OUTPUT_ROOT}-reference.fa" ">reference\n${user_reference}\n")
file(WRITE "${OUTPUT_ROOT}-queries.fa" ">query\n${user_query}\n")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --reference "${OUTPUT_ROOT}-reference.fa"
        --queries "${OUTPUT_ROOT}-queries.fa"
        --methods right-maximal-baseline
        --min-lengths 20
        --output-dir "${user_dir}"
    RESULT_VARIABLE user_status
    OUTPUT_VARIABLE user_stdout
    ERROR_VARIABLE user_stderr)
if(NOT user_status EQUAL 0)
    message(FATAL_ERROR "right-maximal user benchmark failed (${user_status}):\n${user_stdout}\n${user_stderr}")
endif()
file(STRINGS "${user_dir}/query_results.tsv" user_lines)
list(REMOVE_AT user_lines 0)
list(GET user_lines 0 user_line)
string(REPLACE "\t" ";" user_fields "${user_line}")
list(GET user_fields 5 user_query_count)
list(GET user_fields 31 user_iterations)
list(GET user_fields 43 user_strand)
if(NOT user_query_count EQUAL 1)
    message(FATAL_ERROR "right-maximal user query count was not normalized")
endif()
if(user_iterations LESS 2)
    message(FATAL_ERROR "right-maximal user benchmark still used smoke timing")
endif()
if(NOT user_strand STREQUAL "forward")
    message(FATAL_ERROR "right-maximal default strand is no longer forward")
endif()

set(strand_dir "${OUTPUT_ROOT}-strand-nonzero")
file(REMOVE_RECURSE "${strand_dir}")
string(REPEAT "GGGGGTTTTT" 32 strand_reference)
file(WRITE "${OUTPUT_ROOT}-strand-reference.fa"
    ">reference\n${strand_reference}\n")
file(WRITE "${OUTPUT_ROOT}-strand-queries.fa"
    ">reverse-only\nAAAAACCCCC\n")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --reference "${OUTPUT_ROOT}-strand-reference.fa"
        --queries "${OUTPUT_ROOT}-strand-queries.fa"
        --methods right-maximal-baseline
        --min-lengths 10
        --strands forward,reverse-complement,both
        --query-repetitions 1
        --output-dir "${strand_dir}"
    RESULT_VARIABLE strand_status
    OUTPUT_VARIABLE strand_stdout
    ERROR_VARIABLE strand_stderr)
if(NOT strand_status EQUAL 0)
    message(FATAL_ERROR
        "right-maximal strand benchmark failed (${strand_status}):\n"
        "${strand_stdout}\n${strand_stderr}")
endif()
file(STRINGS "${strand_dir}/query_results.tsv" strand_result_lines)
list(REMOVE_AT strand_result_lines 0)
set(forward_matches "")
set(reverse_matches "")
set(both_matches "")
foreach(line IN LISTS strand_result_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 12 total_matches)
    list(GET fields 43 strand)
    if(strand STREQUAL "forward")
        set(forward_matches "${total_matches}")
    elseif(strand STREQUAL "reverse-complement")
        set(reverse_matches "${total_matches}")
    elseif(strand STREQUAL "both")
        set(both_matches "${total_matches}")
    endif()
endforeach()
if(forward_matches STREQUAL "" OR reverse_matches STREQUAL "" OR
   both_matches STREQUAL "")
    message(FATAL_ERROR "right-maximal strand rows are incomplete")
endif()
if(NOT forward_matches EQUAL 0 OR NOT reverse_matches GREATER 0 OR
   NOT both_matches EQUAL reverse_matches)
    message(FATAL_ERROR
        "right-maximal strand semantics are incorrect: forward=${forward_matches}, "
        "reverse=${reverse_matches}, both=${both_matches}")
endif()
