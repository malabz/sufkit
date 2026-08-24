#!/usr/bin/env python3

from __future__ import annotations

import csv
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
TOOL = REPOSITORY / "benchmarks" / "update_benchmark_readme.py"
START = "<!-- SUFKIT_HEADLINE_START -->"
END = "<!-- SUFKIT_HEADLINE_END -->"
BASE_CORRECTNESS = [
    ("exact", "unexpected_status"),
    ("sa-build", "unexpected_status"),
    ("right-maximal", "unexpected_status"),
    ("right-maximal", "naive_oracle"),
    ("exact", "repetition_result_stability"),
    ("right-maximal", "repetition_result_stability"),
    ("sa-build", "repetition_result_stability"),
    ("exact", "cross_method_result_equivalence"),
    ("right-maximal", "cross_method_result_equivalence"),
    ("sa-build", "cross_method_exact_checksum"),
    ("sa-build", "cross_method_right_maximal_checksum"),
]
HEADLINE_AUDIT_CHECKS = [
    "headline_build_same_worker_scope",
    "headline_build_repetitions",
    "headline_build_threads",
    "headline_build_provenance",
    "headline_build_dataset_equivalence",
    "headline_build_total_bases_equivalence",
    "headline_build_dataset_fingerprint_equivalence",
    "headline_sa_canary_checksum_equivalence",
    "headline_exact_build_query_identity",
    "headline_exported_reference_identity",
    "headline_fixed_workload_contract",
    "headline_raw_completeness",
]
STRICT_CORRECTNESS = BASE_CORRECTNESS + [
    ("profile-audit", f"headline:{check}") for check in HEADLINE_AUDIT_CHECKS
]


def write_tsv(path: Path, fieldnames: list[str], rows: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def read_tsv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        assert reader.fieldnames is not None
        return list(reader.fieldnames), list(reader)


class UpdateBenchmarkReadmeTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="sufkit-headline-test-")
        self.root = Path(self.temporary.name)
        self.benchmarks = self.root / "benchmarks"
        self.readme = self.benchmarks / "README.md"
        self.package = self.benchmarks / "results" / "server-test"
        self.readme.parent.mkdir(parents=True)
        self.readme.write_text(
            "# Benchmarks\n\n"
            f"{START}\nplaceholder\n{END}\n\n"
            "## Generic instructions\n\nKeep this text unchanged.\n",
            encoding="utf-8",
        )
        self.make_complete_package()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def make_complete_package(self) -> None:
        (self.package / "headline").mkdir(parents=True)
        (self.package / "figures").mkdir(parents=True)
        (self.package / "README.md").write_text(
            "# Package\n\n**Package status: complete.**\n", encoding="utf-8"
        )
        (self.package / "headline" / "README.md").write_text(
            "# Generated headline\n\n**Headline status: ok.**\n", encoding="utf-8"
        )
        (self.package / "figures" / "headline-performance.svg").write_text(
            '<svg xmlns="http://www.w3.org/2000/svg"/>\n', encoding="utf-8"
        )

        environment = {
            "server_label": "server-test",
            "source_revision": "205023aa8fbaefdbfd7e20f6a84ebbe7a3fad2dd",
            "run_complete": "true",
            "compiler": "GCC",
            "compiler_version": "13.3.0",
            "build_type": "Release",
            "os": "Linux",
            "architecture": "x86_64",
            "cpu_model": "Test CPU",
            "logical_cpus": "64",
            "seed": "20260822",
        }
        write_tsv(
            self.package / "environment.tsv",
            ["key", "value", "source"],
            [
                {"key": key, "value": value, "source": "fixture"}
                for key, value in environment.items()
            ],
        )
        write_tsv(
            self.package / "correctness-summary.tsv",
            ["workload", "check", "groups_checked", "mismatches", "status", "details"],
            [
                {
                    "workload": workload,
                    "check": check,
                    "groups_checked": "1",
                    "mismatches": "0",
                    "status": "ok",
                    "details": "fixture",
                }
                for workload, check in STRICT_CORRECTNESS
            ],
        )

        build_fields = [
            "index", "builder", "threads", "build_time_seconds", "peak_rss_mb",
            "index_size_bytes", "allocated_disk_bytes", "bits_per_base",
            "speedup_vs_divsufsort", "raw_repetitions", "expected_repetitions", "status",
        ]
        build_rows = []
        for index, builder, threads, seconds in (
            ("SA / divsufsort", "divsufsort", "1", "11.111111"),
            ("SA / CaPS", "CaPS", "64", "22.222222"),
            ("FM / SDSL Huffman", "SDSL Huffman", "1", "33.333333"),
        ):
            build_rows.append(
                {
                    "index": index,
                    "builder": builder,
                    "threads": threads,
                    "build_time_seconds": seconds,
                    "peak_rss_mb": "44.444444",
                    "index_size_bytes": "55555555",
                    "allocated_disk_bytes": "55558144",
                    "bits_per_base": "6.666666",
                    "speedup_vs_divsufsort": "1.000000",
                    "raw_repetitions": "3",
                    "expected_repetitions": "3",
                    "status": "ok",
                }
            )
        write_tsv(self.package / "headline" / "headline-build.tsv", build_fields, build_rows)

        query_fields = [
            "index", "operation", "query_count", "seconds_median",
            "queries_per_second", "nanoseconds_per_query", "query_bases_per_second",
            "query_peak_rss_mb", "result_checksum", "raw_repetitions",
            "expected_repetitions", "status",
        ]
        query_rows = []
        for index in ("SA32 default", "FM Huffman"):
            for operation in ("Count", "Locate-1"):
                query_rows.append(
                    {
                        "index": index,
                        "operation": operation,
                        "query_count": "7777",
                        "seconds_median": "0.125000",
                        "queries_per_second": "88888.000000",
                        "nanoseconds_per_query": "999.000000",
                        "query_bases_per_second": "101010.000000",
                        "query_peak_rss_mb": "111.111111",
                        "result_checksum": "abcdef0123456789",
                        "raw_repetitions": "5",
                        "expected_repetitions": "5",
                        "status": "ok",
                    }
                )
        write_tsv(self.package / "headline" / "headline-query.tsv", query_fields, query_rows)

        right_fields = [
            "method", "query_bases", "seconds_median", "query_bases_per_second",
            "matches_per_second", "total_matches", "speedup_vs_baseline",
            "result_checksum", "raw_repetitions", "expected_repetitions", "status",
        ]
        right_rows = []
        for method, speedup in (
            ("SA baseline", "1.000000"),
            ("SA suffix-link default", "2.000000"),
        ):
            right_rows.append(
                {
                    "method": method,
                    "query_bases": "2560000",
                    "seconds_median": "1.250000",
                    "query_bases_per_second": "121212.000000",
                    "matches_per_second": "131313.000000",
                    "total_matches": "141414",
                    "speedup_vs_baseline": speedup,
                    "result_checksum": "0123456789abcdef",
                    "raw_repetitions": "5",
                    "expected_repetitions": "5",
                    "status": "ok",
                }
            )
        write_tsv(
            self.package / "headline" / "headline-right-maximal.tsv",
            right_fields,
            right_rows,
        )
        long_rows = []
        for row in build_rows:
            for metric, unit in (
                ("build_time_seconds", "seconds"),
                ("peak_rss_mb", "MiB"),
                ("index_size_bytes", "bytes"),
                ("bits_per_base", "bits/base"),
            ):
                long_rows.append(
                    {
                        "section": "build",
                        "item": row["index"],
                        "metric": metric,
                        "value": row[metric],
                        "unit": unit,
                        "status": row["status"],
                        "provenance": "raw_repetitions.tsv",
                    }
                )
        for row in query_rows:
            long_rows.append(
                {
                    "section": "exact",
                    "item": f"{row['index']} / {row['operation']}",
                    "metric": "queries_per_second",
                    "value": row["queries_per_second"],
                    "unit": "queries/s",
                    "status": row["status"],
                    "provenance": "raw_repetitions.tsv",
                }
            )
        for row in right_rows:
            long_rows.append(
                {
                    "section": "right-maximal",
                    "item": row["method"],
                    "metric": "query_bases_per_second",
                    "value": row["query_bases_per_second"],
                    "unit": "bases/s",
                    "status": row["status"],
                    "provenance": "raw_repetitions.tsv",
                }
            )
        write_tsv(
            self.package / "headline" / "headline.tsv",
            ["section", "item", "metric", "value", "unit", "status", "provenance"],
            long_rows,
        )

    def invoke(self, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                "-B",
                str(TOOL),
                "--package-dir",
                str(self.package),
                "--readme",
                str(self.readme),
                *extra,
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_update_renders_tsv_values_and_preserves_unmanaged_content(self) -> None:
        before_tail = "## Generic instructions\n\nKeep this text unchanged.\n"
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        rendered = self.readme.read_text(encoding="utf-8")
        self.assertEqual(rendered.count(START), 1)
        self.assertEqual(rendered.count(END), 1)
        self.assertIn(before_tail, rendered)
        self.assertIn("11.111111", rendered)
        self.assertIn("88888.000000", rendered)
        self.assertIn("131313.000000", rendered)
        self.assertIn(r"Environment: server-test; Test CPU; Linux x86\_64", rendered)
        self.assertIn("results/server-test/README.md", rendered)
        self.assertIn("results/server-test/headline/README.md", rendered)
        self.assertIn("results/server-test/figures/headline-performance.svg", rendered)

    def test_check_is_read_only_and_detects_stale_region(self) -> None:
        original = self.readme.read_text(encoding="utf-8")
        stale = self.invoke("--check")
        self.assertEqual(stale.returncode, 1)
        self.assertEqual(self.readme.read_text(encoding="utf-8"), original)

        updated = self.invoke()
        self.assertEqual(updated.returncode, 0, updated.stderr)
        current = self.readme.read_text(encoding="utf-8")
        checked = self.invoke("--check")
        self.assertEqual(checked.returncode, 0, checked.stderr)
        self.assertEqual(self.readme.read_text(encoding="utf-8"), current)

    def test_partial_package_is_rejected_without_writing(self) -> None:
        (self.package / "README.md").write_text(
            "# Package\n\n**Package status: partial.**\n", encoding="utf-8"
        )
        original = self.readme.read_text(encoding="utf-8")
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("must declare status complete exactly once", result.stderr)
        self.assertEqual(self.readme.read_text(encoding="utf-8"), original)

    def test_conflicting_package_status_declarations_are_rejected(self) -> None:
        path = self.package / "README.md"
        path.write_text(
            path.read_text(encoding="utf-8") + "\n**Package status: partial.**\n",
            encoding="utf-8",
        )
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("observed=complete,partial", result.stderr)

    def test_conflicting_headline_status_declarations_are_rejected(self) -> None:
        path = self.package / "headline" / "README.md"
        path.write_text(
            path.read_text(encoding="utf-8") + "\n**Headline status: partial.**\n",
            encoding="utf-8",
        )
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("observed=ok,partial", result.stderr)

    def test_non_ok_headline_row_is_rejected_without_writing(self) -> None:
        path = self.package / "headline" / "headline-query.tsv"
        text = path.read_text(encoding="utf-8")
        path.write_text(text.replace("\tok\n", "\tpartial\n", 1), encoding="utf-8")
        original = self.readme.read_text(encoding="utf-8")
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("non-ok status", result.stderr)
        self.assertEqual(self.readme.read_text(encoding="utf-8"), original)

    def test_incomplete_environment_is_rejected(self) -> None:
        path = self.package / "environment.tsv"
        with path.open("r", encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream, delimiter="\t"))
        rows = [row for row in rows if row["key"] != "cpu_model"]
        write_tsv(path, ["key", "value", "source"], rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("cpu_model=<missing>", result.stderr)

    def test_missing_required_correctness_check_is_rejected(self) -> None:
        path = self.package / "correctness-summary.tsv"
        fields, rows = read_tsv(path)
        rows = [
            row for row in rows
            if row["check"] != "headline:headline_fixed_workload_contract"
        ]
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("headline_fixed_workload_contract", result.stderr)

    def test_missing_exported_reference_identity_check_is_rejected(self) -> None:
        path = self.package / "correctness-summary.tsv"
        fields, rows = read_tsv(path)
        rows = [
            row for row in rows
            if row["check"] != "headline:headline_exported_reference_identity"
        ]
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("headline_exported_reference_identity", result.stderr)

    def test_missing_headline_raw_completeness_check_is_rejected(self) -> None:
        path = self.package / "correctness-summary.tsv"
        fields, rows = read_tsv(path)
        rows = [
            row for row in rows
            if row["check"] != "headline:headline_raw_completeness"
        ]
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("headline_raw_completeness", result.stderr)

    def test_strict_runner_profile_audit_shape_is_accepted(self) -> None:
        self.assertEqual(len(BASE_CORRECTNESS), 11)
        self.assertEqual(len(HEADLINE_AUDIT_CHECKS), 12)
        self.assertEqual(len(STRICT_CORRECTNESS), 23)
        fields, rows = read_tsv(self.package / "correctness-summary.tsv")
        strict = {
            (row["workload"], row["check"])
            for row in rows
            if row["workload"] == "profile-audit" and row["check"].startswith("headline:")
        }
        self.assertEqual(
            strict,
            {("profile-audit", f"headline:{check}") for check in HEADLINE_AUDIT_CHECKS},
        )
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_equivalent_complete_non_strict_shape_is_accepted(self) -> None:
        path = self.package / "correctness-summary.tsv"
        fields, rows = read_tsv(path)
        for row in rows:
            if row["workload"] == "profile-audit" and row["check"].startswith("headline:"):
                row["workload"] = "sa-build"
                row["check"] = row["check"].removeprefix("headline:")
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_partial_strict_shape_cannot_fall_back_to_legacy_rows(self) -> None:
        path = self.package / "correctness-summary.tsv"
        fields, rows = read_tsv(path)
        rows = [
            row for row in rows
            if row["check"] != "headline:headline_exported_reference_identity"
        ]
        rows.extend(
            {
                "workload": "sa-build",
                "check": check,
                "groups_checked": "1",
                "mismatches": "0",
                "status": "ok",
                "details": "legacy fixture",
            }
            for check in HEADLINE_AUDIT_CHECKS
        )
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("strict package is missing", result.stderr)

    def test_duplicate_correctness_check_is_rejected(self) -> None:
        path = self.package / "correctness-summary.tsv"
        fields, rows = read_tsv(path)
        rows.append(dict(rows[0]))
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate correctness check", result.stderr)

    def test_missing_long_form_semantic_row_is_rejected(self) -> None:
        path = self.package / "headline" / "headline.tsv"
        fields, rows = read_tsv(path)
        rows.pop()
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("semantic row set mismatch", result.stderr)
        self.assertIn("missing=", result.stderr)

    def test_duplicate_long_form_semantic_row_is_rejected(self) -> None:
        path = self.package / "headline" / "headline.tsv"
        fields, rows = read_tsv(path)
        rows.append(dict(rows[0]))
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("duplicate headline semantic row", result.stderr)

    def test_inconsistent_long_form_value_is_rejected(self) -> None:
        path = self.package / "headline" / "headline.tsv"
        fields, rows = read_tsv(path)
        rows[0]["value"] = "987654.321"
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("disagrees with dedicated TSV", result.stderr)

    def test_inconsistent_long_form_provenance_is_rejected(self) -> None:
        path = self.package / "headline" / "headline.tsv"
        fields, rows = read_tsv(path)
        rows[0]["provenance"] = "summary.tsv"
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("disagrees with dedicated TSV", result.stderr)

    def test_package_name_must_be_safe(self) -> None:
        invalid = self.package.parent / "bad package"
        self.package.rename(invalid)
        self.package = invalid
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("directory name must use only", result.stderr)

    def test_package_must_be_directly_below_benchmarks_results(self) -> None:
        invalid_parent = self.benchmarks / "elsewhere"
        invalid_parent.mkdir()
        invalid = invalid_parent / self.package.name
        self.package.rename(invalid)
        self.package = invalid
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("must be benchmarks/results/<name>", result.stderr)

    def test_non_finite_numeric_value_is_rejected(self) -> None:
        path = self.package / "headline" / "headline-query.tsv"
        fields, rows = read_tsv(path)
        rows[0]["queries_per_second"] = "nan"
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("finite and non-negative", result.stderr)

    def test_builder_thread_mapping_is_rejected(self) -> None:
        path = self.package / "headline" / "headline-build.tsv"
        fields, rows = read_tsv(path)
        caps = next(row for row in rows if row["index"] == "SA / CaPS")
        caps["threads"] = "63"
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("unexpected thread count for SA / CaPS", result.stderr)

    def test_invalid_checksum_is_rejected(self) -> None:
        path = self.package / "headline" / "headline-query.tsv"
        fields, rows = read_tsv(path)
        rows[0]["result_checksum"] = "not-a-checksum"
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn("not a hexadecimal checksum", result.stderr)

    def test_free_text_is_html_and_markdown_escaped(self) -> None:
        path = self.package / "environment.tsv"
        fields, rows = read_tsv(path)
        label = next(row for row in rows if row["key"] == "server_label")
        label["value"] = "srv<script>*bold*|[link]"
        write_tsv(path, fields, rows)
        result = self.invoke()
        self.assertEqual(result.returncode, 0, result.stderr)
        rendered = self.readme.read_text(encoding="utf-8")
        self.assertNotIn("<script>", rendered)
        self.assertIn(
            r"srv&lt;script&gt;\*bold\*\|\[link\]",
            rendered,
        )


if __name__ == "__main__":
    unittest.main()
