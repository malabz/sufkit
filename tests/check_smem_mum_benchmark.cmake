if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

cmake_policy(SET CMP0007 NEW)

function(assert_rectangular_tsv path label)
    file(STRINGS "${path}" lines)
    if(NOT lines)
        message(FATAL_ERROR "${label} is empty")
    endif()
    list(GET lines 0 header)
    string(REGEX MATCHALL "\t" header_tabs "${header}")
    list(LENGTH header_tabs expected_tabs)
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        string(REGEX MATCHALL "\t" line_tabs "${line}")
        list(LENGTH line_tabs actual_tabs)
        if(NOT actual_tabs EQUAL expected_tabs)
            message(FATAL_ERROR
                "${label} line ${line_number} has ${actual_tabs} tabs; "
                "expected ${expected_tabs}")
        endif()
    endforeach()
endfunction()

function(run_maximal_smoke workload methods)
    set(result_root "${OUTPUT_ROOT}/${workload}")
    file(REMOVE_RECURSE "${result_root}")
    set(extra_arguments)
    if(workload STREQUAL "smem")
        list(APPEND extra_arguments --min-occurrences 1,2)
    endif()
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" bench
            --workload "${workload}"
            --profile smoke
            --scenarios balanced
            --methods "${methods}"
            --min-lengths 20
            ${extra_arguments}
            --build-repetitions 1
            --query-repetitions 1
            --warmups 0
            --output-dir "${result_root}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR
            "${workload} benchmark smoke failed (${status}):\n"
            "${stdout}\n${stderr}")
    endif()

    foreach(name IN ITEMS run_metadata.tsv correctness_summary.tsv
                          build_results.tsv query_results.tsv
                          raw_repetitions.tsv)
        set(path "${result_root}/${name}")
        if(NOT EXISTS "${path}")
            message(FATAL_ERROR "${workload} output is missing ${name}")
        endif()
        assert_rectangular_tsv("${path}" "${workload} ${name}")
    endforeach()

    file(READ "${result_root}/query_results.tsv" query_results)
    file(READ "${result_root}/correctness_summary.tsv" correctness)
    if(workload STREQUAL "smem")
        foreach(token IN ITEMS
                "min_occurrences\ttotal_smems\tsmems_per_second"
                "smem-baseline\tbaseline\tnone\tstreaming\t20"
                "smem-auto-fast\tauto\tsuffix-link\tstreaming\t20"
                "smem-auto-low-memory\tauto\tlcp\tstreaming\t20")
            string(FIND "${query_results}" "${token}" position)
            if(position EQUAL -1)
                message(FATAL_ERROR
                    "SMEM query_results.tsv is missing ${token}")
            endif()
        endforeach()
        if(NOT correctness MATCHES "naive-generalized-smem-forward" OR
           NOT correctness MATCHES "\t1\t[1-9][0-9]*\n" OR
           NOT correctness MATCHES "\t2\t[0-9]+\n")
            message(FATAL_ERROR
                "SMEM correctness summary lacks independent seed totals")
        endif()
    else()
        foreach(token IN ITEMS
                "mum-baseline\tbaseline\tnone\tstreaming\t20"
                "mum-auto-fast\tauto\tsuffix-link\tstreaming\t20"
                "mum-auto-low-memory\tauto\tlcp\tstreaming\t20")
            string(FIND "${query_results}" "${token}" position)
            if(position EQUAL -1)
                message(FATAL_ERROR
                    "MUM query_results.tsv is missing ${token}")
            endif()
        endforeach()
        if(NOT correctness MATCHES "naive-mum-forward")
            message(FATAL_ERROR "MUM correctness summary lacks its oracle")
        endif()
    endif()
endfunction()

run_maximal_smoke(
    smem "smem-baseline,smem-auto-fast,smem-auto-low-memory")
run_maximal_smoke(
    mum "mum-baseline,mum-auto-fast,mum-auto-low-memory")

set(fake_minibwa "${OUTPUT_ROOT}/fake-minibwa.sh")
file(WRITE "${fake_minibwa}" [=[#!/bin/sh
case "$1" in
    version)
        printf '%s\n' 'minibwa-test'
        ;;
    index)
        printf '%s\n' 'fake-index' >"$4.fake"
        ;;
    fastmap)
        min_length=
        while [ "$#" -gt 0 ]; do
            case "$1" in
                -l)
                    min_length=$2
                    shift 2
                    ;;
                *)
                    shift
                    ;;
            esac
        done
        printf 'SQ\tq0\t256\n'
        case "$min_length" in
            20)
                printf 'EM\t0\t20\t1\tref0:+1\n'
                ;;
            21)
                printf 'EM\t0\t21\t1000001\t*\n'
                ;;
            22)
                printf 'EM\t0\t22\t2\tref0:+1\t.\n'
                ;;
            23)
                printf 'EM\t0\t23\t2\tref0:+1\n'
                ;;
            *)
                exit 3
                ;;
        esac
        printf '%s\n' '//'
        ;;
    *)
        exit 2
        ;;
esac
]=])
execute_process(COMMAND chmod +x "${fake_minibwa}"
                RESULT_VARIABLE chmod_minibwa_status)
if(NOT chmod_minibwa_status EQUAL 0)
    message(FATAL_ERROR "cannot make the fake MiniBWA executable")
endif()

set(minibwa_output "${OUTPUT_ROOT}/minibwa-external-scope")
file(REMOVE_RECURSE "${minibwa_output}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload smem
        --profile smoke
        --scenarios balanced
        --methods smem-auto-fast,minibwa
        --min-lengths 20,21,22,23
        --min-occurrences 1
        --minibwa "${fake_minibwa}"
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${minibwa_output}"
    RESULT_VARIABLE minibwa_status
    OUTPUT_VARIABLE minibwa_stdout
    ERROR_VARIABLE minibwa_stderr)
if(NOT minibwa_status EQUAL 0)
    message(FATAL_ERROR
        "MiniBWA external-scope smoke failed (${minibwa_status}):\n"
        "${minibwa_stdout}\n${minibwa_stderr}")
endif()

file(STRINGS "${minibwa_output}/raw_repetitions.tsv" minibwa_raw_lines)
set(found_measured_external FALSE)
set(found_high_frequency_external FALSE)
set(found_dot_incomplete_external FALSE)
set(found_short_incomplete_external FALSE)
set(found_unmeasured_capability FALSE)
foreach(line IN LISTS minibwa_raw_lines)
    string(REPLACE "\t" ";" fields "${line}")
    list(LENGTH fields field_count)
    if(field_count LESS 39)
        continue()
    endif()
    list(GET fields 1 method)
    list(GET fields 2 operation)
    list(GET fields 3 min_length)
    list(GET fields 5 seconds)
    list(GET fields 6 user_cpu)
    list(GET fields 7 system_cpu)
    list(GET fields 8 peak_rss)
    list(GET fields 36 row_status)
    if(method STREQUAL "minibwa" AND
       operation STREQUAL "external-load+query")
        if(seconds STREQUAL "NA" OR user_cpu STREQUAL "NA" OR
           system_cpu STREQUAL "NA" OR peak_rss STREQUAL "NA")
            message(FATAL_ERROR
                "measured MiniBWA raw row lost status/timing/RSS: ${line}")
        endif()
        if(min_length STREQUAL "20" AND
           row_status STREQUAL "external_fmd_scope")
            set(found_measured_external TRUE)
        elseif(min_length STREQUAL "21" AND
               row_status STREQUAL "external_high_frequency")
            set(found_high_frequency_external TRUE)
        elseif(min_length STREQUAL "22" AND
               row_status STREQUAL "external_incomplete_coordinates")
            set(found_dot_incomplete_external TRUE)
        elseif(min_length STREQUAL "23" AND
               row_status STREQUAL "external_incomplete_coordinates")
            set(found_short_incomplete_external TRUE)
        else()
            message(FATAL_ERROR
                "MiniBWA row has an unexpected completeness status: ${line}")
        endif()
    elseif(method STREQUAL "fm-huff" AND operation STREQUAL "streaming")
        if(NOT row_status STREQUAL "not_supported" OR
           NOT seconds STREQUAL "NA" OR NOT peak_rss STREQUAL "NA")
            message(FATAL_ERROR
                "not-supported capability row unexpectedly has measurements: ${line}")
        endif()
        set(found_unmeasured_capability TRUE)
    endif()
endforeach()
if(NOT found_measured_external OR NOT found_high_frequency_external OR
   NOT found_dot_incomplete_external OR
   NOT found_short_incomplete_external OR
   NOT found_unmeasured_capability)
    message(FATAL_ERROR
        "MiniBWA raw output lacks complete, high-frequency, incomplete-coordinate, "
        "or unmeasured capability rows")
endif()

set(fake_mummer "${OUTPUT_ROOT}/fake-mummer-launcher.sh")
file(WRITE "${fake_mummer}" [=[#!/bin/sh
if [ "$1" = "--version" ]; then
    printf '%s\n' '4.0.1'
    exit 0
fi
exit 2
]=])
execute_process(COMMAND chmod +x "${fake_mummer}"
                RESULT_VARIABLE chmod_mummer_status)
if(NOT chmod_mummer_status EQUAL 0)
    message(FATAL_ERROR "cannot make the fake MUMmer4 launcher executable")
endif()
find_program(TEST_RUNTIME_ELF NAMES true REQUIRED)
file(SHA256 "${fake_mummer}" expected_launcher_sha256)
file(SHA256 "${TEST_RUNTIME_ELF}" expected_runtime_sha256)

set(mummer_metadata_output "${OUTPUT_ROOT}/mummer-provenance")
file(REMOVE_RECURSE "${mummer_metadata_output}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload mum
        --profile smoke
        --scenarios balanced
        --methods mum-auto-fast
        --min-lengths 20
        --mummer4 "${fake_mummer}"
        --mummer4-runtime "${TEST_RUNTIME_ELF}"
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${mummer_metadata_output}"
    RESULT_VARIABLE mummer_metadata_status
    OUTPUT_VARIABLE mummer_metadata_stdout
    ERROR_VARIABLE mummer_metadata_stderr)
if(NOT mummer_metadata_status EQUAL 0)
    message(FATAL_ERROR
        "MUMmer4 provenance smoke failed (${mummer_metadata_status}):\n"
        "${mummer_metadata_stdout}\n${mummer_metadata_stderr}")
endif()
file(READ "${mummer_metadata_output}/run_metadata.tsv" mummer_metadata)
foreach(token IN ITEMS
        "mummer_launcher_sha256\tmummer_runtime_sha256"
        "${expected_launcher_sha256}\t${expected_runtime_sha256}")
    string(FIND "${mummer_metadata}" "${token}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR
            "MUMmer4 run metadata lacks launcher/runtime provenance ${token}")
    endif()
endforeach()

set(missing_runtime_output "${OUTPUT_ROOT}/mummer-missing-runtime")
file(REMOVE_RECURSE "${missing_runtime_output}")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --workload mum
        --profile smoke
        --scenarios balanced
        --methods mum-auto-fast
        --min-lengths 20
        --mummer4 "${fake_mummer}"
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${missing_runtime_output}"
    RESULT_VARIABLE missing_runtime_status
    OUTPUT_VARIABLE missing_runtime_stdout
    ERROR_VARIABLE missing_runtime_stderr)
if(missing_runtime_status EQUAL 0 OR
   NOT missing_runtime_stderr MATCHES "--mummer4-runtime")
    message(FATAL_ERROR
        "script MUMmer4 launcher did not require explicit runtime ELF: "
        "${missing_runtime_stdout}\n${missing_runtime_stderr}")
endif()
