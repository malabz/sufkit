if(NOT DEFINED SUFKIT_SA_BUILD_BENCH OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_SA_BUILD_BENCH and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE
    "${OUTPUT_ROOT}"
    "${OUTPUT_ROOT}-full-profile"
    "${OUTPUT_ROOT}-full-reference.fa")
execute_process(
    COMMAND "${SUFKIT_SA_BUILD_BENCH}"
        --profile smoke
        --methods div32,div64,caps32,caps64
        --threads 1,2
        --sampling-rates 1,2
        --acceleration full
        --repetitions 1
        --seed 20260822
        --output-dir "${OUTPUT_ROOT}"
    RESULT_VARIABLE status
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "SA build smoke benchmark failed (${status}):\n${stdout}\n${stderr}")
endif()

foreach(name IN ITEMS run_metadata.tsv build_results.tsv raw_repetitions.tsv)
    set(path "${OUTPUT_ROOT}/${name}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "missing SA build benchmark output: ${name}")
    endif()
    file(SIZE "${path}" size)
    if(size EQUAL 0)
        message(FATAL_ERROR "empty SA build benchmark output: ${name}")
    endif()
endforeach()

file(READ "${OUTPUT_ROOT}/run_metadata.tsv" metadata)
file(READ "${OUTPUT_ROOT}/build_results.tsv" summary)
file(READ "${OUTPUT_ROOT}/raw_repetitions.tsv" raw)

if(NOT metadata MATCHES "reference_read_seconds\tnormalization_seconds")
    message(FATAL_ERROR "SA build metadata is missing reference/normalization timing")
endif()
if(NOT metadata MATCHES "worker_process_model" OR
   NOT metadata MATCHES "clean-exec-phase-v1")
    message(FATAL_ERROR "SA build metadata is missing the clean-exec worker model")
endif()
foreach(metric IN ITEMS sa_seconds isa_seconds lcp_seconds child_seconds
                        build_peak_rss_mb save_peak_rss_mb load_peak_rss_mb
                        serialized_bytes allocated_disk_bytes bits_per_base
                        construction_coordinate_width stored_coordinate_width
                        sa_resource_profile lcp_encoding
                        storage_compaction_seconds build_peak_rss_scope
                        save_peak_rss_scope load_peak_rss_scope)
    if(NOT raw MATCHES "${metric}")
        message(FATAL_ERROR "SA build raw results are missing ${metric}")
    endif()
endforeach()
foreach(metric IN ITEMS construction_coordinate_width stored_coordinate_width
                        sa_resource_profile lcp_encoding
                        storage_compaction_seconds_median build_peak_rss_scope
                        save_peak_rss_scope load_peak_rss_scope)
    if(NOT summary MATCHES "${metric}")
        message(FATAL_ERROR "SA build summary is missing ${metric}")
    endif()
endforeach()
foreach(scope IN ITEMS clean_exec_build_worker_until_index_ready
                       clean_exec_save_worker_including_source_load
                       clean_exec_load_worker_until_index_ready)
    if(NOT raw MATCHES "${scope}" OR NOT summary MATCHES "${scope}")
        message(FATAL_ERROR "SA build results are missing clean-exec scope ${scope}")
    endif()
endforeach()
foreach(method IN ITEMS div32 div64 caps32 caps64)
    foreach(rate IN ITEMS 1 2)
        if(NOT summary MATCHES "${method}[^\n]*\t${rate}\t")
            message(FATAL_ERROR "SA build summary is missing ${method} K=${rate}")
        endif()
    endforeach()
endforeach()

# Full is parsed on a tiny user reference so this test never allocates 256 MiB.
set(reference "${OUTPUT_ROOT}-full-reference.fa")
set(full_output "${OUTPUT_ROOT}-full-profile")
file(WRITE "${reference}" ">r0\nACGTTGCAACGATTCGGTACCTAGGCTAACGTACGTTGCAACGATTCGGTACCTAGGCTAACGT\n")
execute_process(
    COMMAND "${SUFKIT_SA_BUILD_BENCH}"
        --reference "${reference}"
        --methods div32
        --threads 1
        --sampling-rates 1
        --acceleration default
        --repetitions 1
        --output-dir "${full_output}"
    RESULT_VARIABLE full_status
    OUTPUT_VARIABLE full_stdout
    ERROR_VARIABLE full_stderr)
if(NOT full_status EQUAL 0)
    message(FATAL_ERROR "SA build user/full-layout check failed (${full_status}):\n${full_stdout}\n${full_stderr}")
endif()
