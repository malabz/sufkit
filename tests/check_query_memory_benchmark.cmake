cmake_policy(SET CMP0007 NEW)

if(NOT DEFINED SUFKIT_QUERY_MEMORY_BENCH OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR
        "SUFKIT_QUERY_MEMORY_BENCH and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
string(REPEAT "ACGT" 1024 balanced_sequence)
string(REPEAT "A" 2048 homopolymer_sequence)
string(REPEAT "TGCATGCA" 256 second_sequence)
set(reference "${OUTPUT_ROOT}/reference.fa")
file(WRITE "${reference}"
    ">balanced\n${balanced_sequence}\n"
    ">repeat-rich\n${homopolymer_sequence}${second_sequence}\n")

set(expected_checksum "")
foreach(method IN ITEMS sa32 fm-huff)
    set(output "${OUTPUT_ROOT}/${method}.tsv")
    execute_process(
        COMMAND "${SUFKIT_QUERY_MEMORY_BENCH}"
            --reference "${reference}"
            --method "${method}"
            --output "${output}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR
            "${method} phase-RSS verification failed (${status}):\n"
            "${stdout}\n${stderr}")
    endif()
    if(NOT EXISTS "${output}")
        message(FATAL_ERROR "${method} phase-RSS TSV is missing")
    endif()
    file(READ "${output}" contents)
    foreach(field IN ITEMS phase current_rss_mb current_rss_scope
                           index_ready_rss_mb after_rss_mb peak_rss_mb
                           peak_rss_scope result_checksum status)
        if(NOT contents MATCHES "${field}")
            message(FATAL_ERROR
                "${method} phase-RSS TSV is missing ${field}")
        endif()
    endforeach()
    string(FIND "${contents}" "${OUTPUT_ROOT}" leaked_output_path)
    string(FIND "${contents}" "${reference}" leaked_reference_path)
    if(NOT leaked_output_path EQUAL -1 OR
       NOT leaked_reference_path EQUAL -1)
        message(FATAL_ERROR "${method} phase-RSS TSV leaked a path")
    endif()

    file(STRINGS "${output}" lines)
    list(LENGTH lines line_count)
    if(NOT line_count EQUAL 4)
        message(FATAL_ERROR
            "${method} phase-RSS TSV has ${line_count} lines instead of 4")
    endif()
    list(REMOVE_AT lines 0)
    set(seen_build FALSE)
    set(seen_load FALSE)
    set(seen_query FALSE)
    foreach(line IN LISTS lines)
        string(REPLACE "\t" ";" fields "${line}")
        list(LENGTH fields field_count)
        if(NOT field_count EQUAL 17)
            message(FATAL_ERROR
                "${method} phase-RSS row has ${field_count} fields")
        endif()
        list(GET fields 0 recorded_method)
        list(GET fields 1 phase)
        list(GET fields 2 fingerprint)
        list(GET fields 5 current_rss)
        list(GET fields 6 current_scope)
        list(GET fields 7 index_ready_rss)
        list(GET fields 8 after_rss)
        list(GET fields 9 peak_rss)
        list(GET fields 10 peak_scope)
        list(GET fields 11 serialized_bytes)
        list(GET fields 12 query_count)
        list(GET fields 13 total_hits)
        list(GET fields 14 reported_hits)
        list(GET fields 15 checksum)
        list(GET fields 16 row_status)
        if(NOT recorded_method STREQUAL method OR
           NOT row_status STREQUAL "ok")
            message(FATAL_ERROR "${method} phase-RSS row is not successful")
        endif()
        string(LENGTH "${fingerprint}" fingerprint_length)
        if(NOT fingerprint_length EQUAL 16 OR current_rss STREQUAL "NA" OR
           NOT current_rss GREATER 0 OR peak_rss STREQUAL "NA" OR
           NOT peak_rss GREATER 0 OR NOT serialized_bytes GREATER 0)
            message(FATAL_ERROR "${method} phase-RSS row has invalid metrics")
        endif()
        if(peak_rss LESS current_rss)
            message(FATAL_ERROR
                "${method} ${phase} current RSS exceeds its worker peak")
        endif()
        if(phase STREQUAL "build")
            set(seen_build TRUE)
            if(NOT current_scope STREQUAL "after_index_save" OR
               NOT peak_scope STREQUAL "build_worker_lifetime" OR
               NOT index_ready_rss STREQUAL "NA" OR
               NOT after_rss STREQUAL "NA")
                message(FATAL_ERROR "${method} build RSS scope is invalid")
            endif()
        elseif(phase STREQUAL "load")
            set(seen_load TRUE)
            if(NOT current_scope STREQUAL "index_ready" OR
               NOT peak_scope STREQUAL "load_worker_lifetime" OR
               index_ready_rss STREQUAL "NA" OR
               NOT current_rss STREQUAL index_ready_rss OR
               NOT after_rss STREQUAL "NA")
                message(FATAL_ERROR "${method} load RSS scope is invalid")
            endif()
        elseif(phase STREQUAL "query")
            set(seen_query TRUE)
            if(NOT current_scope STREQUAL "after_queries" OR
               NOT peak_scope STREQUAL
                   "query_worker_lifetime_including_load" OR
               index_ready_rss STREQUAL "NA" OR after_rss STREQUAL "NA" OR
               NOT current_rss STREQUAL after_rss OR
               NOT query_count EQUAL 9 OR NOT total_hits GREATER 0 OR
               NOT reported_hits GREATER 0)
                message(FATAL_ERROR "${method} query RSS row is invalid")
            endif()
            string(LENGTH "${checksum}" checksum_length)
            if(NOT checksum_length EQUAL 16)
                message(FATAL_ERROR "${method} query checksum is invalid")
            endif()
            if(expected_checksum STREQUAL "")
                set(expected_checksum "${checksum}")
            elseif(NOT checksum STREQUAL expected_checksum)
                message(FATAL_ERROR
                    "SA and FM phase-RSS query checksums differ")
            endif()
        else()
            message(FATAL_ERROR "${method} emitted an unknown RSS phase")
        endif()
    endforeach()
    if(NOT seen_build OR NOT seen_load OR NOT seen_query)
        message(FATAL_ERROR "${method} did not emit all RSS phases")
    endif()
endforeach()

set(race_output "${OUTPUT_ROOT}/concurrent.tsv")
execute_process(
    COMMAND bash
        "${CMAKE_CURRENT_LIST_DIR}/check_query_memory_race.sh"
        "${SUFKIT_QUERY_MEMORY_BENCH}"
        "${reference}"
        "${race_output}"
        "${OUTPUT_ROOT}"
    RESULT_VARIABLE race_status
    OUTPUT_VARIABLE race_stdout
    ERROR_VARIABLE race_stderr)
if(NOT race_status EQUAL 0)
    message(FATAL_ERROR
        "phase-RSS concurrent publication verification failed "
        "(${race_status}):\n${race_stdout}\n${race_stderr}")
endif()
if(NOT EXISTS "${race_output}")
    message(FATAL_ERROR "phase-RSS concurrent writer produced no output")
endif()
file(STRINGS "${race_output}" race_lines)
list(LENGTH race_lines race_line_count)
if(NOT race_line_count EQUAL 4)
    message(FATAL_ERROR
        "phase-RSS concurrent output has ${race_line_count} lines")
endif()

file(GLOB temporary_artifacts "${OUTPUT_ROOT}/.*.memory-bench.*")
if(temporary_artifacts)
    message(FATAL_ERROR
        "phase-RSS benchmark left temporary artifacts: ${temporary_artifacts}")
endif()
