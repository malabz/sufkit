#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import shlex
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "benchmarks" / "run_server_suite.sh"


class ServerRunnerRecoveryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.run = self.root / "runs" / "fixture-run"
        self.dataset = self.root / "datasets"
        self.package = self.root / "packages" / "fixture-run"
        self.build = self.root / "build"

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def shell(self, body: str, *, expected: int = 0) -> subprocess.CompletedProcess[str]:
        script = textwrap.dedent(
            f"""
            export SUFKIT_BENCH_RUNNER_SOURCE_ONLY=1
            export SUFKIT_BENCH_TEST_SOURCE_IDENTITY={'a' * 64}
            export SUFKIT_BENCH_DATASET_DIR={shlex.quote(str(self.dataset))}
            export SUFKIT_BENCH_PACKAGE_DIR={shlex.quote(str(self.package))}
            export SUFKIT_BENCH_BUILD_DIR={shlex.quote(str(self.build))}
            set -- {shlex.quote(str(ROOT))} {shlex.quote(str(self.run))}
            source {shlex.quote(str(RUNNER))}
            printf 'fixture binary manifest\n' >"${{binary_checksum_manifest}}"

            write_table() {{
                local path=$1
                printf 'column_a\tcolumn_b\nvalue_a\tvalue_b\n' >"${{path}}"
            }}

            write_exact_payload() {{
                local output=$1
                mkdir -p "${{output}}"
                write_table "${{output}}/run_metadata.tsv"
                write_table "${{output}}/build_results.tsv"
                write_table "${{output}}/query_results.tsv"
                write_table "${{output}}/raw_repetitions.tsv"
            }}

            {body}
            """
        )
        result = subprocess.run(
            ["bash", "-c", script], text=True, capture_output=True, check=False
        )
        if result.returncode != expected:
            self.fail(
                f"shell fixture returned {result.returncode}, expected {expected}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
            )
        return result

    def test_failed_partial_is_retained_and_stage_retry_is_atomic(self) -> None:
        self.shell(
            r"""
            target="${run_dir}/smoke/exact"

            fail_producer() {
                local output=$1
                mkdir -p "${output}"
                printf 'diagnostic\n' >"${output}/failure.txt"
                return 7
            }
            fail_stage() {
                run_published_output exact "${target}" fail_producer
            }
            if run_stage fixture_exact validate_exact_output "${target}" fail_stage; then
                echo 'failing producer unexpectedly succeeded' >&2
                exit 30
            fi
            [[ ! -e "${target}" ]]
            partials=("${run_dir}/smoke"/.exact.partial.*)
            [[ ${#partials[@]} -eq 1 && -s "${partials[0]}/failure.txt" ]]
            grep -Fq "partial=${partials[0]}" "${run_dir}/state/fixture_exact.failed"

            succeed_stage() {
                run_published_output exact "${target}" write_exact_payload
            }
            run_stage fixture_exact validate_exact_output "${target}" succeed_stage
            validate_exact_output "${target}"
            [[ ! -e "${run_dir}/state/fixture_exact.failed" ]]
            resolved=("${run_dir}/state"/fixture_exact.failed.retry.*)
            [[ ${#resolved[@]} -eq 1 ]]
            grep -Fq 'validator=validate_exact_output' \
                "${run_dir}/state/fixture_exact.complete"

            should_not_run() {
                printf 'called\n' >"${run_dir}/unexpected-reexecution"
                return 40
            }
            run_stage fixture_exact validate_exact_output "${target}" should_not_run
            [[ ! -e "${run_dir}/unexpected-reexecution" ]]
            """
        )

    def test_runner_lock_is_nonblocking_and_no_clobber_keeps_partial(self) -> None:
        self.shell(
            r"""
            ready="${run_dir}/state/lock-holder-ready"
            release="${run_dir}/state/lock-holder-release"
            (
                acquire_runner_lock
                : >"${ready}"
                while [[ ! -e "${release}" ]]; do sleep 0.01; done
            ) &
            holder=$!
            for unused in {1..200}; do
                if [[ -e "${ready}" ]]; then break; fi
                sleep 0.01
            done
            [[ -e "${ready}" ]]
            if (acquire_runner_lock) 2>/dev/null; then
                echo 'second runner unexpectedly acquired the lock' >&2
                exit 60
            fi
            : >"${release}"
            wait "${holder}"

            partial="${run_dir}/smoke/.exact.partial.race"
            target="${run_dir}/smoke/exact-race"
            mkdir -p "${partial}" "${target}"
            if publish_directory_noclobber "${partial}" "${target}"; then
                echo 'no-clobber publish unexpectedly replaced its target' >&2
                exit 61
            fi
            [[ -d "${partial}" ]]
            [[ ! -e "${target}/$(basename "${partial}")" ]]
            """
        )

    def test_binary_manifest_and_ctest_are_bound_to_source_and_binary_bytes(self) -> None:
        self.shell(
            r"""
            mkdir -p "${build_dir}"
            printf 'source_identity=%s\n' "${current_source_identity}" \
                >"${build_identity_file}"
            cp /bin/true "${sufkit_bin}"
            cp /bin/true "${sa_build_bin}"
            capture_binary_manifest
            validate_binary_manifest_stage unused

            printf '100%% tests passed, 0 tests failed out of 8\n' \
                >"${run_dir}/logs/fixture-ctest.log"
            cp "${binary_checksum_manifest}" "${ctest_binary_checksum_manifest}"
            validate_ctest_stage "${run_dir}/logs/fixture-ctest.log"

            cp /bin/false "${sufkit_bin}"
            if validate_binary_manifest_stage unused; then
                echo 'binary byte drift unexpectedly passed the manifest validator' >&2
                exit 62
            fi
            capture_binary_manifest
            if validate_ctest_stage "${run_dir}/logs/fixture-ctest.log"; then
                echo 'old CTest evidence unexpectedly accepted a new binary digest' >&2
                exit 63
            fi

            printf 'source_identity=%064d\n' 0 >"${build_identity_file}"
            if validate_build_stage unused; then
                echo 'mismatched build-directory identity unexpectedly passed' >&2
                exit 64
            fi
            """
        )

    def test_published_tsv_hash_detects_header_preserving_tamper(self) -> None:
        self.shell(
            r"""
            target="${run_dir}/smoke/exact-hashed"
            run_published_output exact "${target}" write_exact_payload
            validate_exact_output "${target}"
            printf 'changed_a\tchanged_b\n' >>"${target}/raw_repetitions.tsv"
            validate_producer_payload exact "${target}"
            if validate_exact_output "${target}"; then
                echo 'header-preserving TSV tamper unexpectedly passed SHA validation' >&2
                exit 65
            fi
            """
        )

    def test_published_output_marker_binds_source_and_binary_manifest(self) -> None:
        self.shell(
            r"""
            target="${run_dir}/smoke/exact-runtime-identity"
            run_published_output exact "${target}" write_exact_payload
            marker="${target}/${producer_marker}"
            grep -Fxq "source_identity=${current_source_identity}" "${marker}"
            expected_binary_digest=$(sha256sum "${binary_checksum_manifest}" | awk '{ print $1 }')
            grep -Fxq "binary_manifest_sha256=${expected_binary_digest}" "${marker}"
            cp "${marker}" "${run_dir}/marker.original"

            grep -v '^source_identity=' "${run_dir}/marker.original" >"${marker}"
            if validate_exact_output "${target}"; then
                echo 'legacy producer marker without source identity unexpectedly passed' >&2
                exit 68
            fi
            cp "${run_dir}/marker.original" "${marker}"

            current_source_identity=$(printf '%064d' 0)
            if validate_exact_output "${target}"; then
                echo 'producer output unexpectedly crossed source identities' >&2
                exit 69
            fi
            current_source_identity=${SUFKIT_BENCH_TEST_SOURCE_IDENTITY}
            validate_exact_output "${target}"

            printf 'binary drift\n' >>"${binary_checksum_manifest}"
            if validate_exact_output "${target}"; then
                echo 'producer output unexpectedly accepted binary-manifest drift' >&2
                exit 70
            fi
            """
        )

    def test_complete_marker_is_revalidated_before_skip(self) -> None:
        self.shell(
            r"""
            target="${run_dir}/quick/exact"
            publish_stage() {
                run_published_output exact "${target}" write_exact_payload
            }
            run_stage fixture_revalidate validate_exact_output "${target}" publish_stage
            mv "${target}/raw_repetitions.tsv" \
                "${target}/raw_repetitions.tsv.corrupt-for-test"
            if run_stage fixture_revalidate validate_exact_output "${target}" publish_stage; then
                echo 'corrupt published output unexpectedly passed revalidation' >&2
                exit 41
            fi
            invalid=("${run_dir}/state"/fixture_revalidate.complete.invalid.*)
            [[ ${#invalid[@]} -eq 1 ]]
            grep -Fq 'existing_invalid_output=' \
                "${run_dir}/state/fixture_revalidate.failed"
            [[ "$(tail -n 1 "${run_dir}/manifest/stages.tsv" | cut -f5)" == 2 ]]
            """
        )

    def test_headline_export_can_finish_after_result_publish_and_recover(self) -> None:
        self.shell(
            r"""
            headline_payload() {
                local output=$1
                write_exact_payload "${output}"
                mkdir -p "${output}/dataset"
                printf '>ref\nACGTACGT\n' >"${output}/dataset/reference.fa"
                printf '>query\nACGT\n' >"${output}/dataset/queries.fa"
                (cd "${output}/dataset" && \
                    sha256sum reference.fa queries.fa >dataset.sha256)
            }

            run_published_output headline-exact \
                "${run_dir}/headline/exact-build" headline_payload
            [[ ! -e "${headline_dataset_dir}" ]]
            publish_headline_dataset
            publish_headline_manifest
            validate_headline_exact_build_stage

            # Repeating the post-publish half must validate and reuse the exact
            # exported bytes instead of replacing either final directory.
            publish_headline_dataset
            publish_headline_manifest
            validate_headline_exact_build_stage
            sha256sum --check "${run_dir}/manifest/headline-dataset.sha256" >/dev/null

            printf 'T' >>"${headline_dataset_dir}/reference.fa"
            if validate_headline_exact_build_stage; then
                echo 'tampered headline export unexpectedly passed SHA validation' >&2
                exit 42
            fi
            """
        )

    def test_headline_stage_rejects_two_self_consistent_but_different_datasets(self) -> None:
        self.shell(
            r"""
            headline_payload() {
                local output=$1
                write_exact_payload "${output}"
                mkdir -p "${output}/dataset"
                printf '>ref\nACGTACGT\n' >"${output}/dataset/reference.fa"
                printf '>query\nACGT\n' >"${output}/dataset/queries.fa"
                (cd "${output}/dataset" && \
                    sha256sum reference.fa queries.fa >dataset.sha256)
            }
            run_published_output headline-exact \
                "${run_dir}/headline/exact-build" headline_payload
            publish_headline_dataset
            publish_headline_manifest
            validate_headline_exact_build_stage

            printf '>ref\nTTTTACGT\n' \
                >"${run_dir}/headline/exact-build/dataset/reference.fa"
            (cd "${run_dir}/headline/exact-build/dataset" && \
                sha256sum reference.fa queries.fa >dataset.sha256)
            validate_producer_payload headline-exact \
                "${run_dir}/headline/exact-build"
            validate_published_output headline-dataset "${headline_dataset_dir}"
            sha256sum --check "${run_dir}/manifest/headline-dataset.sha256" >/dev/null
            if validate_headline_exact_build_stage; then
                echo 'split-brain headline datasets unexpectedly passed' >&2
                exit 66
            fi
            """
        )

    def test_top_markers_are_archived_and_package_manifest_rebinds_run_inputs(self) -> None:
        self.shell(
            r"""
            printf 'old-all\n' >"${run_dir}/state/ALL_COMPLETE"
            printf 'old-package\n' >"${run_dir}/state/PACKAGE_COMPLETE"
            archive_top_level_completion_markers
            [[ ! -e "${run_dir}/state/ALL_COMPLETE" ]]
            [[ ! -e "${run_dir}/state/PACKAGE_COMPLETE" ]]
            old_all=("${run_dir}/state"/ALL_COMPLETE.revalidate.*)
            old_package=("${run_dir}/state"/PACKAGE_COMPLETE.revalidate.*)
            [[ ${#old_all[@]} -eq 1 && ${#old_package[@]} -eq 1 ]]

            package_fixture="${run_dir}/package-fixture"
            mkdir -p "${package_fixture}"
            printf 'path\tbytes\tsha256\n' >"${package_fixture}/manifest.tsv"
            append_manifest_row() {
                local path=$1
                local relative=${path#"${run_dir}/"}
                printf '%s\t%s\t%s\n' "${relative}" "$(stat -c %s "${path}")" \
                    "$(sha256sum "${path}" | awk '{ print $1 }')" \
                    >>"${package_fixture}/manifest.tsv"
            }
            for profile in smoke quick standard full headline; do
                mkdir -p "${run_dir}/${profile}/fixture"
                printf '%s-input\n' "${profile}" \
                    >"${run_dir}/${profile}/fixture/input.txt"
                append_manifest_row "${run_dir}/${profile}/fixture/input.txt"
            done
            append_manifest_row "${binary_checksum_manifest}"
            write_all_complete_marker
            append_manifest_row "${run_dir}/state/ALL_COMPLETE"
            complete_digest=$(sha256sum "${run_dir}/state/ALL_COMPLETE" | awk '{ print $1 }')
            mkdir -p "${run_dir}/smoke/.exact.partial.retained"
            printf 'ignored\n' >"${run_dir}/smoke/.exact.partial.retained/evidence.txt"

            # Simulate restart after the package directory was published but
            # before PACKAGE_COMPLETE was written.  ALL_COMPLETE is archived
            # and deterministically reconstructed byte-for-byte, so the
            # package input manifest must remain valid.
            [[ ! -e "${run_dir}/state/PACKAGE_COMPLETE" ]]
            archive_top_level_completion_markers
            write_all_complete_marker
            [[ "$(sha256sum "${run_dir}/state/ALL_COMPLETE" | awk '{ print $1 }')" == \
               "${complete_digest}" ]]
            validate_package_input_manifest "${package_fixture}"

            printf 'tamper\n' >>"${run_dir}/quick/fixture/input.txt"
            if validate_package_input_manifest "${package_fixture}"; then
                echo 'stale package manifest unexpectedly accepted changed run input' >&2
                exit 67
            fi
            """
        )

    def test_payload_validators_require_exact_sa_and_right_file_sets(self) -> None:
        self.shell(
            r"""
            exact="${run_dir}/payloads/exact"
            write_exact_payload "${exact}"
            validate_producer_payload exact "${exact}"
            mv "${exact}/query_results.tsv" "${exact}/query_results.tsv.missing"
            if validate_producer_payload exact "${exact}"; then exit 50; fi

            sa="${run_dir}/payloads/sa"
            mkdir -p "${sa}"
            write_table "${sa}/run_metadata.tsv"
            write_table "${sa}/build_results.tsv"
            write_table "${sa}/raw_repetitions.tsv"
            validate_producer_payload sa-build "${sa}"
            mv "${sa}/build_results.tsv" "${sa}/build_results.tsv.missing"
            if validate_producer_payload sa-build "${sa}"; then exit 51; fi

            right="${run_dir}/payloads/right"
            mkdir -p "${right}"
            for file in run_metadata.tsv build_results.tsv query_results.tsv \
                    raw_repetitions.tsv correctness_summary.tsv; do
                write_table "${right}/${file}"
            done
            validate_producer_payload right-maximal "${right}"
            mv "${right}/correctness_summary.tsv" \
                "${right}/correctness_summary.tsv.missing"
            if validate_producer_payload right-maximal "${right}"; then exit 52; fi
            """
        )

    def test_runner_declares_independent_sa_substages_and_strict_package_recovery(self) -> None:
        runner = RUNNER.read_text(encoding="utf-8")
        for profile in ("smoke", "quick", "standard"):
            self.assertIn(f'run_standard_sa_build_stages {profile}', runner)
        self.assertIn("run_full_sa_build_stages", runner)
        self.assertNotIn("run_stage standard_sa_build run_sa_build_standard_matrix", runner)
        self.assertNotIn("run_stage full_sa_build run_sa_build_full_matrix", runner)
        self.assertIn('for profile in smoke quick standard full headline; do', runner)
        self.assertIn('audit_profile "${profile}" >/dev/null || return 1', runner)
        self.assertIn("module.validate_package(package)", runner)
        self.assertIn('validate_complete_package "${package_dir}"', runner)
        self.assertIn('validate_package_input_manifest "${directory}"', runner)
        self.assertIn('printf \'source_identity=%s\\n\'', runner)
        self.assertIn('printf \'package_digest=%s\\n\'', runner)
        self.assertIn("--fm-batch-widths-for fm-balanced:16,32", runner)
        self.assertIn("--fm-batch-widths-for fm-epr:16,32", runner)

    def test_runner_has_explicit_representative_standard_full_variant(self) -> None:
        runner = RUNNER.read_text(encoding="utf-8")
        self.assertIn("SUFKIT_BENCH_SUITE_VARIANT", runner)
        self.assertIn("full-matrix|representative", runner)
        self.assertIn("run_representative_sa_build_stages standard", runner)
        self.assertIn("run_representative_sa_build_stages full", runner)
        self.assertIn("run_representative_exact standard", runner)
        self.assertIn("run_representative_exact full", runner)
        self.assertIn("run_representative_right_maximal standard", runner)
        self.assertIn("run_representative_right_maximal full", runner)


if __name__ == "__main__":
    unittest.main()
