# SPDX-License-Identifier: MIT

if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()
if(NOT DEFINED SUFKIT_EXPECT_LCP_ENCODING)
    set(SUFKIT_EXPECT_LCP_ENCODING "byte-coded")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")
set(reference "${OUTPUT_ROOT}/reference.fa")
set(queries "${OUTPUT_ROOT}/queries.fa")
set(index "${OUTPUT_ROOT}/reference.sufidx")
set(sampled "${OUTPUT_ROOT}/sampled.sufidx")
set(fm "${OUTPUT_ROOT}/reference-fm.sufidx")
set(low_memory "${OUTPUT_ROOT}/low-memory.sufidx")
file(WRITE "${reference}" ">r0\nTTGATTACAGGACGTACGT\n>r1\nCCCCAAAATTTT\n")
file(WRITE "${queries}"
    ">q0\nAAGATTACACCGATTACA\n>q_smem\nACGTACGT\n"
    ">q_mum\nGATTACA\n>q_mum2\nGATTACA\n")

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" build --type sa --input "${reference}"
            --output "${index}" --sa-acceleration full
    RESULT_VARIABLE build_status ERROR_VARIABLE build_error)
if(NOT build_status EQUAL 0)
    message(FATAL_ERROR "cannot build MEM/MAM CLI fixture: ${build_error}")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" build --type sa --input "${reference}"
            --output "${low_memory}" --sa-profile low-memory
    RESULT_VARIABLE low_build_status ERROR_VARIABLE low_build_error)
if(NOT low_build_status EQUAL 0)
    message(FATAL_ERROR "cannot build low-memory CLI fixture: ${low_build_error}")
endif()
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" inspect --index "${low_memory}"
    RESULT_VARIABLE inspect_status OUTPUT_VARIABLE inspect_output
    ERROR_VARIABLE inspect_error)
if(NOT inspect_status EQUAL 0 OR
   NOT inspect_output MATCHES "construction_backend[\t]divsufsort32" OR
   NOT inspect_output MATCHES "sa_resource_profile[\t]low-memory" OR
   NOT inspect_output MATCHES
       "lcp_encoding[\t]${SUFKIT_EXPECT_LCP_ENCODING}")
    message(FATAL_ERROR
        "low-memory inspect metadata is incomplete: ${inspect_error}")
endif()

foreach(conflict IN ITEMS acceleration learned sampling)
    if(conflict STREQUAL "acceleration")
        set(conflicting_arguments --sa-acceleration full)
    elseif(conflict STREQUAL "learned")
        set(conflicting_arguments --learned-index)
    else()
        set(conflicting_arguments --sa-sampling-rate 2)
    endif()
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" build --type sa --input "${reference}"
                --output "${OUTPUT_ROOT}/invalid-${conflict}.sufidx"
                --sa-profile low-memory ${conflicting_arguments}
        RESULT_VARIABLE conflict_status)
    if(conflict_status EQUAL 0)
        message(FATAL_ERROR
            "low-memory accepted conflicting ${conflict} options")
    endif()
endforeach()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" smem --index "${index}"
            --query "${queries}" --min-length 4 --min-occurrences 2
            --algorithm full
    RESULT_VARIABLE smem_status OUTPUT_VARIABLE smem_output
    ERROR_VARIABLE smem_error)
if(NOT smem_status EQUAL 0)
    message(FATAL_ERROR "smem CLI failed: ${smem_error}")
endif()
set(smem_header
    "query_id\tsequence_id\tsequence_name\treference_start\tquery_start\tlength\treference_occurrences\tstrand")
if(NOT smem_output MATCHES
   "${smem_header}" OR
   NOT smem_output MATCHES "q_smem\t0\tr0")
    message(FATAL_ERROR "smem CLI output schema or matches are incorrect")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" mum --index "${index}"
            --query "${queries}" --min-length 7 --algorithm full
    RESULT_VARIABLE mum_status OUTPUT_VARIABLE mum_output
    ERROR_VARIABLE mum_error)
if(NOT mum_status EQUAL 0)
    message(FATAL_ERROR "mum CLI failed: ${mum_error}")
endif()
set(maximal_header
    "query_id\tsequence_id\tsequence_name\treference_start\tquery_start\tlength\tstrand")
if(NOT mum_output MATCHES
   "${maximal_header}" OR
   NOT mum_output MATCHES "q_mum\t0\tr0" OR
   NOT mum_output MATCHES "q_mum2\t0\tr0")
    message(FATAL_ERROR "mum CLI output schema or matches are incorrect")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" smem --index "${index}"
            --query "${queries}" --min-length 4 --min-occurrences 0
    RESULT_VARIABLE invalid_occurrences
    OUTPUT_VARIABLE invalid_occurrences_output)
if(invalid_occurrences EQUAL 0 OR NOT invalid_occurrences_output STREQUAL "")
    message(FATAL_ERROR
        "smem accepted --min-occurrences 0 or polluted stdout")
endif()

foreach(invalid_case IN ITEMS min-length skip)
    if(invalid_case STREQUAL "min-length")
        set(invalid_command mum)
        set(invalid_arguments --min-length 0)
    else()
        set(invalid_command mem)
        set(invalid_arguments --min-length 4 --skip 0)
    endif()
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" ${invalid_command} --index "${index}"
                --query "${queries}" ${invalid_arguments}
        RESULT_VARIABLE invalid_status
        OUTPUT_VARIABLE invalid_output)
    if(invalid_status EQUAL 0 OR NOT invalid_output STREQUAL "")
        message(FATAL_ERROR
            "${invalid_command} accepted invalid ${invalid_case} or polluted stdout")
    endif()
endforeach()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" mum --index "${index}"
            --query "${queries}" --min-length 4 --min-occurrences 2
    RESULT_VARIABLE mum_occurrences)
if(mum_occurrences EQUAL 0)
    message(FATAL_ERROR "mum accepted the SMEM-only --min-occurrences option")
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
foreach(command IN ITEMS smem mum)
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" ${command} --index "${sampled}"
                --query "${queries}" --min-length 4
        RESULT_VARIABLE sampled_status)
    if(sampled_status EQUAL 0)
        message(FATAL_ERROR "${command} accepted a sampled suffix array")
    endif()
endforeach()

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
foreach(command IN ITEMS smem mum)
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" ${command} --index "${fm}"
                --query "${queries}" --min-length 4
        RESULT_VARIABLE fm_status)
    if(fm_status EQUAL 0)
        message(FATAL_ERROR "${command} accepted an FM index")
    endif()
endforeach()
