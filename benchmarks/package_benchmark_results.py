#!/usr/bin/env python3
"""Package a sufkit server benchmark run into reviewable repository artifacts.

The headline is deliberately rebuilt from raw_repetitions.tsv files.  Summary
TSVs are discovered and inventoried, but are never used as a numeric fallback
for a missing headline field.  This makes schema gaps visible as ``NA`` and a
``partial`` status instead of silently publishing numbers with a different
measurement provenance.

Only Python's standard library is required.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import math
import os
from pathlib import Path
import re
import secrets
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass
from typing import Iterable, Mapping, Sequence


NA = "NA"
EXPECTED_NON_OK = {
    "not_applicable",
    "not_supported",
    "skipped_high_frequency",
    "unsupported_input_size",
    "not_applicable:threads_exceed_logical_cpus",
}
HEADLINE_GROUPS = {
    "exact_unique",
    "exact_repetitive",
    "mutated_low_hit",
    "random_no_hit",
}
EXACT_QUERY_GROUPS = (
    "exact_unique", "exact_repetitive", "mutated_low_hit", "random_no_hit",
    "n_boundary", "contig_boundary", "reverse_complement",
)
EXACT_PATTERN_LENGTHS = ("20", "50", "100", "200", "500")
EXACT_STRANDS = ("forward", "reverse-complement", "both")
EXACT_LOCATE_LIMITS = ("1", "10", "1000", "all")
FM_METHODS = ("fm-huff", "fm-balanced", "fm-epr")
FM_BATCH_WIDTHS = {
    "fm-huff": ("1", "4", "8", "16", "32"),
    "fm-balanced": ("16", "32"),
    "fm-epr": ("16", "32"),
}
REPRESENTATIVE_EXACT_METADATA = {
    "fm_query_modes": "scalar",
    "fm_batch_widths": "16",
    "fm_batch_width_overrides": "none",
}
EXACT_METADATA_COLUMNS = (
    "fm_query_modes", "fm_batch_widths", "fm_batch_width_overrides",
)
REGULAR_EXACT_METADATA = {
    "fm_query_modes": "scalar,batch",
    "fm_batch_widths": "1,4,8,16,32",
    "fm_batch_width_overrides": "fm-balanced:16,32;fm-epr:16,32",
}
HEADLINE_EXACT_METADATA = {
    "fm_query_modes": "scalar",
    "fm_batch_widths": "16",
    "fm_batch_width_overrides": "none",
}
RIGHT_QUERY_OPERATIONS = ("streaming", "vector", "max_matches=0", "max_matches=1000")
RIGHT_VECTOR_MATERIALIZATION_THRESHOLD = "1000000"
RIGHT_INTERNAL_METHODS = (
    "right-maximal-baseline",
    "right-maximal-lcp",
    "right-maximal-child",
    "right-maximal-suffix-link",
    "right-maximal-suffix-link-binary",
    "right-maximal-suffix-link-sapling",
    "right-maximal-full",
    "right-maximal-sampled-k4",
    "right-maximal-sampled-k8",
)
PROFILE_SCENARIOS = {
    "smoke": ("mixed",),
    "quick": ("mixed", "balanced", "gc-skewed", "repeat-rich", "n-islands", "many-contig"),
    "standard": ("mixed", "balanced", "gc-skewed", "repeat-rich", "n-islands", "many-contig"),
    "full": ("mixed", "repeat-rich", "many-contig"),
}
PROFILE_BUILD_REPETITIONS = {"smoke": 1, "quick": 3, "standard": 3, "full": 1}
PROFILE_QUERY_REPETITIONS = {"smoke": 3, "quick": 5, "standard": 7, "full": 5}
EXACT_METHODS = {
    "smoke": (
        "naive", "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child",
        "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2", "sa32-sampled-k4",
        "sa32-sampled-k8", *FM_METHODS,
    ),
    "quick": (
        "naive", "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child",
        "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2", "sa32-sampled-k4",
        "sa32-sampled-k8", *FM_METHODS,
    ),
    "standard": (
        "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child",
        "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2", "sa32-sampled-k4",
        "sa32-sampled-k8", *FM_METHODS,
    ),
    "full": (
        "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child",
        "sa64-binary", "sa64-lcp-binary", "sa32-sampled-k2", "sa32-sampled-k4",
        "sa32-sampled-k8", *FM_METHODS,
    ),
}


def _sa_build_matrix(
    *, include_full_and_sapling: bool, caps_threads: tuple[str, ...],
    sampled_rates: tuple[str, ...]) -> dict[str, tuple[tuple[str, str, str, str], ...]]:
    matrix = {
        "sa-build-none-k1": (
            ("div32", "1", "1", "none"), ("div64", "1", "1", "none")),
        "sa-build-default-k1": (
            ("div32", "1", "1", "default"), ("div64", "1", "1", "default")),
        "sa-build-sampled-k2-k4-k8" if sampled_rates == ("2", "4", "8")
        else "sa-build-sampled-k4-k8": tuple(
            ("div32", "1", rate, "default") for rate in sampled_rates),
        "sa-build-caps-default-k1" if caps_threads != ("64",)
        else "sa-build-caps-default-k1-t64": tuple(
            (method, thread, "1", "default")
            for method in ("caps32", "caps64") for thread in caps_threads),
    }
    if include_full_and_sapling:
        matrix["sa-build-full-k1"] = (("div32", "1", "1", "full"),)
        matrix["sa-build-sapling-k1"] = (("div32", "1", "1", "sapling"),)
    return matrix


SA_BUILD_MATRICES = {
    "smoke": _sa_build_matrix(
        include_full_and_sapling=True, caps_threads=("1", "8", "32", "64"),
        sampled_rates=("2", "4", "8")),
    "quick": _sa_build_matrix(
        include_full_and_sapling=True, caps_threads=("1", "8", "32", "64"),
        sampled_rates=("2", "4", "8")),
    "standard": _sa_build_matrix(
        include_full_and_sapling=True, caps_threads=("1", "8", "32", "64"),
        sampled_rates=("2", "4", "8")),
    "full": _sa_build_matrix(
        include_full_and_sapling=False, caps_threads=("64",), sampled_rates=("4", "8")),
}
REPRESENTATIVE_PROFILE_SCENARIOS = {
    "standard": ("mixed",),
    "full": ("mixed",),
}
REPRESENTATIVE_PROFILE_BUILD_REPETITIONS = {"standard": 1, "full": 1}
REPRESENTATIVE_PROFILE_QUERY_REPETITIONS = {"standard": 3, "full": 3}
REPRESENTATIVE_EXACT_METHODS = {
    "standard": ("sa32-binary", "sa64-binary", "sa32-sampled-k4", "fm-huff"),
    "full": ("sa32-binary", "caps32", "fm-huff"),
}
REPRESENTATIVE_EXACT_PATTERN_LENGTHS = {
    "standard": ("50", "100", "200"),
    "full": ("100",),
}
REPRESENTATIVE_EXACT_STRANDS = {
    "standard": ("forward", "both"),
    "full": ("forward",),
}
REPRESENTATIVE_EXACT_LOCATE_LIMITS = {
    "standard": ("1", "1000"),
    "full": ("1",),
}
REPRESENTATIVE_SA_BUILD_MATRICES = {
    "standard": {
        "sa-build-default-k1": (("div32", "1", "1", "default"), ("div64", "1", "1", "default")),
        "sa-build-sampled-k4": (("div32", "1", "4", "default"),),
        "sa-build-caps-default-k1-t64": (("caps32", "64", "1", "default"),),
    },
    "full": {
        "sa-build-default-k1": (("div32", "1", "1", "default"),),
        "sa-build-caps-default-k1-t64": (("caps32", "64", "1", "default"),),
    },
}
REPRESENTATIVE_RIGHT_METHODS = {
    "standard": ("right-maximal-baseline", "right-maximal-suffix-link", "right-maximal-full"),
    "full": ("right-maximal-baseline", "right-maximal-suffix-link"),
}
REPRESENTATIVE_RIGHT_MIN_LENGTHS = {
    "standard": ("50", "100"),
    "full": ("50", "100"),
}
PROFILE_NAMES = {"smoke", "quick", "standard", "full", "user"}
HEADLINE_BUILD_COLUMNS = [
    "index", "builder", "threads", "build_time_seconds", "peak_rss_mb",
    "index_size_bytes", "allocated_disk_bytes", "bits_per_base",
    "speedup_vs_divsufsort", "raw_repetitions", "expected_repetitions", "status",
]
HEADLINE_EXACT_COLUMNS = [
    "index", "operation", "query_count", "seconds_median", "queries_per_second",
    "nanoseconds_per_query", "query_bases_per_second", "query_peak_rss_mb",
    "result_checksum", "raw_repetitions", "expected_repetitions", "status",
]
HEADLINE_RIGHT_COLUMNS = [
    "method", "query_bases", "seconds_median", "query_bases_per_second",
    "matches_per_second", "total_matches", "speedup_vs_baseline", "result_checksum",
    "raw_repetitions", "expected_repetitions", "status",
]
BUILD_AGG_COLUMNS = [
    "source_scope", "profile", "method", "effective_backend", "backend_signature",
    "coordinate_width", "threads", "sampling_rate", "suffix_count", "acceleration",
    "reference_read_seconds_median", "normalization_seconds_median", "sa_seconds_median",
    "isa_seconds_median", "lcp_seconds_median", "child_seconds_median",
    "sapling_seconds_median", "build_seconds_median", "build_seconds_min",
    "build_seconds_max", "build_peak_rss_mb_median", "save_seconds_median",
    "save_peak_rss_mb_median", "load_seconds_median", "load_peak_rss_mb_median",
    "serialized_bytes_median", "allocated_disk_bytes_median", "bits_per_base_median",
    "learned_index_bytes_median", "repetitions", "status",
]
EXACT_AGG_COLUMNS = [
    "source_scope", "profile", "dataset", "scenario", "method", "query_group",
    "pattern_length", "strand", "operation", "max_hits", "fm_query_mode",
    "fm_batch_width", "query_count", "skipped_high_frequency_queries", "safety_status",
    "seconds_median", "seconds_min", "seconds_max",
    "qps_median", "nanoseconds_per_query_median", "query_bases",
    "query_bases_per_second", "peak_rss_mb", "total_hits", "reported_hits",
    "result_checksum", "repetitions", "status",
]
RIGHT_AGG_COLUMNS = [
    "source_scope", "profile", "dataset", "method", "operation", "min_length",
    "seconds_median", "seconds_min", "seconds_max", "query_bases",
    "query_bases_per_second", "matches_per_second", "total_matches", "reported_matches",
    "count_checksum", "result_checksum", "repetitions", "status",
]


class PackageError(RuntimeError):
    pass


@dataclass(frozen=True)
class Table:
    path: Path
    relative: str
    name: str
    columns: tuple[str, ...]
    rows: tuple[dict[str, str], ...]
    workload: str
    profile: str
    scope: str


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Package raw sufkit server benchmark TSVs without inventing missing values")
    commands = parser.add_subparsers(dest="command", required=True)
    audit = commands.add_parser("audit", help="validate one completed profile without writing reports")
    audit.add_argument("--run-dir", required=True, type=Path)
    audit.add_argument("--profile", required=True,
                       choices=["smoke", "quick", "standard", "full", "headline"])
    package_command = commands.add_parser("package", help="create a repository-sized result package")
    package_command.add_argument("--run-dir", required=True, type=Path,
                                 help="server run directory containing smoke/quick/standard/full/headline")
    package_command.add_argument("--output-dir", required=True, type=Path,
                                 help="new repository result directory to create")
    package_command.add_argument("--server-label", default="server",
                                 help="non-identifying server label written to reports")
    package_command.add_argument("--revision", default=None,
                                 help="source revision override; otherwise inferred from the run manifest")
    package_command.add_argument("--require-complete", action="store_true",
                                 help="return non-zero when required headline raw fields are unavailable")
    return parser.parse_args(argv)


def clean_cell(value: object) -> str:
    if value is None:
        return NA
    text = str(value).replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return text if text else NA


def valid_number(value: object) -> bool:
    if value is None:
        return False
    text = str(value).strip()
    if not text or text.upper() == NA:
        return False
    try:
        return math.isfinite(float(text))
    except ValueError:
        return False


def number(value: object) -> float | None:
    return float(str(value)) if valid_number(value) else None


def integer(value: object) -> int | None:
    parsed = number(value)
    return int(parsed) if parsed is not None else None


def median(values: Iterable[object]) -> float | None:
    parsed = [value for item in values if (value := number(item)) is not None]
    return statistics.median(parsed) if parsed else None


def minimum(values: Iterable[object]) -> float | None:
    parsed = [value for item in values if (value := number(item)) is not None]
    return min(parsed) if parsed else None


def maximum(values: Iterable[object]) -> float | None:
    parsed = [value for item in values if (value := number(item)) is not None]
    return max(parsed) if parsed else None


def format_number(value: object, digits: int = 6) -> str:
    parsed = number(value)
    if parsed is None:
        return NA
    if abs(parsed) >= 1.0e12:
        return f"{parsed:.6e}"
    text = f"{parsed:.{digits}f}".rstrip("0").rstrip(".")
    return text if text else "0"


def format_integer(value: object) -> str:
    parsed = integer(value)
    return str(parsed) if parsed is not None else NA


def ratio(numerator: object, denominator: object) -> float | None:
    first = number(numerator)
    second = number(denominator)
    if first is None or second is None or second == 0.0:
        return None
    return first / second


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def profile_from_path(relative: Path) -> str:
    for part in relative.parts:
        if part in PROFILE_NAMES:
            return part
    if relative.parts and relative.parts[0] == "headline":
        return "full"
    return "unknown"


def scope_from_path(relative: Path) -> str:
    return str(relative.parent).replace(os.sep, "/") or "."


def classify_raw(columns: set[str]) -> str:
    if {"phase", "query_group", "pattern_length"}.issubset(columns):
        return "exact"
    if {"effective_backend", "build_wall_seconds", "sa_checksum"}.issubset(columns):
        return "sa-build"
    if {"method", "operation", "min_length", "total_matches"}.issubset(columns):
        return "right-maximal"
    return "unknown"


def classify_table(name: str, columns: set[str]) -> str:
    if name == "correctness_summary.tsv" and "oracle" in columns:
        return "right-maximal"
    if name == "raw_repetitions.tsv":
        return classify_raw(columns)
    if "min_length" in columns or "total_matches" in columns:
        return "right-maximal"
    if "effective_backend" in columns or "suffix_count" in columns:
        return "sa-build"
    if "query_group" in columns or "fm_query_mode" in columns or "run_id" in columns:
        return "exact"
    return "metadata"


def read_table(run_dir: Path, path: Path) -> Table:
    relative_path = path.relative_to(run_dir)
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames is None:
            raise PackageError(f"empty TSV: {relative_path}")
        columns = tuple(reader.fieldnames)
        if len(columns) != len(set(columns)):
            raise PackageError(f"duplicate TSV column in {relative_path}")
        rows: list[dict[str, str]] = []
        for line_number, raw in enumerate(reader, start=2):
            if None in raw:
                raise PackageError(f"too many columns in {relative_path}:{line_number}")
            row = {key: clean_cell(raw.get(key)) for key in columns}
            row["_source_file"] = str(relative_path).replace(os.sep, "/")
            row["_source_scope"] = scope_from_path(relative_path)
            row["_profile"] = profile_from_path(relative_path)
            row["_source_line"] = str(line_number)
            rows.append(row)
    workload = classify_table(path.name, set(columns))
    return Table(path, str(relative_path).replace(os.sep, "/"), path.name,
                 columns, tuple(rows), workload, profile_from_path(relative_path),
                 scope_from_path(relative_path))


def is_hidden_run_path(run_dir: Path, path: Path) -> bool:
    """Ignore retained producer partials and other hidden run artifacts."""
    relative = path.relative_to(run_dir)
    return any(component.startswith(".") for component in relative.parts)


def discover_tables(run_dir: Path) -> list[Table]:
    names = {"run_metadata.tsv", "build_results.tsv", "query_results.tsv", "raw_repetitions.tsv",
             "correctness_summary.tsv"}
    paths = sorted(path for path in run_dir.rglob("*.tsv")
                   if path.name in names and not is_hidden_run_path(run_dir, path))
    if not paths:
        raise PackageError(f"no benchmark TSV files found below {run_dir}")
    return [read_table(run_dir, path) for path in paths]


def discover_profile_tables(run_dir: Path, profile: str) -> list[Table]:
    root = run_dir / ("headline" if profile == "headline" else profile)
    if not root.is_dir():
        return []
    names = {"run_metadata.tsv", "build_results.tsv", "query_results.tsv", "raw_repetitions.tsv",
             "correctness_summary.tsv"}
    paths = sorted(path for path in root.rglob("*.tsv")
                   if path.name in names and not is_hidden_run_path(run_dir, path))
    return [read_table(run_dir, path) for path in paths]


def raw_tables(tables: Sequence[Table], workload: str | None = None,
               headline_only: bool = False) -> list[Table]:
    selected = [table for table in tables if table.name == "raw_repetitions.tsv"]
    if workload is not None:
        selected = [table for table in selected if table.workload == workload]
    if headline_only:
        selected = [table for table in selected if table.relative.startswith("headline/")]
    return selected


def all_rows(tables: Iterable[Table]) -> list[dict[str, str]]:
    return [row for table in tables for row in table.rows]


def row_status(row: Mapping[str, str]) -> str:
    return row.get("status", "ok").strip().lower()


def ok_rows(rows: Iterable[dict[str, str]]) -> list[dict[str, str]]:
    return [row for row in rows if row_status(row) == "ok"]


def first_present(row: Mapping[str, str], names: Sequence[str]) -> str:
    for name in names:
        value = row.get(name)
        if value is not None and value.upper() != NA and value != "":
            return value
    return NA


def numeric_median(rows: Sequence[Mapping[str, str]], names: Sequence[str]) -> float | None:
    values = [first_present(row, names) for row in rows]
    return median(values)


def constant_or_na(rows: Sequence[Mapping[str, str]], names: Sequence[str]) -> str:
    values = {first_present(row, names) for row in rows}
    values.discard(NA)
    return next(iter(values)) if len(values) == 1 else NA


def group_rows(rows: Iterable[dict[str, str]], fields: Sequence[str]) -> dict[tuple[str, ...], list[dict[str, str]]]:
    result: dict[tuple[str, ...], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        result[tuple(row.get(field, NA) for field in fields)].append(row)
    return dict(result)


def method_rows(rows: Sequence[dict[str, str]], aliases: Sequence[str]) -> list[dict[str, str]]:
    return [row for row in rows if row.get("method") in aliases]


HEADLINE_BUILD_METHODS = ("sa32-binary", "caps32", "fm-huff")


def unified_headline_build_rows(tables: Sequence[Table]) -> tuple[str | None, list[dict[str, str]]]:
    """Select one exact-worker scope containing all three headline builders.

    ``headline/exact-build`` is preferred when present, followed by the normal
    ``headline/exact`` scope.  Rows from separate scopes are never pooled.
    """
    raw = ok_rows(all_rows(raw_tables(tables, "exact", headline_only=True)))
    raw = [row for row in raw if row.get("phase") == "build" and
           row.get("method") in HEADLINE_BUILD_METHODS]
    candidates: list[tuple[int, str, list[dict[str, str]]]] = []
    for (scope,), rows in group_rows(raw, ["_source_scope"]).items():
        methods = {row.get("method") for row in rows}
        if methods != set(HEADLINE_BUILD_METHODS):
            continue
        leaf = scope.rsplit("/", 1)[-1]
        priority = 0 if leaf == "exact-build" else (1 if leaf == "exact" else 2)
        candidates.append((priority, scope, rows))
    if not candidates:
        return None, []
    _, scope, rows = min(candidates, key=lambda value: (value[0], value[1]))
    return scope, rows


def aggregate_build_headline(tables: Sequence[Table]) -> list[dict[str, str]]:
    _, exact = unified_headline_build_rows(tables)
    specs = [
        ("SA / divsufsort", "divsufsort", lambda row: row.get("method") == "sa32-binary" and
         row.get("threads") == "1"),
        ("SA / CaPS", "CaPS", lambda row: row.get("method") == "caps32" and
         row.get("threads") == "64"),
        ("FM / SDSL Huffman", "SDSL Huffman", lambda row: row.get("method") == "fm-huff"),
    ]
    output: list[dict[str, str]] = []
    for index_name, builder, predicate in specs:
        rows = [row for row in exact if predicate(row)]
        if not rows:
            output.append({
                "index": index_name, "builder": builder, "threads": NA,
                "build_time_seconds": NA, "peak_rss_mb": NA, "index_size_bytes": NA,
                "allocated_disk_bytes": NA, "bits_per_base": NA,
                "speedup_vs_divsufsort": NA, "raw_repetitions": "0",
                "expected_repetitions": "3",
                "status": "partial_missing_raw_rows",
            })
            continue
        build_seconds = numeric_median(rows, ["build_wall_seconds", "seconds", "build_seconds"])
        peak = numeric_median(rows, ["build_peak_rss_mb", "peak_rss_mb"])
        serialized = numeric_median(rows, ["serialized_bytes"])
        allocated = numeric_median(rows, ["allocated_disk_bytes"])
        total_bases = numeric_median(rows, ["total_bases"])
        bits = ratio(serialized * 8.0 if serialized is not None else None, total_bases)
        required = [build_seconds, peak, serialized]
        expected_repetitions = 3
        repetitions = [integer(row.get("repetition")) for row in rows]
        complete = all(value is not None for value in required) and \
            len(rows) == expected_repetitions and set(repetitions) == {0, 1, 2}
        output.append({
            "index": index_name,
            "builder": builder,
            "threads": constant_or_na(rows, ["threads"]),
            "build_time_seconds": format_number(build_seconds),
            "peak_rss_mb": format_number(peak),
            "index_size_bytes": format_integer(serialized),
            "allocated_disk_bytes": format_integer(allocated),
            "bits_per_base": format_number(bits),
            "speedup_vs_divsufsort": NA,
            "raw_repetitions": str(len(rows)),
            "expected_repetitions": str(expected_repetitions),
            "status": "ok" if complete else "partial_missing_raw_fields_or_repetitions",
        })
    baseline = number(output[0]["build_time_seconds"]) if output else None
    for row in output:
        row["speedup_vs_divsufsort"] = format_number(ratio(baseline, row["build_time_seconds"]))
    return output


def aggregate_exact_headline(tables: Sequence[Table]) -> list[dict[str, str]]:
    query_tables = [table for table in raw_tables(tables, "exact", headline_only=True)
                    if table.scope.rsplit("/", 1)[-1] == "exact-query"]
    raw = ok_rows(all_rows(query_tables))
    raw = [row for row in raw if row.get("phase") == "query" and
           row.get("query_group") in HEADLINE_GROUPS and row.get("pattern_length") == "100" and
           row.get("strand") == "forward" and row.get("fm_query_mode", "scalar") in {"scalar", NA}]
    specs = [
        ("SA32 default", {"sa32-binary", "sa32-lcp-binary", "sa32", "sa32-default"}),
        ("FM Huffman", {"fm", "fm-huff"}),
    ]
    operations = [("count", None, "Count"), ("locate", "1", "Locate-1")]
    output: list[dict[str, str]] = []
    for display, aliases in specs:
        for operation, max_hits, operation_display in operations:
            selected = [row for row in raw if row.get("method") in aliases and
                        row.get("operation") == operation and
                        (max_hits is None or row.get("max_hits") == max_hits)]
            repetitions = group_rows(selected, ["repetition"])
            per_rep: list[dict[str, float | str]] = []
            for repetition, rows in sorted(repetitions.items()):
                groups = {row.get("query_group") for row in rows}
                if groups != HEADLINE_GROUPS:
                    continue
                seconds_values = [number(row.get("seconds")) for row in rows]
                counts = [number(row.get("query_count")) for row in rows]
                bases = [number(row.get("query_bases")) for row in rows]
                if any(value is None for value in seconds_values + counts):
                    continue
                seconds = sum(value for value in seconds_values if value is not None)
                query_count = sum(value for value in counts if value is not None)
                query_bases = (sum(value for value in bases if value is not None)
                               if bases and all(value is not None for value in bases) else None)
                rss = maximum(row.get("peak_rss_mb") for row in rows)
                checksums = "|".join(sorted(
                    f"{row.get('query_group')}={row.get('result_checksum', NA)}" for row in rows))
                checksum = hashlib.sha256(checksums.encode("utf-8")).hexdigest()[:16]
                per_rep.append({
                    "seconds": seconds,
                    "query_count": query_count,
                    "query_bases": query_bases if query_bases is not None else NA,
                    "qps": ratio(query_count, seconds) or 0.0,
                    "ns_per_query": ratio(seconds * 1.0e9, query_count) or 0.0,
                    "query_bases_per_second": ratio(query_bases, seconds) if query_bases is not None else NA,
                    "peak_rss_mb": rss if rss is not None else NA,
                    "checksum": checksum,
                })
            checksums = {str(row["checksum"]) for row in per_rep}
            required = [median(row["qps"] for row in per_rep),
                        median(row["peak_rss_mb"] for row in per_rep)]
            expected_repetitions = 5
            status = "ok" if len(per_rep) == expected_repetitions and all(value is not None for value in required) and len(checksums) == 1 \
                else ("checksum_mismatch" if len(checksums) > 1 else "partial_missing_raw_rows_or_fields")
            output.append({
                "index": display,
                "operation": operation_display,
                "query_count": format_integer(median(row["query_count"] for row in per_rep)),
                "seconds_median": format_number(median(row["seconds"] for row in per_rep)),
                "queries_per_second": format_number(median(row["qps"] for row in per_rep)),
                "nanoseconds_per_query": format_number(median(row["ns_per_query"] for row in per_rep)),
                "query_bases_per_second": format_number(median(row["query_bases_per_second"] for row in per_rep)),
                "query_peak_rss_mb": format_number(median(row["peak_rss_mb"] for row in per_rep)),
                "result_checksum": next(iter(checksums)) if len(checksums) == 1 else NA,
                "raw_repetitions": str(len(per_rep)),
                "expected_repetitions": str(expected_repetitions),
                "status": status,
            })
    return output


def aggregate_right_maximal_headline(tables: Sequence[Table]) -> list[dict[str, str]]:
    raw = ok_rows(all_rows(raw_tables(tables, "right-maximal", headline_only=True)))
    specs = [
        ("SA baseline", {"right-maximal-baseline"}),
        ("SA suffix-link default", {"right-maximal-suffix-link"}),
    ]
    output: list[dict[str, str]] = []
    for display, aliases in specs:
        selected = [row for row in raw if row.get("method") in aliases and
                    row.get("operation") == "streaming" and row.get("min_length") == "50"]
        per_rep: list[dict[str, object]] = []
        for _, rows in sorted(group_rows(selected, ["repetition"]).items()):
            if len(rows) != 1:
                continue
            row = rows[0]
            seconds = number(row.get("seconds"))
            matches = number(row.get("total_matches"))
            query_bases = number(row.get("query_bases"))
            if seconds is None or matches is None:
                continue
            per_rep.append({
                "seconds": seconds,
                "matches_per_second": ratio(matches, seconds),
                "query_bases_per_second": ratio(query_bases, seconds),
                "query_bases": query_bases if query_bases is not None else NA,
                "total_matches": matches,
                "checksum": row.get("result_checksum", NA),
            })
        checksums = {str(row["checksum"]) for row in per_rep}
        qbps = median(row["query_bases_per_second"] for row in per_rep)
        mps = median(row["matches_per_second"] for row in per_rep)
        expected_repetitions = 5
        status = "ok" if len(per_rep) == expected_repetitions and qbps is not None and mps is not None and len(checksums) == 1 \
            else ("checksum_mismatch" if len(checksums) > 1 else "partial_missing_query_bases_or_rows")
        output.append({
            "method": display,
            "query_bases": format_integer(median(row["query_bases"] for row in per_rep)),
            "seconds_median": format_number(median(row["seconds"] for row in per_rep)),
            "query_bases_per_second": format_number(qbps),
            "matches_per_second": format_number(mps),
            "total_matches": format_integer(median(row["total_matches"] for row in per_rep)),
            "speedup_vs_baseline": NA,
            "result_checksum": next(iter(checksums)) if len(checksums) == 1 else NA,
            "raw_repetitions": str(len(per_rep)),
            "expected_repetitions": str(expected_repetitions),
            "status": status,
        })
    baseline_seconds = number(output[0]["seconds_median"]) if output else None
    for row in output:
        row["speedup_vs_baseline"] = format_number(ratio(baseline_seconds, row["seconds_median"]))
    return output


def aggregate_exact_rows(tables: Sequence[Table]) -> list[dict[str, str]]:
    raw = [row for row in all_rows(raw_tables(tables, "exact"))
           if row.get("phase") == "query"]
    keys = ["_source_scope", "_profile", "dataset", "scenario", "method", "query_group",
            "pattern_length", "strand", "operation", "max_hits", "fm_query_mode", "fm_batch_width"]
    output: list[dict[str, str]] = []
    for key, source_rows in sorted(group_rows(raw, keys).items()):
        rows = ok_rows(source_rows)
        seconds = [row.get("seconds") for row in rows]
        query_count = numeric_median(source_rows, ["query_count"])
        sec_median = median(seconds)
        qps = ratio(query_count, sec_median)
        query_bases = numeric_median(source_rows, ["query_bases"])
        checksums = {row.get("result_checksum", NA) for row in rows}
        totals = {row.get("total_hits", NA) for row in rows}
        reported = {row.get("reported_hits", NA) for row in rows}
        skipped = constant_or_na(source_rows, ["skipped_high_frequency_queries"])
        safety_status = constant_or_na(source_rows, ["safety_status", "status"])
        source_statuses = sorted({row_status(row) for row in source_rows})
        stable = bool(rows) and len(checksums) == len(totals) == len(reported) == 1
        identity = dict(zip(keys, key))
        identity["source_scope"] = identity.pop("_source_scope")
        identity["profile"] = identity.pop("_profile")
        output.append(identity | {
            "query_count": format_integer(query_count),
            "skipped_high_frequency_queries": skipped,
            "safety_status": safety_status,
            "seconds_median": format_number(sec_median),
            "seconds_min": format_number(minimum(seconds)),
            "seconds_max": format_number(maximum(seconds)),
            "qps_median": format_number(qps),
            "nanoseconds_per_query_median": format_number(
                ratio(sec_median * 1.0e9 if sec_median is not None else None, query_count)),
            "query_bases": format_integer(query_bases),
            "query_bases_per_second": format_number(ratio(query_bases, sec_median)),
            "peak_rss_mb": format_number(maximum(row.get("peak_rss_mb") for row in rows)),
            "total_hits": next(iter(totals)) if len(totals) == 1 else NA,
            "reported_hits": next(iter(reported)) if len(reported) == 1 else NA,
            "result_checksum": next(iter(checksums)) if len(checksums) == 1 else NA,
            "repetitions": str(len(rows)),
            "status": ("ok" if stable else
                       (source_statuses[0] if len(source_statuses) == 1 and
                        source_statuses[0] in EXPECTED_NON_OK else "repetition_mismatch")),
        })
    return output


def aggregate_sa_build_rows(tables: Sequence[Table]) -> list[dict[str, str]]:
    raw = ok_rows(all_rows(raw_tables(tables, "sa-build")))
    keys = ["_source_scope", "_profile", "method", "effective_backend", "backend_signature",
            "coordinate_width", "threads", "sampling_rate", "suffix_count", "acceleration"]
    metrics = [
        ("reference_read_seconds", ["reference_read_seconds"]),
        ("normalization_seconds", ["normalization_seconds"]),
        ("sa_seconds", ["sa_seconds"]),
        ("isa_seconds", ["isa_seconds"]),
        ("lcp_seconds", ["lcp_seconds"]),
        ("child_seconds", ["child_seconds"]),
        ("sapling_seconds", ["sapling_seconds"]),
        ("build_seconds", ["build_wall_seconds"]),
        ("build_peak_rss_mb", ["build_peak_rss_mb", "peak_rss_mb"]),
        ("save_seconds", ["save_seconds"]),
        ("save_peak_rss_mb", ["save_peak_rss_mb"]),
        ("load_seconds", ["load_seconds"]),
        ("load_peak_rss_mb", ["load_peak_rss_mb"]),
        ("serialized_bytes", ["serialized_bytes"]),
        ("allocated_disk_bytes", ["allocated_disk_bytes"]),
        ("bits_per_base", ["bits_per_base"]),
        ("learned_index_bytes", ["learned_index_bytes"]),
    ]
    output: list[dict[str, str]] = []
    for key, rows in sorted(group_rows(raw, keys).items()):
        values = dict(zip(keys, key))
        values["source_scope"] = values.pop("_source_scope")
        values["profile"] = values.pop("_profile")
        for output_name, input_names in metrics:
            values[f"{output_name}_median"] = format_number(numeric_median(rows, input_names))
        values["build_seconds_min"] = format_number(minimum(row.get("build_wall_seconds") for row in rows))
        values["build_seconds_max"] = format_number(maximum(row.get("build_wall_seconds") for row in rows))
        values["repetitions"] = str(len(rows))
        values["status"] = "ok"
        output.append(values)
    return output


def aggregate_exact_build_rows(tables: Sequence[Table]) -> list[dict[str, str]]:
    raw = ok_rows(all_rows(raw_tables(tables, "exact")))
    raw = [row for row in raw if row.get("phase") in {"build", "save", "load"}]
    keys = ["_source_scope", "_profile", "dataset", "scenario", "method"]
    output: list[dict[str, str]] = []
    for key, rows in sorted(group_rows(raw, keys).items()):
        identity = dict(zip(keys, key))
        identity["source_scope"] = identity.pop("_source_scope")
        identity["profile"] = identity.pop("_profile")
        build = [row for row in rows if row.get("phase") == "build"]
        save = [row for row in rows if row.get("phase") == "save"]
        load = [row for row in rows if row.get("phase") == "load"]
        serialized = numeric_median(rows, ["serialized_bytes"])
        allocated = numeric_median(rows, ["allocated_disk_bytes"])
        total_bases = numeric_median(rows, ["total_bases"])
        output.append({
            "source_scope": identity["source_scope"],
            "profile": identity["profile"],
            "method": identity["method"],
            "effective_backend": constant_or_na(rows, ["backend", "effective_backend", "method"]),
            "backend_signature": constant_or_na(rows, ["backend_signature"]),
            "coordinate_width": constant_or_na(rows, ["coordinate_width"]),
            "threads": constant_or_na(rows, ["threads"]),
            "sampling_rate": constant_or_na(rows, ["sa_sampling_rate", "sampling_rate"]),
            "suffix_count": constant_or_na(rows, ["suffix_count"]),
            "acceleration": constant_or_na(rows, ["acceleration"]),
            "reference_read_seconds_median": NA,
            "normalization_seconds_median": NA,
            "sa_seconds_median": format_number(numeric_median(build, ["sa_build_seconds"])),
            "isa_seconds_median": format_number(numeric_median(build, ["isa_build_seconds"])),
            "lcp_seconds_median": format_number(numeric_median(build, ["lcp_build_seconds"])),
            "child_seconds_median": format_number(numeric_median(build, ["child_build_seconds"])),
            "sapling_seconds_median": format_number(numeric_median(build, ["learned_index_build_seconds"])),
            "build_seconds_median": format_number(numeric_median(build, ["seconds", "build_seconds"])),
            "build_seconds_min": format_number(minimum(row.get("seconds") for row in build)),
            "build_seconds_max": format_number(maximum(row.get("seconds") for row in build)),
            "build_peak_rss_mb_median": format_number(numeric_median(build, ["peak_rss_mb"])),
            "save_seconds_median": format_number(numeric_median(save, ["seconds", "save_seconds"])),
            "save_peak_rss_mb_median": format_number(numeric_median(save, ["peak_rss_mb"])),
            "load_seconds_median": format_number(numeric_median(load, ["seconds", "load_seconds"])),
            "load_peak_rss_mb_median": format_number(numeric_median(load, ["peak_rss_mb"])),
            "serialized_bytes_median": format_integer(serialized),
            "allocated_disk_bytes_median": format_integer(allocated),
            "bits_per_base_median": format_number(
                ratio(serialized * 8.0 if serialized is not None else None, total_bases)),
            "learned_index_bytes_median": format_integer(numeric_median(rows, ["learned_index_bytes"])),
            "repetitions": str(len(build)),
            "status": "ok" if build else "partial",
        })
    return output


def aggregate_right_maximal_rows(tables: Sequence[Table]) -> list[dict[str, str]]:
    raw = [row for row in all_rows(raw_tables(tables, "right-maximal"))
           if row.get("operation") in RIGHT_QUERY_OPERATIONS]
    keys = ["_source_scope", "_profile", "dataset", "method", "operation", "min_length"]
    output: list[dict[str, str]] = []
    for key, source_rows in sorted(group_rows(raw, keys).items()):
        rows = ok_rows(source_rows)
        if not rows:
            identity = dict(zip(keys, key))
            identity["source_scope"] = identity.pop("_source_scope")
            identity["profile"] = identity.pop("_profile")
            statuses = sorted({row_status(row) for row in source_rows})
            output.append(identity | {column: NA for column in RIGHT_AGG_COLUMNS
                                      if column not in identity and column != "status"} |
                          {"status": statuses[0] if len(statuses) == 1 else "mixed_non_ok"})
            continue
        seconds = [row.get("seconds") for row in rows]
        sec_median = median(seconds)
        total_matches = numeric_median(rows, ["total_matches"])
        query_bases = numeric_median(rows, ["query_bases"])
        checksums = {row.get("result_checksum", NA) for row in rows}
        counts = {row.get("count_checksum", NA) for row in rows}
        identity = dict(zip(keys, key))
        identity["source_scope"] = identity.pop("_source_scope")
        identity["profile"] = identity.pop("_profile")
        output.append(identity | {
            "seconds_median": format_number(sec_median),
            "seconds_min": format_number(minimum(seconds)),
            "seconds_max": format_number(maximum(seconds)),
            "query_bases": format_integer(query_bases),
            "query_bases_per_second": format_number(ratio(query_bases, sec_median)),
            "matches_per_second": format_number(ratio(total_matches, sec_median)),
            "total_matches": format_integer(total_matches),
            "reported_matches": constant_or_na(rows, ["reported_matches"]),
            "count_checksum": next(iter(counts)) if len(counts) == 1 else NA,
            "result_checksum": next(iter(checksums)) if len(checksums) == 1 else NA,
            "repetitions": str(len(rows)),
            "status": "ok" if len(checksums) == 1 and len(counts) == 1 else "repetition_mismatch",
        })
    return output


def summarize_correctness(tables: Sequence[Table]) -> tuple[list[dict[str, str]], bool]:
    summaries: list[dict[str, str]] = []
    blocked = False
    for workload in ("exact", "sa-build", "right-maximal"):
        rows = all_rows(raw_tables(tables, workload))
        unexpected = [row for row in rows if row_status(row) not in {"ok"} | EXPECTED_NON_OK]
        summaries.append({
            "workload": workload,
            "check": "unexpected_status",
            "groups_checked": str(len(rows)),
            "mismatches": str(len(unexpected)),
            "status": "fail" if unexpected else ("ok" if rows else "partial"),
            "details": "; ".join(
                f"{row.get('_source_file')}:{row.get('_source_line')}={row_status(row)}"
                for row in unexpected[:10]) or NA,
        })
        blocked = blocked or bool(unexpected)

    oracle_rows = [row for table in tables if table.name == "correctness_summary.tsv"
                   for row in table.rows]
    oracle_failures = [row for row in oracle_rows if row_status(row) != "ok"]
    summaries.append({"workload": "right-maximal", "check": "naive_oracle",
                      "groups_checked": str(len(oracle_rows)),
                      "mismatches": str(len(oracle_failures)),
                      "status": "fail" if oracle_failures else ("ok" if oracle_rows else "partial"),
                      "details": "; ".join(
                          f"{row.get('dataset')}/min={row.get('min_length')}={row_status(row)}"
                          for row in oracle_failures[:10]) or NA})
    blocked = blocked or bool(oracle_failures)

    repetition_specs = [
        ("exact", ["_source_scope", "dataset", "scenario", "method", "query_group",
                   "pattern_length", "strand", "operation", "max_hits", "fm_query_mode", "fm_batch_width"],
         ["query_count", "skipped_high_frequency_queries", "query_bases",
          "total_hits", "reported_hits", "result_checksum"],
         lambda row: row.get("phase") == "query"),
        ("right-maximal", ["_source_scope", "dataset", "method", "operation", "min_length"],
         ["total_matches", "reported_matches", "count_checksum", "result_checksum"],
         lambda row: True),
        ("sa-build", ["_source_scope", "method", "threads", "sampling_rate", "acceleration"],
         ["sa_checksum", "exact_checksum", "right_maximal_checksum"],
         lambda row: True),
    ]
    for workload, keys, values, predicate in repetition_specs:
        source = [row for row in ok_rows(all_rows(raw_tables(tables, workload))) if predicate(row)]
        mismatches = []
        checked = 0
        for key, rows in group_rows(source, keys).items():
            if len(rows) < 2:
                continue
            checked += 1
            signatures = {tuple(row.get(field, NA) for field in values) for row in rows}
            if len(signatures) > 1:
                mismatches.append("/".join(key))
        summaries.append({"workload": workload, "check": "repetition_result_stability",
                          "groups_checked": str(checked), "mismatches": str(len(mismatches)),
                          "status": "fail" if mismatches else ("ok" if source else "partial"),
                          "details": "; ".join(mismatches[:10]) or
                          ("single-repetition groups; cardinality checked by profile matrix"
                           if source and not checked else NA)})
        blocked = blocked or bool(mismatches)

    exact = [row for row in ok_rows(all_rows(raw_tables(tables, "exact"))) if row.get("phase") == "query"]
    exact_keys = ["_source_scope", "dataset", "scenario", "query_group", "pattern_length", "strand",
                  "operation", "max_hits", "fm_query_mode", "fm_batch_width", "repetition"]
    exact_mismatch: list[str] = []
    checked = 0
    for key, rows in group_rows(exact, exact_keys).items():
        methods = {row.get("method") for row in rows}
        if len(methods) < 2:
            continue
        checked += 1
        values = {(
            row.get("query_count"), row.get("skipped_high_frequency_queries"),
            row.get("query_bases"), row.get("total_hits"), row.get("reported_hits"),
            row.get("result_checksum")) for row in rows}
        if len(values) > 1:
            exact_mismatch.append("/".join(key))
    summaries.append({"workload": "exact", "check": "cross_method_result_equivalence",
                      "groups_checked": str(checked), "mismatches": str(len(exact_mismatch)),
                      "status": "fail" if exact_mismatch else ("ok" if checked else "partial"),
                      "details": "; ".join(exact_mismatch[:10]) or NA})
    blocked = blocked or bool(exact_mismatch)

    right = ok_rows(all_rows(raw_tables(tables, "right-maximal")))
    right_keys = ["_source_scope", "dataset", "operation", "min_length", "repetition"]
    right_mismatch: list[str] = []
    checked = 0
    for key, rows in group_rows(right, right_keys).items():
        internal = [row for row in rows if row.get("method") != "mummer4"]
        if len({row.get("method") for row in internal}) < 2:
            continue
        checked += 1
        values = {(row.get("total_matches"), row.get("reported_matches"),
                   row.get("count_checksum"), row.get("result_checksum")) for row in internal}
        if len(values) > 1:
            right_mismatch.append("/".join(key))
    summaries.append({"workload": "right-maximal", "check": "cross_method_result_equivalence",
                      "groups_checked": str(checked), "mismatches": str(len(right_mismatch)),
                      "status": "fail" if right_mismatch else ("ok" if checked else "partial"),
                      "details": "; ".join(right_mismatch[:10]) or NA})
    blocked = blocked or bool(right_mismatch)

    sa = ok_rows(all_rows(raw_tables(tables, "sa-build")))
    for checksum_name in ("exact_checksum", "right_maximal_checksum"):
        groups = group_rows(sa, ["_profile", "sampling_rate", "acceleration", "repetition"])
        mismatches = []
        checked = 0
        for key, rows in groups.items():
            if len({row.get("method") for row in rows}) < 2:
                continue
            checked += 1
            values = {row.get(checksum_name, NA) for row in rows}
            if len(values) > 1:
                mismatches.append("/".join(key))
        summaries.append({"workload": "sa-build", "check": f"cross_method_{checksum_name}",
                          "groups_checked": str(checked), "mismatches": str(len(mismatches)),
                          "status": "fail" if mismatches else ("ok" if checked else "partial"),
                          "details": "; ".join(mismatches[:10]) or NA})
        blocked = blocked or bool(mismatches)
    return summaries, blocked


def union_columns(rows: Sequence[Mapping[str, str]], preferred: Sequence[str] = ()) -> list[str]:
    available = {key for row in rows for key in row}
    ordered = [key for key in preferred if key in available]
    ordered.extend(sorted(available - set(ordered)))
    return ordered


def write_tsv(path: Path, rows: Sequence[Mapping[str, object]], columns: Sequence[str] | None = None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if columns is None:
        columns = union_columns(rows)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(columns), delimiter="\t", lineterminator="\n",
                                extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow({column: clean_cell(row.get(column)) for column in columns})


def copy_raw_union(path: Path, tables: Sequence[Table]) -> None:
    rows = all_rows(tables)
    write_tsv(path, rows, ["_source_file", "_source_scope", "_profile", "_source_line"] +
              [column for column in union_columns(rows) if not column.startswith("_")])


def read_revision(run_dir: Path, override: str | None) -> str:
    if override:
        return override
    revision_path = run_dir / "manifest" / "SOURCE_REVISION.txt"
    if revision_path.exists():
        match = re.search(r"\b[0-9a-fA-F]{7,40}\b", revision_path.read_text(encoding="utf-8", errors="replace"))
        if match:
            return match.group(0)
    match = re.search(r"\b[0-9a-fA-F]{7,40}\b", run_dir.name)
    return match.group(0) if match else "unknown"


def environment_rows(run_dir: Path, tables: Sequence[Table], revision: str,
                     server_label: str) -> list[dict[str, str]]:
    output = [
        {"key": "server_label", "value": server_label, "source": "packager argument"},
        {"key": "source_revision", "value": revision, "source": "manifest/SOURCE_REVISION.txt"},
        {"key": "run_complete", "value": str((run_dir / "state" / "ALL_COMPLETE").exists()).lower(),
         "source": "state/ALL_COMPLETE"},
        {"key": "run_id", "value": run_dir.name, "source": "run directory basename"},
    ]
    wanted = ["timestamp", "compiler", "compiler_version", "cmake_version", "build_type", "os", "architecture",
              "cpu_model", "logical_cpus", "seed"]
    for key in wanted:
        values = sorted({row.get(key, NA) for table in tables if table.name == "run_metadata.tsv"
                         for row in table.rows if row.get(key, NA) != NA})
        if values:
            output.append({"key": key, "value": " | ".join(values), "source": "run_metadata.tsv"})
    environment = run_dir / "manifest" / "environment.txt"
    if environment.exists():
        text = environment.read_text(encoding="utf-8", errors="replace")
        patterns = {
            "cpu_model_manifest": r"(?m)^Model name:\s*(.+)$",
            "architecture_manifest": r"(?m)^Architecture:\s*(.+)$",
            "logical_cpus_manifest": r"(?m)^CPU\(s\):\s*(\d+)\s*$",
            "threads_per_core": r"(?m)^Thread\(s\) per core:\s*(\d+)\s*$",
            "cores_per_socket": r"(?m)^Core\(s\) per socket:\s*(\d+)\s*$",
            "sockets": r"(?m)^Socket\(s\):\s*(\d+)\s*$",
            "numa_nodes": r"(?m)^NUMA node\(s\):\s*(\d+)\s*$",
        }
        for key, pattern in patterns.items():
            match = re.search(pattern, text)
            if match:
                output.append({"key": key, "value": match.group(1).strip(),
                               "source": "manifest/environment.txt"})
    return output


def manifest_rows(run_dir: Path, tables: Sequence[Table]) -> list[dict[str, str]]:
    result = []
    by_path = {table.path: table for table in tables}
    for path in sorted(run_dir.rglob("*")):
        if not path.is_file() or is_hidden_run_path(run_dir, path):
            continue
        relative = str(path.relative_to(run_dir)).replace(os.sep, "/")
        table = by_path.get(path)
        schema = ",".join(table.columns) if table else NA
        result.append({
            "path": relative,
            "bytes": str(path.stat().st_size),
            "sha256": sha256(path),
            "rows": str(len(table.rows)) if table else NA,
            "columns": str(len(table.columns)) if table else NA,
            "schema": schema,
            "schema_sha256": hashlib.sha256(schema.encode("utf-8")).hexdigest() if table else NA,
            "workload": table.workload if table else "artifact",
            "profile": table.profile if table else profile_from_path(path.relative_to(run_dir)),
        })
    return result


def markdown_table(columns: Sequence[str], rows: Sequence[Mapping[str, object]]) -> str:
    if not rows:
        return "_No rows available._\n"
    lines = ["| " + " | ".join(columns) + " |",
             "|" + "|".join("---" for _ in columns) + "|"]
    for row in rows:
        values = [clean_cell(row.get(column)).replace("|", "\\|") for column in columns]
        lines.append("| " + " | ".join(values) + " |")
    return "\n".join(lines) + "\n"


def status_of(rows: Sequence[Mapping[str, str]]) -> str:
    statuses = {row.get("status", "partial") for row in rows}
    if any("mismatch" in value or value == "fail" for value in statuses):
        return "blocked"
    return "ok" if statuses == {"ok"} else "partial"


def svg_bar_panel(x: int, y: int, width: int, height: int, title: str,
                  labels: Sequence[str], series: Sequence[tuple[str, Sequence[float | None], str]],
                  unit: str) -> str:
    parts = [f'<text x="{x}" y="{y + 18}" class="panel-title">{html.escape(title)}</text>']
    plot_y = y + 34
    plot_h = height - 58
    plot_x = x + 42
    plot_w = width - 54
    numeric = [value for _, values, _ in series for value in values if value is not None]
    maximum_value = max(numeric) if numeric else 1.0
    parts.append(f'<line x1="{plot_x}" y1="{plot_y + plot_h}" x2="{plot_x + plot_w}" y2="{plot_y + plot_h}" class="axis"/>')
    groups = max(1, len(labels))
    group_width = plot_w / groups
    bar_width = min(26.0, group_width / max(2.0, len(series) + 0.5))
    for index, label in enumerate(labels):
        center = plot_x + group_width * (index + 0.5)
        for series_index, (series_name, values, color) in enumerate(series):
            value = values[index] if index < len(values) else None
            offset = (series_index - (len(series) - 1) / 2.0) * bar_width
            bar_x = center + offset - bar_width * 0.42
            if value is None:
                parts.append(f'<text x="{bar_x + bar_width * .42:.1f}" y="{plot_y + plot_h - 6}" class="na">NA</text>')
                continue
            bar_h = 0.0 if maximum_value == 0 else (value / maximum_value) * (plot_h - 30)
            bar_y = plot_y + plot_h - bar_h
            parts.append(f'<rect x="{bar_x:.1f}" y="{bar_y:.1f}" width="{bar_width * .84:.1f}" height="{bar_h:.1f}" fill="{color}" rx="2"/>')
            parts.append(f'<text x="{bar_x + bar_width * .42:.1f}" y="{bar_y - 4:.1f}" class="value">{html.escape(short_number(value))}</text>')
        parts.append(f'<text x="{center:.1f}" y="{plot_y + plot_h + 15}" class="label">{html.escape(label)}</text>')
    parts.append(f'<text x="{plot_x}" y="{plot_y + 10}" class="unit">{html.escape(unit)}</text>')
    legend_x = plot_x + 6
    for index, (series_name, _, color) in enumerate(series):
        legend_y = y + height - 5
        offset = index * 100
        parts.append(f'<rect x="{legend_x + offset}" y="{legend_y - 8}" width="9" height="9" fill="{color}"/>')
        parts.append(f'<text x="{legend_x + offset + 13}" y="{legend_y}" class="legend">{html.escape(series_name)}</text>')
    return "".join(parts)


def short_number(value: float) -> str:
    if value >= 1.0e9:
        return f"{value / 1.0e9:.1f}G"
    if value >= 1.0e6:
        return f"{value / 1.0e6:.1f}M"
    if value >= 1.0e3:
        return f"{value / 1.0e3:.1f}k"
    if value >= 100:
        return f"{value:.0f}"
    return f"{value:.1f}"


def write_headline_svg(path: Path, builds: Sequence[Mapping[str, str]],
                       queries: Sequence[Mapping[str, str]]) -> None:
    labels = [row["index"].replace(" / ", "\n") for row in builds]
    build_time = [number(row.get("build_time_seconds")) for row in builds]
    rss = [number(row.get("peak_rss_mb")) for row in builds]
    size_mib = [ratio(row.get("index_size_bytes"), 1024 * 1024) for row in builds]
    query_labels = ["SA", "FM"]
    count = []
    locate = []
    for name in ("SA32 default", "FM Huffman"):
        count.append(next((ratio(row.get("queries_per_second"), 1.0e6) for row in queries
                           if row.get("index") == name and row.get("operation") == "Count"), None))
        locate.append(next((ratio(row.get("queries_per_second"), 1.0e6) for row in queries
                            if row.get("index") == name and row.get("operation") == "Locate-1"), None))
    svg = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="430" viewBox="0 0 1440 430">',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#172033}.panel-title{font-size:18px;font-weight:700}.label{font-size:10px;text-anchor:middle}.value{font-size:9px;text-anchor:middle}.unit,.legend{font-size:10px;fill:#536174}.axis{stroke:#a8b1c1;stroke-width:1}.na{font-size:9px;text-anchor:middle;fill:#a33}.panel{fill:#fafbfc;stroke:#dce1e8}</style>',
        '<rect width="1440" height="430" fill="white"/>',
        '<rect class="panel" x="10" y="10" width="460" height="410" rx="8"/>',
        '<rect class="panel" x="490" y="10" width="460" height="410" rx="8"/>',
        '<rect class="panel" x="970" y="10" width="460" height="410" rx="8"/>',
        svg_bar_panel(20, 20, 440, 385, "A. Build time", labels,
                      [("seconds", build_time, "#3567a8")], "seconds (median)"),
        svg_bar_panel(500, 20, 440, 385, "B. Peak RSS / index size", labels,
                      [("RSS", rss, "#d87b33"), ("index", size_mib, "#51a36c")], "MiB"),
        svg_bar_panel(980, 20, 440, 385, "C. Exact throughput", query_labels,
                      [("count", count, "#6d57b5"), ("locate-1", locate, "#c45273")], "million queries/s"),
        '</svg>\n',
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(svg), encoding="utf-8")


def preferred_profile(rows: Sequence[Mapping[str, str]]) -> tuple[str, list[Mapping[str, str]]]:
    for profile in ("full", "standard", "quick", "smoke", "user"):
        selected = [row for row in rows if row.get("profile") == profile]
        if selected:
            return profile, selected
    return "unknown", list(rows)


def write_detail_svg(path: Path, title: str, labels: Sequence[str],
                     series: Sequence[tuple[str, Sequence[float | None], str]], unit: str,
                     note: str) -> None:
    has_value = any(value is not None for _, values, _ in series for value in values)
    svg = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="520" viewBox="0 0 1000 520">',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#172033}.panel-title{font-size:20px;font-weight:700}.label{font-size:10px;text-anchor:middle}.value{font-size:9px;text-anchor:middle}.unit,.legend{font-size:11px;fill:#536174}.axis{stroke:#a8b1c1;stroke-width:1}.na{font-size:10px;text-anchor:middle;fill:#a33}.panel{fill:#fafbfc;stroke:#dce1e8}.note{font-size:11px;fill:#536174}</style>',
        '<rect width="1000" height="520" fill="white"/>',
        '<rect class="panel" x="10" y="10" width="980" height="500" rx="8"/>',
    ]
    if has_value:
        svg.append(svg_bar_panel(25, 20, 950, 450, title, labels, series, unit))
    else:
        svg.append(f'<text x="500" y="245" class="na">NA / partial: no matching raw measurements</text>')
        svg.append(f'<text x="35" y="52" class="panel-title">{html.escape(title)}</text>')
    svg.append(f'<text x="35" y="495" class="note">{html.escape(note)}</text>')
    svg.append('</svg>\n')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(svg), encoding="utf-8")


def write_detail_figures(output_dir: Path, build_rows: Sequence[Mapping[str, str]],
                         exact_rows: Sequence[Mapping[str, str]],
                         right_rows: Sequence[Mapping[str, str]]) -> None:
    figures = output_dir / "figures"
    build_profile, builds = preferred_profile(build_rows)
    builds = [row for row in builds if row.get("status") == "ok"]

    scaling = [row for row in builds if row.get("sampling_rate") == "1" and
               row.get("acceleration") in {"suffix-link", "default", "none"}]
    scaling = sorted(scaling, key=lambda row: (
        row.get("method", ""), integer(row.get("threads")) or 0,
        row.get("acceleration", "")))[:16]
    scaling_labels = [f"{row.get('method')}/t{row.get('threads')}/{row.get('acceleration')}" for row in scaling]
    write_detail_svg(
        figures / "build-scaling.svg", "SA construction scaling", scaling_labels,
        [("build", [number(row.get("build_seconds_median")) for row in scaling], "#3567a8")],
        "seconds (median)", f"profile={build_profile}; values recomputed from raw repetitions")

    memory_candidates = [row for row in builds if row.get("sampling_rate") == "1" or
                         row.get("method", "").startswith("fm")]
    memory_candidates = sorted(memory_candidates, key=lambda row: (
        row.get("method", ""), integer(row.get("threads")) or 0,
        row.get("acceleration", "")))[:14]
    memory_labels = [f"{row.get('method')}/t{row.get('threads')}" for row in memory_candidates]
    write_detail_svg(
        figures / "memory-and-size.svg", "Construction memory and persisted size", memory_labels,
        [
            ("RSS", [number(row.get("build_peak_rss_mb_median")) for row in memory_candidates], "#d87b33"),
            ("index", [ratio(row.get("serialized_bytes_median"), 1024 * 1024)
                       for row in memory_candidates], "#51a36c"),
        ], "MiB", f"profile={build_profile}; peak RSS and logical serialized bytes")

    exact_profile, exact = preferred_profile(exact_rows)
    exact = [row for row in exact if row.get("status") == "ok" and
             row.get("scenario") == "mixed" and row.get("pattern_length") == "100" and
             row.get("strand") == "forward" and row.get("query_group") in HEADLINE_GROUPS and
             (row.get("operation") == "count" or
              (row.get("operation") == "locate" and row.get("max_hits") == "1"))]
    methods = sorted({row.get("method", NA) for row in exact})[:14]
    count_values = []
    locate_values = []
    for method in methods:
        count_values.append(median(row.get("qps_median") for row in exact
                                   if row.get("method") == method and row.get("operation") == "count"))
        locate_values.append(median(row.get("qps_median") for row in exact
                                    if row.get("method") == method and row.get("operation") == "locate"))
    write_detail_svg(
        figures / "count-locate-details.svg", "Exact count and locate-1", methods,
        [("count", count_values, "#6d57b5"), ("locate-1", locate_values, "#c45273")],
        "queries/s", f"profile={exact_profile}; mixed, length=100, forward; median across ordinary groups")

    right_profile, right = preferred_profile(right_rows)
    right = [row for row in right if row.get("status") == "ok" and
             row.get("operation") == "streaming" and row.get("min_length") == "50"]
    right = sorted(right, key=lambda row: row.get("method", ""))[:16]
    right_labels = [row.get("method", NA).replace("right-maximal-", "") for row in right]
    write_detail_svg(
        figures / "right-maximal-ablation.svg", "Right-maximal streaming ablation", right_labels,
        [("matches", [number(row.get("matches_per_second")) for row in right], "#3a8c78")],
        "matches/s", f"profile={right_profile}; min_length=50; right-maximal, not full MEM")

    caps = [row for row in builds if row.get("method", "").startswith("caps") and
            row.get("sampling_rate") == "1"]
    threads = sorted({integer(row.get("threads")) for row in caps if integer(row.get("threads")) is not None})
    thread_labels = [str(value) for value in threads]
    caps_series = []
    colors = {"caps32": "#2f76b7", "caps64": "#d47a2f"}
    for method in ("caps32", "caps64"):
        values = []
        for thread in threads:
            values.append(median(row.get("build_seconds_median") for row in caps
                                 if row.get("method") == method and integer(row.get("threads")) == thread))
        caps_series.append((method, values, colors[method]))
    write_detail_svg(
        figures / "caps-thread-scaling.svg", "CaPS thread scaling", thread_labels, caps_series,
        "seconds (median)", f"profile={build_profile}; K=1; lower is better")


def write_headline(output_dir: Path, builds: list[dict[str, str]],
                   exact: list[dict[str, str]], right: list[dict[str, str]]) -> str:
    headline_dir = output_dir / "headline"
    write_tsv(headline_dir / "headline-build.tsv", builds, HEADLINE_BUILD_COLUMNS)
    write_tsv(headline_dir / "headline-query.tsv", exact, HEADLINE_EXACT_COLUMNS)
    write_tsv(headline_dir / "headline-right-maximal.tsv", right, HEADLINE_RIGHT_COLUMNS)
    long_rows: list[dict[str, str]] = []
    for row in builds:
        for metric, unit in (("build_time_seconds", "seconds"), ("peak_rss_mb", "MiB"),
                             ("index_size_bytes", "bytes"), ("bits_per_base", "bits/base")):
            long_rows.append({"section": "build", "item": row["index"], "metric": metric,
                              "value": row[metric], "unit": unit, "status": row["status"],
                              "provenance": "raw_repetitions.tsv"})
    for row in exact:
        long_rows.append({"section": "exact", "item": f"{row['index']} / {row['operation']}",
                          "metric": "queries_per_second", "value": row["queries_per_second"],
                          "unit": "queries/s", "status": row["status"],
                          "provenance": "raw_repetitions.tsv"})
    for row in right:
        long_rows.append({"section": "right-maximal", "item": row["method"],
                          "metric": "query_bases_per_second", "value": row["query_bases_per_second"],
                          "unit": "bases/s", "status": row["status"],
                          "provenance": "raw_repetitions.tsv"})
    write_tsv(headline_dir / "headline.tsv", long_rows,
              ["section", "item", "metric", "value", "unit", "status", "provenance"])
    write_headline_svg(output_dir / "figures" / "headline-performance.svg", builds, exact)
    overall = "ok" if all(status_of(rows) == "ok" for rows in (builds, exact, right)) else "partial"
    readme = f"""# Full mixed headline benchmark

**Headline status: {overall}.** Every numeric cell below was recomputed from
`raw_repetitions.tsv`. `NA` means the producer schema did not carry the needed
raw field; no summary TSV was used as a hidden fallback.

Fixed workload: 256 MiB synthetic `mixed` reference, four contigs, seed
20260822; 10,000 queries; exact length 100, forward strand, count and locate-1;
right-maximal query length 256 and minimum length 50. Build uses three measured
repetitions in one unified exact-worker scope (`sa32-binary`, `caps32`, and
`fm-huff` on the same dataset); query uses one warm-up and five measured
repetitions. The right-maximal worker reloads that exported synthetic
full/mixed FASTA and therefore truthfully records its runtime scenario as
`user-reference`; the export SHA-256 and both benchmark fingerprint dialects
are audited before publication.

## Construction

{markdown_table(['index', 'builder', 'threads', 'build_time_seconds', 'peak_rss_mb', 'index_size_bytes', 'bits_per_base', 'speedup_vs_divsufsort', 'status'], builds)}

## Exact query

Headline throughput includes only `exact_unique`, `exact_repetitive`,
`mutated_low_hit`, and `random_no_hit`.

{markdown_table(['index', 'operation', 'queries_per_second', 'query_peak_rss_mb', 'status'], exact)}

## Right-maximal exact match

This is the streaming callback path and is intentionally not called MEM.

{markdown_table(['method', 'query_bases_per_second', 'matches_per_second', 'speedup_vs_baseline', 'status'], right)}

![Headline performance](../figures/headline-performance.svg)
"""
    (headline_dir / "README.md").write_text(readme, encoding="utf-8")
    return overall


def write_section_readmes(output_dir: Path) -> None:
    (output_dir / "build" / "README.md").write_text(
        """# Construction results

`build-results.tsv` is recomputed from SA-build and exact-index raw repetitions,
so standalone SA and FM rows share one phase-oriented view. It separates
reference reading, normalization, SA, ISA, LCP, CHILD, learned-index, save, and
load phases when those fields are present. `caps-scaling.tsv` and
`sampling-space.tsv` are filtered views. Missing producer fields remain `NA`.

- [Build scaling](../figures/build-scaling.svg)
- [Memory and persisted size](../figures/memory-and-size.svg)
- [CaPS thread scaling](../figures/caps-thread-scaling.svg)
""", encoding="utf-8")
    (output_dir / "exact" / "README.md").write_text(
        """# Exact-query results

Count and locate are aggregated independently from exact raw repetitions.
Timing excludes index loading and TSV formatting according to the producer
contract. `fm-batch-results.tsv` is a filtered view; scalar and batch rows must
not be ranked as if they were the same operation.

- [Count and locate details](../figures/count-locate-details.svg)
""", encoding="utf-8")
    (output_dir / "right-maximal" / "README.md").write_text(
        """# Right-maximal exact-match results

These methods enumerate right-maximal exact matches, not full MEMs. Streaming,
vector materialization, count-only (`max_matches=0`), and bounded materialization
remain separate operations. FM rows marked `not_supported` are capabilities,
not zero-throughput measurements. Build, save, and load phase rows are retained
only in the raw evidence and construction tables; they are excluded from this
query algorithm ablation.

- [Algorithm ablation](../figures/right-maximal-ablation.svg)
""", encoding="utf-8")
    (output_dir / "scenarios" / "README.md").write_text(
        """# Scenario effects

These TSVs are filtered views of the exact-query aggregate. They preserve
pattern length, strand, operation, method, and query group so readers do not
mistake a pooled mix for a controlled comparison. Synthetic scenarios do not
establish performance on every real genome.
""", encoding="utf-8")


def write_commands(output_dir: Path) -> None:
    (output_dir / "commands.md").write_text(
        """# Reproduction commands

The server suite was designed to run from a fresh, additive run directory:

```bash
bash benchmarks/run_server_suite.sh SOURCE_DIR NEW_RUN_DIR
```

Package an existing run without reading personal paths into the report:

```bash
python3 benchmarks/package_benchmark_results.py \\
  package \\
  --run-dir RUN_DIR \\
  --output-dir benchmarks/results/server-REVISION-YYYYMMDD \\
  --revision REVISION \\
  --server-label server \\
  --require-complete
```

The runner executes Release build, CTest, smoke, quick, standard, full, audits,
and the fixed headline in sequence. State markers and logs remain in the server
run directory. The packager does not delete or alter server artifacts.
""", encoding="utf-8")


def tables_for_profile(tables: Sequence[Table], profile: str) -> list[Table]:
    prefix = "headline/" if profile == "headline" else f"{profile}/"
    return [table for table in tables if table.relative.startswith(prefix)]


def audit_check(profile: str, name: str, failures: Sequence[str], success: str) -> dict[str, str]:
    return {
        "profile": profile,
        "check": name,
        "status": "fail" if failures else "ok",
        "details": "; ".join(failures[:12]) if failures else success,
    }


def repetition_failure(
    rows: Sequence[Mapping[str, str]], expected: int, first: int,
    allowed_placeholder_statuses: set[str] | None = None,
    single_capability_placeholder: bool = False) -> str | None:
    """Return a concise error when a required measured slice is incomplete.

    A capability/safety placeholder is allowed only when the caller explicitly
    names its status.  Supported rows may therefore not disguise a missing
    repetition with a generic non-ok row.
    """
    if not rows:
        return "missing"
    statuses = {row_status(row) for row in rows}
    placeholders = allowed_placeholder_statuses or set()
    if "ok" not in statuses:
        if not statuses or not statuses <= placeholders:
            return "unexpected_status=" + ",".join(sorted(statuses))
        if single_capability_placeholder:
            repetitions = [integer(row.get("repetition")) for row in rows]
            return None if len(rows) == 1 and repetitions[0] is not None else \
                f"capability_rows={len(rows)}; expected=1"
    elif statuses != {"ok"}:
        return "mixed_status=" + ",".join(sorted(statuses))
    repetitions = [integer(row.get("repetition")) for row in rows]
    expected_values = set(range(first, first + expected))
    observed_values = {value for value in repetitions if value is not None}
    if len(rows) != expected or len(observed_values) != expected or observed_values != expected_values:
        observed = ",".join(str(value) for value in sorted(observed_values)) or NA
        return f"repetitions={observed}; expected={first}..{first + expected - 1}"
    return None


def controlled_right_vector_skip_failure(
        rows: Sequence[Mapping[str, str]], streaming_rows: Sequence[Mapping[str, str]],
        expected_repetitions: int) -> str | None:
    """Validate the benchmark's intentional high-frequency vector skip.

    Streaming is the preflight that determines whether full vector
    materialization would exceed the fixed safety threshold.  A skipped vector
    slice is valid only when every measured repetition carries that decision
    and its count-only identity agrees with the corresponding streaming row.
    """
    failure = repetition_failure(
        rows, expected_repetitions, 0, {"skipped_high_frequency"})
    if failure:
        return failure
    if {row_status(row) for row in rows} != {"skipped_high_frequency"}:
        return "vector_skip_status_is_not_uniform"
    streaming_failure = repetition_failure(streaming_rows, expected_repetitions, 0)
    if streaming_failure:
        return "streaming_preflight=" + streaming_failure
    streaming_by_repetition = {
        integer(row.get("repetition")): row for row in streaming_rows
    }
    failures: list[str] = []
    for row in rows:
        repetition = integer(row.get("repetition"))
        streaming = streaming_by_repetition.get(repetition)
        if row.get("vector_skipped") != "1":
            failures.append(f"rep{repetition}/vector_skipped={row.get('vector_skipped', NA)}")
        threshold = row.get("materialization_match_threshold", NA)
        if threshold != RIGHT_VECTOR_MATERIALIZATION_THRESHOLD:
            failures.append(f"rep{repetition}/threshold={threshold}")
        total = integer(row.get("total_matches"))
        if total is None or total <= int(RIGHT_VECTOR_MATERIALIZATION_THRESHOLD):
            failures.append(f"rep{repetition}/total_matches={row.get('total_matches', NA)}")
        if row.get("reported_matches") != "0" or row.get("result_checksum") not in {"0", "0000000000000000"}:
            failures.append(f"rep{repetition}/materialized_result_not_empty")
        if row.get("peak_rss_scope") != "not_applicable" or valid_number(row.get("seconds")):
            failures.append(f"rep{repetition}/skip_has_performance_measurement")
        if streaming is None:
            failures.append(f"rep{repetition}/streaming_preflight=missing")
            continue
        for field in ("total_matches", "count_checksum", "query_bases"):
            if row.get(field, NA) != streaming.get(field, NA):
                failures.append(
                    f"rep{repetition}/{field}:vector={row.get(field, NA)},"
                    f"streaming={streaming.get(field, NA)}")
    return ";".join(failures[:6]) if failures else None


def normalized_acceleration(value: str) -> str:
    """Map the requested default layout to its persisted canonical name."""
    return "default" if value in {"default", "suffix-link"} else value


def right_scenarios(tables: Sequence[Table]) -> dict[tuple[str, str], str]:
    """Map (scope,dataset) to scenario using right-maximal metadata."""
    output: dict[tuple[str, str], str] = {}
    scopes = {table.scope for table in raw_tables(tables, "right-maximal")}
    for table in tables:
        if table.scope not in scopes or table.name != "run_metadata.tsv":
            continue
        for row in table.rows:
            dataset = row.get("dataset", NA)
            scenario = row.get("scenario", NA)
            if dataset != NA and scenario != NA:
                output[(table.scope, dataset)] = scenario
    return output


def suite_variant_for_run(run_dir: Path) -> str:
    """Read the runner-selected matrix variant without changing old runs."""
    scope = run_dir / "manifest" / "execution-scope.tsv"
    if not scope.is_file():
        return "full-matrix"
    try:
        with scope.open(encoding="utf-8", newline="") as stream:
            for row in csv.DictReader(stream, delimiter="\t"):
                if row.get("key") == "suite_variant":
                    value = (row.get("value") or "").strip()
                    return value or "full-matrix"
    except (OSError, csv.Error):
        return "full-matrix"
    return "full-matrix"


def profile_matrix_contract(
        run_dir: Path, profile: str) -> dict[str, object]:
    """Return the exact matrix contract for a regular profile/run variant."""
    if suite_variant_for_run(run_dir) == "representative" and profile in REPRESENTATIVE_PROFILE_SCENARIOS:
        return {
            "scenarios": REPRESENTATIVE_PROFILE_SCENARIOS[profile],
            "methods": REPRESENTATIVE_EXACT_METHODS[profile],
            "build_repetitions": REPRESENTATIVE_PROFILE_BUILD_REPETITIONS[profile],
            "query_repetitions": REPRESENTATIVE_PROFILE_QUERY_REPETITIONS[profile],
            "pattern_lengths": REPRESENTATIVE_EXACT_PATTERN_LENGTHS[profile],
            "strands": REPRESENTATIVE_EXACT_STRANDS[profile],
            "locate_limits": REPRESENTATIVE_EXACT_LOCATE_LIMITS[profile],
            "sa_build_matrices": REPRESENTATIVE_SA_BUILD_MATRICES[profile],
            "right_methods": REPRESENTATIVE_RIGHT_METHODS[profile],
            "right_min_lengths": REPRESENTATIVE_RIGHT_MIN_LENGTHS[profile],
            "fm_batch_widths": {"fm-huff": ()},
            "metadata": REPRESENTATIVE_EXACT_METADATA,
        }
    return {
        "scenarios": PROFILE_SCENARIOS[profile],
        "methods": EXACT_METHODS[profile],
        "build_repetitions": PROFILE_BUILD_REPETITIONS[profile],
        "query_repetitions": PROFILE_QUERY_REPETITIONS[profile],
        "pattern_lengths": EXACT_PATTERN_LENGTHS,
        "strands": EXACT_STRANDS,
        "locate_limits": EXACT_LOCATE_LIMITS,
        "sa_build_matrices": SA_BUILD_MATRICES[profile],
        "right_methods": (*RIGHT_INTERNAL_METHODS, *FM_METHODS),
        "right_min_lengths": ("20", "50", "100"),
        "fm_batch_widths": FM_BATCH_WIDTHS,
        "metadata": REGULAR_EXACT_METADATA,
    }


def audit_expected_matrix(
        tables: Sequence[Table], profile: str, run_dir: Path) -> list[dict[str, str]]:
    """Audit the exact matrix launched by ``run_server_suite.sh``.

    The regular profiles deliberately have a closed experimental matrix.  A
    raw file that contains one plausible row is not sufficient: every expected
    scenario, method, operation slice, build layout, and measured repetition
    must be represented.  The only short-form rows accepted are explicit
    safety/capability placeholders.
    """
    if profile not in PROFILE_SCENARIOS:
        return []
    checks: list[dict[str, str]] = []
    contract = profile_matrix_contract(run_dir, profile)
    scenarios = contract["scenarios"]
    methods = contract["methods"]
    build_repetitions = contract["build_repetitions"]
    query_repetitions = contract["query_repetitions"]
    pattern_lengths = contract["pattern_lengths"]
    strands = contract["strands"]
    locate_limits = contract["locate_limits"]

    exact = all_rows(raw_tables(tables, "exact"))
    exact_query = [row for row in exact if row.get("phase") == "query"]
    observed_scenarios = {row.get("scenario") for row in exact_query}
    missing = [scenario for scenario in scenarios if scenario not in observed_scenarios]
    checks.append(audit_check(
        profile, "matrix_expected_scenarios", missing,
        "scenarios=" + ",".join(scenarios)))

    method_failures: list[str] = []
    for scenario in scenarios:
        observed = {row.get("method") for row in exact_query if row.get("scenario") == scenario}
        method_failures.extend(
            f"{scenario}/{method}" for method in methods if method not in observed)
    checks.append(audit_check(
        profile, "matrix_exact_methods", method_failures,
        f"{len(scenarios)} scenario(s) x {len(methods)} method(s)"))

    exact_query_groups = group_rows(exact_query, [
        "scenario", "method", "query_group", "pattern_length", "strand",
        "operation", "max_hits", "fm_query_mode", "fm_batch_width",
    ])
    query_failures: list[str] = []

    def require_exact_slice(
        scenario: str, method: str, group: str, length: str, strand: str,
        operation: str, max_hits: str, mode: str, width: str) -> None:
        key = (scenario, method, group, length, strand, operation, max_hits, mode, width)
        rows = exact_query_groups.get(key, [])
        expected = 1 if method == "naive" and profile == "quick" else query_repetitions
        placeholders = {"not_applicable"}
        if operation == "locate" and max_hits == "all":
            placeholders.add("skipped_high_frequency")
        failure = repetition_failure(rows, expected, 0, placeholders)
        if failure:
            query_failures.append(
                f"{scenario}/{method}/{group}/L{length}/{strand}/"
                f"{operation}:{max_hits}/{mode}:{width}={failure}")

    for scenario in scenarios:
        for method in methods:
            if method in FM_METHODS:
                observed_widths = {
                    row.get("fm_batch_width", NA) for row in exact_query
                    if row.get("scenario") == scenario and row.get("method") == method and
                    row.get("operation") == "count" and row.get("fm_query_mode") == "batch"
                }
                expected_widths = set(contract["fm_batch_widths"].get(method, ()))
                unexpected_widths = sorted(observed_widths - expected_widths)
                if unexpected_widths:
                    query_failures.append(
                        f"{scenario}/{method}/unexpected_batch_widths=" +
                        ",".join(unexpected_widths))
            for group in EXACT_QUERY_GROUPS:
                for length in pattern_lengths:
                    for strand in strands:
                        require_exact_slice(
                            scenario, method, group, length, strand,
                            "count", NA, "scalar", NA)
                        for limit in locate_limits:
                            require_exact_slice(
                                scenario, method, group, length, strand,
                                "locate", limit, "scalar", NA)
                        if method in FM_METHODS:
                            for width in contract["fm_batch_widths"].get(method, ()):
                                require_exact_slice(
                                    scenario, method, group, length, strand,
                                    "count", NA, "batch", width)
    checks.append(audit_check(
        profile, "matrix_exact_query_slices", query_failures,
        f"all exact query slices have {query_repetitions} measured repetition(s)"
        + ("; quick naive uses 1" if profile == "quick" else "")))

    exact_build_groups = group_rows(
        [row for row in exact if row.get("phase") == "build"], ["scenario", "method"])
    build_failures: list[str] = []
    for scenario in scenarios:
        for method in methods:
            expected = 1 if method == "naive" else build_repetitions
            failure = repetition_failure(
                exact_build_groups.get((scenario, method), []), expected, 0)
            if failure:
                build_failures.append(f"{scenario}/{method}={failure}")
    checks.append(audit_check(
        profile, "matrix_exact_build_repetitions", build_failures,
        f"all exact build slices have {build_repetitions} repetition(s); naive uses 1"))

    sa_rows = []
    for source in all_rows(raw_tables(tables, "sa-build")):
        row = dict(source)
        row["acceleration"] = normalized_acceleration(row.get("acceleration", NA))
        sa_rows.append(row)
    sa_groups = group_rows(
        sa_rows, ["_source_scope", "method", "threads", "sampling_rate", "acceleration"])
    sa_failures: list[str] = []
    sa_build_matrices = contract["sa_build_matrices"]
    for scope_name, tuples in sa_build_matrices.items():
        scope = f"{profile}/{scope_name}"
        for method, threads, sampling_rate, acceleration in tuples:
            key = (scope, method, threads, sampling_rate,
                   normalized_acceleration(acceleration))
            failure = repetition_failure(
                sa_groups.get(key, []), build_repetitions, 1,
                {"not_applicable:threads_exceed_logical_cpus"},
                single_capability_placeholder=True)
            if failure:
                sa_failures.append(
                    f"{scope_name}/{method}/t{threads}/k{sampling_rate}/"
                    f"{acceleration}={failure}")
    checks.append(audit_check(
        profile, "matrix_sa_build_submatrices", sa_failures,
        f"{sum(len(value) for value in sa_build_matrices.values())} "
        "SA-build configurations complete"))

    right = all_rows(raw_tables(tables, "right-maximal"))
    scenario_map = right_scenarios(tables)
    annotated_right: list[dict[str, str]] = []
    for row in right:
        annotated = dict(row)
        annotated["_scenario"] = scenario_map.get(
            (row.get("_source_scope", NA), row.get("dataset", NA)), NA)
        annotated_right.append(annotated)
    right_query = [row for row in annotated_right if row.get("operation") in RIGHT_QUERY_OPERATIONS]
    right_method_failures: list[str] = []
    expected_right_methods = contract["right_methods"]
    for scenario in scenarios:
        observed = {row.get("method") for row in right_query if row.get("_scenario") == scenario}
        right_method_failures.extend(
            f"{scenario}/{method}" for method in expected_right_methods if method not in observed)
    checks.append(audit_check(
        profile, "matrix_right_maximal_methods", right_method_failures,
        f"{len(scenarios)} scenario(s) x {len(expected_right_methods)} method/capability rows"))

    right_query_groups = group_rows(
        right_query, ["_scenario", "method", "operation", "min_length"])
    right_query_failures: list[str] = []
    for scenario in scenarios:
        for method in expected_right_methods:
            for operation in RIGHT_QUERY_OPERATIONS:
                for min_length in contract["right_min_lengths"]:
                    rows = right_query_groups.get(
                        (scenario, method, operation, min_length), [])
                    if method in FM_METHODS:
                        failure = repetition_failure(
                            rows, 1, 0, {"not_supported"}, single_capability_placeholder=True)
                    elif operation == "vector" and \
                            {row_status(row) for row in rows} == {"skipped_high_frequency"}:
                        streaming_rows = right_query_groups.get(
                            (scenario, method, "streaming", min_length), [])
                        failure = controlled_right_vector_skip_failure(
                            rows, streaming_rows, query_repetitions)
                    else:
                        failure = repetition_failure(rows, query_repetitions, 0)
                    if failure:
                        right_query_failures.append(
                            f"{scenario}/{method}/{operation}/min{min_length}={failure}")
    checks.append(audit_check(
        profile, "matrix_right_maximal_query_slices", right_query_failures,
        f"all right-maximal query slices have {query_repetitions} repetition(s); "
        "FM capability rows are not_supported"))

    right_build_groups = group_rows(
        [row for row in annotated_right if row.get("operation") == "build"],
        ["_scenario", "method"])
    right_build_failures: list[str] = []
    for scenario in scenarios:
        for method in tuple(
                method for method in expected_right_methods if method in RIGHT_INTERNAL_METHODS):
            failure = repetition_failure(
                right_build_groups.get((scenario, method), []), build_repetitions, 0)
            if failure:
                right_build_failures.append(f"{scenario}/{method}={failure}")
    checks.append(audit_check(
        profile, "matrix_right_maximal_build_repetitions", right_build_failures,
        f"all right-maximal build slices have {build_repetitions} repetition(s)"))
    return checks


def audit_exact_metadata_contract(
        tables: Sequence[Table], profile: str, run_dir: Path) -> dict[str, str]:
    """Require the metadata schema that defines the measured FM width matrix."""
    exact_raw = raw_tables(tables, "exact")
    raw_scopes = sorted({table.scope for table in exact_raw})
    failures: list[str] = []

    def validate_scope(scope: str, expected: Mapping[str, str]) -> None:
        metadata = [table for table in tables
                    if table.scope == scope and table.name == "run_metadata.tsv"]
        if len(metadata) != 1:
            failures.append(f"{scope}/run_metadata_count={len(metadata)}")
            return
        table = metadata[0]
        missing = [column for column in EXACT_METADATA_COLUMNS
                   if column not in table.columns]
        if missing:
            failures.append(f"{scope}/missing_columns=" + ",".join(missing))
            return
        if not table.rows:
            failures.append(f"{scope}/metadata_rows=0")
            return
        for field, expected_value in expected.items():
            values = {row.get(field, NA) for row in table.rows}
            if values != {expected_value}:
                rendered = ",".join(sorted(values)) or "<empty>"
                failures.append(
                    f"{scope}/{field}={rendered};expected={expected_value}")

    if profile == "headline":
        expected_suffixes = ("exact-build", "exact-query")
        for suffix in expected_suffixes:
            candidates = [scope for scope in raw_scopes
                          if scope.rsplit("/", 1)[-1] == suffix]
            if len(candidates) != 1:
                failures.append(f"headline/{suffix}/raw_scope_count={len(candidates)}")
                continue
            validate_scope(candidates[0], HEADLINE_EXACT_METADATA)
    else:
        expected_metadata = profile_matrix_contract(run_dir, profile)["metadata"]
        if not raw_scopes:
            failures.append(f"{profile}/exact/raw_scope_count=0")
        for scope in raw_scopes:
            validate_scope(scope, expected_metadata)

    return audit_check(
        profile, "exact_metadata_contract", failures,
        ("regular benchmark metadata matches its declared suite matrix" if profile != "headline"
         else "exact-build and exact-query are scalar-only with no overrides"))


def audit_profile(run_dir: Path, profile: str) -> tuple[list[dict[str, str]], bool]:
    """Validate files, schemas, statuses, and checksums for one suite stage."""
    if not run_dir.is_dir():
        raise PackageError(f"run directory does not exist: {run_dir}")
    selected = discover_profile_tables(run_dir, profile)
    checks: list[dict[str, str]] = []
    failed = False

    raw = raw_tables(selected)
    workloads = {table.workload for table in raw}
    required_workloads = ("exact", "right-maximal") if profile == "headline" else \
        ("exact", "sa-build", "right-maximal")
    for workload in required_workloads:
        present = workload in workloads
        checks.append({"profile": profile, "check": f"required_workload_{workload}",
                       "status": "ok" if present else "fail",
                       "details": "present" if present else "raw_repetitions.tsv not found"})
        failed = failed or not present
    unknown = [table.relative for table in raw if table.workload == "unknown"]
    checks.append({"profile": profile, "check": "recognized_raw_schemas",
                   "status": "fail" if unknown else "ok",
                   "details": "; ".join(unknown) or "all raw schemas recognized"})
    failed = failed or bool(unknown)

    required_columns = {
        "exact": {"method", "phase", "query_group", "pattern_length", "strand", "operation",
                  "repetition", "query_count", "skipped_high_frequency_queries", "seconds",
                  "peak_rss_mb", "total_hits",
                  "reported_hits", "result_checksum", "serialized_bytes", "allocated_disk_bytes",
                  "total_bases", "threads", "backend", "backend_signature", "sdsl_version",
                  "coordinate_width", "sa_sampling_rate", "canary_total_hits",
                  "canary_reported_hits", "canary_checksum", "query_threads",
                  "fm_query_mode", "fm_batch_width", "status"},
        "sa-build": {"method", "effective_backend", "threads", "sampling_rate", "acceleration",
                     "repetition", "build_wall_seconds", "build_peak_rss_mb", "serialized_bytes",
                     "allocated_disk_bytes", "sa_checksum", "exact_checksum",
                     "right_maximal_checksum", "status"},
        "right-maximal": {"dataset", "method", "operation", "min_length", "repetition", "seconds",
                          "peak_rss_mb", "peak_rss_scope", "query_bases", "serialized_bytes",
                          "allocated_disk_bytes", "auxiliary_bytes", "total_matches",
                          "materialization_match_threshold", "vector_skipped",
                          "reported_matches", "count_checksum", "result_checksum", "status"},
    }
    for table in raw:
        missing = sorted(required_columns.get(table.workload, set()) - set(table.columns))
        checks.append({"profile": profile, "check": f"schema:{table.relative}",
                       "status": "fail" if missing else "ok",
                       "details": "missing " + ",".join(missing) if missing else
                                  f"{len(table.columns)} columns; {len(table.rows)} rows"})
        failed = failed or bool(missing)

        siblings = {candidate.name for candidate in selected if candidate.scope == table.scope}
        required_files = {"run_metadata.tsv", "build_results.tsv", "raw_repetitions.tsv"}
        if table.workload in {"exact", "right-maximal"}:
            required_files.add("query_results.tsv")
        if table.workload == "right-maximal":
            required_files.add("correctness_summary.tsv")
        missing_files = sorted(required_files - siblings)
        checks.append({"profile": profile, "check": f"files:{table.scope}",
                       "status": "fail" if missing_files else "ok",
                       "details": "missing " + ",".join(missing_files) if missing_files else
                                  "required TSV files present"})
        failed = failed or bool(missing_files)

    exact_metadata = audit_exact_metadata_contract(selected, profile, run_dir)
    checks.append(exact_metadata)
    failed = failed or exact_metadata["status"] != "ok"

    correctness, blocked = summarize_correctness(selected)
    for row in correctness:
        check_status = row["status"]
        checks.append({"profile": profile,
                       "check": f"{row['workload']}:{row['check']}",
                       "status": check_status,
                       "details": f"groups={row['groups_checked']}; mismatches={row['mismatches']}; "
                                  f"details={row['details']}"})
    failed = failed or blocked

    matrix_checks = audit_expected_matrix(selected, profile, run_dir)
    checks.extend(matrix_checks)
    failed = failed or any(row["status"] != "ok" for row in matrix_checks)

    if profile == "headline" and not failed:
        identity_checks = audit_headline_sa_identity(selected, run_dir)
        checks.extend(identity_checks)
        failed = failed or any(row["status"] != "ok" for row in identity_checks)

    if profile == "headline" and not failed:
        builds = aggregate_build_headline(selected)
        exact = aggregate_exact_headline(selected)
        right = aggregate_right_maximal_headline(selected)
        headline_status = "ok" if all(status_of(rows) == "ok" for rows in (builds, exact, right)) else "fail"
        details = []
        for row in [*builds, *exact, *right]:
            if row.get("status") != "ok":
                identity = row.get("index") or row.get("method") or "unknown"
                details.append(f"{identity}={row.get('status')}")
        checks.append({"profile": profile, "check": "headline_raw_completeness",
                       "status": headline_status,
                       "details": "; ".join(details) or "fixed 3-build/5-query raw evidence complete"})
        failed = failed or headline_status != "ok"
    return checks, failed


def print_audit(checks: Sequence[Mapping[str, str]]) -> None:
    writer = csv.DictWriter(sys.stdout, fieldnames=["profile", "check", "status", "details"],
                            delimiter="\t", lineterminator="\n")
    writer.writeheader()
    for row in checks:
        writer.writerow({key: clean_cell(row.get(key)) for key in writer.fieldnames})


def headline_reference_identity(run_dir: Path) -> dict[str, str]:
    """Recompute both benchmark fingerprint dialects for the exported FASTA.

    The exact benchmark fingerprints sufkit's normalized byte encoding,
    including one separator byte after every contig.  The right-maximal
    benchmark fingerprints the normalized ASCII bases without separators.
    They intentionally differ, so the SHA-256 manifest is the common identity
    anchor rather than equality between the two FNV values.
    """
    manifest = run_dir / "manifest" / "headline-dataset.sha256"
    if not manifest.is_file():
        raise PackageError("headline dataset SHA-256 manifest is missing")
    candidates: list[tuple[str, str]] = []
    for line_number, raw_line in enumerate(
            manifest.read_text(encoding="utf-8").splitlines(), start=1):
        parts = raw_line.strip().split(maxsplit=1)
        if len(parts) != 2 or re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]) is None:
            raise PackageError(
                f"invalid headline dataset SHA-256 manifest line {line_number}")
        path_text = parts[1].lstrip("*")
        if Path(path_text).name == "reference.fa":
            candidates.append((parts[0].lower(), path_text))
    if len(candidates) != 1:
        raise PackageError(
            "headline dataset SHA-256 manifest must name exactly one reference.fa")
    expected_sha256, path_text = candidates[0]
    reference = Path(path_text)
    if not reference.is_absolute():
        possible = (
            manifest.parent / reference,
            run_dir / reference,
            run_dir.parent.parent / reference,
            Path.cwd() / reference,
        )
        reference = next((candidate for candidate in possible if candidate.is_file()), possible[0])
    if not reference.is_file():
        raise PackageError("headline exported reference.fa is unavailable")

    sha256 = hashlib.sha256()
    exact_fingerprint = 14695981039346656037
    right_fingerprint = 1469598103934665603
    fnv_prime = 1099511628211
    mask = (1 << 64) - 1
    exact_codes = {ord("A"): 2, ord("C"): 3, ord("G"): 4, ord("T"): 5}
    right_codes = {ord("A"): ord("A"), ord("C"): ord("C"),
                   ord("G"): ord("G"), ord("T"): ord("T")}
    total_bases = 0
    contigs = 0
    in_record = False
    record_bases = 0

    def mix(value: int, byte: int) -> int:
        return ((value ^ byte) * fnv_prime) & mask

    with reference.open("rb") as stream:
        for raw_line in stream:
            sha256.update(raw_line)
            line = raw_line.rstrip(b"\r\n")
            if line.startswith(b">"):
                if in_record and record_bases == 0:
                    raise PackageError("headline exported reference contains an empty contig")
                if in_record:
                    exact_fingerprint = mix(exact_fingerprint, 1)
                if not line[1:].split(maxsplit=1):
                    raise PackageError("headline exported reference contains an empty name")
                in_record = True
                record_bases = 0
                contigs += 1
                continue
            sequence = b"".join(line.split())
            if not sequence:
                continue
            if not in_record:
                raise PackageError("headline exported reference has sequence before its header")
            for raw_base in sequence:
                base = raw_base - 32 if ord("a") <= raw_base <= ord("z") else raw_base
                exact_code = exact_codes.get(base, 6)
                right_code = right_codes.get(base, ord("N"))
                exact_fingerprint = mix(exact_fingerprint, exact_code)
                right_fingerprint = mix(right_fingerprint, right_code)
                total_bases += 1
                record_bases += 1
    if not in_record:
        raise PackageError("headline exported reference contains no FASTA records")
    if record_bases == 0:
        raise PackageError("headline exported reference contains an empty contig")
    exact_fingerprint = mix(exact_fingerprint, 1)
    actual_sha256 = sha256.hexdigest()
    if actual_sha256 != expected_sha256:
        raise PackageError("headline exported reference SHA-256 does not match its manifest")
    return {
        "sha256": actual_sha256,
        "total_bases": str(total_bases),
        "contigs": str(contigs),
        "exact_fingerprint": f"{exact_fingerprint:016x}",
        # right_maximal_benchmark.cpp writes this value with std::hex but no
        # fixed width; preserve that producer representation for metadata
        # comparison (the exact benchmark uses a padded 16-digit helper).
        "right_fingerprint": f"{right_fingerprint:x}",
    }


def audit_headline_sa_identity(
        tables: Sequence[Table], run_dir: Path) -> list[dict[str, str]]:
    scope, selected = unified_headline_build_rows(tables)
    output: list[dict[str, str]] = []
    methods = {row.get("method") for row in selected}
    scopes = {row.get("_source_scope") for row in selected}
    same_scope = scope is not None and methods == set(HEADLINE_BUILD_METHODS) and scopes == {scope}
    output.append({"profile": "headline", "check": "headline_build_same_worker_scope",
                   "status": "ok" if same_scope else "fail",
                   "details": f"methods={','.join(sorted(str(value) for value in methods))}; "
                              f"scope={scope or NA}"})

    repetitions_valid = True
    repetition_details = []
    for method in HEADLINE_BUILD_METHODS:
        rows = [row for row in selected if row.get("method") == method]
        repetitions = {integer(row.get("repetition")) for row in rows}
        valid = len(rows) == 3 and repetitions == {0, 1, 2}
        repetitions_valid = repetitions_valid and valid
        repetition_text = ",".join(
            str(value) for value in sorted(
                value for value in repetitions if value is not None))
        repetition_details.append(f"{method}={repetition_text or NA}")
    output.append({"profile": "headline", "check": "headline_build_repetitions",
                   "status": "ok" if repetitions_valid else "fail",
                   "details": "; ".join(repetition_details)})

    thread_values = {
        method: {row.get("threads", NA) for row in selected if row.get("method") == method}
        for method in HEADLINE_BUILD_METHODS
    }
    thread_valid = thread_values["sa32-binary"] == {"1"} and \
        thread_values["caps32"] == {"64"} and thread_values["fm-huff"] == {"1"}
    output.append({"profile": "headline", "check": "headline_build_threads",
                   "status": "ok" if thread_valid else "fail",
                   "details": "; ".join(
                       f"{method}={','.join(sorted(values))}" for method, values in thread_values.items())})

    provenance_failures: list[str] = []
    for method in HEADLINE_BUILD_METHODS:
        method_rows = [row for row in selected if row.get("method") == method]
        fields = ["backend", "backend_signature", "coordinate_width", "query_threads"]
        if method != "fm-huff":
            fields.append("sa_sampling_rate")
        for field in fields:
            values = {row.get(field, NA) for row in method_rows} - {NA, ""}
            if len(values) != 1:
                provenance_failures.append(
                    f"{method}/{field}={','.join(sorted(values)) or NA}")
    for method in ("sa32-binary", "caps32"):
        widths = {row.get("coordinate_width", NA) for row in selected
                  if row.get("method") == method}
        sampling = {row.get("sa_sampling_rate", NA) for row in selected
                    if row.get("method") == method}
        if widths != {"32"}:
            provenance_failures.append(f"{method}/coordinate_width_expected_32")
        if sampling != {"1"}:
            provenance_failures.append(f"{method}/sa_sampling_rate_expected_1")
    sdsl_versions = {row.get("sdsl_version", NA) for row in selected
                     if row.get("method") == "fm-huff"} - {NA, ""}
    if len(sdsl_versions) != 1:
        provenance_failures.append(
            "fm-huff/sdsl_version=" + (",".join(sorted(sdsl_versions)) or NA))
    output.append({"profile": "headline", "check": "headline_build_provenance",
                   "status": "fail" if provenance_failures else "ok",
                   "details": "; ".join(provenance_failures[:12]) or
                              "backend/signature/width/sampling/threads provenance stable"})

    for field in ("dataset", "total_bases"):
        per_method = {}
        for method in HEADLINE_BUILD_METHODS:
            values = {row.get(field, NA) for row in selected if row.get("method") == method}
            values.discard(NA)
            per_method[method] = values
        valid = all(len(values) == 1 for values in per_method.values()) and \
            len({next(iter(values)) for values in per_method.values()}) == 1
        detail = "; ".join(f"{method}={','.join(sorted(values)) or NA}"
                           for method, values in per_method.items())
        output.append({"profile": "headline", "check": f"headline_build_{field}_equivalence",
                       "status": "ok" if valid else "fail", "details": detail})

    datasets = {row.get("dataset") for row in selected if row.get("dataset") not in {None, NA}}
    fingerprints: set[str] = set()
    for table in tables:
        if table.scope != scope or table.name != "run_metadata.tsv":
            continue
        for row in table.rows:
            if datasets and row.get("dataset") not in datasets:
                continue
            value = first_present(row, ["dataset_fingerprint", "fingerprint"])
            if value != NA:
                fingerprints.add(value)
    valid_fingerprint = len(datasets) == 1 and len(fingerprints) == 1
    output.append({"profile": "headline",
                   "check": "headline_build_dataset_fingerprint_equivalence",
                   "status": "ok" if valid_fingerprint else "fail",
                   "details": f"dataset={','.join(sorted(datasets)) or NA}; "
                              f"fingerprint={','.join(sorted(fingerprints)) or NA}"})

    checksum_fields = ("canary_total_hits", "canary_reported_hits", "canary_checksum")
    comparable: list[tuple[str, set[str], set[str]]] = []
    for field in checksum_fields:
        values = []
        for method in ("sa32-binary", "caps32"):
            method_values = {row.get(field, NA) for row in selected
                             if row.get("method") == method} - {NA, ""}
            if field == "canary_checksum":
                method_values -= {"0", "0000000000000000"}
            values.append(method_values)
        comparable.append((field, values[0], values[1]))
    checksum_valid = all(len(left) == len(right) == 1 and left == right
                         for _, left, right in comparable)
    checksum_detail = "; ".join(
        f"{field}:sa32={','.join(sorted(left)) or NA},caps32={','.join(sorted(right)) or NA}"
        for field, left, right in comparable)
    output.append({"profile": "headline", "check": "headline_sa_canary_checksum_equivalence",
                   "status": "ok" if checksum_valid else "fail", "details": checksum_detail})

    query_scopes = sorted({table.scope for table in raw_tables(tables, "exact", headline_only=True)
                           if table.scope.rsplit("/", 1)[-1] == "exact-query"})
    query_scope = query_scopes[0] if len(query_scopes) == 1 else None
    identity_fields = (
        "dataset_fingerprint", "seed", "scenario", "total_bases",
        "query_set_checksum", "query_count", "query_bases",
    )
    build_metadata = [row for table in tables
                      if table.scope == scope and table.name == "run_metadata.tsv"
                      for row in table.rows]
    query_metadata = [row for table in tables
                      if table.scope == query_scope and table.name == "run_metadata.tsv"
                      for row in table.rows]
    identity_failures: list[str] = []
    for field in identity_fields:
        build_values = {row.get(field, NA) for row in build_metadata} - {NA, ""}
        query_values = {row.get(field, NA) for row in query_metadata} - {NA, ""}
        if len(build_values) != 1 or build_values != query_values:
            identity_failures.append(
                f"{field}:build={','.join(sorted(build_values)) or NA},"
                f"query={','.join(sorted(query_values)) or NA}")
    if query_scope is None:
        identity_failures.insert(0, "exact-query scope missing or ambiguous")
    output.append({"profile": "headline", "check": "headline_exact_build_query_identity",
                   "status": "fail" if identity_failures else "ok",
                   "details": "; ".join(identity_failures[:12]) or
                              f"build_scope={scope}; query_scope={query_scope}"})

    exported_reference: dict[str, str] = {}
    exported_failure = ""
    try:
        exported_reference = headline_reference_identity(run_dir)
    except (OSError, UnicodeError, PackageError) as error:
        exported_failure = str(error)
    output.append({
        "profile": "headline",
        "check": "headline_exported_reference_identity",
        "status": "fail" if exported_failure else "ok",
        "details": exported_failure or (
            f"sha256={exported_reference['sha256']}; "
            f"exact_fingerprint={exported_reference['exact_fingerprint']}; "
            f"right_fingerprint={exported_reference['right_fingerprint']}"),
    })

    fixed_failures: list[str] = []

    def require_metadata(scope_name: str | None, label: str,
                         expected: Mapping[str, str]) -> None:
        metadata = [row for table in tables
                    if table.scope == scope_name and table.name == "run_metadata.tsv"
                    for row in table.rows]
        if not metadata:
            fixed_failures.append(f"{label}/metadata=missing")
            return
        for field, expected_value in expected.items():
            values = {row.get(field, NA) for row in metadata}
            if values != {expected_value}:
                fixed_failures.append(
                    f"{label}/{field}={','.join(sorted(values)) or NA}; expected={expected_value}")

    fixed_exact = {
        "profile": "full", "scenario": "mixed", "total_bases": "268435456",
        "contigs": "4", "query_count": "10000", "seed": "20260822",
    }
    if exported_reference:
        fixed_exact.update({
            "dataset_fingerprint": exported_reference["exact_fingerprint"],
            "total_bases": exported_reference["total_bases"],
            "contigs": exported_reference["contigs"],
        })
    require_metadata(scope, "exact-build", fixed_exact)
    require_metadata(query_scope, "exact-query", fixed_exact)

    exact_headline = aggregate_exact_headline(tables)
    if len(exact_headline) != 4 or any(row.get("status") != "ok" for row in exact_headline):
        fixed_failures.append("exact-query/raw=missing length100 forward count/locate1 repetitions")

    right_scopes = sorted({table.scope for table in raw_tables(tables, "right-maximal", headline_only=True)
                           if table.scope.rsplit("/", 1)[-1] == "right-maximal"})
    right_scope = right_scopes[0] if len(right_scopes) == 1 else None
    fixed_right = {
        "profile": "full", "scenario": "user-reference", "seed": "20260822",
        "total_bases": "268435456", "contigs": "4",
        "query_count": "10000", "query_bases": "2560000",
    }
    if exported_reference:
        fixed_right.update({
            "dataset_fingerprint": exported_reference["right_fingerprint"],
            "total_bases": exported_reference["total_bases"],
            "contigs": exported_reference["contigs"],
        })
    require_metadata(right_scope, "right-maximal", fixed_right)
    right_headline = aggregate_right_maximal_headline(tables)
    if len(right_headline) != 2 or any(row.get("status") != "ok" for row in right_headline):
        fixed_failures.append(
            "right-maximal/raw=missing baseline/suffix-link min50 streaming repetitions")
    output.append({"profile": "headline", "check": "headline_fixed_workload_contract",
                   "status": "fail" if fixed_failures else "ok",
                   "details": "; ".join(fixed_failures[:12]) or
                              "synthetic full/mixed export reused as user-reference; "
                              "256MiB, 4 contigs, 10000-query fixed workload"})
    return output


def _package_into(run_dir: Path, output_dir: Path, server_label: str,
                  revision_override: str | None, require_complete: bool) -> int:
    run_dir = run_dir.resolve()
    output_dir = output_dir.resolve()
    if not run_dir.is_dir():
        raise PackageError(f"run directory does not exist: {run_dir}")
    try:
        output_dir.relative_to(run_dir)
    except ValueError:
        pass
    else:
        raise PackageError("output directory must be outside the input run directory")
    if output_dir.exists():
        raise PackageError(f"output directory already exists: {output_dir}")
    output_dir.mkdir(parents=True)
    tables = discover_tables(run_dir)
    revision = read_revision(run_dir, revision_override)

    correctness, blocked = summarize_correctness(tables)
    headline_tables = tables_for_profile(tables, "headline")
    if require_complete:
        for profile in ("smoke", "quick", "standard", "full", "headline"):
            profile_checks, profile_failed = audit_profile(run_dir, profile)
            for row in profile_checks:
                # Match the audit CLI's exit semantics exactly.  Informational
                # ``partial`` checks (for example, a workload deliberately not
                # required by headline) are not failures unless audit_profile
                # also reports the profile failed.
                failed = row["status"] == "fail"
                audit_detail = row["details"]
                if row["status"] != ("fail" if failed else "ok"):
                    audit_detail = f"audit_status={row['status']}; {audit_detail}"
                correctness.append({
                    "workload": "profile-audit",
                    "check": f"{profile}:{row['check']}",
                    "groups_checked": "1",
                    "mismatches": "1" if failed else "0",
                    "status": "fail" if failed else "ok",
                    "details": audit_detail,
                })
            blocked = blocked or profile_failed
    elif raw_tables(headline_tables, "exact"):
        for row in audit_headline_sa_identity(headline_tables, run_dir):
            failed = row["status"] != "ok"
            correctness.append({"workload": "sa-build", "check": row["check"],
                                "groups_checked": "1", "mismatches": "1" if failed else "0",
                                "status": "fail" if failed else "ok", "details": row["details"]})
            blocked = blocked or failed
    write_tsv(output_dir / "correctness-summary.tsv", correctness,
              ["workload", "check", "groups_checked", "mismatches", "status", "details"])
    write_tsv(output_dir / "environment.tsv", environment_rows(run_dir, tables, revision, server_label),
              ["key", "value", "source"])
    write_tsv(output_dir / "manifest.tsv", manifest_rows(run_dir, tables),
              ["path", "bytes", "sha256", "rows", "columns", "schema", "schema_sha256",
               "workload", "profile"])
    write_commands(output_dir)

    build_rows = aggregate_sa_build_rows(tables) + aggregate_exact_build_rows(tables)
    build_rows.sort(key=lambda row: (
        row.get("profile", ""), row.get("source_scope", ""), row.get("method", ""),
        integer(row.get("threads")) or 0, integer(row.get("sampling_rate")) or 0))
    exact_rows = aggregate_exact_rows(tables)
    right_rows = aggregate_right_maximal_rows(tables)
    write_tsv(output_dir / "build" / "build-results.tsv", build_rows, BUILD_AGG_COLUMNS)
    write_tsv(output_dir / "build" / "caps-scaling.tsv",
              [row for row in build_rows if row.get("method", "").startswith("caps")],
              BUILD_AGG_COLUMNS)
    write_tsv(output_dir / "build" / "sampling-space.tsv",
              [row for row in build_rows if row.get("sampling_rate") not in {NA, "1"}],
              BUILD_AGG_COLUMNS)
    build_raw = all_rows(raw_tables(tables, "sa-build"))
    build_raw.extend(row for row in all_rows(raw_tables(tables, "exact"))
                     if row.get("phase") in {"build", "save", "load"})
    write_tsv(output_dir / "build" / "raw-repetitions.tsv", build_raw,
              ["_source_file", "_source_scope", "_profile", "_source_line"] +
              [column for column in union_columns(build_raw) if not column.startswith("_")])

    write_tsv(output_dir / "exact" / "count-results.tsv",
              [row for row in exact_rows if row.get("operation") == "count"],
              EXACT_AGG_COLUMNS)
    write_tsv(output_dir / "exact" / "locate-results.tsv",
              [row for row in exact_rows if row.get("operation") == "locate"],
              EXACT_AGG_COLUMNS)
    write_tsv(output_dir / "exact" / "fm-batch-results.tsv",
              [row for row in exact_rows if row.get("method", "").startswith("fm") and
               row.get("fm_query_mode") == "batch"], EXACT_AGG_COLUMNS)
    copy_raw_union(output_dir / "exact" / "raw-repetitions.tsv", raw_tables(tables, "exact"))

    write_tsv(output_dir / "right-maximal" / "algorithm-ablation.tsv", right_rows, RIGHT_AGG_COLUMNS)
    write_tsv(output_dir / "right-maximal" / "sampled-sa-results.tsv",
              [row for row in right_rows if "sampled" in row.get("method", "")],
              RIGHT_AGG_COLUMNS)
    copy_raw_union(output_dir / "right-maximal" / "raw-repetitions.tsv",
                   raw_tables(tables, "right-maximal"))

    gc_scenarios = {"mixed", "balanced", "gc-skewed", "repeat-rich"}
    boundary_scenarios = {"mixed", "n-islands", "many-contig"}
    write_tsv(output_dir / "scenarios" / "gc-and-repeat-effects.tsv",
              [row for row in exact_rows if row.get("scenario") in gc_scenarios],
              EXACT_AGG_COLUMNS)
    write_tsv(output_dir / "scenarios" / "contig-and-n-effects.tsv",
              [row for row in exact_rows if row.get("scenario") in boundary_scenarios],
              EXACT_AGG_COLUMNS)
    write_section_readmes(output_dir)
    write_detail_figures(output_dir, build_rows, exact_rows, right_rows)

    headline_status = "blocked"
    if not blocked:
        headline_status = write_headline(
            output_dir, aggregate_build_headline(tables),
            aggregate_exact_headline(tables), aggregate_right_maximal_headline(tables))

    run_complete = (run_dir / "state" / "ALL_COMPLETE").exists()
    correctness_complete = all(row.get("status") == "ok" for row in correctness)
    overall = "blocked" if blocked else (
        "complete" if run_complete and headline_status == "ok" and correctness_complete else "partial")
    headline_link = "- [Headline](headline/README.md)\n" if not blocked else ""
    readme = f"""# sufkit server benchmark result package

**Package status: {overall}.** Source revision: `{revision}`. Server label:
`{server_label}`. The input run completion marker was
`{'present' if run_complete else 'absent'}`.

{headline_link}- [Construction](build/README.md)
- [Exact query](exact/README.md)
- [Right-maximal exact match](right-maximal/README.md)
- [Scenario effects](scenarios/README.md)
- [Correctness audit](correctness-summary.tsv)
- [Environment](environment.tsv)
- [Input manifest](manifest.tsv)
- [Commands](commands.md)

The package contains TSV, Markdown, manifest hashes, and SVG only. Large index
files, generated FASTA, worker scratch data, and logs remain in the server run.
Results are controlled synthetic evidence; no real-genome benchmark is implied.
"""
    (output_dir / "README.md").write_text(readme, encoding="utf-8")
    if blocked:
        return 1
    if require_complete and overall != "complete":
        return 2
    return 0


def package(run_dir: Path, output_dir: Path, server_label: str,
            revision_override: str | None, require_complete: bool) -> int:
    """Build in a unique sibling directory and publish by one atomic rename."""
    run_dir = run_dir.resolve()
    output_dir = output_dir.resolve()
    if not run_dir.is_dir():
        raise PackageError(f"run directory does not exist: {run_dir}")
    try:
        output_dir.relative_to(run_dir)
    except ValueError:
        pass
    else:
        raise PackageError("output directory must be outside the input run directory")
    if output_dir.exists():
        raise PackageError(f"output directory already exists: {output_dir}")
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    staging = output_dir.parent / (
        f".{output_dir.name}.partial.{os.getpid()}.{secrets.token_hex(6)}")
    try:
        status = _package_into(
            run_dir, staging, server_label, revision_override, require_complete)
    except Exception:
        print(f"benchmark packaging retained failed partial directory: {staging}",
              file=sys.stderr)
        raise
    if status != 0:
        print(f"benchmark packaging retained incomplete partial directory: {staging}",
              file=sys.stderr)
        return status
    if output_dir.exists():
        raise PackageError(f"output directory appeared during packaging: {output_dir}")
    staging.rename(output_dir)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "audit":
            checks, failed = audit_profile(args.run_dir.resolve(), args.profile)
            print_audit(checks)
            return 1 if failed else 0
        return package(args.run_dir, args.output_dir, args.server_label,
                       args.revision, args.require_complete)
    except (OSError, csv.Error, PackageError) as error:
        print(f"benchmark packaging error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
