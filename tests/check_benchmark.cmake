if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

set(first "${OUTPUT_ROOT}/first")
set(second "${OUTPUT_ROOT}/second")
file(REMOVE_RECURSE "${OUTPUT_ROOT}")

foreach(output_dir IN ITEMS "${first}" "${second}")
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" bench
            --profile smoke
            --scenarios balanced
            --methods naive,sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,fm
            --pattern-lengths 20,50
            --locate-limits 1,all
            --build-repetitions 1
            --query-repetitions 1
            --warmups 0
            --output-dir "${output_dir}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "smoke benchmark failed (${status}):\n${stdout}\n${stderr}")
    endif()
    foreach(name IN ITEMS run_metadata.tsv build_results.tsv query_results.tsv raw_repetitions.tsv)
        if(NOT EXISTS "${output_dir}/${name}")
            message(FATAL_ERROR "missing benchmark result: ${output_dir}/${name}")
        endif()
    endforeach()
endforeach()

file(READ "${first}/run_metadata.tsv" first_metadata)
file(READ "${second}/run_metadata.tsv" second_metadata)
if(NOT first_metadata MATCHES "methods\tpattern_lengths\tlocate_limits\tbuild_repetitions")
    message(FATAL_ERROR "run_metadata.tsv does not record benchmark parameters")
endif()
if(NOT first_metadata MATCHES "learned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits")
    message(FATAL_ERROR "run_metadata.tsv does not record learned-index parameters")
endif()
string(REGEX MATCH "synthetic-smoke-balanced\t([0-9a-f]+)" first_match "${first_metadata}")
set(first_fingerprint "${CMAKE_MATCH_1}")
string(REGEX MATCH "synthetic-smoke-balanced\t([0-9a-f]+)" second_match "${second_metadata}")
set(second_fingerprint "${CMAKE_MATCH_1}")
if(first_fingerprint STREQUAL "" OR NOT first_fingerprint STREQUAL second_fingerprint)
    message(FATAL_ERROR "fixed-seed benchmark fingerprint is not deterministic")
endif()

file(READ "${first}/build_results.tsv" build_results)
file(READ "${first}/query_results.tsv" query_results)
file(READ "${first}/raw_repetitions.tsv" raw_results)
foreach(method IN ITEMS naive sa32-binary sa32-lcp-binary sa32-sapling sa32-child fm)
    if(NOT build_results MATCHES "\t${method}\t")
        message(FATAL_ERROR "build_results.tsv is missing ${method}")
    endif()
endforeach()
if(NOT query_results MATCHES "query_group\tpattern_length\tstrand\toperation\tmax_hits")
    message(FATAL_ERROR "query_results.tsv has an unexpected schema")
endif()
foreach(metric IN ITEMS suffix_comparisons character_comparisons gallop_probes predictions
                        prediction_error_mean prediction_error_p50 prediction_error_p95
                        prediction_error_p99 full_binary_fallbacks)
    if(NOT query_results MATCHES "${metric}")
        message(FATAL_ERROR "query_results.tsv is missing learned-search metric ${metric}")
    endif()
endforeach()
if(NOT build_results MATCHES "learned_index_build_seconds_median" OR
   NOT build_results MATCHES "learned_index_bytes")
    message(FATAL_ERROR "build_results.tsv is missing learned-index build metrics")
endif()
if(NOT raw_results MATCHES "phase\tquery_group\tpattern_length\tstrand")
    message(FATAL_ERROR "raw_repetitions.tsv has an unexpected schema")
endif()
if(NOT raw_results MATCHES "query_definition" OR NOT raw_results MATCHES "query_id\tquery_source")
    message(FATAL_ERROR "raw_repetitions.tsv is missing ordered query definitions")
endif()

set(user_dir "${OUTPUT_ROOT}/user-high-frequency")
string(REPEAT "A" 100100 homopolymer)
file(WRITE "${OUTPUT_ROOT}/reference.fa" ">homopolymer\n${homopolymer}\n")
file(WRITE "${OUTPUT_ROOT}/queries.fa" ">frequent\nAAAAAAAAAAAAAAAAAAAA\n")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --reference "${OUTPUT_ROOT}/reference.fa"
        --queries "${OUTPUT_ROOT}/queries.fa"
        --methods fm
        --locate-limits all
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${user_dir}"
    RESULT_VARIABLE user_status
    OUTPUT_VARIABLE user_stdout
    ERROR_VARIABLE user_stderr)
if(NOT user_status EQUAL 0)
    message(FATAL_ERROR "user FASTA benchmark failed (${user_status}):\n${user_stdout}\n${user_stderr}")
endif()
file(READ "${user_dir}/query_results.tsv" user_results)
if(NOT user_results MATCHES "skipped_high_frequency")
    message(FATAL_ERROR "complete locate was not skipped for a high-frequency query")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench --profile smoke --reference missing.fa --output-dir invalid
    RESULT_VARIABLE conflict_status
    OUTPUT_QUIET ERROR_QUIET)
if(conflict_status EQUAL 0)
    message(FATAL_ERROR "conflicting --profile and --reference options were accepted")
endif()
