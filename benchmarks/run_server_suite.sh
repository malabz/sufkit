#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 SOURCE_DIR RUN_DIR" >&2
    exit 2
fi

source_dir=$1
run_dir=$2
experiment_root=$(dirname "$(dirname "${run_dir}")")
build_dir=${SUFKIT_BENCH_BUILD_DIR:-"${experiment_root}/build/205023a-release"}
dataset_dir=${SUFKIT_BENCH_DATASET_DIR:-"${experiment_root}/datasets/seed-20260822"}
package_dir=${SUFKIT_BENCH_PACKAGE_DIR:-"${experiment_root}/packages/$(basename "${run_dir}")"}
seed=20260822
run_id=$(basename "${run_dir}")
suite_variant=${SUFKIT_BENCH_SUITE_VARIANT:-full-matrix}
case "${suite_variant}" in
    full-matrix|representative) ;;
    *)
        echo "invalid SUFKIT_BENCH_SUITE_VARIANT: ${suite_variant}" >&2
        exit 2
        ;;
esac
headline_dataset_dir="${dataset_dir}/${run_id}/headline-full-mixed"
producer_marker=.sufkit-producer-complete
producer_checksum_file=.sufkit-producer-files.sha256
current_source_identity=
if [[ "${SUFKIT_BENCH_RUNNER_SOURCE_ONLY:-0}" == 1 ]]; then
    current_source_identity=${SUFKIT_BENCH_TEST_SOURCE_IDENTITY:-}
fi
runner_lock_fd=
build_identity_file="${build_dir}/.sufkit-source-identity"
binary_checksum_manifest="${run_dir}/manifest/binaries.sha256"
ctest_binary_checksum_manifest="${run_dir}/manifest/ctest-binaries.sha256"

if [[ ! -f "${source_dir}/CMakeLists.txt" ]]; then
    echo "invalid sufkit source directory: ${source_dir}" >&2
    exit 2
fi

mkdir -p "${run_dir}/logs" "${run_dir}/manifest" "${run_dir}/state" \
    "${run_dir}/smoke" "${run_dir}/quick" "${run_dir}/standard" \
    "${run_dir}/full" "${run_dir}/headline" "${run_dir}/raw" \
    "${run_dir}/summary" "${dataset_dir}"
stage_manifest="${run_dir}/manifest/stages.tsv"

initialize_stage_manifest() {
    if path_exists "${stage_manifest}"; then return 0; fi
    local partial="${run_dir}/manifest/.stages.tsv.$$.$RANDOM.partial"
    printf 'stage\tstart_utc\tend_utc\telapsed_seconds\texit_status\n' >"${partial}"
    publish_file_noclobber "${partial}" "${stage_manifest}"
}

archive_state_file() {
    local path=$1
    local reason=$2
    if [[ ! -e "${path}" ]]; then return 0; fi
    local archive="${path}.${reason}.$(date -u +%Y%m%dT%H%M%SZ).$$.${RANDOM}"
    mv -- "${path}" "${archive}"
}

compute_source_identity() {
    if [[ -s "${source_dir}/SOURCE_ARCHIVE.sha256" ]]; then
        sha256sum "${source_dir}/SOURCE_ARCHIVE.sha256" | awk '{ print $1 }'
        return
    fi
    if git -C "${source_dir}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        {
            git -C "${source_dir}" rev-parse HEAD
            git -C "${source_dir}" diff --no-ext-diff --binary HEAD --
            while IFS= read -r -d '' relative; do
                printf 'untracked\0%s\0' "${relative}"
                sha256sum -- "${source_dir}/${relative}"
            done < <(git -C "${source_dir}" ls-files --others --exclude-standard -z)
        } | sha256sum | awk '{ print $1 }'
        return
    fi
    (
        cd "${source_dir}"
        while IFS= read -r -d '' relative; do
            printf '%s\0' "${relative#./}"
            sha256sum -- "${relative}"
        done < <(find . -path './.git' -prune -o -type f -print0 | sort -z)
    ) | sha256sum | awk '{ print $1 }'
}

initialize_source_identity() {
    if [[ -z "${current_source_identity}" ]]; then
        current_source_identity=$(compute_source_identity)
    fi
    if [[ ! "${current_source_identity}" =~ ^[[:xdigit:]]{64}$ ]]; then
        echo "invalid source identity: ${current_source_identity:-<empty>}" >&2
        return 2
    fi
}

acquire_runner_lock() {
    if ! command -v flock >/dev/null 2>&1; then
        echo "flock is required for benchmark run serialization" >&2
        return 2
    fi
    exec {runner_lock_fd}>>"${run_dir}/state/runner.lock"
    if ! flock -n "${runner_lock_fd}"; then
        echo "another benchmark runner already owns ${run_dir}" >&2
        return 2
    fi
    printf 'pid=%s\nstarted_utc=%s\n' "$$" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >"${run_dir}/state/runner.lock"
}

archive_top_level_completion_markers() {
    archive_state_file "${run_dir}/state/ALL_COMPLETE" revalidate
    archive_state_file "${run_dir}/state/PACKAGE_COMPLETE" revalidate
}

path_exists() {
    [[ -e "$1" || -L "$1" ]]
}

publish_directory_noclobber() {
    local source=$1
    local target=$2
    if path_exists "${target}"; then
        echo "publication target already exists: ${target}" >&2
        return 2
    fi
    if ! mv -T -n -- "${source}" "${target}"; then
        echo "atomic directory publication failed: ${source} -> ${target}" >&2
        return 2
    fi
    if path_exists "${source}"; then
        echo "publication target appeared concurrently; retaining ${source}" >&2
        return 2
    fi
    [[ -d "${target}" ]]
}

publish_file_noclobber() {
    local source=$1
    local target=$2
    if path_exists "${target}"; then
        echo "publication target already exists: ${target}" >&2
        return 2
    fi
    if ! mv -T -n -- "${source}" "${target}"; then
        echo "atomic file publication failed: ${source} -> ${target}" >&2
        return 2
    fi
    if path_exists "${source}"; then
        echo "publication target appeared concurrently; retaining ${source}" >&2
        return 2
    fi
    [[ -f "${target}" ]]
}

record_stage_attempt_detail() {
    local detail=$1
    local target=${SUFKIT_STAGE_ATTEMPT_FILE:-}
    if [[ -z "${target}" ]]; then return 0; fi
    printf '%s\n' "${detail}" >>"${target}"
}

validate_tsv() {
    local path=$1
    [[ -s "${path}" ]] || return 1
    local header
    IFS= read -r header <"${path}" || return 1
    [[ "${header}" == *$'\t'* ]] || return 1
    awk 'NR == 2 { found = 1; exit } END { exit !found }' "${path}"
}

set_required_producer_tsvs() {
    local kind=$1
    required_producer_tsvs=()
    case "${kind}" in
    exact|headline-exact)
        required_producer_tsvs=(
            run_metadata.tsv build_results.tsv query_results.tsv raw_repetitions.tsv)
        ;;
    sa-build)
        required_producer_tsvs=(run_metadata.tsv build_results.tsv raw_repetitions.tsv)
        ;;
    right-maximal)
        required_producer_tsvs=(
            run_metadata.tsv build_results.tsv query_results.tsv raw_repetitions.tsv
            correctness_summary.tsv)
        ;;
    headline-dataset)
        ;;
    *)
        echo "unknown producer validation kind: ${kind}" >&2
        return 2
        ;;
    esac
}

write_producer_checksums() {
    local kind=$1
    local directory=$2
    set_required_producer_tsvs "${kind}"
    if [[ ${#required_producer_tsvs[@]} -eq 0 ]]; then return 0; fi
    (cd "${directory}" && sha256sum -- "${required_producer_tsvs[@]}" \
        >"${producer_checksum_file}")
}

validate_producer_checksums() {
    local kind=$1
    local directory=$2
    set_required_producer_tsvs "${kind}"
    if [[ ${#required_producer_tsvs[@]} -eq 0 ]]; then return 0; fi
    local checksum_path="${directory}/${producer_checksum_file}"
    [[ -s "${checksum_path}" ]] || return 1
    [[ "$(wc -l <"${checksum_path}")" -eq ${#required_producer_tsvs[@]} ]] || return 1
    local required
    for required in "${required_producer_tsvs[@]}"; do
        awk -v required="${required}" '$2 == required { found = 1 } END { exit !found }' \
            "${checksum_path}" || return 1
    done
    (cd "${directory}" && sha256sum --check --strict "${producer_checksum_file}" >/dev/null)
}

current_binary_manifest_digest() {
    [[ -s "${binary_checksum_manifest}" ]] || {
        echo "binary checksum manifest is unavailable: ${binary_checksum_manifest}" >&2
        return 2
    }
    sha256sum "${binary_checksum_manifest}" | awk '{ print $1 }'
}

write_producer_marker() {
    local kind=$1
    local directory=$2
    initialize_source_identity
    local binary_manifest_digest
    binary_manifest_digest=$(current_binary_manifest_digest)
    {
        printf 'kind=%s\n' "${kind}"
        printf 'stage=%s\n' "${SUFKIT_CURRENT_STAGE:-manual}"
        printf 'attempt=%s\n' "${SUFKIT_STAGE_ATTEMPT_ID:-manual}"
        printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'source_identity=%s\n' "${current_source_identity}"
        printf 'binary_manifest_sha256=%s\n' "${binary_manifest_digest}"
    } >"${directory}/${producer_marker}"
}

validate_producer_marker() {
    local kind=$1
    local directory=$2
    initialize_source_identity
    local marker="${directory}/${producer_marker}"
    [[ -s "${marker}" && "$(wc -l <"${marker}")" -eq 6 ]] || return 1
    local binary_manifest_digest
    binary_manifest_digest=$(current_binary_manifest_digest) || return 1
    [[ "$(grep -Fxc "kind=${kind}" "${marker}")" -eq 1 ]] &&
    [[ "$(grep -c '^stage=' "${marker}")" -eq 1 ]] &&
    [[ "$(grep -c '^attempt=' "${marker}")" -eq 1 ]] &&
    [[ "$(grep -c '^completed_utc=' "${marker}")" -eq 1 ]] &&
    [[ "$(grep -Fxc "source_identity=${current_source_identity}" "${marker}")" -eq 1 ]] &&
    [[ "$(grep -Fxc "binary_manifest_sha256=${binary_manifest_digest}" "${marker}")" -eq 1 ]]
}

validate_producer_payload() {
    local kind=$1
    local directory=$2
    [[ -d "${directory}" ]] || return 1
    case "${kind}" in
    exact)
        validate_tsv "${directory}/run_metadata.tsv" &&
        validate_tsv "${directory}/build_results.tsv" &&
        validate_tsv "${directory}/query_results.tsv" &&
        validate_tsv "${directory}/raw_repetitions.tsv"
        ;;
    sa-build)
        validate_tsv "${directory}/run_metadata.tsv" &&
        validate_tsv "${directory}/build_results.tsv" &&
        validate_tsv "${directory}/raw_repetitions.tsv"
        ;;
    right-maximal)
        validate_tsv "${directory}/run_metadata.tsv" &&
        validate_tsv "${directory}/build_results.tsv" &&
        validate_tsv "${directory}/query_results.tsv" &&
        validate_tsv "${directory}/raw_repetitions.tsv" &&
        validate_tsv "${directory}/correctness_summary.tsv"
        ;;
    headline-exact)
        validate_producer_payload exact "${directory}" &&
        [[ -s "${directory}/dataset/reference.fa" ]] &&
        [[ -s "${directory}/dataset/queries.fa" ]] &&
        [[ -s "${directory}/dataset/dataset.sha256" ]] &&
        (cd "${directory}/dataset" && sha256sum --check dataset.sha256 >/dev/null)
        ;;
    headline-dataset)
        [[ -s "${directory}/reference.fa" ]] &&
        [[ -s "${directory}/queries.fa" ]] &&
        [[ -s "${directory}/dataset.sha256" ]] &&
        (cd "${directory}" && sha256sum --check dataset.sha256 >/dev/null)
        ;;
    *)
        echo "unknown producer validation kind: ${kind}" >&2
        return 2
        ;;
    esac
}

validate_published_output() {
    local kind=$1
    local directory=$2
    validate_producer_payload "${kind}" "${directory}" || return 1
    validate_producer_marker "${kind}" "${directory}" || return 1
    validate_producer_checksums "${kind}" "${directory}"
}

validate_exact_output() { validate_published_output exact "$1"; }
validate_sa_build_output() { validate_published_output sa-build "$1"; }
validate_right_maximal_output() { validate_published_output right-maximal "$1"; }

create_unique_partial_directory() {
    local output=$1
    local parent
    local base
    parent=$(dirname "${output}")
    base=$(basename "${output}")
    mkdir -p "${parent}"
    local attempt=${SUFKIT_STAGE_ATTEMPT_ID:-manual-$(date -u +%Y%m%dT%H%M%SZ)-$$}
    local candidate
    local index
    for index in 0 1 2 3 4 5 6 7 8 9; do
        candidate="${parent}/.${base}.partial.${attempt}.${index}"
        if mkdir "${candidate}" 2>/dev/null; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    echo "cannot allocate a unique partial directory beside ${output}" >&2
    return 1
}

run_published_output() {
    local kind=$1
    local output=$2
    shift 2
    if path_exists "${output}"; then
        if validate_published_output "${kind}" "${output}"; then
            echo "recovering already published ${kind} output: ${output}" >&2
            return 0
        fi
        record_stage_attempt_detail "existing_invalid_output=${output}"
        echo "refusing to overwrite an existing unvalidated output: ${output}" >&2
        return 2
    fi

    local partial
    partial=$(create_unique_partial_directory "${output}")
    record_stage_attempt_detail "partial=${partial}"
    local status=0
    (
        set -Eeuo pipefail
        trap 'producer_status=$?; printf "producer command failed (status=%s): %s\n" "${producer_status}" "${BASH_COMMAND}" >&2; exit "${producer_status}"' ERR
        "$@" "${partial}"
    ) &
    local producer_pid=$!
    if wait "${producer_pid}"; then status=0; else status=$?; fi
    if [[ ${status} -ne 0 ]]; then
        echo "retaining failed ${kind} partial output: ${partial}" >&2
        return "${status}"
    fi
    if ! validate_producer_payload "${kind}" "${partial}"; then
        echo "retaining invalid ${kind} partial output: ${partial}" >&2
        return 2
    fi
    write_producer_checksums "${kind}" "${partial}"
    write_producer_marker "${kind}" "${partial}"
    if ! publish_directory_noclobber "${partial}" "${output}"; then
        record_stage_attempt_detail "publication_race_partial=${partial}"
        return 2
    fi
    validate_published_output "${kind}" "${output}"
}

run_stage() {
    local stage=$1
    local validator=$2
    local validator_argument=$3
    shift 3
    local complete="${run_dir}/state/${stage}.complete"
    local failed="${run_dir}/state/${stage}.failed"
    local attempt_file="${run_dir}/state/${stage}.attempt.current"
    local log="${run_dir}/logs/${stage}.log"
    local status=0
    local start_utc
    local end_utc
    local start_epoch
    local end_epoch
    initialize_source_identity
    initialize_stage_manifest
    if [[ -f "${complete}" ]]; then
        if ! grep -Fxq "source_identity=${current_source_identity}" "${complete}"; then
            echo "completed stage ${stage} belongs to another source identity; retrying" >&2
            archive_state_file "${complete}" source-identity-mismatch
        elif "${validator}" "${validator_argument}"; then
            echo "skipping completed and revalidated stage ${stage}" >&2
            return 0
        else
            echo "completed stage ${stage} failed revalidation; preserving its marker and retrying" >&2
            archive_state_file "${complete}" invalid
        fi
    fi
    archive_state_file "${failed}" retry
    archive_state_file "${attempt_file}" retry
    archive_state_file "${log}" retry
    start_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    start_epoch=$(date +%s)
    local attempt_id="$(date -u +%Y%m%dT%H%M%SZ).$$.${RANDOM}"
    # Launch the stage in a background subshell before waiting for it.  Placing
    # a function call directly in an `if`/`||` condition disables errexit
    # inside that function in Bash, which can make a failed intermediate
    # command look successful when a later command returns zero.  The child is
    # not itself in a conditional context, so `-eE` and the ERR trap preserve
    # the first unhandled failure across compound stage functions.
    (
        set -Eeuo pipefail
        export SUFKIT_CURRENT_STAGE="${stage}"
        export SUFKIT_STAGE_ATTEMPT_ID="${attempt_id}"
        export SUFKIT_STAGE_ATTEMPT_FILE="${attempt_file}"
        trap 'stage_status=$?; printf "stage command failed (status=%s): %s\n" "${stage_status}" "${BASH_COMMAND}" >&2; exit "${stage_status}"' ERR
        "$@"
    ) >"${log}" 2>&1 &
    local stage_pid=$!
    if wait "${stage_pid}"; then
        status=0
    else
        status=$?
    fi
    if [[ ${status} -eq 0 ]] && ! "${validator}" "${validator_argument}"; then
        status=2
        echo "stage ${stage} command succeeded but its validator failed" >>"${log}"
    fi
    end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    end_epoch=$(date +%s)
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "${stage}" "${start_utc}" "${end_utc}" "$((end_epoch - start_epoch))" "${status}" \
        >>"${stage_manifest}"
    if [[ ${status} -eq 0 ]]; then
        archive_state_file "${failed}" resolved
        archive_state_file "${attempt_file}" completed
        {
            printf 'stage=%s\n' "${stage}"
            printf 'attempt=%s\n' "${attempt_id}"
            printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
            printf 'source_identity=%s\n' "${current_source_identity}"
            printf 'validator=%s\n' "${validator}"
            printf 'validator_argument=%s\n' "${validator_argument}"
        } >"${complete}"
        return 0
    fi
    {
        printf 'stage=%s\n' "${stage}"
        printf 'attempt=%s\n' "${attempt_id}"
        printf 'failed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        printf 'exit_status=%s\n' "${status}"
        if [[ -s "${attempt_file}" ]]; then cat "${attempt_file}"; fi
    } >"${failed}"
    echo "stage ${stage} failed; see ${log}" >&2
    return "${status}"
}

capture_environment() {
    {
        date -u +%Y-%m-%dT%H:%M:%SZ
        uname -srm
        lscpu
        if command -v numactl >/dev/null 2>&1; then numactl --hardware; fi
        free -b
        if command -v swapon >/dev/null 2>&1; then swapon --show --bytes; fi
        if [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
            printf 'cpu0_scaling_governor\t'
            tr -d '\n' </sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
            printf '\n'
        fi
        df -B1 "${run_dir}"
        gcc --version
        command -v python3
        python3 --version
        cmake --version
        ninja --version
        printf 'affinity_source\t%s\n' 'lscpu NUMA node 0, one logical CPU per physical core'
        printf 'numa_node\t0\n'
        printf 'numa0_physical_cpu_list\t%s\n' "${physical_node0_cpus}"
        printf 'numa0_physical_cpu_count\t%s\n' "${physical_node0_cpu_count}"
        printf 'query_cpu\t%s\n' "${query_cpu}"
        printf 'query_affinity_command\t%s\n' "${query_pin_prefix[*]:-none}"
        printf 'parallel_affinity_command\t%s\n' "${parallel_pin_prefix[*]:-none}"
    } >"${run_dir}/manifest/environment.txt"
    {
        printf 'key\tvalue\n'
        printf 'run_id\t%s\n' "${run_id}"
        printf 'seed\t%s\n' "${seed}"
        printf 'profiles\t%s\n' 'smoke,quick,standard,full,headline'
        printf 'suite_variant\t%s\n' "${suite_variant}"
        printf 'scenarios_smoke\t%s\n' 'mixed'
        printf 'scenarios_quick\t%s\n' 'mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig'
        printf 'scenarios_standard\t%s\n' 'mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig'
        printf 'scenarios_full\t%s\n' 'mixed,repeat-rich,many-contig'
        printf 'execution_order\t%s\n' 'sequential stages and sequential benchmark methods'
        printf 'query_affinity_scope\t%s\n' "single physical core on NUMA node 0: CPU ${query_cpu}"
        printf 'parallel_affinity_scope\t%s\n' "all physical cores on NUMA node 0: CPUs ${physical_node0_cpus}"
        printf 'parallel_affinity_cpu_count\t%s\n' "${physical_node0_cpu_count}"
        printf 'headline_dataset_scope\t%s\n' "seed-${seed}/${run_id}/headline-full-mixed"
        printf 'headline_build_scope\t%s\n' 'one exact-benchmark worker protocol for divsufsort32, CaPS32, and FM Huffman'
        printf 'headline_query_scope\t%s\n' 'separate single-physical-core exact run on the same deterministic dataset fingerprint'
        printf 'standard_scope\t%s\n' "$(if [[ "${suite_variant}" == representative ]]; then echo mixed-representative; else echo full-matrix; fi)"
        printf 'full_scope\t%s\n' "$(if [[ "${suite_variant}" == representative ]]; then echo mixed-representative; else echo full-matrix; fi)"
    } >"${run_dir}/manifest/execution-scope.tsv"
    printf '%s\n' "${current_source_identity}" \
        >"${run_dir}/manifest/source-identity.sha256"
    if [[ -f "${source_dir}/SOURCE_REVISION.txt" ]]; then
        cp "${source_dir}/SOURCE_REVISION.txt" "${run_dir}/manifest/SOURCE_REVISION.txt"
    elif git -C "${source_dir}" rev-parse HEAD >"${run_dir}/manifest/SOURCE_REVISION.txt" 2>/dev/null; then
        git -C "${source_dir}" status --short >>"${run_dir}/manifest/SOURCE_REVISION.txt"
    fi
    if [[ -f "${source_dir}/SOURCE_ARCHIVE.sha256" ]]; then
        cp "${source_dir}/SOURCE_ARCHIVE.sha256" \
            "${run_dir}/manifest/SOURCE_ARCHIVE.sha256"
    fi
}

validate_build_dir_identity() {
    [[ -s "${build_identity_file}" ]] || return 1
    grep -Fxq "source_identity=${current_source_identity}" "${build_identity_file}"
}

require_compatible_build_directory() {
    if [[ -e "${build_dir}/CMakeCache.txt" || -e "${build_identity_file}" ]]; then
        if ! validate_build_dir_identity; then
            echo "build directory source identity is missing or mismatched: ${build_dir}" >&2
            echo "use a fresh SUFKIT_BENCH_BUILD_DIR; the existing directory was preserved" >&2
            return 2
        fi
    fi
}

write_build_dir_identity() {
    local partial="${build_identity_file}.partial.$$.$RANDOM"
    printf 'source_identity=%s\n' "${current_source_identity}" >"${partial}"
    mv -T -- "${partial}" "${build_identity_file}"
}

configure_release() {
    require_compatible_build_directory
    cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DSUFKIT_BUILD_CLI=ON \
        -DSUFKIT_BUILD_TESTS=ON \
        -DSUFKIT_BUILD_BENCHMARKS=ON \
        -DSUFKIT_BUILD_EXAMPLES=ON \
        -DSUFKIT_ENABLE_CAPS=ON
    write_build_dir_identity
}

build_release() {
    validate_build_dir_identity || {
        echo "refusing to build in an unbound build directory: ${build_dir}" >&2
        return 2
    }
    cmake --build "${build_dir}" -j 64
}

test_release() {
    validate_binary_manifest_stage
    ctest --test-dir "${build_dir}" --output-on-failure -j 8
    local partial="${ctest_binary_checksum_manifest}.partial.$$.$RANDOM"
    cp -- "${binary_checksum_manifest}" "${partial}"
    mv -T -- "${partial}" "${ctest_binary_checksum_manifest}"
}

capture_binary_manifest() {
    validate_build_stage unused
    sha256sum "${sufkit_bin}" "${sa_build_bin}" >"${binary_checksum_manifest}"
    {
        cat "${binary_checksum_manifest}"
        file "${sufkit_bin}" "${sa_build_bin}"
        ldd "${sufkit_bin}"
        ldd "${sa_build_bin}"
    } >"${run_dir}/manifest/binaries.txt"
    sha256sum "${source_dir}/benchmarks/run_server_suite.sh" \
        >"${run_dir}/manifest/runner.sha256"
}

validate_environment_stage() {
    [[ -s "${run_dir}/manifest/environment.txt" ]] &&
    validate_tsv "${run_dir}/manifest/execution-scope.tsv" &&
    [[ -s "${run_dir}/manifest/SOURCE_REVISION.txt" ]] &&
    [[ "$(<"${run_dir}/manifest/source-identity.sha256")" == \
       "${current_source_identity}" ]] || return 1
    if [[ -f "${source_dir}/SOURCE_ARCHIVE.sha256" ]]; then
        [[ -s "${run_dir}/manifest/SOURCE_ARCHIVE.sha256" ]] || return 1
        cmp -s "${source_dir}/SOURCE_ARCHIVE.sha256" \
            "${run_dir}/manifest/SOURCE_ARCHIVE.sha256"
    fi
}

validate_configure_stage() {
    local cache="${build_dir}/CMakeCache.txt"
    [[ -s "${cache}" ]] && validate_build_dir_identity || return 1
    local source_real
    source_real=$(cd "${source_dir}" && pwd -P)
    grep -Fxq 'CMAKE_BUILD_TYPE:STRING=Release' "${cache}" &&
    grep -Fxq 'SUFKIT_BUILD_TESTS:BOOL=ON' "${cache}" &&
    grep -Fxq 'SUFKIT_BUILD_BENCHMARKS:BOOL=ON' "${cache}" &&
    grep -Fxq 'SUFKIT_ENABLE_CAPS:BOOL=ON' "${cache}" &&
    grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=${source_real}" "${cache}"
}

validate_build_stage() {
    validate_build_dir_identity &&
    [[ -x "${sufkit_bin}" && -x "${sa_build_bin}" ]]
}

validate_binary_manifest_stage() {
    validate_build_stage unused &&
    [[ -s "${run_dir}/manifest/binaries.txt" &&
       -s "${binary_checksum_manifest}" &&
       -s "${run_dir}/manifest/runner.sha256" ]] &&
    [[ "$(wc -l <"${binary_checksum_manifest}")" -eq 2 ]] &&
    sha256sum --check --strict "${binary_checksum_manifest}" >/dev/null &&
    sha256sum --check "${run_dir}/manifest/runner.sha256" >/dev/null
}

validate_ctest_stage() {
    local log=$1
    validate_binary_manifest_stage unused &&
    [[ -s "${ctest_binary_checksum_manifest}" ]] &&
    cmp -s "${binary_checksum_manifest}" "${ctest_binary_checksum_manifest}" &&
    [[ -s "${log}" ]] && grep -Fq '100% tests passed' "${log}" || return 1
    local tests_run
    tests_run=$(sed -nE \
        's/.*100% tests passed, [0-9]+ tests failed out of ([0-9]+).*/\1/p' \
        "${log}" | tail -n 1)
    [[ "${tests_run}" =~ ^[0-9]+$ && "${tests_run}" -ge 7 ]]
}

audit_profile() {
    local profile=$1
    python3 "${source_dir}/benchmarks/package_benchmark_results.py" audit \
        --run-dir "${run_dir}" \
        --profile "${profile}"
}

validate_profile_audit() {
    local profile=$1
    audit_profile "${profile}" >/dev/null
}

validate_package_input_manifest() {
    local directory=$1
    python3 -B - "${directory}/manifest.tsv" "${run_dir}" <<'PY'
import csv
import hashlib
import pathlib
import sys

manifest = pathlib.Path(sys.argv[1])
run = pathlib.Path(sys.argv[2]).resolve()
profiles = {"smoke", "quick", "standard", "full", "headline"}
stable_manifest_files = {
    "environment.txt", "execution-scope.tsv", "SOURCE_REVISION.txt",
    "SOURCE_ARCHIVE.sha256", "source-identity.sha256", "binaries.txt",
    "binaries.sha256", "ctest-binaries.sha256", "runner.sha256",
    "headline-dataset.sha256",
}

def hidden(parts):
    return any(part.startswith(".") for part in parts)

def stable(relative):
    parts = relative.parts
    if not parts or hidden(parts):
        return False
    if parts[0] in profiles:
        return True
    if len(parts) == 2 and parts[0] == "manifest" and parts[1] in stable_manifest_files:
        return True
    return relative.as_posix() == "state/ALL_COMPLETE"

def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()

if not manifest.is_file():
    raise SystemExit(f"package input manifest is missing: {manifest}")
with manifest.open("r", encoding="utf-8", newline="") as stream:
    reader = csv.DictReader(stream, delimiter="\t")
    required = {"path", "bytes", "sha256"}
    if reader.fieldnames is None or not required.issubset(reader.fieldnames):
        raise SystemExit("package input manifest has an incompatible schema")
    rows = list(reader)

bound = {}
for row in rows:
    text = row.get("path", "")
    relative = pathlib.PurePosixPath(text)
    if relative.is_absolute() or ".." in relative.parts or not stable(relative):
        continue
    if text in bound:
        raise SystemExit(f"duplicate stable package input manifest path: {text}")
    bound[text] = row

current = {}
for root_name in sorted(profiles):
    root = run / root_name
    if not root.is_dir():
        raise SystemExit(f"current run profile directory is missing: {root_name}")
    for path in root.rglob("*"):
        if path.is_file():
            relative = path.relative_to(run)
            if stable(pathlib.PurePosixPath(relative.as_posix())):
                current[relative.as_posix()] = path
for name in sorted(stable_manifest_files):
    path = run / "manifest" / name
    if path.is_file():
        current[path.relative_to(run).as_posix()] = path
all_complete = run / "state" / "ALL_COMPLETE"
if all_complete.is_file():
    current["state/ALL_COMPLETE"] = all_complete

missing = sorted(set(current) - set(bound))
stale = sorted(set(bound) - set(current))
if missing or stale:
    raise SystemExit(
        "package input manifest/current run path mismatch: "
        f"missing={','.join(missing[:8]) or '<none>'}; "
        f"stale={','.join(stale[:8]) or '<none>'}")
if not current:
    raise SystemExit("package input manifest binds no stable run inputs")

for text, path in sorted(current.items()):
    row = bound[text]
    size = str(path.stat().st_size)
    if row.get("bytes") != size:
        raise SystemExit(
            f"package input size mismatch for {text}: manifest={row.get('bytes')}, current={size}")
    actual = digest(path)
    if row.get("sha256") != actual:
        raise SystemExit(
            f"package input SHA-256 mismatch for {text}: "
            f"manifest={row.get('sha256')}, current={actual}")
PY
}

package_tree_digest() {
    local directory=$1
    python3 -B - "${directory}" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1]).resolve()
if not root.is_dir():
    raise SystemExit(f"package directory is missing: {root}")
tree = hashlib.sha256()
for path in sorted(path for path in root.rglob("*") if path.is_file()):
    relative = path.relative_to(root).as_posix().encode("utf-8")
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    tree.update(relative)
    tree.update(b"\0")
    tree.update(value.hexdigest().encode("ascii"))
    tree.update(b"\0")
print(tree.hexdigest())
PY
}

write_all_complete_marker() {
    initialize_source_identity
    local target="${run_dir}/state/ALL_COMPLETE"
    local partial="${run_dir}/state/.ALL_COMPLETE.${SUFKIT_STAGE_ATTEMPT_ID:-runner}.$$.partial"
    {
        printf 'source_identity=%s\n' "${current_source_identity}"
        printf 'profiles=smoke,quick,standard,full,headline\n'
        printf 'suite_variant=%s\n' "${suite_variant}"
    } >"${partial}"
    publish_file_noclobber "${partial}" "${target}"
}

validate_complete_package() {
    local directory=$1
    local profile
    for profile in smoke quick standard full headline; do
        audit_profile "${profile}" >/dev/null || return 1
    done
    local required
    for required in \
        README.md correctness-summary.tsv environment.tsv manifest.tsv commands.md \
        headline/README.md headline/headline.tsv headline/headline-build.tsv \
        headline/headline-query.tsv headline/headline-right-maximal.tsv \
        build/README.md build/build-results.tsv build/caps-scaling.tsv \
        build/sampling-space.tsv build/raw-repetitions.tsv \
        exact/README.md exact/count-results.tsv exact/locate-results.tsv \
        exact/fm-batch-results.tsv exact/raw-repetitions.tsv \
        right-maximal/README.md right-maximal/algorithm-ablation.tsv \
        right-maximal/sampled-sa-results.tsv right-maximal/raw-repetitions.tsv \
        scenarios/README.md scenarios/gc-and-repeat-effects.tsv \
        scenarios/contig-and-n-effects.tsv \
        figures/headline-performance.svg figures/build-scaling.svg \
        figures/memory-and-size.svg figures/count-locate-details.svg \
        figures/right-maximal-ablation.svg figures/caps-thread-scaling.svg; do
        if [[ ! -s "${directory}/${required}" ]]; then
            echo "complete package validation is missing ${required}" >&2
            return 1
        fi
    done
    python3 -B -c '
import importlib.util
import pathlib
import sys

module_path = pathlib.Path(sys.argv[1])
package = pathlib.Path(sys.argv[2])
spec = importlib.util.spec_from_file_location("sufkit_update_benchmark_readme", module_path)
if spec is None or spec.loader is None:
    raise SystemExit("cannot load benchmark package validator")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
module.validate_package(package)
' "${source_dir}/benchmarks/update_benchmark_readme.py" "${directory}"
    validate_package_input_manifest "${directory}"
}

package_results() {
    if [[ -e "${package_dir}" ]]; then
        if validate_complete_package "${package_dir}"; then
            echo "recovering already published and strictly validated package: ${package_dir}" >&2
            return 0
        fi
        echo "refusing to overwrite an existing invalid package: ${package_dir}" >&2
        return 2
    fi
    python3 "${source_dir}/benchmarks/package_benchmark_results.py" package \
        --run-dir "${run_dir}" \
        --output-dir "${package_dir}" \
        --server-label server-205023a \
        --require-complete
    validate_complete_package "${package_dir}"
}

query_pin_prefix=()
parallel_pin_prefix=()
physical_node0_cpus=
physical_node0_cpu_count=0
query_cpu=
sufkit_bin="${build_dir}/sufkit"
sa_build_bin="${build_dir}/sufkit_sa_build_bench"

initialize_affinity() {
    physical_node0_cpus=$(LC_ALL=C lscpu -p=CPU,CORE,SOCKET,NODE | awk -F, '
        $1 !~ /^#/ && ($4 == "0" || $4 == "") && !seen[$3 ":" $2]++ {
            if (count++ != 0) printf ",";
            printf "%s", $1
        }
        END { printf "\n" }
    ')
    if [[ -z "${physical_node0_cpus}" ]]; then
        echo "could not select one CPU per physical core from lscpu for NUMA node 0" >&2
        return 2
    fi
    query_cpu=${physical_node0_cpus%%,*}
    physical_node0_cpu_count=$(awk -F, '{ print NF }' <<<"${physical_node0_cpus}")
    if command -v numactl >/dev/null 2>&1; then
        query_pin_prefix=(numactl --physcpubind="${query_cpu}" --membind=0)
        parallel_pin_prefix=(numactl --physcpubind="${physical_node0_cpus}" --membind=0)
    elif command -v taskset >/dev/null 2>&1; then
        query_pin_prefix=(taskset -c "${query_cpu}")
        parallel_pin_prefix=(taskset -c "${physical_node0_cpus}")
    else
        echo "neither numactl nor taskset is available for required benchmark affinity" >&2
        return 2
    fi
}

exact_methods_all="naive,sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,sa64-binary,sa64-lcp-binary,sa32-sampled-k2,sa32-sampled-k4,sa32-sampled-k8,fm-huff,fm-balanced,fm-epr"
exact_methods_indexed="sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,sa64-binary,sa64-lcp-binary,sa32-sampled-k2,sa32-sampled-k4,sa32-sampled-k8,fm-huff,fm-balanced,fm-epr"
exact_methods_full="sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,sa64-binary,sa64-lcp-binary,sa32-sampled-k2,sa32-sampled-k4,sa32-sampled-k8,fm-huff,fm-balanced,fm-epr"
right_maximal_methods="right-maximal-baseline,right-maximal-lcp,right-maximal-child,right-maximal-suffix-link,right-maximal-suffix-link-binary,right-maximal-suffix-link-sapling,right-maximal-full,right-maximal-sampled-k4,right-maximal-sampled-k8"

run_exact_command() {
    local profile=$1
    local scenarios=$2
    local methods=$3
    local output=$4
    "${query_pin_prefix[@]}" "${sufkit_bin}" bench \
        --profile "${profile}" \
        --scenarios "${scenarios}" \
        --methods "${methods}" \
        --pattern-lengths 20,50,100,200,500 \
        --locate-limits 1,10,1000,all \
        --fm-query-modes scalar,batch \
        --fm-batch-widths 1,4,8,16,32 \
        --fm-batch-widths-for fm-balanced:16,32 \
        --fm-batch-widths-for fm-epr:16,32 \
        --seed "${seed}" \
        --output-dir "${output}"
}

run_exact() {
    local profile=$1
    local scenarios=$2
    local methods=$3
    local output="${run_dir}/${profile}/exact"
    run_published_output exact "${output}" run_exact_command \
        "${profile}" "${scenarios}" "${methods}"
}

run_representative_exact_command() {
    local profile=$1
    local scenarios=$2
    local methods=$3
    local pattern_lengths=$4
    local locate_limits=$5
    local build_repetitions=$6
    local query_repetitions=$7
    local output=$8
    local -a sa_thread_args=()
    if [[ "${methods}" == *caps32* ]]; then sa_thread_args=(--sa-threads 64); fi
    "${query_pin_prefix[@]}" "${sufkit_bin}" bench \
        --profile "${profile}" \
        --scenarios "${scenarios}" \
        --methods "${methods}" \
        --pattern-lengths "${pattern_lengths}" \
        --locate-limits "${locate_limits}" \
        --fm-query-modes scalar \
        --fm-batch-widths 16 \
        "${sa_thread_args[@]}" \
        --seed "${seed}" \
        --build-repetitions "${build_repetitions}" \
        --query-repetitions "${query_repetitions}" \
        --warmups 1 \
        --output-dir "${output}"
}

run_representative_exact() {
    local profile=$1
    local output="${run_dir}/${profile}/exact"
    run_published_output exact "${output}" run_representative_exact_command \
        "${profile}" mixed "$2" "$3" "$4" "$5" "$6"
}

run_right_maximal_command() {
    local profile=$1
    local scenarios=$2
    local output=$3
    "${query_pin_prefix[@]}" "${sufkit_bin}" bench \
        --workload right-maximal \
        --profile "${profile}" \
        --scenarios "${scenarios}" \
        --methods "${right_maximal_methods}" \
        --min-lengths 20,50,100 \
        --seed "${seed}" \
        --output-dir "${output}"
}

run_right_maximal() {
    local profile=$1
    local scenarios=$2
    local output="${run_dir}/${profile}/right-maximal"
    run_published_output right-maximal "${output}" run_right_maximal_command \
        "${profile}" "${scenarios}"
}

run_representative_right_maximal_command() {
    local profile=$1
    local methods=$2
    local min_lengths=$3
    local build_repetitions=$4
    local query_repetitions=$5
    local output=$6
    "${query_pin_prefix[@]}" "${sufkit_bin}" bench \
        --workload right-maximal \
        --profile "${profile}" \
        --scenarios mixed \
        --methods "${methods}" \
        --min-lengths "${min_lengths}" \
        --seed "${seed}" \
        --build-repetitions "${build_repetitions}" \
        --query-repetitions "${query_repetitions}" \
        --warmups 1 \
        --output-dir "${output}"
}

run_representative_right_maximal() {
    local profile=$1
    local methods=$2
    local min_lengths=$3
    local build_repetitions=$4
    local query_repetitions=$5
    local output="${run_dir}/${profile}/right-maximal"
    run_published_output right-maximal "${output}" run_representative_right_maximal_command \
        "${profile}" "${methods}" "${min_lengths}" "${build_repetitions}" \
        "${query_repetitions}"
}

run_sa_build_command() {
    local profile=$1
    local label=$2
    local methods=$3
    local threads=$4
    local sampling_rates=$5
    local acceleration=$6
    local repetitions=$7
    local output=$8
    local -a phase_pin=("${query_pin_prefix[@]}")
    local -a repetition_args=()
    if [[ -n "${repetitions}" ]]; then repetition_args=(--repetitions "${repetitions}"); fi
    if [[ "${methods}" == *caps* ]]; then phase_pin=("${parallel_pin_prefix[@]}"); fi
    "${phase_pin[@]}" "${sa_build_bin}" \
        --profile "${profile}" \
        --methods "${methods}" \
        --threads "${threads}" \
        --sampling-rates "${sampling_rates}" \
        --acceleration "${acceleration}" \
        --seed "${seed}" \
        "${repetition_args[@]}" \
        --output-dir "${output}"
}

run_sa_build_matrix() {
    local profile=$1
    local label=$2
    local methods=$3
    local threads=$4
    local sampling_rates=$5
    local acceleration=$6
    local output="${run_dir}/${profile}/sa-build-${label}"
    run_published_output sa-build "${output}" run_sa_build_command \
        "${profile}" "${label}" "${methods}" "${threads}" \
        "${sampling_rates}" "${acceleration}" "${7:-}"
}

run_headline_exact_build_command() {
    local output=$1
    local export_directory="${output}/dataset"
    local reference_export="${export_directory}/reference.fa"
    local query_export="${export_directory}/queries.fa"
    mkdir -p "${export_directory}"
    "${parallel_pin_prefix[@]}" "${sufkit_bin}" bench \
        --profile full \
        --scenarios mixed \
        --methods sa32-binary,caps32,fm-huff \
        --sa-threads 64 \
        --pattern-lengths 100 \
        --locate-limits 1 \
        --fm-query-modes scalar \
        --seed "${seed}" \
        --build-repetitions 3 \
        --query-repetitions 1 \
        --warmups 0 \
        --export-reference "${reference_export}" \
        --export-queries "${query_export}" \
        --output-dir "${output}"
    if [[ ! -s "${reference_export}" || ! -s "${query_export}" ]]; then
        echo "headline export did not produce both non-empty dataset files" >&2
        return 2
    fi
    (cd "${export_directory}" && sha256sum reference.fa queries.fa >dataset.sha256)
}

publish_headline_dataset() {
    local source="${run_dir}/headline/exact-build/dataset"
    if path_exists "${headline_dataset_dir}"; then
        if validate_published_output headline-dataset "${headline_dataset_dir}" &&
           cmp -s "${source}/dataset.sha256" "${headline_dataset_dir}/dataset.sha256"; then
            echo "recovering already published headline dataset: ${headline_dataset_dir}" >&2
            return 0
        fi
        record_stage_attempt_detail "existing_invalid_headline_dataset=${headline_dataset_dir}"
        echo "refusing to overwrite an existing invalid headline dataset" >&2
        return 2
    fi
    local partial
    partial=$(create_unique_partial_directory "${headline_dataset_dir}")
    record_stage_attempt_detail "headline_dataset_partial=${partial}"
    cp --reflink=auto -- "${source}/reference.fa" "${partial}/reference.fa"
    cp --reflink=auto -- "${source}/queries.fa" "${partial}/queries.fa"
    cp -- "${source}/dataset.sha256" "${partial}/dataset.sha256"
    validate_producer_payload headline-dataset "${partial}"
    write_producer_marker headline-dataset "${partial}"
    if ! publish_directory_noclobber "${partial}" "${headline_dataset_dir}"; then
        record_stage_attempt_detail "headline_dataset_publication_race=${partial}"
        return 2
    fi
    validate_published_output headline-dataset "${headline_dataset_dir}"
}

publish_headline_manifest() {
    local manifest="${run_dir}/manifest/headline-dataset.sha256"
    local expected="${run_dir}/manifest/.headline-dataset.sha256.${SUFKIT_STAGE_ATTEMPT_ID:-manual}.partial"
    sha256sum "${headline_dataset_dir}/reference.fa" "${headline_dataset_dir}/queries.fa" >"${expected}"
    if path_exists "${manifest}"; then
        if cmp -s "${expected}" "${manifest}" && sha256sum --check "${manifest}" >/dev/null; then
            mv -- "${expected}" \
                "${run_dir}/state/headline-dataset-manifest.${SUFKIT_STAGE_ATTEMPT_ID:-manual}.verified"
            return 0
        fi
        record_stage_attempt_detail "existing_invalid_headline_manifest=${manifest}"
        echo "refusing to overwrite an invalid headline SHA-256 manifest" >&2
        return 2
    fi
    if ! publish_file_noclobber "${expected}" "${manifest}"; then
        record_stage_attempt_detail "headline_manifest_publication_race=${expected}"
        return 2
    fi
    sha256sum --check "${manifest}" >/dev/null
}

validate_headline_exact_build_stage() {
    validate_published_output headline-exact "${run_dir}/headline/exact-build" &&
    validate_published_output headline-dataset "${headline_dataset_dir}" &&
    cmp -s "${run_dir}/headline/exact-build/dataset/dataset.sha256" \
        "${headline_dataset_dir}/dataset.sha256" &&
    [[ -s "${run_dir}/manifest/headline-dataset.sha256" ]] &&
    sha256sum --check "${run_dir}/manifest/headline-dataset.sha256" >/dev/null
}

run_headline_exact_build() {
    run_published_output headline-exact "${run_dir}/headline/exact-build" \
        run_headline_exact_build_command
    chmod a-w "${run_dir}/headline/exact-build/dataset/reference.fa" \
        "${run_dir}/headline/exact-build/dataset/queries.fa"
    publish_headline_dataset
    chmod a-w "${headline_dataset_dir}/reference.fa" \
        "${headline_dataset_dir}/queries.fa" "${headline_dataset_dir}"
    publish_headline_manifest
}

run_headline_exact_query_command() {
    local output=$1
    "${query_pin_prefix[@]}" "${sufkit_bin}" bench \
        --profile full \
        --scenarios mixed \
        --methods sa32-binary,fm-huff \
        --pattern-lengths 100 \
        --locate-limits 1 \
        --fm-query-modes scalar \
        --seed "${seed}" \
        --build-repetitions 1 \
        --query-repetitions 5 \
        --warmups 1 \
        --output-dir "${output}"
}

run_headline_exact_query() {
    run_published_output exact "${run_dir}/headline/exact-query" \
        run_headline_exact_query_command
}

run_headline_right_maximal_command() {
    local output=$1
    "${query_pin_prefix[@]}" "${sufkit_bin}" bench \
        --workload right-maximal \
        --profile full \
        --reference "${headline_dataset_dir}/reference.fa" \
        --methods right-maximal-baseline,right-maximal-suffix-link \
        --min-lengths 50 \
        --seed "${seed}" \
        --build-repetitions 3 \
        --query-repetitions 5 \
        --warmups 1 \
        --output-dir "${output}"
}

run_headline_right_maximal() {
    run_published_output right-maximal "${run_dir}/headline/right-maximal" \
        run_headline_right_maximal_command
}

run_standard_sa_build_stages() {
    local profile=$1
    run_stage "${profile}_sa_build_none_k1" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-none-k1" \
        run_sa_build_matrix "${profile}" none-k1 div32,div64 1 1 none
    run_stage "${profile}_sa_build_default_k1" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-default-k1" \
        run_sa_build_matrix "${profile}" default-k1 div32,div64 1 1 default 1
    run_stage "${profile}_sa_build_sampled_k2_k4_k8" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-sampled-k2-k4-k8" \
        run_sa_build_matrix "${profile}" sampled-k2-k4-k8 div32 1 2,4,8 default
    run_stage "${profile}_sa_build_full_k1" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-full-k1" \
        run_sa_build_matrix "${profile}" full-k1 div32 1 1 full
    run_stage "${profile}_sa_build_sapling_k1" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-sapling-k1" \
        run_sa_build_matrix "${profile}" sapling-k1 div32 1 1 sapling
    run_stage "${profile}_sa_build_caps_default_k1" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-caps-default-k1" \
        run_sa_build_matrix "${profile}" caps-default-k1 caps32,caps64 1,8,32,64 1 default
}

run_full_sa_build_stages() {
    local profile=full
    run_stage full_sa_build_none_k1 validate_sa_build_output \
        "${run_dir}/full/sa-build-none-k1" \
        run_sa_build_matrix "${profile}" none-k1 div32,div64 1 1 none
    run_stage full_sa_build_default_k1 validate_sa_build_output \
        "${run_dir}/full/sa-build-default-k1" \
        run_sa_build_matrix "${profile}" default-k1 div32,div64 1 1 default
    run_stage full_sa_build_sampled_k4_k8 validate_sa_build_output \
        "${run_dir}/full/sa-build-sampled-k4-k8" \
        run_sa_build_matrix "${profile}" sampled-k4-k8 div32 1 4,8 default
    run_stage full_sa_build_caps_default_k1_t64 validate_sa_build_output \
        "${run_dir}/full/sa-build-caps-default-k1-t64" \
        run_sa_build_matrix "${profile}" caps-default-k1-t64 caps32,caps64 64 1 default
}

run_representative_sa_build_stages() {
    local profile=$1
    run_stage "${profile}_sa_build_default_k1" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-default-k1" \
        run_sa_build_matrix "${profile}" default-k1 div32,div64 1 1 default
    run_stage "${profile}_sa_build_sampled_k4" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-sampled-k4" \
        run_sa_build_matrix "${profile}" sampled-k4 div32 1 4 default 1
    run_stage "${profile}_sa_build_caps_default_k1_t64" validate_sa_build_output \
        "${run_dir}/${profile}/sa-build-caps-default-k1-t64" \
        run_sa_build_matrix "${profile}" caps-default-k1-t64 caps32 64 1 default 1
}

if [[ "${SUFKIT_BENCH_RUNNER_SOURCE_ONLY:-0}" == 1 ]]; then
    return 0 2>/dev/null || exit 0
fi

acquire_runner_lock
initialize_source_identity
archive_top_level_completion_markers
initialize_affinity
run_stage environment validate_environment_stage unused capture_environment
run_stage configure validate_configure_stage unused configure_release
run_stage build validate_build_stage unused build_release
run_stage binary_manifest validate_binary_manifest_stage unused capture_binary_manifest
run_stage ctest validate_ctest_stage "${run_dir}/logs/ctest.log" test_release

run_stage smoke_exact validate_exact_output "${run_dir}/smoke/exact" \
    run_exact smoke mixed "${exact_methods_all}"
run_standard_sa_build_stages smoke
run_stage smoke_right_maximal validate_right_maximal_output \
    "${run_dir}/smoke/right-maximal" run_right_maximal smoke mixed
run_stage smoke_audit validate_profile_audit smoke audit_profile smoke

run_stage quick_exact validate_exact_output "${run_dir}/quick/exact" \
    run_exact quick mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig \
    "${exact_methods_all}"
run_standard_sa_build_stages quick
run_stage quick_right_maximal validate_right_maximal_output \
    "${run_dir}/quick/right-maximal" run_right_maximal quick \
    mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig
run_stage quick_audit validate_profile_audit quick audit_profile quick

if [[ "${suite_variant}" == representative ]]; then
    run_stage standard_exact validate_exact_output "${run_dir}/standard/exact" \
        run_representative_exact standard \
        "sa32-binary,sa64-binary,sa32-sampled-k4,fm-huff" 50,100,200 1,1000 1 3
    run_representative_sa_build_stages standard
    run_stage standard_right_maximal validate_right_maximal_output \
        "${run_dir}/standard/right-maximal" \
        run_representative_right_maximal standard \
        "right-maximal-baseline,right-maximal-suffix-link,right-maximal-full" 50,100 1 3
    run_stage standard_audit validate_profile_audit standard audit_profile standard

    run_stage full_exact validate_exact_output "${run_dir}/full/exact" \
        run_representative_exact full \
        "sa32-binary,caps32,fm-huff" 100 1 1 3
    run_representative_sa_build_stages full
    run_stage full_right_maximal validate_right_maximal_output \
        "${run_dir}/full/right-maximal" \
        run_representative_right_maximal full \
        "right-maximal-baseline,right-maximal-suffix-link" 50,100 1 3
    run_stage full_audit validate_profile_audit full audit_profile full
else
    run_stage standard_exact validate_exact_output "${run_dir}/standard/exact" \
        run_exact standard mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig \
        "${exact_methods_indexed}"
    run_standard_sa_build_stages standard
    run_stage standard_right_maximal validate_right_maximal_output \
        "${run_dir}/standard/right-maximal" run_right_maximal standard \
        mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig
    run_stage standard_audit validate_profile_audit standard audit_profile standard

    run_stage full_exact validate_exact_output "${run_dir}/full/exact" \
        run_exact full mixed,repeat-rich,many-contig "${exact_methods_full}"
    run_full_sa_build_stages
    run_stage full_right_maximal validate_right_maximal_output \
        "${run_dir}/full/right-maximal" run_right_maximal full mixed,repeat-rich,many-contig
    run_stage full_audit validate_profile_audit full audit_profile full
fi

run_stage headline_exact_build validate_headline_exact_build_stage unused \
    run_headline_exact_build
run_stage headline_exact_query validate_exact_output \
    "${run_dir}/headline/exact-query" run_headline_exact_query
run_stage headline_right_maximal validate_right_maximal_output \
    "${run_dir}/headline/right-maximal" run_headline_right_maximal
run_stage headline_audit validate_profile_audit headline audit_profile headline

write_all_complete_marker
run_stage package_results validate_complete_package "${package_dir}" package_results
validate_complete_package "${package_dir}"
package_digest=$(package_tree_digest "${package_dir}")
package_complete_partial="${run_dir}/state/.PACKAGE_COMPLETE.${SUFKIT_STAGE_ATTEMPT_ID:-runner}.$$.partial"
{
    printf 'completed_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'source_identity=%s\n' "${current_source_identity}"
    printf 'package_dir=%s\n' "${package_dir}"
    printf 'package_digest=%s\n' "${package_digest}"
    printf 'validator=strict-five-profile-plus-package-semantics-and-file-tree\n'
} >"${package_complete_partial}"
publish_file_noclobber "${package_complete_partial}" "${run_dir}/state/PACKAGE_COMPLETE"
echo "benchmark suite completed: ${run_dir}"
echo "repository-sized package: ${package_dir}"
