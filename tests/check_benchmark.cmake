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
            --methods naive,sa32,sa64,fm
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
foreach(method IN ITEMS naive sa32 sa64 fm)
    if(NOT build_results MATCHES "\t${method}\t")
        message(FATAL_ERROR "build_results.tsv is missing ${method}")
    endif()
endforeach()
if(NOT query_results MATCHES "query_group\tpattern_length\tstrand\toperation\tmax_hits")
    message(FATAL_ERROR "query_results.tsv has an unexpected schema")
endif()
if(NOT raw_results MATCHES "phase\tquery_group\tpattern_length\tstrand")
    message(FATAL_ERROR "raw_repetitions.tsv has an unexpected schema")
endif()
if(NOT raw_results MATCHES "query_definition" OR NOT raw_results MATCHES "query_id\tquery_source")
    message(FATAL_ERROR "raw_repetitions.tsv is missing ordered query definitions")
endif()

set(fm_dir "${OUTPUT_ROOT}/fm-backends")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke
        --scenarios balanced
        --methods fm-huff,fm-balanced,fm-epr
        --fm-query-modes scalar,batch
        --fm-batch-widths 1,4
        --pattern-lengths 20
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
        --methods fm
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

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench --profile smoke --reference missing.fa --output-dir invalid
    RESULT_VARIABLE conflict_status
    OUTPUT_QUIET ERROR_QUIET)
if(conflict_status EQUAL 0)
    message(FATAL_ERROR "conflicting --profile and --reference options were accepted")
endif()
