cmake_policy(SET CMP0007 NEW)

if(NOT DEFINED SUFKIT_QUERY_MEMORY_BENCH OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR
        "SUFKIT_QUERY_MEMORY_BENCH and OUTPUT_ROOT are required")
endif()
if(NOT DEFINED SUFKIT_EXPECT_FAST_LCP_ENCODING)
    set(SUFKIT_EXPECT_FAST_LCP_ENCODING "raw")
endif()
if(NOT DEFINED SUFKIT_EXPECT_LOW_LCP_ENCODING)
    set(SUFKIT_EXPECT_LOW_LCP_ENCODING "byte-coded")
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
foreach(method IN ITEMS
        sa32
        sa32-low-memory
        sa64-store32-fast
        sa64-store40-low-memory
        sa64-store48-low-memory
        fm-huff)
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
                           index_ready_rss_mb after_rss_mb current_pss_mb
                           index_ready_pss_mb after_pss_mb peak_rss_mb
                           peak_rss_scope construction_coordinate_width
                           stored_coordinate_width sa_profile lcp_encoding
                           resident_core_bytes result_checksum status)
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
        if(NOT field_count EQUAL 25)
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
        list(GET fields 9 current_pss)
        list(GET fields 10 index_ready_pss)
        list(GET fields 11 after_pss)
        list(GET fields 12 peak_rss)
        list(GET fields 13 peak_scope)
        list(GET fields 14 serialized_bytes)
        list(GET fields 15 construction_width)
        list(GET fields 16 stored_width)
        list(GET fields 17 profile)
        list(GET fields 18 lcp_encoding)
        list(GET fields 19 resident_core_bytes)
        list(GET fields 20 query_count)
        list(GET fields 21 total_hits)
        list(GET fields 22 reported_hits)
        list(GET fields 23 checksum)
        list(GET fields 24 row_status)
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
               NOT after_rss STREQUAL "NA" OR
               current_pss STREQUAL "NA" OR
               NOT current_pss GREATER 0 OR
               NOT index_ready_pss STREQUAL "NA" OR
               NOT after_pss STREQUAL "NA")
                message(FATAL_ERROR "${method} build RSS scope is invalid")
            endif()
        elseif(phase STREQUAL "load")
            set(seen_load TRUE)
            if(NOT current_scope STREQUAL "index_ready" OR
               NOT peak_scope STREQUAL "load_worker_lifetime" OR
               index_ready_rss STREQUAL "NA" OR
               NOT current_rss STREQUAL index_ready_rss OR
               NOT after_rss STREQUAL "NA" OR
               index_ready_pss STREQUAL "NA" OR
               NOT current_pss STREQUAL index_ready_pss OR
               NOT after_pss STREQUAL "NA")
                message(FATAL_ERROR "${method} load RSS scope is invalid")
            endif()
        elseif(phase STREQUAL "query")
            set(seen_query TRUE)
            if(NOT current_scope STREQUAL "after_queries" OR
               NOT peak_scope STREQUAL
                   "query_worker_lifetime_including_load" OR
               index_ready_rss STREQUAL "NA" OR after_rss STREQUAL "NA" OR
               index_ready_pss STREQUAL "NA" OR after_pss STREQUAL "NA" OR
               NOT current_pss STREQUAL after_pss OR
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

        if(method STREQUAL "fm-huff")
            if(NOT construction_width STREQUAL "NA" OR
               NOT stored_width STREQUAL "NA" OR
               NOT profile STREQUAL "NA" OR
               NOT lcp_encoding STREQUAL "NA")
                message(FATAL_ERROR
                    "FM phase-RSS row contains standalone-SA metadata")
            endif()
        else()
            if(method MATCHES "^sa64")
                set(expected_construction_width 64)
            else()
                set(expected_construction_width 32)
            endif()
            if(method MATCHES "store40")
                set(expected_stored_width 40)
            elseif(method MATCHES "store48")
                set(expected_stored_width 48)
            else()
                set(expected_stored_width 32)
            endif()
            if(method MATCHES "low-memory")
                set(expected_profile "low-memory")
                set(expected_lcp_encoding
                    "${SUFKIT_EXPECT_LOW_LCP_ENCODING}")
            else()
                set(expected_profile "fast")
                set(expected_lcp_encoding
                    "${SUFKIT_EXPECT_FAST_LCP_ENCODING}")
            endif()
            if(NOT construction_width EQUAL expected_construction_width OR
               NOT stored_width EQUAL expected_stored_width OR
               NOT profile STREQUAL expected_profile OR
               NOT lcp_encoding STREQUAL
                   "${expected_lcp_encoding}" OR
               NOT resident_core_bytes GREATER 0)
                message(FATAL_ERROR
                    "${method} phase-RSS row has incorrect storage metadata")
            endif()
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
