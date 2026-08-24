# SPDX-License-Identifier: MIT

if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
set(reference "${OUTPUT_ROOT}/reference.fa")
set(queries "${OUTPUT_ROOT}/queries.fa")
set(index "${OUTPUT_ROOT}/reference.sufidx")
set(sampled "${OUTPUT_ROOT}/sampled.sufidx")
set(fm "${OUTPUT_ROOT}/reference-fm.sufidx")
file(WRITE "${reference}" ">r0\nTTGATTACAGGACGTACGT\n>r1\nCCCCAAAATTTT\n")
file(WRITE "${queries}" ">q0\nAAGATTACACCGATTACA\n")

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" build --type sa --input "${reference}"
            --output "${index}" --sa-acceleration full
    RESULT_VARIABLE build_status ERROR_VARIABLE build_error)
if(NOT build_status EQUAL 0)
    message(FATAL_ERROR "cannot build MEM/MAM CLI fixture: ${build_error}")
endif()

foreach(command IN ITEMS mem mam)
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" ${command} --index "${index}"
                --query "${queries}" --min-length 7 --algorithm full
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${command} CLI failed: ${error}")
    endif()
    if(NOT output MATCHES
       "query_id\tsequence_id\tsequence_name\treference_start\tquery_start\tlength\tstrand")
        message(FATAL_ERROR "${command} CLI output schema is incorrect")
    endif()
    if(NOT output MATCHES "q0\t0\tr0")
        message(FATAL_ERROR "${command} CLI did not emit the expected match")
    endif()
endforeach()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" build --type sa --input "${reference}"
            --output "${sampled}" --sa-sampling-rate 2
    RESULT_VARIABLE sampled_build ERROR_VARIABLE sampled_build_error)
if(NOT sampled_build EQUAL 0)
    message(FATAL_ERROR "cannot build sampled CLI fixture: ${sampled_build_error}")
endif()
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" mam --index "${sampled}"
            --query "${queries}" --min-length 7
    RESULT_VARIABLE sampled_mam)
if(sampled_mam EQUAL 0)
    message(FATAL_ERROR "mam accepted a sampled suffix array")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" build --type fm --input "${reference}"
            --output "${fm}"
    RESULT_VARIABLE fm_build ERROR_VARIABLE fm_build_error)
if(NOT fm_build EQUAL 0)
    message(FATAL_ERROR "cannot build FM CLI fixture: ${fm_build_error}")
endif()
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" mem --index "${fm}"
            --query "${queries}" --min-length 7
    RESULT_VARIABLE fm_mem)
if(fm_mem EQUAL 0)
    message(FATAL_ERROR "mem accepted an FM index")
endif()
