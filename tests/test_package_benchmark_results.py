#!/usr/bin/env python3

from __future__ import annotations

import csv
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "benchmarks" / "package_benchmark_results.py"
FIXTURE = ROOT / "tests" / "fixtures" / "benchmark_results" / "run"
SPEC = importlib.util.spec_from_file_location("package_benchmark_results", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def make_temporary_copy_user_writable(root: Path) -> None:
    """Undo read-only provenance modes only inside a test-owned temp tree."""
    for path in (root, *root.rglob("*")):
        path.chmod(path.stat().st_mode | stat.S_IWUSR)


def rewrite_without(path: Path, removed: set[str]) -> None:
    content = rows(path)
    fieldnames = [name for name in content[0] if name not in removed]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        for row in content:
            writer.writerow({name: row[name] for name in fieldnames})


def write_rows(path: Path, fieldnames: list[str], content: list[dict[str, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n",
            extrasaction="ignore")
        writer.writeheader()
        writer.writerows(content)


def rewrite_filter(path: Path, keep) -> None:
    content = rows(path)
    fieldnames = list(content[0])
    write_rows(path, fieldnames, [row for row in content if keep(row)])


def retained_partials(output: Path) -> list[Path]:
    return sorted(output.parent.glob(f".{output.name}.partial.*"))


def fixture_columns(relative: str) -> list[str]:
    with (FIXTURE / relative).open("r", encoding="utf-8", newline="") as stream:
        reader = csv.reader(stream, delimiter="\t")
        return next(reader)


def blank_row(columns: list[str], **values: str) -> dict[str, str]:
    row = {column: "NA" for column in columns}
    row.update(values)
    return row


def create_profile_fixture(root: Path, profile: str) -> None:
    """Create a compact but matrix-complete synthetic server profile.

    Non-timed exact slices use explicit ``not_applicable`` placeholders; one
    ordinary count slice per method carries every expected repetition so tests
    can independently remove and detect a repetition.
    """
    scenarios = MODULE.PROFILE_SCENARIOS[profile]
    methods = MODULE.EXACT_METHODS[profile]
    build_repetitions = MODULE.PROFILE_BUILD_REPETITIONS[profile]
    query_repetitions = MODULE.PROFILE_QUERY_REPETITIONS[profile]

    exact_dir = root / profile / "exact"
    exact_columns = fixture_columns("headline/exact/raw_repetitions.tsv")
    for field in (
        "skipped_high_frequency_queries", "backend", "backend_signature", "sdsl_version",
        "coordinate_width", "sa_sampling_rate", "canary_total_hits",
        "canary_reported_hits", "canary_checksum", "query_threads",
    ):
        if field not in exact_columns:
            exact_columns.append(field)
    exact_raw: list[dict[str, str]] = []
    metadata: list[dict[str, str]] = []
    for scenario in scenarios:
        dataset = f"{profile}-{scenario}"
        metadata.append({
            "run_id": "matrix", "timestamp": "2026-08-24T00:00:00Z",
            "profile": profile, "scenario": scenario, "seed": "20260822",
            "dataset": dataset, "dataset_fingerprint": f"fp-{profile}-{scenario}",
            "total_bases": "16384", "contigs": "4", "compiler": "GCC",
            "compiler_version": "13", "cmake_version": "3.26", "build_type": "Release",
            "os": "Linux", "architecture": "x86_64", "cpu_model": "fixture",
            "logical_cpus": "128", "fm_query_modes": "scalar,batch",
            "fm_batch_widths": "1,4,8,16,32",
            "fm_batch_width_overrides": "fm-balanced:16,32;fm-epr:16,32",
            "worker_process_model": MODULE.CLEAN_EXEC_WORKER_MODEL,
        })
        for method in methods:
            builds = 1 if method == "naive" else build_repetitions
            for repetition in range(builds):
                exact_raw.append(blank_row(
                    exact_columns, run_id="matrix", dataset=dataset, scenario=scenario,
                    method=method, phase="build", query_group="NA", pattern_length="NA",
                    strand="NA", operation="build", max_hits="NA",
                    repetition=str(repetition), query_count="0", seconds="1",
                    skipped_high_frequency_queries="0",
                    user_cpu_seconds="1", system_cpu_seconds="0", peak_rss_mb="10",
                    peak_rss_scope="build_worker_clean_exec_reference_plus_build",
                    total_hits="0", reported_hits="0",
                    result_checksum="0", status="ok", fm_query_mode="NA",
                    fm_batch_width="NA", query_bases="0", serialized_bytes="1024",
                    allocated_disk_bytes="4096", total_bases="16384", threads="1",
                    backend=method, backend_signature=method,
                    sdsl_version=("3.0.3" if method in MODULE.FM_METHODS else "NA"),
                    coordinate_width=("32" if "32" in method else
                                      ("64" if "64" in method else "0")),
                    sa_sampling_rate=(method.rsplit("-k", 1)[1] if "sampled-k" in method else "1"),
                    canary_total_hits="1", canary_reported_hits="1",
                    canary_checksum="canary", query_threads="0"))

            def add_query(operation: str, max_hits: str, mode: str, width: str,
                          group: str, length: str, strand: str) -> None:
                measured = group == "exact_unique" and length == "20" and strand == "forward" \
                    and operation == "count" and mode == "scalar"
                repetitions = 1 if method == "naive" and profile == "quick" else query_repetitions
                status = "ok" if measured else (
                    "skipped_high_frequency" if operation == "locate" and max_hits == "all"
                    else "not_applicable")
                for repetition in range(repetitions):
                    exact_raw.append(blank_row(
                        exact_columns, run_id="matrix", dataset=dataset, scenario=scenario,
                        method=method, phase="query", query_group=group,
                        pattern_length=length, strand=strand, operation=operation,
                        max_hits=max_hits, repetition=str(repetition), query_count="1",
                        skipped_high_frequency_queries=("1" if status == "skipped_high_frequency" else "0"),
                        seconds="0.001" if status == "ok" else "NA",
                        user_cpu_seconds="0", system_cpu_seconds="0",
                        peak_rss_mb="10" if status == "ok" else "NA",
                        peak_rss_scope=(
                            "count_worker_clean_exec_required_dataset_plus_load_plus_query"
                            if operation == "count" else
                            f"locate_worker_{max_hits}_clean_exec_required_dataset_plus_load_plus_query"),
                        total_hits="1" if status == "ok" else "0",
                        reported_hits="1" if status == "ok" else "0",
                        result_checksum="shared" if status == "ok" else "0", status=status,
                        fm_query_mode=mode, fm_batch_width=width, query_bases="20",
                        serialized_bytes="1024", allocated_disk_bytes="4096",
                        total_bases="16384", threads="1"))

            for group in MODULE.EXACT_QUERY_GROUPS:
                for length in MODULE.EXACT_PATTERN_LENGTHS:
                    for strand in MODULE.EXACT_STRANDS:
                        add_query("count", "NA", "scalar", "NA", group, length, strand)
                        for limit in MODULE.EXACT_LOCATE_LIMITS:
                            add_query("locate", limit, "scalar", "NA", group, length, strand)
                        if method in MODULE.FM_METHODS:
                            for width in MODULE.FM_BATCH_WIDTHS[method]:
                                add_query("count", "NA", "batch", width, group, length, strand)

    metadata_columns = fixture_columns("headline/exact/run_metadata.tsv")
    for field in MODULE.EXACT_METADATA_COLUMNS:
        if field not in metadata_columns:
            metadata_columns.append(field)
    write_rows(exact_dir / "run_metadata.tsv", metadata_columns, metadata)
    write_rows(exact_dir / "raw_repetitions.tsv", exact_columns, exact_raw)
    write_rows(exact_dir / "build_results.tsv", ["method", "status"],
               [{"method": method, "status": "ok"} for method in methods])
    write_rows(exact_dir / "query_results.tsv", ["method", "status"],
               [{"method": method, "status": "ok"} for method in methods])

    sa_columns = fixture_columns("headline/sa-build/raw_repetitions.tsv")
    for field in (
        "construction_coordinate_width", "stored_coordinate_width",
        "sa_resource_profile", "lcp_encoding", "storage_compaction_seconds",
        "build_peak_rss_scope", "save_peak_rss_scope", "load_peak_rss_scope",
    ):
        if field not in sa_columns:
            sa_columns.append(field)
    for scope_name, configurations in MODULE.SA_BUILD_MATRICES[profile].items():
        scope = root / profile / scope_name
        raw: list[dict[str, str]] = []
        for method, threads, sampling_rate, acceleration in configurations:
            for repetition in range(1, build_repetitions + 1):
                raw.append(blank_row(
                    sa_columns, method=method, effective_backend=method,
                    backend_signature=method, coordinate_width="32" if method.endswith("32") else "64",
                    construction_coordinate_width=(
                        "32" if method.endswith("32") else "64"),
                    stored_coordinate_width=("32" if method.endswith("32") else "64"),
                    sa_resource_profile="fast", lcp_encoding="raw",
                    threads=threads, sampling_rate=sampling_rate, suffix_count="100",
                    subproblem_count="1",
                    acceleration=("suffix-link" if acceleration == "default" else acceleration),
                    repetition=str(repetition), total_bases="16384", sequence_count="4",
                    reference_read_seconds="0.1", normalization_seconds="0.1", sa_seconds="1",
                    storage_compaction_seconds="0.01",
                    isa_seconds="1", lcp_seconds="1", child_seconds="0", sapling_seconds="0",
                    build_wall_seconds="3", user_cpu_seconds="3", system_cpu_seconds="0",
                    peak_rss_mb="10", build_peak_rss_mb="10",
                    build_peak_rss_scope="clean_exec_build_worker_until_index_ready",
                    save_seconds="0.1",
                    save_user_cpu_seconds="0", save_system_cpu_seconds="0",
                    save_peak_rss_mb="10",
                    save_peak_rss_scope="clean_exec_save_worker_including_source_load",
                    load_seconds="0.1", load_user_cpu_seconds="0",
                    load_system_cpu_seconds="0", load_peak_rss_mb="10",
                    load_peak_rss_scope="clean_exec_load_worker_until_index_ready",
                    serialized_bytes="1024", allocated_disk_bytes="4096", bits_per_base="0.5",
                    learned_index_bytes="0", sa_checksum="sa", exact_checksum="exact",
                    right_maximal_checksum="right", status="ok"))
        write_rows(scope / "raw_repetitions.tsv", sa_columns, raw)
        write_rows(
            scope / "run_metadata.tsv",
            ["profile", "dataset_fingerprint", "worker_process_model"],
            [{"profile": profile, "dataset_fingerprint": f"fp-{profile}",
              "worker_process_model": MODULE.CLEAN_EXEC_WORKER_MODEL}])
        write_rows(scope / "build_results.tsv", ["method", "status"],
                   [{"method": row["method"], "status": "ok"} for row in raw])

    right_dir = root / profile / "right-maximal"
    right_columns = fixture_columns("headline/right-maximal/raw_repetitions.tsv")
    for field in ("materialization_match_threshold", "vector_skipped"):
        if field not in right_columns:
            right_columns.append(field)
    right_raw: list[dict[str, str]] = []
    right_metadata: list[dict[str, str]] = []
    oracle: list[dict[str, str]] = []
    for scenario in scenarios:
        dataset = f"{profile}-{scenario}"
        right_metadata.append({
            "profile": profile, "scenario": scenario, "seed": "20260822", "dataset": dataset,
            "dataset_fingerprint": f"fp-{profile}-{scenario}", "total_bases": "16384",
            "query_count": "100", "query_bases": "25600",
            "worker_process_model": MODULE.CLEAN_EXEC_WORKER_MODEL})
        for length in ("20", "50", "100"):
            oracle.append({
                "dataset": dataset, "scenario": scenario, "oracle": "naive", "min_length": length,
                "reference_bases": "4096", "query_count": "16", "query_bases": "4096",
                "total_matches": "1", "result_checksum": "right", "status": "ok"})
        for method in MODULE.RIGHT_INTERNAL_METHODS:
            for repetition in range(build_repetitions):
                right_raw.append(blank_row(
                    right_columns, dataset=dataset, method=method, operation="build",
                    min_length="0", repetition=str(repetition), seconds="1",
                    peak_rss_mb="10",
                    peak_rss_scope="build_worker_clean_exec_reference_plus_build",
                    query_bases="0",
                    serialized_bytes="1024", allocated_disk_bytes="4096", auxiliary_bytes="0",
                    total_matches="0", reported_matches="0", count_checksum="0",
                    result_checksum="0", status="ok"))
            for operation in MODULE.RIGHT_QUERY_OPERATIONS:
                for length in ("20", "50", "100"):
                    for repetition in range(query_repetitions):
                        right_raw.append(blank_row(
                            right_columns, dataset=dataset, method=method, operation=operation,
                            min_length=length, repetition=str(repetition), seconds="0.01",
                            peak_rss_mb="10",
                            peak_rss_scope=(
                                "query_worker_clean_exec_queries_plus_load_plus_query"),
                            query_bases="25600",
                            serialized_bytes="1024", allocated_disk_bytes="4096", auxiliary_bytes="0",
                            materialization_match_threshold="1000000", vector_skipped="0",
                            total_matches="1", reported_matches="1", count_checksum="right-count",
                            result_checksum="right-result", status="ok"))
        for method in MODULE.FM_METHODS:
            for operation in MODULE.RIGHT_QUERY_OPERATIONS:
                for length in ("20", "50", "100"):
                    right_raw.append(blank_row(
                        right_columns, dataset=dataset, method=method, operation=operation,
                        min_length=length, repetition="0", seconds="NA", peak_rss_mb="NA",
                        peak_rss_scope="not_applicable", query_bases="25600",
                        serialized_bytes="0", allocated_disk_bytes="0", auxiliary_bytes="0",
                        materialization_match_threshold="1000000", vector_skipped="0",
                        total_matches="0", reported_matches="0", count_checksum="NA",
                        result_checksum="NA", status="not_supported"))
    write_rows(right_dir / "raw_repetitions.tsv", right_columns, right_raw)
    write_rows(right_dir / "run_metadata.tsv", list(right_metadata[0]), right_metadata)
    write_rows(right_dir / "build_results.tsv", ["method", "status"],
               [{"method": method, "status": "ok"} for method in MODULE.RIGHT_INTERNAL_METHODS])
    write_rows(right_dir / "query_results.tsv", ["method", "status"],
               [{"method": method, "status": "ok"} for method in
                (*MODULE.RIGHT_INTERNAL_METHODS, *MODULE.FM_METHODS)])
    write_rows(right_dir / "correctness_summary.tsv", list(oracle[0]), oracle)


def expand_headline_repetitions(run: Path) -> None:
    for scope in ("exact-build", "exact-query"):
        exact_path = run / "headline" / scope / "raw_repetitions.tsv"
        exact = rows(exact_path)
        fieldnames = list(exact[0])
        expanded = []
        for row in exact:
            repetitions = range(3) if row["phase"] == "build" else (
                range(5) if row["phase"] == "query" else range(1))
            for repetition in repetitions:
                copy = dict(row)
                copy["repetition"] = str(repetition)
                expanded.append(copy)
        write_rows(exact_path, fieldnames, expanded)


def prepare_unified_headline_scope(run: Path) -> None:
    exact_path = run / "headline" / "exact" / "raw_repetitions.tsv"
    exact = rows(exact_path)
    fieldnames = list(exact[0])
    if "skipped_high_frequency_queries" not in fieldnames:
        fieldnames.append("skipped_high_frequency_queries")
    for field in (
        "backend", "backend_signature", "sdsl_version", "coordinate_width",
        "sa_sampling_rate", "canary_total_hits", "canary_reported_hits",
        "canary_checksum", "query_threads",
    ):
        if field not in fieldnames:
            fieldnames.append(field)
    for row in exact:
        for field in fieldnames:
            row.setdefault(field, "NA")
        row["skipped_high_frequency_queries"] = row.get("skipped_high_frequency_queries") or "0"
        if row["method"] == "sa32-lcp-binary":
            row["method"] = "sa32-binary"
        row["peak_rss_scope"] = (
            "build_worker_clean_exec_reference_plus_build"
            if row["phase"] == "build" else
            ("count_worker_clean_exec_required_dataset_plus_load_plus_query"
             if row["operation"] == "count" else
             f"locate_worker_{row['max_hits']}_clean_exec_required_dataset_plus_load_plus_query"))
    fm_build = next(row for row in exact if row["method"] == "fm-huff" and row["phase"] == "build")
    for method, seconds, rss, size, threads in (
        ("sa32-binary", "13.0", "2048", "2147483648", "1"),
        ("caps32", "6.0", "3072", "2147483648", "64"),
    ):
        row = next((candidate for candidate in exact
                    if candidate["method"] == method and candidate["phase"] == "build"), None)
        if row is None:
            row = dict(fm_build)
            exact.append(row)
        row.update({
            "method": method, "seconds": seconds, "peak_rss_mb": rss,
            "serialized_bytes": size, "allocated_disk_bytes": size,
            "threads": threads, "canary_total_hits": "1",
            "canary_reported_hits": "1", "canary_checksum": "sa-canary",
            "query_threads": "1", "coordinate_width": "32", "sa_sampling_rate": "1",
            "backend": "divsufsort32" if method == "sa32-binary" else "caps32",
            "backend_signature": method, "sdsl_version": "NA",
        })
    fm_build.update({
        "backend": "sdsl_csa_wt_huff", "backend_signature": "sdsl-huff",
        "sdsl_version": "3.0.3", "coordinate_width": "0", "sa_sampling_rate": "NA",
        "canary_total_hits": "1", "canary_reported_hits": "1",
        "canary_checksum": "sa-canary", "query_threads": "1",
    })
    write_rows(exact_path, fieldnames, exact)
    write_rows(
        run / "headline" / "exact" / "build_results.tsv",
        ["run_id", "dataset", "method", "serialized_bytes", "status"],
        [
            {"run_id": "fixture", "dataset": "full-mixed", "method": "sa32-binary",
             "serialized_bytes": "2147483648", "status": "ok"},
            {"run_id": "fixture", "dataset": "full-mixed", "method": "caps32",
             "serialized_bytes": "2147483648", "status": "ok"},
            {"run_id": "fixture", "dataset": "full-mixed", "method": "fm-huff",
             "serialized_bytes": "536870912", "status": "ok"},
        ])
    # Keep deliberately implausible legacy standalone rows in the input.  The
    # unified headline must ignore them rather than silently pooling scopes.
    legacy_path = run / "headline" / "sa-build" / "raw_repetitions.tsv"
    legacy = rows(legacy_path)
    legacy_fields = list(legacy[0])
    for field in (
        "construction_coordinate_width", "stored_coordinate_width",
        "sa_resource_profile", "lcp_encoding", "storage_compaction_seconds",
        "build_peak_rss_scope", "save_peak_rss_scope", "load_peak_rss_scope",
    ):
        if field not in legacy_fields:
            legacy_fields.append(field)
    for row in legacy:
        row["build_wall_seconds"] = "999"
        row.update({
            "construction_coordinate_width": row["coordinate_width"],
            "stored_coordinate_width": row["coordinate_width"],
            "sa_resource_profile": "fast",
            "lcp_encoding": "raw",
            "storage_compaction_seconds": "0.01",
            "build_peak_rss_scope": "clean_exec_build_worker_until_index_ready",
            "save_peak_rss_scope": "clean_exec_save_worker_including_source_load",
            "load_peak_rss_scope": "clean_exec_load_worker_until_index_ready",
        })
    write_rows(legacy_path, legacy_fields, legacy)
    legacy_metadata_path = run / "headline" / "sa-build" / "run_metadata.tsv"
    legacy_metadata = rows(legacy_metadata_path)
    legacy_metadata_fields = list(legacy_metadata[0])
    if "worker_process_model" not in legacy_metadata_fields:
        legacy_metadata_fields.append("worker_process_model")
    for row in legacy_metadata:
        row["worker_process_model"] = MODULE.CLEAN_EXEC_WORKER_MODEL
    write_rows(legacy_metadata_path, legacy_metadata_fields, legacy_metadata)

    exact_scope = run / "headline" / "exact"
    metadata = rows(exact_scope / "run_metadata.tsv")
    metadata_fields = list(metadata[0])
    for field in ("query_set_checksum", "query_count", "query_bases",
                  *MODULE.EXACT_METADATA_COLUMNS):
        if field not in metadata_fields:
            metadata_fields.append(field)
    for row in metadata:
        row.update({
            "query_set_checksum": "query-set", "query_count": "10000",
            "query_bases": "1000000",
            "fm_query_modes": "scalar", "fm_batch_widths": "16",
            "fm_batch_width_overrides": "none",
            "worker_process_model": MODULE.CLEAN_EXEC_WORKER_MODEL,
        })
    definition = blank_row(
        fieldnames, run_id="fixture", dataset="full-mixed", scenario="mixed",
        method="dataset", phase="query_definition", query_group="exact_unique",
        pattern_length="100", strand="NA", operation="definition", max_hits="NA",
        repetition="0", query_count="1", skipped_high_frequency_queries="0", seconds="0",
        user_cpu_seconds="0", system_cpu_seconds="0", peak_rss_mb="0",
        peak_rss_scope="not_applicable", total_hits="0", reported_hits="0",
        result_checksum="0", status="ok", query_id="q0", query_source="fixture",
        fm_query_mode="NA", fm_batch_width="NA", query_bases="100",
        serialized_bytes="0", allocated_disk_bytes="0", total_bases="268435456",
        threads="1", canary_checksum="NA")
    for name, selected in (
        ("exact-build", [row for row in exact if row["phase"] == "build"] + [definition]),
        ("exact-query", [row for row in exact if row["phase"] == "query"] + [definition]),
    ):
        destination = run / "headline" / name
        write_rows(destination / "raw_repetitions.tsv", fieldnames, selected)
        write_rows(destination / "run_metadata.tsv", metadata_fields, metadata)
        for companion in ("build_results.tsv", "query_results.tsv"):
            shutil.copy2(exact_scope / companion, destination / companion)
    shutil.rmtree(exact_scope)

    right_path = run / "headline" / "right-maximal" / "raw_repetitions.tsv"
    right_metadata_path = run / "headline" / "right-maximal" / "run_metadata.tsv"
    right_metadata = rows(right_metadata_path)
    right_metadata_fields = list(right_metadata[0])
    if "contigs" not in right_metadata_fields:
        right_metadata_fields.append("contigs")
    if "worker_process_model" not in right_metadata_fields:
        right_metadata_fields.append("worker_process_model")
    for row in right_metadata:
        row.update({
            "scenario": "user-reference", "dataset_fingerprint": "right-reference-fp",
            "contigs": "4",
            "worker_process_model": MODULE.CLEAN_EXEC_WORKER_MODEL,
        })
    write_rows(right_metadata_path, right_metadata_fields, right_metadata)

    right = rows(right_path)
    fieldnames = list(right[0])
    for field in ("materialization_match_threshold", "vector_skipped"):
        if field not in fieldnames:
            fieldnames.append(field)
    expanded = []
    for row in right:
        row.setdefault("materialization_match_threshold", "1000000")
        row.setdefault("vector_skipped", "0")
        row["peak_rss_scope"] = (
            "query_worker_clean_exec_queries_plus_load_plus_query")
        for repetition in range(5):
            copy = dict(row)
            copy["repetition"] = str(repetition)
            expanded.append(copy)
    with right_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(expanded)


class HeadlineReferenceIdentityTest(unittest.TestCase):
    def test_recomputes_manifest_sha_and_both_fingerprint_dialects(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run = root / "run"
            reference = root / "dataset" / "reference.fa"
            reference.parent.mkdir()
            content = b">c1\nacgTUx\n>c2\nNN\n"
            reference.write_bytes(content)
            manifest = run / "manifest" / "headline-dataset.sha256"
            manifest.parent.mkdir(parents=True)
            manifest.write_text(
                "d03926a83018907cd9fb1cd8e78cb4616117643be768e242a84b2558c9a7f8a8  "
                f"{reference}\n",
                encoding="utf-8")
            identity = MODULE.headline_reference_identity(run)
            self.assertEqual(identity, {
                "sha256": "d03926a83018907cd9fb1cd8e78cb4616117643be768e242a84b2558c9a7f8a8",
                "total_bases": "8", "contigs": "2",
                "exact_fingerprint": "6699aa9934f457b3",
                "right_fingerprint": "313823280a7a432c",
            })
            manifest.write_text(f"{'0' * 64}  {reference}\n", encoding="utf-8")
            with self.assertRaises(MODULE.PackageError):
                MODULE.headline_reference_identity(run)


class BenchmarkResultPackagerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.run = self.root / "run"
        shutil.copytree(FIXTURE, self.run)
        make_temporary_copy_user_writable(self.run)
        prepare_unified_headline_scope(self.run)
        expand_headline_repetitions(self.run)
        self.identity_patch = mock.patch.object(
            MODULE, "headline_reference_identity", return_value={
                "sha256": "a" * 64,
                "total_bases": "268435456", "contigs": "4",
                "exact_fingerprint": "abcdef",
                "right_fingerprint": "right-reference-fp",
            })
        self.identity_patch.start()

    def tearDown(self) -> None:
        self.identity_patch.stop()
        self.temporary.cleanup()

    def test_recomputes_headline_from_raw_repetitions(self) -> None:
        output = self.root / "result"
        status = MODULE.package(self.run, output, "fixture-server", None, False)
        self.assertEqual(status, 0)
        commands = (output / "commands.md").read_text(encoding="utf-8")
        self.assertIn("--require-complete", commands)

        builds = {row["index"]: row for row in rows(output / "headline" / "headline-build.tsv")}
        self.assertEqual(builds["SA / divsufsort"]["build_time_seconds"], "13")
        self.assertEqual(builds["SA / CaPS"]["build_time_seconds"], "6")
        self.assertEqual(builds["SA / CaPS"]["speedup_vs_divsufsort"], "2.166667")
        self.assertEqual(builds["FM / SDSL Huffman"]["index_size_bytes"], "536870912")

        exact = {(row["index"], row["operation"]): row
                 for row in rows(output / "headline" / "headline-query.tsv")}
        self.assertEqual(exact[("SA32 default", "Count")]["queries_per_second"], "40000")
        self.assertEqual(exact[("FM Huffman", "Locate-1")]["queries_per_second"], "18181.818182")

        right = {row["method"]: row
                 for row in rows(output / "headline" / "headline-right-maximal.tsv")}
        self.assertEqual(right["SA baseline"]["query_bases_per_second"], "256000")
        self.assertEqual(right["SA suffix-link default"]["speedup_vs_baseline"], "5")

        figures = {
            "headline-performance.svg", "build-scaling.svg", "memory-and-size.svg",
            "count-locate-details.svg", "right-maximal-ablation.svg", "caps-thread-scaling.svg",
        }
        for name in figures:
            content = (output / "figures" / name).read_text(encoding="utf-8")
            self.assertNotIn("nan", content.lower())
            self.assertNotIn("None", content)
            ET.fromstring(content)
        svg = (output / "figures" / "headline-performance.svg").read_text(encoding="utf-8")
        self.assertIn("A. Build time", svg)
        self.assertIn("B. Peak RSS / index size", svg)
        self.assertIn("C. Exact throughput", svg)
        self.assertTrue((output / "manifest.tsv").is_file())

    def test_require_complete_reaudits_every_profile_and_publishes_atomically(self) -> None:
        output = self.root / "strict-result"
        # ALL_COMPLETE plus a valid headline is insufficient when the four
        # regular profile directories are absent.
        self.assertEqual(
            MODULE.package(self.run, output, "fixture-server", None, True), 1)
        self.assertFalse(output.exists())
        failed_partial, = retained_partials(output)
        failed_checks = rows(failed_partial / "correctness-summary.tsv")
        self.assertTrue(any(
            row["workload"] == "profile-audit" and
            row["check"].startswith("smoke:") and row["status"] == "fail"
            for row in failed_checks))

        successful_audit = lambda _run, profile: ([{
            "profile": profile, "check": "strict_fixture_contract",
            "status": "ok", "details": "complete",
        }], False)
        with mock.patch.object(MODULE, "audit_profile", side_effect=successful_audit) as audit:
            self.assertEqual(
                MODULE.package(self.run, output, "fixture-server", None, True), 0)
        self.assertTrue(output.is_dir())
        self.assertEqual(
            [call.args[1] for call in audit.call_args_list],
            ["smoke", "quick", "standard", "full", "headline"])
        successful_checks = rows(output / "correctness-summary.tsv")
        strict_checks = [row for row in successful_checks
                         if row["workload"] == "profile-audit"]
        self.assertEqual(len(strict_checks), 5)
        self.assertTrue(all(row["status"] == "ok" for row in strict_checks))
        # The failed attempt is retained for diagnosis; the retry publishes a
        # separate fully built directory without overwriting it.
        self.assertTrue(failed_partial.is_dir())

    def test_headline_audit_has_stable_read_only_interface(self) -> None:
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertFalse(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["required_workload_exact"]["status"], "ok")
        self.assertNotIn("required_workload_sa-build", by_name)
        self.assertEqual(by_name["required_workload_right-maximal"]["status"], "ok")
        self.assertEqual(by_name["headline_build_same_worker_scope"]["status"], "ok")
        self.assertEqual(by_name["headline_build_dataset_fingerprint_equivalence"]["status"], "ok")
        self.assertEqual(by_name["headline_build_repetitions"]["status"], "ok")
        self.assertEqual(by_name["headline_build_provenance"]["status"], "ok")
        self.assertEqual(by_name["headline_sa_canary_checksum_equivalence"]["status"], "ok")
        self.assertEqual(by_name["headline_exact_build_query_identity"]["status"], "ok")
        self.assertEqual(by_name["headline_exported_reference_identity"]["status"], "ok")
        self.assertEqual(by_name["headline_fixed_workload_contract"]["status"], "ok")
        self.assertEqual(by_name["headline_raw_completeness"]["status"], "ok")
        self.assertEqual(by_name["exact_metadata_contract"]["status"], "ok")

        rewrite_without(self.run / "headline" / "right-maximal" / "raw_repetitions.tsv",
                        {"query_bases"})
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        self.assertTrue(any(row["check"].startswith("schema:") and row["status"] == "fail"
                            for row in checks))

    def test_headline_exact_metadata_contract_is_scalar_only_without_overrides(self) -> None:
        mutations = (
            ("exact-build", "fm_query_modes", "scalar,batch"),
            ("exact-query", "fm_batch_width_overrides", ""),
            ("exact-query", "fm_batch_widths", "32"),
        )
        originals = {
            scope: (self.run / "headline" / scope / "run_metadata.tsv").read_bytes()
            for scope in ("exact-build", "exact-query")
        }
        for scope, field, value in mutations:
            with self.subTest(scope=scope, field=field):
                for restored, content in originals.items():
                    (self.run / "headline" / restored / "run_metadata.tsv").write_bytes(content)
                path = self.run / "headline" / scope / "run_metadata.tsv"
                metadata = rows(path)
                for row in metadata:
                    row[field] = value
                write_rows(path, list(metadata[0]), metadata)
                checks, failed = MODULE.audit_profile(self.run, "headline")
                self.assertTrue(failed)
                contract = {row["check"]: row for row in checks}["exact_metadata_contract"]
                self.assertEqual(contract["status"], "fail")
                self.assertIn(field, contract["details"])

    def test_clean_exec_contract_rejects_wrong_worker_model(self) -> None:
        paths = (
            self.run / "headline" / "exact-build" / "run_metadata.tsv",
            self.run / "headline" / "right-maximal" / "run_metadata.tsv",
            self.run / "headline" / "sa-build" / "run_metadata.tsv",
        )
        originals = {path: path.read_bytes() for path in paths}
        for path in paths:
            with self.subTest(scope=path.parent.name):
                for restored, content in originals.items():
                    restored.write_bytes(content)
                metadata = rows(path)
                metadata[0]["worker_process_model"] = "legacy-fork-only"
                write_rows(path, list(metadata[0]), metadata)
                checks, failed = MODULE.audit_profile(self.run, "headline")
                self.assertTrue(failed)
                contract = {
                    row["check"]: row for row in checks
                }["clean_exec_worker_contract"]
                self.assertEqual(contract["status"], "fail")
                self.assertIn("worker_process_model", contract["details"])

    def test_clean_exec_contract_rejects_noncanonical_rss_scopes(self) -> None:
        cases = (
            (self.run / "headline" / "exact-query" / "raw_repetitions.tsv",
             "peak_rss_scope", lambda row: row["phase"] == "query"),
            (self.run / "headline" / "right-maximal" / "raw_repetitions.tsv",
             "peak_rss_scope", lambda row: row["operation"] == "streaming"),
            (self.run / "headline" / "sa-build" / "raw_repetitions.tsv",
             "build_peak_rss_scope", lambda row: row["status"] == "ok"),
        )
        originals = {path: path.read_bytes() for path, _, _ in cases}
        for path, field, predicate in cases:
            with self.subTest(scope=path.parent.name, field=field):
                for restored, content in originals.items():
                    restored.write_bytes(content)
                content = rows(path)
                selected = next(row for row in content if predicate(row))
                selected[field] = "legacy_or_ambiguous_scope"
                write_rows(path, list(content[0]), content)
                checks, failed = MODULE.audit_profile(self.run, "headline")
                self.assertTrue(failed)
                contract = {
                    row["check"]: row for row in checks
                }["clean_exec_worker_contract"]
                self.assertEqual(contract["status"], "fail")
                self.assertIn(field, contract["details"])

    def test_exact_unsupported_save_without_worker_uses_not_applicable_scope(self) -> None:
        self.assertEqual(
            MODULE.expected_exact_rss_scope({
                "method": "sa32-binary", "phase": "save", "operation": "save",
                "status": "unsupported_input_size",
            }),
            "not_applicable")
        self.assertEqual(
            MODULE.expected_exact_rss_scope({
                "method": "sa32-binary", "phase": "save", "operation": "save",
                "status": "ok",
            }),
            "save_worker_clean_exec_load_plus_save")

    def test_audit_cli_returns_tsv_and_zero(self) -> None:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            status = MODULE.main(["audit", "--run-dir", str(self.run), "--profile", "headline"])
        self.assertEqual(status, 0)
        self.assertTrue(stdout.getvalue().startswith("profile\tcheck\tstatus\tdetails\n"))
        self.assertIn("headline_raw_completeness\tok", stdout.getvalue())

    def test_missing_raw_field_is_na_and_partial(self) -> None:
        rewrite_without(self.run / "headline" / "exact-build" / "raw_repetitions.tsv",
                        {"serialized_bytes", "allocated_disk_bytes"})
        output = self.root / "partial"
        status = MODULE.package(self.run, output, "fixture-server", None, False)
        self.assertEqual(status, 0)
        builds = {row["index"]: row for row in rows(output / "headline" / "headline-build.tsv")}
        self.assertEqual(builds["FM / SDSL Huffman"]["index_size_bytes"], "NA")
        self.assertEqual(builds["FM / SDSL Huffman"]["status"],
                         "partial_missing_raw_fields_or_repetitions")
        self.assertIn("Package status: partial", (output / "README.md").read_text(encoding="utf-8"))

        strict_output = self.root / "strict-partial"
        strict_status = MODULE.package(self.run, strict_output, "fixture-server", None, True)
        self.assertEqual(strict_status, 1)
        self.assertFalse(strict_output.exists())
        self.assertEqual(len(retained_partials(strict_output)), 1)

    def test_correctness_mismatch_blocks_headline(self) -> None:
        path = self.run / "headline" / "exact-query" / "raw_repetitions.tsv"
        text = path.read_text(encoding="utf-8")
        path.write_text(text.replace("\t100\t100\ta1\tok\t", "\t100\t100\tdifferent\tok\t", 1),
                        encoding="utf-8")
        output = self.root / "blocked"
        status = MODULE.package(self.run, output, "fixture-server", None, False)
        self.assertEqual(status, 1)
        self.assertFalse(output.exists())
        partial, = retained_partials(output)
        self.assertFalse((partial / "headline").exists())
        self.assertIn("Package status: blocked", (partial / "README.md").read_text(encoding="utf-8"))
        checks = rows(partial / "correctness-summary.tsv")
        self.assertTrue(any(row["status"] == "fail" for row in checks))

    def test_headline_sa32_binary_alias_is_selected(self) -> None:
        path = self.run / "headline" / "exact-query" / "raw_repetitions.tsv"
        output = self.root / "binary-alias"
        self.assertEqual(MODULE.package(self.run, output, "fixture-server", None, False), 0)
        exact = {(row["index"], row["operation"]): row
                 for row in rows(output / "headline" / "headline-query.tsv")}
        self.assertEqual(exact[("SA32 default", "Count")]["status"], "ok")
        self.assertEqual(exact[("SA32 default", "Locate-1")]["status"], "ok")

    def test_headline_query_ignores_query_rows_in_exact_build_scope(self) -> None:
        build_path = self.run / "headline" / "exact-build" / "raw_repetitions.tsv"
        build = rows(build_path)
        fieldnames = list(build[0])
        query = rows(self.run / "headline" / "exact-query" / "raw_repetitions.tsv")
        injected = [dict(row) for row in query if row["phase"] == "query"]
        for row in injected:
            row["seconds"] = "0.000001"
        write_rows(build_path, fieldnames, build + injected)
        output = self.root / "query-scope"
        self.assertEqual(MODULE.package(self.run, output, "fixture-server", None, False), 0)
        exact = {(row["index"], row["operation"]): row
                 for row in rows(output / "headline" / "headline-query.tsv")}
        self.assertEqual(exact[("SA32 default", "Count")]["queries_per_second"], "40000")

    def test_headline_identity_rejects_query_set_or_canary_mismatch(self) -> None:
        metadata_path = self.run / "headline" / "exact-query" / "run_metadata.tsv"
        metadata = rows(metadata_path)
        metadata[0]["query_set_checksum"] = "different-query-set"
        write_rows(metadata_path, list(metadata[0]), metadata)
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["headline_exact_build_query_identity"]["status"], "fail")

        metadata[0]["query_set_checksum"] = "query-set"
        write_rows(metadata_path, list(metadata[0]), metadata)
        build_path = self.run / "headline" / "exact-build" / "raw_repetitions.tsv"
        build = rows(build_path)
        for row in build:
            if row["method"] == "caps32":
                row["canary_checksum"] = "NA"
        write_rows(build_path, list(build[0]), build)
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["headline_sa_canary_checksum_equivalence"]["status"], "fail")

    def test_headline_right_metadata_is_bound_to_the_exported_reference(self) -> None:
        metadata_path = self.run / "headline" / "right-maximal" / "run_metadata.tsv"
        metadata = rows(metadata_path)
        metadata[0]["dataset_fingerprint"] = "abcdef"
        write_rows(metadata_path, list(metadata[0]), metadata)
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        contract = {row["check"]: row for row in checks}["headline_fixed_workload_contract"]
        self.assertEqual(contract["status"], "fail")
        self.assertIn("right-maximal/dataset_fingerprint=abcdef", contract["details"])

    def test_smoke_sized_fake_headline_is_rejected(self) -> None:
        for scope in ("exact-build", "exact-query"):
            path = self.run / "headline" / scope / "run_metadata.tsv"
            metadata = rows(path)
            for row in metadata:
                row.update({
                    "profile": "smoke", "total_bases": "16384",
                    "query_count": "100", "query_bases": "10000",
                })
            write_rows(path, list(metadata[0]), metadata)
        path = self.run / "headline" / "right-maximal" / "run_metadata.tsv"
        metadata = rows(path)
        for row in metadata:
            row.update({
                "profile": "smoke", "total_bases": "65536",
                "query_count": "100", "query_bases": "12800",
            })
        write_rows(path, list(metadata[0]), metadata)
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["headline_fixed_workload_contract"]["status"], "fail")
        self.assertIn("exact-build/profile=smoke", by_name["headline_fixed_workload_contract"]["details"])

    def test_exact_detail_preserves_safety_skip_fields(self) -> None:
        path = self.run / "headline" / "exact-query" / "raw_repetitions.tsv"
        content = rows(path)
        for row in content:
            if (row["method"] == "sa32-binary" and row["query_group"] == "exact_repetitive" and
                    row["operation"] == "locate" and row["max_hits"] == "1"):
                row["status"] = "skipped_high_frequency"
                row["skipped_high_frequency_queries"] = "1"
                row["seconds"] = "NA"
                row["peak_rss_mb"] = "NA"
        write_rows(path, list(content[0]), content)
        output = self.root / "safety-fields"
        # Altering a fixed headline slice correctly blocks headline publication,
        # but workload-level detailed TSVs are still emitted for diagnosis.
        self.assertEqual(MODULE.package(self.run, output, "fixture-server", None, False), 1)
        self.assertFalse(output.exists())
        partial, = retained_partials(output)
        detail = next(row for row in rows(partial / "exact" / "locate-results.tsv")
                      if row["source_scope"] == "headline/exact-query" and
                      row["method"] == "sa32-binary" and
                      row["query_group"] == "exact_repetitive" and row["max_hits"] == "1")
        self.assertEqual(detail["skipped_high_frequency_queries"], "1")
        self.assertEqual(detail["safety_status"], "skipped_high_frequency")
        self.assertEqual(detail["status"], "skipped_high_frequency")

    def test_correctness_compares_query_count_skip_count_and_query_bases(self) -> None:
        path = self.run / "headline" / "exact-query" / "raw_repetitions.tsv"
        content = rows(path)
        changed = False
        for row in content:
            if (not changed and row["method"] == "sa32-binary" and
                    row["query_group"] == "exact_unique" and row["operation"] == "count" and
                    row["repetition"] == "0"):
                row["skipped_high_frequency_queries"] = "1"
                changed = True
        self.assertTrue(changed)
        write_rows(path, list(content[0]), content)
        output = self.root / "cardinality-mismatch"
        self.assertEqual(MODULE.package(self.run, output, "fixture-server", None, False), 1)
        self.assertFalse(output.exists())
        partial, = retained_partials(output)
        correctness = rows(partial / "correctness-summary.tsv")
        self.assertTrue(any(row["workload"] == "exact" and row["status"] == "fail"
                            for row in correctness))

    def test_headline_rejects_split_builder_scopes_and_missing_repetition(self) -> None:
        source = self.run / "headline" / "exact-build" / "raw_repetitions.tsv"
        content = rows(source)
        fieldnames = list(content[0])
        caps = [row for row in content if row["phase"] == "build" and row["method"] == "caps32"]
        write_rows(source, fieldnames, [row for row in content if row not in caps])
        split = self.run / "headline" / "exact-build-caps"
        write_rows(split / "raw_repetitions.tsv", fieldnames, caps)
        shutil.copy2(self.run / "headline" / "exact-build" / "run_metadata.tsv", split / "run_metadata.tsv")
        shutil.copy2(self.run / "headline" / "exact-build" / "build_results.tsv", split / "build_results.tsv")
        shutil.copy2(self.run / "headline" / "exact-build" / "query_results.tsv", split / "query_results.tsv")
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["headline_build_same_worker_scope"]["status"], "fail")

        shutil.rmtree(split)
        prepare = rows(source)
        # Restore caps in the unified scope, but deliberately omit repetition 2.
        write_rows(source, fieldnames, prepare + [row for row in caps if row["repetition"] != "2"])
        checks, failed = MODULE.audit_profile(self.run, "headline")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["headline_build_repetitions"]["status"], "fail")

    def test_right_maximal_detail_excludes_build_save_and_load(self) -> None:
        path = self.run / "headline" / "right-maximal" / "raw_repetitions.tsv"
        content = rows(path)
        fieldnames = list(content[0])
        phase = dict(content[0])
        for operation in ("build", "save", "load"):
            copy = dict(phase)
            copy["operation"] = operation
            copy["min_length"] = "0"
            copy["peak_rss_scope"] = {
                "build": "build_worker_clean_exec_reference_plus_build",
                "save": "save_worker_clean_exec_load_plus_save",
                "load": "load_worker_clean_exec_load",
            }[operation]
            content.append(copy)
        write_rows(path, fieldnames, content)
        output = self.root / "right-filter"
        self.assertEqual(MODULE.package(self.run, output, "fixture-server", None, False), 0)
        details = rows(output / "right-maximal" / "algorithm-ablation.tsv")
        self.assertTrue(details)
        self.assertLessEqual(
            {row["operation"] for row in details}, set(MODULE.RIGHT_QUERY_OPERATIONS))
        self.assertFalse({"build", "save", "load"} & {row["operation"] for row in details})


class BenchmarkMatrixAuditTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "run"
        self.root.mkdir()

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def assert_matrix_ok(self, profile: str) -> None:
        checks, failed = MODULE.audit_profile(self.root, profile)
        failures = [row for row in checks if row["status"] == "fail"]
        self.assertFalse(failed, failures)
        matrix = [row for row in checks if row["check"].startswith("matrix_")]
        self.assertEqual(len(matrix), 8)
        self.assertTrue(all(row["status"] == "ok" for row in matrix), matrix)

    def test_all_regular_profiles_require_the_runner_matrix(self) -> None:
        for profile in ("smoke", "quick", "standard", "full"):
            with self.subTest(profile=profile):
                create_profile_fixture(self.root, profile)
                self.assert_matrix_ok(profile)
                shutil.rmtree(self.root / profile)

    def test_representative_standard_and_full_contract_is_explicit(self) -> None:
        manifest = self.root / "manifest"
        manifest.mkdir()
        (manifest / "execution-scope.tsv").write_text(
            "key\tvalue\n"
            "suite_variant\trepresentative\n",
            encoding="utf-8",
        )
        standard = MODULE.profile_matrix_contract(self.root, "standard")
        full = MODULE.profile_matrix_contract(self.root, "full")
        self.assertEqual(standard["scenarios"], ("mixed",))
        self.assertEqual(standard["methods"], (
            "sa32-binary", "sa64-binary", "sa32-sampled-k4", "fm-huff"))
        self.assertEqual(standard["pattern_lengths"], ("50", "100", "200"))
        self.assertEqual(standard["query_repetitions"], 3)
        self.assertEqual(full["scenarios"], ("mixed",))
        self.assertEqual(full["methods"], ("sa32-binary", "caps32", "fm-huff"))
        self.assertEqual(full["pattern_lengths"], ("100",))
        self.assertEqual(full["right_methods"], (
            "right-maximal-baseline", "right-maximal-suffix-link"))

    def test_runner_exact_method_contract_is_independent_and_complete(self) -> None:
        expected = {
            "exact_methods_all": (
                "naive", "sa32-binary", "sa32-lcp-binary", "sa32-sapling",
                "sa32-child", "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2",
                "sa32-sampled-k4", "sa32-sampled-k8", "fm-huff", "fm-balanced", "fm-epr"),
            "exact_methods_indexed": (
                "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child",
                "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2", "sa32-sampled-k4",
                "sa32-sampled-k8", "fm-huff", "fm-balanced", "fm-epr"),
            "exact_methods_full": (
                "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child",
                "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2", "sa32-sampled-k4",
                "sa32-sampled-k8", "fm-huff", "fm-balanced", "fm-epr"),
        }
        runner = (ROOT / "benchmarks" / "run_server_suite.sh").read_text(encoding="utf-8")
        observed = {}
        for name in expected:
            prefix = f'{name}="'
            line = next(line for line in runner.splitlines() if line.startswith(prefix))
            observed[name] = tuple(line[len(prefix):-1].split(","))
        self.assertEqual(observed, expected)
        self.assertEqual(MODULE.EXACT_METHODS["smoke"], expected["exact_methods_all"])
        self.assertEqual(MODULE.EXACT_METHODS["quick"], expected["exact_methods_all"])
        self.assertEqual(MODULE.EXACT_METHODS["standard"], expected["exact_methods_indexed"])
        self.assertEqual(MODULE.EXACT_METHODS["full"], expected["exact_methods_full"])

    def test_strict_fm_batch_width_matrix_rejects_missing_or_extra_widths(self) -> None:
        create_profile_fixture(self.root, "smoke")
        path = self.root / "smoke" / "exact" / "raw_repetitions.tsv"
        content = rows(path)
        observed = {
            method: {row["fm_batch_width"] for row in content
                     if row["method"] == method and row["fm_query_mode"] == "batch"}
            for method in MODULE.FM_METHODS
        }
        self.assertEqual(observed, {
            method: set(MODULE.FM_BATCH_WIDTHS[method]) for method in MODULE.FM_METHODS
        })
        self.assert_matrix_ok("smoke")

        fieldnames = list(content[0])
        extra = dict(next(row for row in content
                          if row["method"] == "fm-balanced" and
                          row["fm_query_mode"] == "batch" and
                          row["fm_batch_width"] == "16"))
        extra["fm_batch_width"] = "1"
        write_rows(path, fieldnames, content + [extra])
        checks, failed = MODULE.audit_profile(self.root, "smoke")
        self.assertTrue(failed)
        matrix = {row["check"]: row for row in checks}["matrix_exact_query_slices"]
        self.assertEqual(matrix["status"], "fail")
        self.assertIn("fm-balanced/unexpected_batch_widths=1", matrix["details"])

        write_rows(path, fieldnames, [
            row for row in content
            if not (row["method"] == "fm-epr" and row["fm_query_mode"] == "batch" and
                    row["fm_batch_width"] == "16")
        ])
        checks, failed = MODULE.audit_profile(self.root, "smoke")
        self.assertTrue(failed)
        matrix = {row["check"]: row for row in checks}["matrix_exact_query_slices"]
        self.assertEqual(matrix["status"], "fail")
        self.assertIn("fm-epr", matrix["details"])

    def test_exact_metadata_contract_rejects_missing_empty_and_wrong_values(self) -> None:
        create_profile_fixture(self.root, "smoke")
        path = self.root / "smoke" / "exact" / "run_metadata.tsv"
        original = path.read_bytes()

        def require_contract_failure() -> None:
            checks, failed = MODULE.audit_profile(self.root, "smoke")
            self.assertTrue(failed)
            contract = {row["check"]: row for row in checks}["exact_metadata_contract"]
            self.assertEqual(contract["status"], "fail")

        rewrite_without(path, {"fm_batch_width_overrides"})
        require_contract_failure()

        path.write_bytes(original)
        metadata = rows(path)
        for row in metadata:
            row["fm_batch_width_overrides"] = ""
        write_rows(path, list(metadata[0]), metadata)
        require_contract_failure()

        path.write_bytes(original)
        metadata = rows(path)
        for row in metadata:
            row["fm_query_modes"] = "batch,scalar"
        write_rows(path, list(metadata[0]), metadata)
        require_contract_failure()

    def test_exact_raw_schema_explicitly_requires_fm_mode_and_width(self) -> None:
        for missing in ("fm_query_mode", "fm_batch_width"):
            with self.subTest(missing=missing):
                create_profile_fixture(self.root, "smoke")
                path = self.root / "smoke" / "exact" / "raw_repetitions.tsv"
                rewrite_without(path, {missing})
                checks, failed = MODULE.audit_profile(self.root, "smoke")
                self.assertTrue(failed)
                schema = [row for row in checks
                          if row["check"].startswith("schema:smoke/exact/")]
                self.assertEqual(len(schema), 1)
                self.assertEqual(schema[0]["status"], "fail")
                shutil.rmtree(self.root / "smoke")

    def test_hidden_failed_partial_is_ignored_by_audit_and_manifest(self) -> None:
        create_profile_fixture(self.root, "smoke")
        hidden = self.root / "smoke" / ".exact.partial.failed-attempt"
        shutil.copytree(self.root / "smoke" / "exact", hidden)
        (hidden / ".sufkit-producer-complete").write_text(
            "kind=exact\n", encoding="utf-8")
        (self.root / "smoke" / "exact" / ".sufkit-producer-complete").write_text(
            "kind=exact\n", encoding="utf-8")

        checks, failed = MODULE.audit_profile(self.root, "smoke")
        self.assertFalse(failed)
        self.assertTrue(all(".exact.partial" not in row["details"] for row in checks))
        tables = MODULE.discover_tables(self.root)
        self.assertTrue(all(".exact.partial" not in table.relative for table in tables))
        manifest = MODULE.manifest_rows(self.root, tables)
        self.assertTrue(all(".exact.partial" not in row["path"] for row in manifest))
        self.assertTrue(all(row["path"] != ".sufkit-producer-complete" for row in manifest))

    def test_missing_scenario_and_exact_method_fail_audit_and_cli(self) -> None:
        create_profile_fixture(self.root, "quick")
        exact = self.root / "quick" / "exact" / "raw_repetitions.tsv"
        rewrite_filter(exact, lambda row: row["scenario"] != "balanced")
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_expected_scenarios"]["status"], "fail")
        self.assertEqual(by_name["matrix_exact_methods"]["status"], "fail")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(MODULE.main([
                "audit", "--run-dir", str(self.root), "--profile", "quick"]), 1)

        shutil.rmtree(self.root / "quick")
        create_profile_fixture(self.root, "smoke")
        exact = self.root / "smoke" / "exact" / "raw_repetitions.tsv"
        rewrite_filter(
            exact, lambda row: not (row["phase"] == "query" and row["method"] == "sa32-child"))
        checks, failed = MODULE.audit_profile(self.root, "smoke")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_exact_methods"]["status"], "fail")
        self.assertEqual(by_name["matrix_exact_query_slices"]["status"], "fail")

    def test_missing_exact_query_or_build_repetition_fails(self) -> None:
        create_profile_fixture(self.root, "quick")
        exact = self.root / "quick" / "exact" / "raw_repetitions.tsv"
        rewrite_filter(exact, lambda row: not (
            row["scenario"] == "mixed" and row["method"] == "sa32-binary" and
            row["phase"] == "query" and row["query_group"] == "exact_unique" and
            row["pattern_length"] == "20" and row["strand"] == "forward" and
            row["operation"] == "count" and row["fm_query_mode"] == "scalar" and
            row["repetition"] == "4"))
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_exact_query_slices"]["status"], "fail")

        shutil.rmtree(self.root / "quick")
        create_profile_fixture(self.root, "quick")
        exact = self.root / "quick" / "exact" / "raw_repetitions.tsv"
        rewrite_filter(exact, lambda row: not (
            row["scenario"] == "mixed" and row["method"] == "fm-huff" and
            row["phase"] == "build" and row["repetition"] == "2"))
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_exact_build_repetitions"]["status"], "fail")

    def test_missing_sa_submatrix_tuple_or_repetition_fails(self) -> None:
        create_profile_fixture(self.root, "quick")
        path = self.root / "quick" / "sa-build-caps-default-k1" / "raw_repetitions.tsv"
        rewrite_filter(path, lambda row: not (
            row["method"] == "caps64" and row["threads"] == "32"))
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_sa_build_submatrices"]["status"], "fail")

        shutil.rmtree(self.root / "quick")
        create_profile_fixture(self.root, "quick")
        path = self.root / "quick" / "sa-build-sampled-k2-k4-k8" / "raw_repetitions.tsv"
        rewrite_filter(path, lambda row: not (
            row["sampling_rate"] == "4" and row["repetition"] == "3"))
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_sa_build_submatrices"]["status"], "fail")

    def test_missing_right_method_or_repetition_fails(self) -> None:
        create_profile_fixture(self.root, "smoke")
        path = self.root / "smoke" / "right-maximal" / "raw_repetitions.tsv"
        rewrite_filter(path, lambda row: row["method"] != "right-maximal-sampled-k8")
        checks, failed = MODULE.audit_profile(self.root, "smoke")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_right_maximal_methods"]["status"], "fail")
        self.assertEqual(by_name["matrix_right_maximal_query_slices"]["status"], "fail")
        self.assertEqual(by_name["matrix_right_maximal_build_repetitions"]["status"], "fail")

        shutil.rmtree(self.root / "smoke")
        create_profile_fixture(self.root, "quick")
        path = self.root / "quick" / "right-maximal" / "raw_repetitions.tsv"
        rewrite_filter(path, lambda row: not (
            row["method"] == "right-maximal-full" and row["operation"] == "streaming" and
            row["min_length"] == "50" and row["repetition"] == "4"))
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["matrix_right_maximal_query_slices"]["status"], "fail")

    def test_placeholder_rows_still_require_repetitions_except_capabilities(self) -> None:
        skipped = [
            {"status": "skipped_high_frequency", "repetition": str(repetition)}
            for repetition in range(5)
        ]
        self.assertIsNone(MODULE.repetition_failure(
            skipped, 5, 0, {"skipped_high_frequency"}))
        self.assertIsNotNone(MODULE.repetition_failure(
            skipped[:-1], 5, 0, {"skipped_high_frequency"}))
        capability = [{"status": "not_supported", "repetition": "0"}]
        self.assertIsNone(MODULE.repetition_failure(
            capability, 1, 0, {"not_supported"}, single_capability_placeholder=True))

    def test_high_frequency_vector_skip_is_controlled_by_streaming_preflight(self) -> None:
        streaming = [{
            "status": "ok", "repetition": str(repetition), "query_bases": "25600",
            "total_matches": "1000001", "count_checksum": "high-count",
        } for repetition in range(3)]
        skipped = [{
            "status": "skipped_high_frequency", "repetition": str(repetition),
            "query_bases": "25600", "materialization_match_threshold": "1000000",
            "vector_skipped": "1", "total_matches": "1000001",
            "reported_matches": "0", "count_checksum": "high-count",
            "result_checksum": "0", "seconds": "NA", "peak_rss_scope": "not_applicable",
        } for repetition in range(3)]
        self.assertIsNone(MODULE.controlled_right_vector_skip_failure(skipped, streaming, 3))
        mutations = {
            "missing_repetition": lambda value: value.pop(),
            "flag": lambda value: value[0].update(vector_skipped="0"),
            "threshold": lambda value: value[0].update(materialization_match_threshold="999999"),
            "total": lambda value: value[0].update(total_matches="1000002"),
            "checksum": lambda value: value[0].update(count_checksum="different"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                candidate = [dict(row) for row in skipped]
                mutate(candidate)
                self.assertIsNotNone(
                    MODULE.controlled_right_vector_skip_failure(candidate, streaming, 3))

        create_profile_fixture(self.root, "smoke")
        path = self.root / "smoke" / "right-maximal" / "raw_repetitions.tsv"
        content = rows(path)
        for row in content:
            if row["method"] not in MODULE.RIGHT_INTERNAL_METHODS or row["min_length"] != "20":
                continue
            if row["operation"] == "streaming":
                row.update(total_matches="1000001", count_checksum="high-count")
            elif row["operation"] == "vector":
                row.update({
                    "status": "skipped_high_frequency", "seconds": "NA",
                    "peak_rss_mb": "NA", "peak_rss_scope": "not_applicable",
                    "materialization_match_threshold": "1000000", "vector_skipped": "1",
                    "total_matches": "1000001", "reported_matches": "0",
                    "count_checksum": "high-count", "result_checksum": "0",
                })
        write_rows(path, list(content[0]), content)
        self.assert_matrix_ok("smoke")

    def test_sa_default_canonical_name_and_cpu_limit_placeholder(self) -> None:
        create_profile_fixture(self.root, "quick")
        # The fixture writes requested `default` as the persisted canonical
        # `suffix-link`; the complete strict audit must still pass.
        self.assert_matrix_ok("quick")

        path = self.root / "quick" / "sa-build-caps-default-k1" / "raw_repetitions.tsv"
        content = rows(path)
        retained = [row for row in content
                    if not (row["method"] == "caps64" and row["threads"] == "64")]
        placeholder = dict(next(row for row in content
                                if row["method"] == "caps64" and row["threads"] == "64"))
        placeholder["repetition"] = "0"
        placeholder["status"] = "not_applicable:threads_exceed_logical_cpus"
        for field in ("build_peak_rss_scope", "save_peak_rss_scope",
                      "load_peak_rss_scope"):
            placeholder[field] = "NA"
        write_rows(path, list(content[0]), retained + [placeholder])
        self.assert_matrix_ok("quick")

        placeholder["status"] = "not_applicable:different_reason"
        write_rows(path, list(content[0]), retained + [placeholder])
        checks, failed = MODULE.audit_profile(self.root, "quick")
        self.assertTrue(failed)
        by_name = {row["check"]: row for row in checks}
        self.assertEqual(by_name["sa-build:unexpected_status"]["status"], "fail")
        self.assertEqual(by_name["matrix_sa_build_submatrices"]["status"], "fail")


if __name__ == "__main__":
    unittest.main()
