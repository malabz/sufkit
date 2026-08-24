cmake_policy(SET CMP0007 NEW)

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
            --methods naive,sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,sa32-sampled-k2-binary,sa32-sampled-k4-lcp-binary,sa32-sampled-k8-binary,sa64-sampled-k2-lcp-binary,fm
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
foreach(field IN ITEMS git_commit git_dirty compile_flags cpu_flags executable_sha256
                       cpu_affinity sse42_compiled sse42_runtime
                       command_line_redacted peak_rss_scope)
    if(NOT first_metadata MATCHES "${field}")
        message(FATAL_ERROR "run_metadata.tsv is missing ${field}")
    endif()
endforeach()
file(STRINGS "${first}/run_metadata.tsv" metadata_lines)
list(GET metadata_lines 1 metadata_row)
string(REPLACE "\t" ";" metadata_fields "${metadata_row}")
list(GET metadata_fields 37 executable_hash)
list(GET metadata_fields 39 sse42_compiled)
list(GET metadata_fields 40 sse42_runtime)
list(GET metadata_fields 41 recorded_command)
list(GET metadata_fields 42 peak_rss_scope)
string(LENGTH "${executable_hash}" executable_hash_length)
if(NOT executable_hash_length EQUAL 64)
    message(FATAL_ERROR "run metadata executable SHA-256 is invalid")
endif()
file(SHA256 "${SUFKIT_EXECUTABLE}" expected_executable_hash)
if(NOT executable_hash STREQUAL expected_executable_hash)
    message(FATAL_ERROR "run metadata fingerprints the wrong executable")
endif()
if(NOT sse42_compiled EQUAL 1 OR NOT sse42_runtime EQUAL 1)
    message(FATAL_ERROR "run metadata does not record active SSE4.2 support")
endif()
if(NOT peak_rss_scope STREQUAL "method_process_lifetime")
    message(FATAL_ERROR "run metadata has an incorrect peak RSS scope")
endif()
string(FIND "${recorded_command}" "<path>" redacted_position)
string(FIND "${recorded_command}" "${OUTPUT_ROOT}" leaked_path_position)
if(redacted_position EQUAL -1 OR NOT leaked_path_position EQUAL -1)
    message(FATAL_ERROR "run metadata command path redaction failed")
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
foreach(method IN ITEMS sa32-sampled-k2-binary sa32-sampled-k4-lcp-binary
                        sa32-sampled-k8-binary sa64-sampled-k2-lcp-binary)
    if(NOT build_results MATCHES "\t${method}\t")
        message(FATAL_ERROR "build_results.tsv is missing ${method}")
    endif()
endforeach()
if(NOT build_results MATCHES "status\tsa_sampling_rate")
    message(FATAL_ERROR "build_results.tsv is missing SA sampling metadata")
endif()
file(STRINGS "${first}/build_results.tsv" build_lines)
list(REMOVE_AT build_lines 0)
foreach(line IN LISTS build_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 3 method)
    list(GET fields 26 sampling_rate)
    if(method MATCHES "sampled-k2" AND NOT sampling_rate EQUAL 2)
        message(FATAL_ERROR "${method} reported the wrong sampling rate")
    elseif(method MATCHES "sampled-k4" AND NOT sampling_rate EQUAL 4)
        message(FATAL_ERROR "${method} reported the wrong sampling rate")
    elseif(method MATCHES "sampled-k8" AND NOT sampling_rate EQUAL 8)
        message(FATAL_ERROR "${method} reported the wrong sampling rate")
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
file(STRINGS "${first}/raw_repetitions.tsv" raw_lines)
list(REMOVE_AT raw_lines 0)
foreach(line IN LISTS raw_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 4 phase)
    list(GET fields 19 row_status)
    if(phase STREQUAL "query" AND row_status STREQUAL "ok")
        list(GET fields 11 measured_queries)
        if(measured_queries LESS 1)
            message(FATAL_ERROR "measured benchmark query count is invalid")
        endif()
    endif()
endforeach()

set(fm_dir "${OUTPUT_ROOT}/fm-backends")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke
        --scenarios balanced
        --methods fm-huff,fm-balanced,fm-epr
        --fm-query-modes scalar,batch,batch-mixed
        --fm-batch-widths 1,4
        --pattern-lengths 20,50
        --locate-limits 1
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${fm_dir}"
    RESULT_VARIABLE fm_status
    OUTPUT_VARIABLE fm_stdout
    ERROR_VARIABLE fm_stderr)
if(NOT fm_status EQUAL 0)
    message(FATAL_ERROR "FM backend benchmark failed (${fm_status}):\n${fm_stdout}\n${fm_stderr}")
endif()
file(READ "${fm_dir}/run_metadata.tsv" fm_metadata)
file(READ "${fm_dir}/build_results.tsv" fm_builds)
file(READ "${fm_dir}/query_results.tsv" fm_queries)
file(READ "${fm_dir}/raw_repetitions.tsv" fm_raw)
if(NOT fm_metadata MATCHES "fm_query_modes\tfm_batch_widths")
    message(FATAL_ERROR "FM benchmark metadata fields are missing")
endif()
foreach(method IN ITEMS fm-huff fm-balanced fm-epr)
    if(NOT fm_builds MATCHES "\t${method}\t")
        message(FATAL_ERROR "FM build results are missing ${method}")
    endif()
endforeach()
if(NOT fm_queries MATCHES "fm_query_mode\tfm_batch_width\tquery_bases\tquery_bases_per_second\tspeedup_vs_fm_huff_scalar")
    message(FATAL_ERROR "FM query summary fields are missing")
endif()
if(NOT fm_queries MATCHES "\tbatch\t1\t" OR NOT fm_queries MATCHES "\tbatch\t4\t")
    message(FATAL_ERROR "FM batch width rows are missing")
endif()
set(have_scalar_mixed FALSE)
set(have_batch_mixed_1 FALSE)
set(have_batch_mixed_4 FALSE)
file(STRINGS "${fm_dir}/query_results.tsv" fm_query_lines)
list(REMOVE_AT fm_query_lines 0)
foreach(line IN LISTS fm_query_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 4 query_group)
    list(GET fields 5 pattern_length)
    list(GET fields 9 query_count)
    list(GET fields 33 row_status)
    list(GET fields 34 query_mode)
    list(GET fields 35 batch_width)
    if(query_group STREQUAL "mixed_length" AND
       pattern_length STREQUAL "mixed" AND row_status STREQUAL "ok" AND
       query_count GREATER 0)
        if(query_mode STREQUAL "scalar" AND batch_width STREQUAL "NA")
            set(have_scalar_mixed TRUE)
        elseif(query_mode STREQUAL "batch-mixed" AND batch_width EQUAL 1)
            set(have_batch_mixed_1 TRUE)
        elseif(query_mode STREQUAL "batch-mixed" AND batch_width EQUAL 4)
            set(have_batch_mixed_4 TRUE)
        endif()
    endif()
endforeach()
if(NOT have_scalar_mixed OR NOT have_batch_mixed_1 OR NOT have_batch_mixed_4)
    message(FATAL_ERROR "FM mixed-length scalar/batch rows are missing")
endif()
if(NOT fm_raw MATCHES "fm_query_mode\tfm_batch_width\tquery_bases")
    message(FATAL_ERROR "FM raw repetition fields are missing")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --methods fm,fm-huff --output-dir "${OUTPUT_ROOT}/alias-conflict"
    RESULT_VARIABLE alias_status
    OUTPUT_QUIET ERROR_QUIET)
if(alias_status EQUAL 0)
    message(FATAL_ERROR "fm and fm-huff aliases were accepted together")
endif()

set(user_dir "${OUTPUT_ROOT}/user-high-frequency")
string(REPEAT "A" 100100 homopolymer)
file(WRITE "${OUTPUT_ROOT}/reference.fa" ">homopolymer\n${homopolymer}\n")
file(WRITE "${OUTPUT_ROOT}/queries.fa" ">frequent\nAAAAAAAAAAAAAAAAAAAA\n")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --reference "${OUTPUT_ROOT}/reference.fa"
        --queries "${OUTPUT_ROOT}/queries.fa"
        --methods sa32-binary,fm-huff
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
file(STRINGS "${user_dir}/query_results.tsv" user_lines)
list(REMOVE_AT user_lines 0)
foreach(line IN LISTS user_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(GET fields 7 operation)
    list(GET fields 33 row_status)
    if(operation STREQUAL "count" AND row_status STREQUAL "ok")
        list(GET fields 9 logical_queries)
        list(GET fields 15 logical_hits)
        if(NOT logical_queries EQUAL 1)
            message(FATAL_ERROR "calibrated query count was not normalized to one logical pass")
        endif()
        if(logical_hits GREATER 100081)
            message(FATAL_ERROR "calibrated hit count was not normalized to one logical pass")
        endif()
    endif()
endforeach()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench --profile smoke --reference missing.fa --output-dir invalid
    RESULT_VARIABLE conflict_status
    OUTPUT_QUIET ERROR_QUIET)
if(conflict_status EQUAL 0)
    message(FATAL_ERROR "conflicting --profile and --reference options were accepted")
endif()
