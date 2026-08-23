if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload mem
        --profile smoke
        --methods mem-baseline,mem-suffix-link-binary,mem-suffix-link-sapling,mem-full
        --min-lengths 20,50
        --output-dir "${OUTPUT_ROOT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "MEM smoke benchmark failed (${status}):\n${stdout}\n${stderr}")
endif()

foreach(name IN ITEMS run_metadata.tsv build_results.tsv query_results.tsv raw_repetitions.tsv)
    set(path "${OUTPUT_ROOT}/${name}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "missing MEM benchmark output: ${name}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "empty MEM benchmark output: ${name}")
    endif()
endforeach()

file(READ "${OUTPUT_ROOT}/query_results.tsv" query_results)
file(READ "${OUTPUT_ROOT}/run_metadata.tsv" run_metadata)
if(NOT run_metadata MATCHES "learned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits")
    message(FATAL_ERROR "MEM run_metadata.tsv does not record learned-index parameters")
endif()
foreach(token IN ITEMS "mem-baseline" "mem-suffix-link-binary" "mem-suffix-link-sapling" "mem-full"
                       "result_checksum" "learned_lookup_calls" "suffix_link_success_rate"
                       "prediction_error_mean" "full_binary_fallbacks")
    string(FIND "${query_results}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "query_results.tsv is missing ${token}")
    endif()
endforeach()
