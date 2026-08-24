#!/usr/bin/env python3
"""Render the top-level benchmark headline from a completed result package.

The managed Markdown region is derived exclusively from package TSV files.
Content outside the start/end markers is preserved byte-for-byte.  This tool
does not run benchmarks and deliberately refuses partial or failed packages.
"""

from __future__ import annotations

import argparse
import csv
import html
import math
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from typing import Mapping, Sequence


START_MARKER = "<!-- SUFKIT_HEADLINE_START -->"
END_MARKER = "<!-- SUFKIT_HEADLINE_END -->"

BUILD_COLUMNS = (
    "index",
    "builder",
    "threads",
    "build_time_seconds",
    "peak_rss_mb",
    "index_size_bytes",
    "bits_per_base",
    "speedup_vs_divsufsort",
    "status",
)
QUERY_COLUMNS = (
    "index",
    "operation",
    "query_count",
    "queries_per_second",
    "nanoseconds_per_query",
    "query_bases_per_second",
    "query_peak_rss_mb",
    "status",
)
RIGHT_COLUMNS = (
    "method",
    "query_bases_per_second",
    "matches_per_second",
    "speedup_vs_baseline",
    "status",
)
BUILD_REQUIRED_COLUMNS = BUILD_COLUMNS[:-1] + (
    "allocated_disk_bytes",
    "raw_repetitions",
    "expected_repetitions",
    "status",
)
QUERY_REQUIRED_COLUMNS = QUERY_COLUMNS[:-1] + (
    "result_checksum",
    "raw_repetitions",
    "expected_repetitions",
    "status",
)
RIGHT_REQUIRED_COLUMNS = RIGHT_COLUMNS[:-1] + (
    "query_bases",
    "seconds_median",
    "total_matches",
    "result_checksum",
    "raw_repetitions",
    "expected_repetitions",
    "status",
)
ENVIRONMENT_KEYS = (
    "server_label",
    "source_revision",
    "run_complete",
    "compiler",
    "compiler_version",
    "build_type",
    "os",
    "architecture",
    "cpu_model",
    "logical_cpus",
    "seed",
)
REQUIRED_BASE_CORRECTNESS_CHECKS = {
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
}
REQUIRED_HEADLINE_AUDIT_CHECKS = {
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
}
EXPECTED_BUILDERS = {
    "SA / divsufsort": ("divsufsort", 1),
    "SA / CaPS": ("CaPS", 64),
    "FM / SDSL Huffman": ("SDSL Huffman", 1),
}
EXPECTED_QUERIES = {
    ("SA32 default", "Count"),
    ("SA32 default", "Locate-1"),
    ("FM Huffman", "Count"),
    ("FM Huffman", "Locate-1"),
}
EXPECTED_RIGHT_METHODS = {"SA baseline", "SA suffix-link default"}
SAFE_PACKAGE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
HEX_CHECKSUM = re.compile(r"[0-9a-fA-F]{1,64}")


class HeadlineError(RuntimeError):
    """Raised when a result package cannot safely drive the README."""


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "update the managed benchmarks/README.md headline from one "
            "complete packaged benchmark result"
        )
    )
    parser.add_argument("--package-dir", required=True, type=Path)
    parser.add_argument(
        "--readme",
        type=Path,
        default=Path(__file__).resolve().parent / "README.md",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify that the managed region is current without writing it",
    )
    return parser.parse_args(argv)


def read_tsv(path: Path, required_columns: Sequence[str]) -> list[dict[str, str]]:
    if not path.is_file():
        raise HeadlineError(f"required TSV is missing: {path}")
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames is None:
            raise HeadlineError(f"TSV has no header: {path}")
        if len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise HeadlineError(f"TSV has duplicate header columns: {path}")
        missing = [column for column in required_columns if column not in reader.fieldnames]
        if missing:
            raise HeadlineError(
                f"TSV is missing required column(s) {','.join(missing)}: {path}"
            )
        rows = [dict(row) for row in reader]
    if not rows:
        raise HeadlineError(f"TSV has no data rows: {path}")
    for line, row in enumerate(rows, start=2):
        if None in row or any(value is None for value in row.values()):
            raise HeadlineError(f"malformed TSV row at {path}:{line}")
    return rows


def require_ok(rows: Sequence[Mapping[str, str]], path: Path) -> None:
    failures = [
        f"line {line}: {row.get('status', '<missing>')}"
        for line, row in enumerate(rows, start=2)
        if row.get("status") != "ok"
    ]
    if failures:
        raise HeadlineError(
            f"package contains non-ok status in {path}: " + "; ".join(failures[:8])
        )


def require_values(
    rows: Sequence[Mapping[str, str]], columns: Sequence[str], path: Path
) -> None:
    failures: list[str] = []
    for line, row in enumerate(rows, start=2):
        for column in columns:
            value = row.get(column, "")
            if not value or value == "NA":
                failures.append(f"line {line}/{column}={value or '<empty>'}")
    if failures:
        raise HeadlineError(
            f"package contains missing headline value in {path}: "
            + "; ".join(failures[:8])
        )


def require_integer(value: str, field: str, *, positive: bool = False) -> int:
    if re.fullmatch(r"[0-9]+", value) is None:
        raise HeadlineError(f"{field} is not a non-negative integer: {value}")
    parsed = int(value)
    if positive and parsed == 0:
        raise HeadlineError(f"{field} must be positive")
    return parsed


def require_float(value: str, field: str) -> float:
    try:
        parsed = float(value)
    except ValueError as error:
        raise HeadlineError(f"{field} is not a floating-point number: {value}") from error
    if not math.isfinite(parsed) or parsed < 0:
        raise HeadlineError(f"{field} must be finite and non-negative: {value}")
    return parsed


def require_checksum(value: str, field: str, *, exact_width: int | None = None) -> None:
    if HEX_CHECKSUM.fullmatch(value) is None:
        raise HeadlineError(f"{field} is not a hexadecimal checksum: {value}")
    if exact_width is not None and len(value) != exact_width:
        raise HeadlineError(f"{field} must contain {exact_width} hexadecimal digits")


def status_declaration(text: str, label: str, expected: str) -> None:
    pattern = re.compile(
        rf"(?m)^\*\*{re.escape(label)} status: ([A-Za-z0-9_-]+)\.\*\*"
    )
    statuses = pattern.findall(text)
    if statuses != [expected]:
        rendered = ",".join(statuses) if statuses else "<missing>"
        raise HeadlineError(
            f"{label.lower()} README must declare status {expected} exactly once; "
            f"observed={rendered}"
        )


def validate_correctness(
    rows: Sequence[Mapping[str, str]], path: Path
) -> None:
    require_ok(rows, path)
    index: dict[tuple[str, str], int] = {}
    for line, row in enumerate(rows, start=2):
        key = (row["workload"], row["check"])
        if key in index:
            raise HeadlineError(
                f"duplicate correctness check {key[0]}/{key[1]} at "
                f"{path}:{index[key]} and {line}"
            )
        index[key] = line
        require_integer(row["groups_checked"], f"{path}:{line}/groups_checked")
        mismatches = require_integer(
            row["mismatches"], f"{path}:{line}/mismatches"
        )
        if mismatches != 0:
            raise HeadlineError(f"non-zero mismatch count at {path}:{line}")
    missing_base = sorted(REQUIRED_BASE_CORRECTNESS_CHECKS - set(index))
    if missing_base:
        raise HeadlineError(
            "correctness summary is missing required publication check(s): "
            + "; ".join(f"{workload}/{check}" for workload, check in missing_base)
        )

    strict_expected = {
        ("profile-audit", f"headline:{check}")
        for check in REQUIRED_HEADLINE_AUDIT_CHECKS
    }
    strict_present = {
        key for key in index
        if key[0] == "profile-audit" and key[1].startswith("headline:headline_")
    }
    if strict_present:
        missing_strict = sorted(strict_expected - set(index))
        if missing_strict:
            raise HeadlineError(
                "strict package is missing required headline profile-audit check(s): "
                + "; ".join(f"{workload}/{check}" for workload, check in missing_strict)
            )
        return

    non_strict_expected = {
        ("sa-build", check) for check in REQUIRED_HEADLINE_AUDIT_CHECKS
    }
    missing_non_strict = sorted(non_strict_expected - set(index))
    if missing_non_strict:
        raise HeadlineError(
            "package provides neither the complete strict nor equivalent non-strict "
            "headline audit shape; missing: "
            + "; ".join(
                f"{workload}/{check}" for workload, check in missing_non_strict
            )
        )


def unique_environment(rows: Sequence[Mapping[str, str]], path: Path) -> dict[str, str]:
    values: dict[str, list[str]] = {}
    for row in rows:
        key = row.get("key", "")
        value = row.get("value", "")
        if key:
            values.setdefault(key, []).append(value)
    environment: dict[str, str] = {}
    for key in ENVIRONMENT_KEYS:
        candidates = values.get(key, [])
        if len(candidates) != 1 or not candidates[0] or candidates[0] == "NA":
            rendered = ",".join(candidates) if candidates else "<missing>"
            raise HeadlineError(
                f"environment key must have one non-NA value: {key}={rendered} ({path})"
            )
        environment[key] = candidates[0]
    if environment["run_complete"].lower() != "true":
        raise HeadlineError("package environment reports run_complete != true")
    if environment["source_revision"].lower() == "unknown":
        raise HeadlineError("package source revision is unknown")
    if re.fullmatch(r"[0-9a-fA-F]{7,40}", environment["source_revision"]) is None:
        raise HeadlineError("package source revision is not a Git object ID")
    require_integer(environment["logical_cpus"], "environment/logical_cpus", positive=True)
    require_integer(environment["seed"], "environment/seed")
    return environment


def validate_headline_values(
    builds: Sequence[Mapping[str, str]],
    queries: Sequence[Mapping[str, str]],
    right: Sequence[Mapping[str, str]],
    build_path: Path,
    query_path: Path,
    right_path: Path,
) -> None:
    if len(builds) != len(EXPECTED_BUILDERS):
        raise HeadlineError(
            f"headline build table requires exactly {len(EXPECTED_BUILDERS)} rows, "
            f"found {len(builds)}"
        )
    build_index: dict[str, int] = {}
    for line, row in enumerate(builds, start=2):
        name = row["index"]
        if name in build_index:
            raise HeadlineError(f"duplicate headline build index at {build_path}:{line}: {name}")
        build_index[name] = line
        if name not in EXPECTED_BUILDERS:
            raise HeadlineError(f"unexpected headline build index at {build_path}:{line}: {name}")
        expected_builder, expected_threads = EXPECTED_BUILDERS[name]
        if row["builder"] != expected_builder:
            raise HeadlineError(
                f"unexpected builder for {name}: {row['builder']} != {expected_builder}"
            )
        threads = require_integer(row["threads"], f"{build_path}:{line}/threads", positive=True)
        if threads != expected_threads:
            raise HeadlineError(
                f"unexpected thread count for {name}: {threads} != {expected_threads}"
            )
        require_float(row["build_time_seconds"], f"{build_path}:{line}/build_time_seconds")
        require_float(row["peak_rss_mb"], f"{build_path}:{line}/peak_rss_mb")
        require_integer(row["index_size_bytes"], f"{build_path}:{line}/index_size_bytes")
        require_integer(row["allocated_disk_bytes"], f"{build_path}:{line}/allocated_disk_bytes")
        require_float(row["bits_per_base"], f"{build_path}:{line}/bits_per_base")
        require_float(
            row["speedup_vs_divsufsort"],
            f"{build_path}:{line}/speedup_vs_divsufsort",
        )

    if len(queries) != len(EXPECTED_QUERIES):
        raise HeadlineError(
            f"headline query table requires exactly {len(EXPECTED_QUERIES)} rows, "
            f"found {len(queries)}"
        )
    query_index: dict[tuple[str, str], int] = {}
    for line, row in enumerate(queries, start=2):
        key = (row["index"], row["operation"])
        if key in query_index:
            raise HeadlineError(
                f"duplicate headline query row at {query_path}:{line}: {key[0]}/{key[1]}"
            )
        query_index[key] = line
        if key not in EXPECTED_QUERIES:
            raise HeadlineError(
                f"unexpected headline query row at {query_path}:{line}: {key[0]}/{key[1]}"
            )
        require_integer(row["query_count"], f"{query_path}:{line}/query_count", positive=True)
        for field in (
            "seconds_median",
            "queries_per_second",
            "nanoseconds_per_query",
            "query_bases_per_second",
            "query_peak_rss_mb",
        ):
            require_float(row[field], f"{query_path}:{line}/{field}")
        require_checksum(
            row["result_checksum"], f"{query_path}:{line}/result_checksum", exact_width=16
        )

    if len(right) != len(EXPECTED_RIGHT_METHODS):
        raise HeadlineError(
            f"headline right-maximal table requires exactly {len(EXPECTED_RIGHT_METHODS)} "
            f"rows, found {len(right)}"
        )
    right_index: dict[str, int] = {}
    for line, row in enumerate(right, start=2):
        method = row["method"]
        if method in right_index:
            raise HeadlineError(
                f"duplicate headline right-maximal method at {right_path}:{line}: {method}"
            )
        right_index[method] = line
        if method not in EXPECTED_RIGHT_METHODS:
            raise HeadlineError(
                f"unexpected headline right-maximal method at {right_path}:{line}: {method}"
            )
        require_integer(row["query_bases"], f"{right_path}:{line}/query_bases", positive=True)
        require_integer(row["total_matches"], f"{right_path}:{line}/total_matches")
        for field in (
            "seconds_median",
            "query_bases_per_second",
            "matches_per_second",
            "speedup_vs_baseline",
        ):
            require_float(row[field], f"{right_path}:{line}/{field}")
        require_checksum(row["result_checksum"], f"{right_path}:{line}/result_checksum")

    for path, rows, expected in (
        (build_path, builds, 3),
        (query_path, queries, 5),
        (right_path, right, 5),
    ):
        for line, row in enumerate(rows, start=2):
            raw_repetitions = require_integer(
                row["raw_repetitions"], f"{path}:{line}/raw_repetitions"
            )
            expected_repetitions = require_integer(
                row["expected_repetitions"], f"{path}:{line}/expected_repetitions"
            )
            if raw_repetitions != expected or expected_repetitions != expected:
                raise HeadlineError(
                    f"unexpected repetition evidence at {path}:{line}: "
                    f"raw={raw_repetitions}, expected={expected_repetitions}; "
                    f"required={expected}"
                )


def expected_long_headline(
    builds: Sequence[Mapping[str, str]],
    queries: Sequence[Mapping[str, str]],
    right: Sequence[Mapping[str, str]],
) -> dict[tuple[str, str, str], tuple[str, str, str, str]]:
    expected: dict[tuple[str, str, str], tuple[str, str, str, str]] = {}
    for row in builds:
        for metric, unit in (
            ("build_time_seconds", "seconds"),
            ("peak_rss_mb", "MiB"),
            ("index_size_bytes", "bytes"),
            ("bits_per_base", "bits/base"),
        ):
            expected[("build", row["index"], metric)] = (
                row[metric], unit, row["status"], "raw_repetitions.tsv"
            )
    for row in queries:
        expected[("exact", f"{row['index']} / {row['operation']}", "queries_per_second")] = (
            row["queries_per_second"],
            "queries/s",
            row["status"],
            "raw_repetitions.tsv",
        )
    for row in right:
        expected[("right-maximal", row["method"], "query_bases_per_second")] = (
            row["query_bases_per_second"],
            "bases/s",
            row["status"],
            "raw_repetitions.tsv",
        )
    return expected


def validate_long_headline(
    rows: Sequence[Mapping[str, str]],
    expected: Mapping[tuple[str, str, str], tuple[str, str, str, str]],
    path: Path,
) -> None:
    if len(expected) != 18:
        raise HeadlineError("internal headline contract does not contain 18 semantic rows")
    index: dict[tuple[str, str, str], tuple[str, str, str, str]] = {}
    for line, row in enumerate(rows, start=2):
        key = (row["section"], row["item"], row["metric"])
        if key in index:
            raise HeadlineError(
                f"duplicate headline semantic row at {path}:{line}: {'/'.join(key)}"
            )
        index[key] = (row["value"], row["unit"], row["status"], row["provenance"])
    missing = sorted(set(expected) - set(index))
    unexpected = sorted(set(index) - set(expected))
    if missing or unexpected:
        details = []
        if missing:
            details.append("missing=" + ";".join("/".join(key) for key in missing))
        if unexpected:
            details.append("unexpected=" + ";".join("/".join(key) for key in unexpected))
        raise HeadlineError("headline semantic row set mismatch: " + " | ".join(details))
    for key, expected_value in expected.items():
        if index[key] != expected_value:
            raise HeadlineError(
                f"headline semantic row disagrees with dedicated TSV for {'/'.join(key)}: "
                f"observed={index[key]}, expected={expected_value}"
            )


def validate_package(package_dir: Path) -> tuple[
    dict[str, str],
    list[dict[str, str]],
    list[dict[str, str]],
    list[dict[str, str]],
]:
    package_dir = package_dir.resolve()
    package_readme = package_dir / "README.md"
    if not package_readme.is_file():
        raise HeadlineError(f"package README is missing: {package_readme}")
    package_text = package_readme.read_text(encoding="utf-8")
    status_declaration(package_text, "Package", "complete")

    headline_readme = package_dir / "headline" / "README.md"
    figure = package_dir / "figures" / "headline-performance.svg"
    if not headline_readme.is_file() or headline_readme.stat().st_size == 0:
        raise HeadlineError(f"headline README is missing or empty: {headline_readme}")
    if not figure.is_file() or figure.stat().st_size == 0:
        raise HeadlineError(f"headline figure is missing or empty: {figure}")
    status_declaration(headline_readme.read_text(encoding="utf-8"), "Headline", "ok")

    environment_path = package_dir / "environment.tsv"
    environment_rows = read_tsv(environment_path, ("key", "value", "source"))
    environment = unique_environment(environment_rows, environment_path)

    correctness_path = package_dir / "correctness-summary.tsv"
    correctness = read_tsv(
        correctness_path,
        ("workload", "check", "groups_checked", "mismatches", "status", "details"),
    )
    validate_correctness(correctness, correctness_path)

    build_path = package_dir / "headline" / "headline-build.tsv"
    query_path = package_dir / "headline" / "headline-query.tsv"
    right_path = package_dir / "headline" / "headline-right-maximal.tsv"
    long_path = package_dir / "headline" / "headline.tsv"
    expected_headline_tsvs = {
        build_path.name,
        query_path.name,
        right_path.name,
        long_path.name,
    }
    observed_headline_tsvs = {
        path.name for path in (package_dir / "headline").glob("*.tsv")
    }
    if observed_headline_tsvs != expected_headline_tsvs:
        raise HeadlineError(
            "headline TSV set differs from the supported complete schema: "
            f"observed={','.join(sorted(observed_headline_tsvs)) or '<none>'}"
        )
    builds = read_tsv(build_path, BUILD_REQUIRED_COLUMNS)
    queries = read_tsv(query_path, QUERY_REQUIRED_COLUMNS)
    right = read_tsv(right_path, RIGHT_REQUIRED_COLUMNS)
    long_rows = read_tsv(
        long_path,
        ("section", "item", "metric", "value", "unit", "status", "provenance"),
    )
    require_ok(builds, build_path)
    require_ok(queries, query_path)
    require_ok(right, right_path)
    require_ok(long_rows, long_path)
    require_values(builds, BUILD_COLUMNS[:-1], build_path)
    require_values(queries, QUERY_COLUMNS[:-1], query_path)
    require_values(right, RIGHT_COLUMNS[:-1], right_path)
    require_values(
        long_rows,
        ("section", "item", "metric", "value", "unit", "provenance"),
        long_path,
    )
    validate_headline_values(builds, queries, right, build_path, query_path, right_path)
    validate_long_headline(
        long_rows, expected_long_headline(builds, queries, right), long_path
    )
    return environment, builds, queries, right


def markdown_cell(value: str) -> str:
    escaped = html.escape(value.replace("\r", " ").replace("\n", " "), quote=True)
    return re.sub(r"([\\`*{}\[\]()#+!_|])", r"\\\1", escaped)


def markdown_table(
    headers: Sequence[str],
    columns: Sequence[str],
    rows: Sequence[Mapping[str, str]],
) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join("---" for _ in headers) + "|",
    ]
    for row in rows:
        lines.append(
            "| " + " | ".join(markdown_cell(row[column]) for column in columns) + " |"
        )
    return "\n".join(lines)


def render_environment(environment: Mapping[str, str]) -> str:
    compiler = " ".join(
        part for part in (environment["compiler"], environment["compiler_version"]) if part
    )
    return (
        f"Environment: {markdown_cell(environment['server_label'])}; "
        f"{markdown_cell(environment['cpu_model'])}; "
        f"{markdown_cell(environment['os'])} {markdown_cell(environment['architecture'])}; "
        f"{markdown_cell(compiler)}; {markdown_cell(environment['build_type'])}; "
        f"{markdown_cell(environment['logical_cpus'])} logical CPUs; "
        f"seed {markdown_cell(environment['seed'])}; source "
        f"`{markdown_cell(environment['source_revision'])}`."
    )


def validate_package_location(package_dir: Path, readme: Path) -> None:
    if readme.name != "README.md" or readme.parent.name != "benchmarks":
        raise HeadlineError("target README must be benchmarks/README.md")
    package_name = package_dir.name
    if SAFE_PACKAGE_NAME.fullmatch(package_name) is None:
        raise HeadlineError(
            "package directory name must use only letters, digits, dot, underscore, and hyphen"
        )
    expected_parent = (readme.parent / "results").resolve()
    if package_dir.parent != expected_parent:
        raise HeadlineError(
            f"package directory must be benchmarks/results/<name>: {package_dir}"
        )


def package_relative_to_readme(package_dir: Path, readme: Path) -> str:
    try:
        relative = os.path.relpath(package_dir.resolve(), readme.resolve().parent)
    except ValueError as error:
        raise HeadlineError("package and README must be on the same filesystem") from error
    relative_path = Path(relative)
    if relative_path.parts and relative_path.parts[0] == "..":
        raise HeadlineError("package directory must be below the benchmark README directory")
    return relative_path.as_posix()


def render_managed_region(
    package_dir: Path,
    readme: Path,
    environment: Mapping[str, str],
    builds: Sequence[Mapping[str, str]],
    queries: Sequence[Mapping[str, str]],
    right: Sequence[Mapping[str, str]],
) -> str:
    package_link = package_relative_to_readme(package_dir, readme)
    build_table = markdown_table(
        (
            "Index",
            "Builder",
            "Threads",
            "Build (s)",
            "Peak RSS (MiB)",
            "Index size (bytes)",
            "bits/base",
            "Speedup vs divsufsort",
        ),
        BUILD_COLUMNS[:-1],
        builds,
    )
    query_table = markdown_table(
        (
            "Index",
            "Operation",
            "Queries",
            "Queries/s",
            "ns/query",
            "Query bases/s",
            "Query RSS (MiB)",
        ),
        QUERY_COLUMNS[:-1],
        queries,
    )
    right_table = markdown_table(
        ("Method", "Query bases/s", "Matches/s", "Speedup vs baseline"),
        RIGHT_COLUMNS[:-1],
        right,
    )
    return f"""{START_MARKER}
<!-- Generated by benchmarks/update_benchmark_readme.py; do not edit this region. -->

## Latest reproducible server headline

{render_environment(environment)}

- [Complete result package]({package_link}/README.md)
- [Headline methodology and raw-derived tables]({package_link}/headline/README.md)

### Construction

{build_table}

### Exact count and locate-1

{query_table}

### Right-maximal exact match

{right_table}

![Headline performance]({package_link}/figures/headline-performance.svg)

{END_MARKER}"""


def replace_managed_region(original: str, managed: str) -> str:
    start_count = original.count(START_MARKER)
    end_count = original.count(END_MARKER)
    if start_count != 1 or end_count != 1:
        raise HeadlineError(
            "README must contain exactly one SUFKIT_HEADLINE_START/END marker pair"
        )
    start = original.index(START_MARKER)
    end = original.index(END_MARKER)
    if end < start:
        raise HeadlineError("README headline markers are out of order")
    end += len(END_MARKER)
    return original[:start] + managed + original[end:]


def atomic_write(path: Path, text: str) -> None:
    original_mode = stat.S_IMODE(path.stat().st_mode)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=path.parent,
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, original_mode)
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def run(args: argparse.Namespace) -> int:
    package_dir = args.package_dir.resolve()
    readme = args.readme.resolve()
    if not readme.is_file():
        raise HeadlineError(f"benchmark README is missing: {readme}")
    validate_package_location(package_dir, readme)
    environment, builds, queries, right = validate_package(package_dir)
    managed = render_managed_region(
        package_dir, readme, environment, builds, queries, right
    )
    with readme.open("r", encoding="utf-8", newline="") as stream:
        original = stream.read()
    expected = replace_managed_region(original, managed)
    if args.check:
        if expected != original:
            print(
                f"benchmark README headline is out of date for package: {package_dir}",
                file=sys.stderr,
            )
            return 1
        return 0
    if expected != original:
        atomic_write(readme, expected)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    try:
        return run(parse_args(sys.argv[1:] if argv is None else argv))
    except (OSError, csv.Error, HeadlineError) as error:
        print(f"benchmark README update error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
