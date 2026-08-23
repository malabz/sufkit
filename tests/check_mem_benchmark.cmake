if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload mem
        --profile smoke
        --methods mem-baseline,mem-lcp,mem-child,mem-suffix-link,mem-full
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
foreach(token IN ITEMS "mem-baseline" "mem-lcp" "mem-child" "mem-suffix-link" "mem-full" "result_checksum")
    string(FIND "${query_results}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "query_results.tsv is missing ${token}")
    endif()
endforeach()
