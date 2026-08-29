if(NOT DEFINED SUFKIT_EXECUTABLE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "SUFKIT_EXECUTABLE and OUTPUT_ROOT are required")
endif()
if(NOT DEFINED SUFKIT_EXPECT_FAST_LCP_ENCODING)
    set(SUFKIT_EXPECT_FAST_LCP_ENCODING "raw")
endif()
if(NOT DEFINED SUFKIT_EXPECT_LOW_LCP_ENCODING)
    set(SUFKIT_EXPECT_LOW_LCP_ENCODING "byte-coded")
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
                "${label} line ${line_number} has ${actual_tabs} tabs; expected ${expected_tabs}")
        endif()
    endforeach()
endfunction()

function(assert_exact_storage_mode build_path method construction_width
         stored_width profile expect_isa)
    file(STRINGS "${build_path}" build_lines)
    set(found FALSE)
    foreach(line IN LISTS build_lines)
        string(REPLACE "\t" ";" columns "${line}")
        list(LENGTH columns column_count)
        if(column_count EQUAL 41)
            list(GET columns 3 actual_method)
            if(actual_method STREQUAL "${method}")
                if(found)
                    message(FATAL_ERROR
                        "build_results.tsv contains duplicate ${method} rows")
                endif()
                set(found TRUE)
                list(GET columns 31 status)
                list(GET columns 32 actual_construction_width)
                list(GET columns 33 actual_stored_width)
                list(GET columns 34 actual_profile)
                list(GET columns 35 lcp_encoding)
                list(GET columns 37 isa_bytes)
                if(profile STREQUAL "fast")
                    set(expected_lcp_encoding
                        "${SUFKIT_EXPECT_FAST_LCP_ENCODING}")
                else()
                    set(expected_lcp_encoding
                        "${SUFKIT_EXPECT_LOW_LCP_ENCODING}")
                endif()
                if(NOT status STREQUAL "ok" OR
                   NOT actual_construction_width STREQUAL
                       "${construction_width}" OR
                   NOT actual_stored_width STREQUAL "${stored_width}" OR
                   NOT actual_profile STREQUAL "${profile}" OR
                   NOT lcp_encoding STREQUAL
                       "${expected_lcp_encoding}")
                    message(FATAL_ERROR
                        "unexpected ${method} storage metadata: ${line}")
                endif()
                if(expect_isa AND isa_bytes STREQUAL "0")
                    message(FATAL_ERROR
                        "${method} fast profile did not retain the ISA")
                elseif(NOT expect_isa AND NOT isa_bytes STREQUAL "0")
                    message(FATAL_ERROR
                        "${method} low-memory profile retained the ISA")
                endif()
            endif()
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "build_results.tsv is missing ${method}")
    endif()
endfunction()

function(assert_exact_clean_exec_scopes raw_path method)
    file(STRINGS "${raw_path}" raw_lines)
    set(saw_build FALSE)
    set(saw_save FALSE)
    set(saw_load FALSE)
    set(saw_count FALSE)
    set(saw_locate FALSE)
    foreach(line IN LISTS raw_lines)
        string(REPLACE "\t" ";" columns "${line}")
        list(LENGTH columns column_count)
        if(column_count EQUAL 66)
            list(GET columns 3 actual_method)
            if(actual_method STREQUAL "${method}")
                list(GET columns 4 phase)
                list(GET columns 8 operation)
                list(GET columns 17 scope)
                if(phase STREQUAL "build")
                    if(NOT scope STREQUAL
                           "build_worker_clean_exec_reference_plus_build")
                        message(FATAL_ERROR
                            "${method} has an invalid build RSS scope: ${scope}")
                    endif()
                    set(saw_build TRUE)
                elseif(phase STREQUAL "save")
                    if(NOT scope STREQUAL
                           "save_worker_clean_exec_load_plus_save")
                        message(FATAL_ERROR
                            "${method} has an invalid save RSS scope: ${scope}")
                    endif()
                    set(saw_save TRUE)
                elseif(phase STREQUAL "load")
                    if(NOT scope STREQUAL
                           "load_worker_clean_exec_load_plus_canary")
                        message(FATAL_ERROR
                            "${method} has an invalid load RSS scope: ${scope}")
                    endif()
                    set(saw_load TRUE)
                elseif(phase STREQUAL "query" AND operation STREQUAL "count")
                    if(NOT scope STREQUAL
                           "count_worker_clean_exec_required_dataset_plus_load_plus_query")
                        message(FATAL_ERROR
                            "${method} has an invalid count RSS scope: ${scope}")
                    endif()
                    set(saw_count TRUE)
                elseif(phase STREQUAL "query" AND operation STREQUAL "locate")
                    if(NOT scope STREQUAL
                           "locate_worker_1_clean_exec_required_dataset_plus_load_plus_query")
                        message(FATAL_ERROR
                            "${method} has an invalid locate RSS scope: ${scope}")
                    endif()
                    set(saw_locate TRUE)
                endif()
            endif()
        endif()
    endforeach()
    if(NOT saw_build OR NOT saw_save OR NOT saw_load OR NOT saw_count OR
       NOT saw_locate)
        message(FATAL_ERROR
            "${method} is missing one or more clean-exec benchmark phases")
    endif()
endfunction()

set(first "${OUTPUT_ROOT}/first")
set(second "${OUTPUT_ROOT}/second")
file(REMOVE_RECURSE "${OUTPUT_ROOT}")

foreach(output_dir IN ITEMS "${first}" "${second}")
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" bench
            --profile smoke
            --scenarios balanced
            --methods naive,sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,sa64-binary,sa64-lcp-binary,sa32-sampled-k2,sa32-sampled-k4,sa32-sampled-k8,sa64-sampled-k2,sa64-sampled-k4,sa64-sampled-k8,fm
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

set(storage_modes_dir "${OUTPUT_ROOT}/storage-modes")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke
        --scenarios balanced
        --methods sa32-fast,sa32-low-memory,sa64-fast,sa64-low-memory,sa64-store32-fast,sa64-store40-low-memory,sa64-store48-low-memory,sa64-store64-fast
        --pattern-lengths 20
        --locate-limits 1
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --output-dir "${storage_modes_dir}"
    RESULT_VARIABLE storage_modes_status
    OUTPUT_VARIABLE storage_modes_stdout
    ERROR_VARIABLE storage_modes_stderr)
if(NOT storage_modes_status EQUAL 0)
    message(FATAL_ERROR
        "storage-mode smoke benchmark failed (${storage_modes_status}):\n"
        "${storage_modes_stdout}\n${storage_modes_stderr}")
endif()

foreach(name IN ITEMS run_metadata.tsv build_results.tsv query_results.tsv
                      raw_repetitions.tsv)
    assert_rectangular_tsv("${storage_modes_dir}/${name}"
                           "storage-mode ${name}")
endforeach()

set(storage_builds "${storage_modes_dir}/build_results.tsv")
assert_exact_storage_mode("${storage_builds}" sa32-fast 32 32 fast TRUE)
assert_exact_storage_mode("${storage_builds}" sa32-low-memory 32 32
                          low-memory FALSE)
assert_exact_storage_mode("${storage_builds}" sa64-fast 64 32 fast TRUE)
assert_exact_storage_mode("${storage_builds}" sa64-low-memory 64 32
                          low-memory FALSE)
assert_exact_storage_mode("${storage_builds}" sa64-store32-fast 64 32 fast
                          TRUE)
assert_exact_storage_mode("${storage_builds}" sa64-store40-low-memory 64 40
                          low-memory FALSE)
assert_exact_storage_mode("${storage_builds}" sa64-store48-low-memory 64 48
                          low-memory FALSE)
assert_exact_storage_mode("${storage_builds}" sa64-store64-fast 64 64 fast
                          TRUE)

foreach(method IN ITEMS sa32-fast sa32-low-memory sa64-fast sa64-low-memory
                        sa64-store32-fast sa64-store40-low-memory
                        sa64-store48-low-memory sa64-store64-fast)
    assert_exact_clean_exec_scopes(
        "${storage_modes_dir}/raw_repetitions.tsv" "${method}")
endforeach()

file(READ "${storage_modes_dir}/run_metadata.tsv" storage_metadata)
if(NOT storage_metadata MATCHES "worker_process_model" OR
   NOT storage_metadata MATCHES "clean-exec-phase-v1")
    message(FATAL_ERROR
        "storage-mode metadata does not identify clean-exec workers")
endif()

set(export_dir "${OUTPUT_ROOT}/export-smoke")
set(export_reference "${OUTPUT_ROOT}/export-reference.fa")
set(export_queries "${OUTPUT_ROOT}/export-queries.fa")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke
        --scenarios mixed
        --methods sa32-binary
        --pattern-lengths 20
        --locate-limits 1
        --build-repetitions 1
        --query-repetitions 1
        --warmups 0
        --export-reference "${export_reference}"
        --export-queries "${export_queries}"
        --output-dir "${export_dir}"
    RESULT_VARIABLE export_status
    OUTPUT_VARIABLE export_stdout
    ERROR_VARIABLE export_stderr)
if(NOT export_status EQUAL 0)
    message(FATAL_ERROR "synthetic dataset export failed (${export_status}):\n${export_stdout}\n${export_stderr}")
endif()
foreach(exported IN ITEMS "${export_reference}" "${export_queries}")
    if(NOT EXISTS "${exported}")
        message(FATAL_ERROR "synthetic dataset export is missing: ${exported}")
    endif()
    file(SIZE "${exported}" exported_size)
    if(exported_size EQUAL 0)
        message(FATAL_ERROR "synthetic dataset export is empty: ${exported}")
    endif()
endforeach()
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --scenarios mixed --methods sa32-binary
        --pattern-lengths 20 --locate-limits 1
        --build-repetitions 1 --query-repetitions 1 --warmups 0
        --export-reference "${export_reference}"
        --export-queries "${export_queries}"
        --output-dir "${OUTPUT_ROOT}/export-overwrite"
    RESULT_VARIABLE export_overwrite_status
    OUTPUT_QUIET ERROR_QUIET)
if(export_overwrite_status EQUAL 0)
    message(FATAL_ERROR "synthetic dataset export overwrote an existing file")
endif()

file(READ "${first}/run_metadata.tsv" first_metadata)
file(READ "${second}/run_metadata.tsv" second_metadata)
if(NOT first_metadata MATCHES "methods\tpattern_lengths\tlocate_limits\tbuild_repetitions")
    message(FATAL_ERROR "run_metadata.tsv does not record benchmark parameters")
endif()
if(NOT first_metadata MATCHES "learned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits")
    message(FATAL_ERROR "run_metadata.tsv does not record learned-index parameters")
endif()
if(NOT first_metadata MATCHES "worker_process_model" OR
   NOT first_metadata MATCHES "clean-exec-phase-v1")
    message(FATAL_ERROR "run_metadata.tsv does not record clean-exec worker provenance")
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
foreach(method IN ITEMS naive sa32-binary sa32-lcp-binary sa32-sapling sa32-child
                        sa64-binary sa64-lcp-binary
                        sa32-sampled-k2 sa32-sampled-k4 sa32-sampled-k8
                        sa64-sampled-k2 sa64-sampled-k4 sa64-sampled-k8 fm)
    if(NOT build_results MATCHES "\t${method}\t")
        message(FATAL_ERROR "build_results.tsv is missing ${method}")
    endif()
endforeach()
if(NOT query_results MATCHES "query_group\tpattern_length\tstrand\toperation\tmax_hits")
    message(FATAL_ERROR "query_results.tsv has an unexpected schema")
endif()
foreach(metric IN ITEMS suffix_comparisons character_comparisons gallop_probes predictions
                        prediction_error_mean prediction_error_p50 prediction_error_p95
                        prediction_error_p99 full_binary_fallbacks)
    if(NOT query_results MATCHES "${metric}")
        message(FATAL_ERROR "query_results.tsv is missing learned-search metric ${metric}")
    endif()
endforeach()
if(NOT build_results MATCHES "learned_index_build_seconds_median" OR
   NOT build_results MATCHES "learned_index_bytes")
    message(FATAL_ERROR "build_results.tsv is missing learned-index build metrics")
endif()
foreach(metric IN ITEMS sa_sampling_rate allocated_disk_bytes
                        build_worker_peak_rss_mb_median save_worker_peak_rss_mb_median
                        load_worker_peak_rss_mb_median query_worker_peak_rss_mb_max)
    if(NOT build_results MATCHES "${metric}")
        message(FATAL_ERROR "build_results.tsv is missing ${metric}")
    endif()
endforeach()
if(NOT query_results MATCHES "query_worker_peak_rss_mb")
    message(FATAL_ERROR "query_results.tsv is missing query worker RSS")
endif()
if(NOT raw_results MATCHES "phase\tquery_group\tpattern_length\tstrand")
    message(FATAL_ERROR "raw_repetitions.tsv has an unexpected schema")
endif()
if(NOT raw_results MATCHES "query_definition" OR NOT raw_results MATCHES "query_id\tquery_source")
    message(FATAL_ERROR "raw_repetitions.tsv is missing ordered query definitions")
endif()
if(NOT raw_results MATCHES
   "query_bases_per_second\tspeedup_vs_fm_huff_scalar\tthreads\ttotal_bases\tserialized_bytes\tallocated_disk_bytes\tlearned_index_bytes")
    message(FATAL_ERROR "raw_repetitions.tsv is missing build/index provenance fields")
endif()
if(NOT raw_results MATCHES
   "backend\tbackend_signature\tsdsl_version\tcoordinate_width\tsa_sampling_rate\tcanary_total_hits\tcanary_reported_hits\tcanary_checksum\tquery_threads")
    message(FATAL_ERROR "raw_repetitions.tsv is missing backend/canary/thread provenance")
endif()
file(STRINGS "${first}/raw_repetitions.tsv" raw_lines)
list(GET raw_lines 0 raw_header)
string(REGEX MATCHALL "\t" raw_header_tabs "${raw_header}")
list(LENGTH raw_header_tabs expected_raw_tabs)
set(raw_line_number 0)
foreach(raw_line IN LISTS raw_lines)
    math(EXPR raw_line_number "${raw_line_number} + 1")
    string(REGEX MATCHALL "\t" raw_line_tabs "${raw_line}")
    list(LENGTH raw_line_tabs actual_raw_tabs)
    if(NOT actual_raw_tabs EQUAL expected_raw_tabs)
        message(FATAL_ERROR
            "raw_repetitions.tsv line ${raw_line_number} has ${actual_raw_tabs} tabs; expected ${expected_raw_tabs}")
    endif()
endforeach()
if(NOT raw_results MATCHES "peak_rss_mb\tpeak_rss_scope" OR
   NOT raw_results MATCHES "build_worker_clean_exec_reference_plus_build" OR
   NOT raw_results MATCHES "save_worker_clean_exec_load_plus_save" OR
   NOT raw_results MATCHES "load_worker_clean_exec_load_plus_canary" OR
   NOT raw_results MATCHES
       "count_worker_clean_exec_required_dataset_plus_load_plus_query" OR
   NOT raw_results MATCHES
       "locate_worker_1_clean_exec_required_dataset_plus_load_plus_query")
    message(FATAL_ERROR "raw_repetitions.tsv is missing isolated worker RSS scopes")
endif()

if(SUFKIT_CAPS_ENABLED)
    set(caps_dir "${OUTPUT_ROOT}/caps-unified")
    execute_process(
        COMMAND "${SUFKIT_EXECUTABLE}" bench
            --profile smoke --scenarios balanced
            --methods sa32-binary,caps32,fm-huff
            --sa-threads 2
            --pattern-lengths 20 --locate-limits 1
            --build-repetitions 1 --query-repetitions 1 --warmups 0
            --output-dir "${caps_dir}"
        RESULT_VARIABLE caps_status
        OUTPUT_VARIABLE caps_stdout
        ERROR_VARIABLE caps_stderr)
    if(NOT caps_status EQUAL 0)
        message(FATAL_ERROR
            "unified CaPS exact benchmark failed (${caps_status}):\n${caps_stdout}\n${caps_stderr}")
    endif()
    file(READ "${caps_dir}/run_metadata.tsv" caps_metadata)
    file(READ "${caps_dir}/build_results.tsv" caps_builds)
    file(READ "${caps_dir}/raw_repetitions.tsv" caps_raw)
    if(NOT caps_metadata MATCHES "sa_threads" OR NOT caps_metadata MATCHES "\t2\t")
        message(FATAL_ERROR "unified CaPS benchmark metadata does not record --sa-threads")
    endif()
    if(NOT caps_builds MATCHES "\tcaps32\t[^\n]*\t32\t1\t2\t")
        message(FATAL_ERROR "unified CaPS build row does not report 32-bit/2-thread provenance")
    endif()
    if(NOT caps_raw MATCHES "\tcaps32\tbuild\t[^\n]*\t2\t")
        message(FATAL_ERROR "unified CaPS raw build row does not report its thread count")
    endif()
endif()
foreach(width IN ITEMS 32 64)
    foreach(rate IN ITEMS 2 4 8)
        set(sampled "sa${width}-sampled-k${rate}")
        if(NOT build_results MATCHES "\t${sampled}\t[^\n]*\t${width}\t${rate}\t1\t")
            message(FATAL_ERROR "sampled benchmark row does not report its coordinate width/rate: ${sampled}")
        endif()
    endforeach()
endforeach()

set(fm_dir "${OUTPUT_ROOT}/fm-backends")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke
        --scenarios balanced
        --methods fm-huff,fm-balanced,fm-epr
        --fm-query-modes scalar,batch
        --fm-batch-widths 1,4,8,16,32
        --fm-batch-widths-for fm-balanced:16,32
        --fm-batch-widths-for fm-epr:16,32
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
if(NOT fm_metadata MATCHES "fm_query_modes\tfm_batch_widths\tfm_batch_width_overrides")
    message(FATAL_ERROR "FM benchmark metadata fields are missing")
endif()
if(NOT fm_metadata MATCHES "fm-balanced:16,32;fm-epr:16,32")
    message(FATAL_ERROR "FM benchmark metadata does not record method-specific width overrides")
endif()
foreach(method IN ITEMS fm-huff fm-balanced fm-epr)
    if(NOT fm_builds MATCHES "\t${method}\t")
        message(FATAL_ERROR "FM build results are missing ${method}")
    endif()
endforeach()
if(NOT fm_queries MATCHES "fm_query_mode\tfm_batch_width\tquery_bases\tquery_bases_per_second\tspeedup_vs_fm_huff_scalar")
    message(FATAL_ERROR "FM query summary fields are missing")
endif()
foreach(width IN ITEMS 1 4 8 16 32)
    if(NOT fm_queries MATCHES "\tfm-huff\t[^\n]*\tbatch\t${width}\t")
        message(FATAL_ERROR "fm-huff batch width ${width} row is missing")
    endif()
endforeach()
foreach(method IN ITEMS fm-balanced fm-epr)
    foreach(width IN ITEMS 16 32)
        if(NOT fm_queries MATCHES "\t${method}\t[^\n]*\tbatch\t${width}\t")
            message(FATAL_ERROR "${method} batch width ${width} row is missing")
        endif()
    endforeach()
    if(fm_queries MATCHES "\t${method}\t[^\n]*\tbatch\t(1|4|8)\t")
        message(FATAL_ERROR "${method} unexpectedly ran a non-contract batch width")
    endif()
endforeach()
if(NOT fm_raw MATCHES "fm_query_mode\tfm_batch_width\tquery_bases")
    message(FATAL_ERROR "FM raw repetition fields are missing")
endif()

# Method-specific overrides replace the global default only for that method;
# users retain the generic ability to benchmark any legal width explicitly.
set(fm_custom_dir "${OUTPUT_ROOT}/fm-custom-widths")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --scenarios balanced --methods fm-balanced
        --fm-query-modes batch --fm-batch-widths 16
        --fm-batch-widths-for fm-balanced:1,4
        --pattern-lengths 20 --locate-limits 1
        --build-repetitions 1 --query-repetitions 1 --warmups 0
        --output-dir "${fm_custom_dir}"
    RESULT_VARIABLE fm_custom_status
    OUTPUT_VARIABLE fm_custom_stdout
    ERROR_VARIABLE fm_custom_stderr)
if(NOT fm_custom_status EQUAL 0)
    message(FATAL_ERROR
        "custom FM batch widths failed (${fm_custom_status}):\n${fm_custom_stdout}\n${fm_custom_stderr}")
endif()
file(READ "${fm_custom_dir}/query_results.tsv" fm_custom_queries)
foreach(width IN ITEMS 1 4)
    if(NOT fm_custom_queries MATCHES "\tfm-balanced\t[^\n]*\tbatch\t${width}\t")
        message(FATAL_ERROR "custom fm-balanced batch width ${width} row is missing")
    endif()
endforeach()
if(fm_custom_queries MATCHES "\tfm-balanced\t[^\n]*\tbatch\t16\t")
    message(FATAL_ERROR "method-specific FM widths did not replace the global default")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --scenarios balanced --methods fm
        --fm-query-modes batch
        --fm-batch-widths-for fm:1
        --fm-batch-widths-for fm-huff:4
        --output-dir "${OUTPUT_ROOT}/fm-duplicate-override"
    RESULT_VARIABLE fm_duplicate_override_status
    OUTPUT_QUIET ERROR_QUIET)
if(fm_duplicate_override_status EQUAL 0)
    message(FATAL_ERROR "duplicate FM alias/method batch-width overrides were accepted")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --scenarios balanced --methods fm-huff
        --fm-query-modes batch --fm-batch-widths 16,16
        --output-dir "${OUTPUT_ROOT}/fm-duplicate-global-width"
    RESULT_VARIABLE fm_duplicate_global_width_status
    OUTPUT_QUIET ERROR_QUIET)
if(fm_duplicate_global_width_status EQUAL 0)
    message(FATAL_ERROR "a duplicate global FM batch width was accepted")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --scenarios balanced --methods fm-balanced
        --fm-query-modes batch
        --fm-batch-widths-for fm-balanced:16,16
        --output-dir "${OUTPUT_ROOT}/fm-duplicate-method-width"
    RESULT_VARIABLE fm_duplicate_method_width_status
    OUTPUT_QUIET ERROR_QUIET)
if(fm_duplicate_method_width_status EQUAL 0)
    message(FATAL_ERROR "a duplicate method-specific FM batch width was accepted")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --profile smoke --scenarios balanced --methods fm-huff
        --fm-query-modes batch
        --fm-batch-widths-for fm-epr:16,32
        --output-dir "${OUTPUT_ROOT}/fm-unselected-override"
    RESULT_VARIABLE fm_unselected_override_status
    OUTPUT_QUIET ERROR_QUIET)
if(fm_unselected_override_status EQUAL 0)
    message(FATAL_ERROR "an FM batch-width override for an unselected backend was accepted")
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

set(partial_user_dir "${OUTPUT_ROOT}/user-partial-high-frequency")
string(REPEAT "C" 2020 moderate_repeat)
file(WRITE "${OUTPUT_ROOT}/partial-reference.fa"
    ">very_frequent\n${homopolymer}\n>moderately_frequent\n${moderate_repeat}\n")
file(WRITE "${OUTPUT_ROOT}/partial-queries.fa"
    ">very_frequent\nAAAAAAAAAAAAAAAAAAAA\n>moderately_frequent\nCCCCCCCCCCCCCCCCCCCC\n")
execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench
        --reference "${OUTPUT_ROOT}/partial-reference.fa"
        --queries "${OUTPUT_ROOT}/partial-queries.fa"
        --methods fm --locate-limits all
        --build-repetitions 1 --query-repetitions 1 --warmups 0
        --output-dir "${partial_user_dir}"
    RESULT_VARIABLE partial_user_status
    OUTPUT_VARIABLE partial_user_stdout
    ERROR_VARIABLE partial_user_stderr)
if(NOT partial_user_status EQUAL 0)
    message(FATAL_ERROR
        "partial high-frequency benchmark failed (${partial_user_status}):\n${partial_user_stdout}\n${partial_user_stderr}")
endif()
file(READ "${partial_user_dir}/query_results.tsv" partial_user_results)
if(NOT partial_user_results MATCHES
   "\tfm\tuser_hit_gt_1000\t20\tforward\tlocate\tall\t1\t1\t[^\n]*\tok\t")
    message(FATAL_ERROR
        "complete locate did not retain the safe query while recording one skipped high-frequency query")
endif()

execute_process(
    COMMAND "${SUFKIT_EXECUTABLE}" bench --profile smoke --reference missing.fa --output-dir invalid
    RESULT_VARIABLE conflict_status
    OUTPUT_QUIET ERROR_QUIET)
if(conflict_status EQUAL 0)
    message(FATAL_ERROR "conflicting --profile and --reference options were accepted")
endif()
