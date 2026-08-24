if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile smoke
        --methods right-maximal-baseline,right-maximal-suffix-link-binary,right-maximal-suffix-link-sapling,right-maximal-full,right-maximal-sampled-k4,right-maximal-sampled-k8
        --min-lengths 20,50
        --build-repetitions 2
        --query-repetitions 2
        --warmups 0
        --output-dir "${OUTPUT_ROOT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "right-maximal exact match smoke benchmark failed (${status}):\n${stdout}\n${stderr}")
endif()

foreach(name IN ITEMS run_metadata.tsv correctness_summary.tsv build_results.tsv query_results.tsv raw_repetitions.tsv)
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
file(READ "${OUTPUT_ROOT}/run_metadata.tsv" run_metadata)
file(READ "${OUTPUT_ROOT}/build_results.tsv" build_results)
file(READ "${OUTPUT_ROOT}/raw_repetitions.tsv" raw_repetitions)
file(READ "${OUTPUT_ROOT}/correctness_summary.tsv" correctness_summary)
if(NOT run_metadata MATCHES "build_repetitions\tquery_repetitions\twarmups" OR
   NOT run_metadata MATCHES "vector_materialization_match_threshold" OR
   NOT run_metadata MATCHES "learned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits")
    message(FATAL_ERROR "right-maximal exact match run_metadata.tsv does not record learned-index parameters")
endif()
foreach(token IN ITEMS "naive_right_maximal_oracle_status" "oracle_reference_bases"
                       "oracle_query_bases" "passed")
    string(FIND "${run_metadata}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "run_metadata.tsv is missing oracle marker ${token}")
    endif()
endforeach()
foreach(token IN ITEMS "right-maximal-baseline" "right-maximal-suffix-link-binary"
                       "right-maximal-suffix-link-sapling" "right-maximal-full"
                       "right-maximal-sampled-k4" "right-maximal-sampled-k8"
                       "operation" "streaming" "vector" "max_matches=0" "max_matches=1000"
                       "reported_matches" "count_checksum" "result_checksum"
                       "materialization_match_threshold" "vector_skipped"
                       "query_peak_rss_mb" "peak_rss_scope"
                       "query_worker_inherited_controller_dataset_queries_plus_load_plus_query"
                       "learned_lookup_calls" "suffix_link_success_rate"
                       "prediction_error_mean" "full_binary_fallbacks"
                       "fm-huff" "fm-balanced" "fm-epr" "not_supported")
    string(FIND "${query_results}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "query_results.tsv is missing ${token}")
    endif()
endforeach()

foreach(token IN ITEMS "sa_sampling_rate\trepetitions" "right-maximal-sampled-k4"
                       "right-maximal-sampled-k8" "build_peak_rss_mb"
                       "save_peak_rss_mb" "load_peak_rss_mb"
                       "build_worker_inherited_controller_dataset_plus_build"
                       "save_worker_inherited_controller_dataset_plus_load_plus_save"
                       "load_worker_inherited_controller_dataset_plus_load"
                       "allocated_disk_bytes")
    string(FIND "${build_results}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "build_results.tsv is missing ${token}")
    endif()
endforeach()

foreach(token IN ITEMS "naive-right-maximal-forward" "synthetic-right-maximal-smoke-mixed"
                       "\tok")
    string(FIND "${correctness_summary}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "correctness_summary.tsv is missing ${token}")
    endif()
endforeach()

foreach(token IN ITEMS "user_cpu_seconds" "system_cpu_seconds" "peak_rss_mb"
                       "peak_rss_scope" "query_bases" "serialized_bytes"
                       "allocated_disk_bytes" "auxiliary_bytes"
                       "materialization_match_threshold" "vector_skipped"
                       "save_worker_inherited_controller_dataset_plus_load_plus_save"
                       "query_worker_inherited_controller_dataset_queries_plus_load_plus_query"
                       "fm-huff\tstreaming\t20\t0\tNA" "not_supported")
    string(FIND "${raw_repetitions}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "raw_repetitions.tsv is missing resource/capability evidence ${token}")
    endif()
endforeach()

foreach(token IN ITEMS "\tbuild\t0\t1\t" "\tstreaming\t20\t1\t"
                       "\tvector\t20\t1\t" "\tmax_matches=0\t20\t1\t"
                       "\tmax_matches=1000\t20\t1\t")
    string(FIND "${raw_repetitions}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "raw_repetitions.tsv is missing repetition evidence ${token}")
    endif()
endforeach()

# Exercise the full profile parser without allocating its 256 MiB synthetic
# dataset: a user-provided reference keeps this acceptance check lightweight.
set(full_reference "${OUTPUT_ROOT}-full-reference.fa")
set(full_output "${OUTPUT_ROOT}-full-profile")
file(REMOVE_RECURSE "${full_output}")
file(WRITE "${full_reference}"
    ">ref0\nACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGT\n")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile full
        --reference "${full_reference}"
        --methods right-maximal-baseline
        --min-lengths 500
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${full_output}"
    RESULT_VARIABLE full_status
    OUTPUT_VARIABLE full_stdout
    ERROR_VARIABLE full_stderr)
if(NOT full_status EQUAL 0)
    message(FATAL_ERROR "right-maximal full-profile acceptance check failed (${full_status}):\n${full_stdout}\n${full_stderr}")
endif()

file(READ "${full_output}/run_metadata.tsv" full_metadata)
if(NOT full_metadata MATCHES "full\tuser-reference")
    message(FATAL_ERROR "full external-reference metadata does not retain the full profile")
endif()
if(NOT full_metadata MATCHES "\t10000\t2560000\t")
    message(FATAL_ERROR "full external-reference query generation did not use the full profile")
endif()

set(scenario_output "${OUTPUT_ROOT}-scenario-names")
file(REMOVE_RECURSE "${scenario_output}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile smoke
        --scenarios gc-skewed,n-islands
        --methods right-maximal-baseline
        --min-lengths 20
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${scenario_output}"
    RESULT_VARIABLE scenario_status
    OUTPUT_VARIABLE scenario_stdout
    ERROR_VARIABLE scenario_stderr)
if(NOT scenario_status EQUAL 0)
    message(FATAL_ERROR "right-maximal scenario-name check failed (${scenario_status}):\n${scenario_stdout}\n${scenario_stderr}")
endif()
file(READ "${scenario_output}/run_metadata.tsv" scenario_metadata)
foreach(token IN ITEMS "smoke\tgc-skewed" "smoke\tn-islands")
    string(FIND "${scenario_metadata}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "run_metadata.tsv is missing explicit scenario ${token}")
    endif()
endforeach()

# A deliberately repetitive but small fixture must cross the unlimited-vector
# safety threshold. Streaming and bounded max_matches paths still run; the
# full-vector row must be explicit NA/skipped and must retain the true count.
set(high_frequency_reference "${OUTPUT_ROOT}-high-frequency-reference.fa")
set(high_frequency_queries "${OUTPUT_ROOT}-high-frequency-queries.fa")
set(high_frequency_output "${OUTPUT_ROOT}-high-frequency")
file(REMOVE_RECURSE "${high_frequency_output}")
string(REPEAT "A" 512 high_frequency_reference_sequence)
string(REPEAT "A" 64 high_frequency_query_sequence)
file(WRITE "${high_frequency_reference}"
    ">repeat-reference\n${high_frequency_reference_sequence}\n")
file(WRITE "${high_frequency_queries}" "")
foreach(query_id RANGE 0 1999)
    file(APPEND "${high_frequency_queries}"
        ">repeat-query-${query_id}\n${high_frequency_query_sequence}\n")
endforeach()
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload right-maximal
        --profile smoke
        --reference "${high_frequency_reference}"
        --queries "${high_frequency_queries}"
        --methods right-maximal-baseline
        --min-lengths 20
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${high_frequency_output}"
    RESULT_VARIABLE high_frequency_status
    OUTPUT_VARIABLE high_frequency_stdout
    ERROR_VARIABLE high_frequency_stderr)
if(NOT high_frequency_status EQUAL 0)
    message(FATAL_ERROR
        "right-maximal high-frequency safety check failed (${high_frequency_status}):\n"
        "${high_frequency_stdout}\n${high_frequency_stderr}")
endif()
file(READ "${high_frequency_output}/query_results.tsv" high_frequency_results)
file(STRINGS "${high_frequency_output}/query_results.tsv" high_frequency_vector_rows
    REGEX "vector.*skipped_high_frequency$")
list(LENGTH high_frequency_vector_rows high_frequency_vector_row_count)
if(NOT high_frequency_vector_row_count EQUAL 1)
    message(FATAL_ERROR
        "expected exactly one skipped high-frequency vector row, found "
        "${high_frequency_vector_row_count}:\n${high_frequency_results}")
endif()
list(GET high_frequency_vector_rows 0 high_frequency_vector_row)
string(REPLACE "\t" ";" high_frequency_vector_columns
    "${high_frequency_vector_row}")
list(GET high_frequency_vector_columns 8 high_frequency_vector_seconds)
list(GET high_frequency_vector_columns 13 high_frequency_vector_rss)
list(GET high_frequency_vector_columns 14 high_frequency_vector_scope)
list(GET high_frequency_vector_columns 15 high_frequency_vector_total)
list(GET high_frequency_vector_columns 16 high_frequency_vector_reported)
list(GET high_frequency_vector_columns 35 high_frequency_vector_threshold)
list(GET high_frequency_vector_columns 36 high_frequency_vector_skipped)
list(GET high_frequency_vector_columns 37 high_frequency_vector_status_value)
if(NOT high_frequency_vector_seconds STREQUAL "NA" OR
   NOT high_frequency_vector_rss STREQUAL "NA" OR
   NOT high_frequency_vector_scope STREQUAL "not_applicable" OR
   NOT high_frequency_vector_total GREATER 1000000 OR
   NOT high_frequency_vector_reported EQUAL 0 OR
   NOT high_frequency_vector_threshold EQUAL 1000000 OR
   NOT high_frequency_vector_skipped EQUAL 1 OR
   NOT high_frequency_vector_status_value STREQUAL "skipped_high_frequency")
    message(FATAL_ERROR
        "invalid high-frequency vector safety row: ${high_frequency_vector_row}")
endif()
foreach(operation IN ITEMS streaming "max_matches=0" "max_matches=1000")
    if(NOT high_frequency_results MATCHES "${operation}.*ok")
        message(FATAL_ERROR
            "high-frequency bounded operation ${operation} did not complete")
    endif()
endforeach()
file(READ "${high_frequency_output}/raw_repetitions.tsv" high_frequency_raw)
if(NOT high_frequency_raw MATCHES
   "materialization_match_threshold\tvector_skipped" OR
   NOT high_frequency_raw MATCHES "vector.*1000000\t1.*skipped_high_frequency")
    message(FATAL_ERROR
        "raw repetitions do not preserve high-frequency skip evidence")
endif()
