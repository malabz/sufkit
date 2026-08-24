#!/usr/bin/env python3
"""Render benchmarks/README.md from a validated completed-stage snapshot."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path
import sys
from typing import Mapping, Sequence

import update_benchmark_readme as strict


class SnapshotHeadlineError(RuntimeError):
    pass


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="update benchmark headline from a stage snapshot")
    parser.add_argument("--package-dir", required=True, type=Path)
    parser.add_argument("--readme", type=Path,
                        default=Path(__file__).resolve().parent / "README.md")
    parser.add_argument("--check", action="store_true")
    return parser.parse_args(argv)


def read_tsv(path: Path) -> list[dict[str, str]]:
    if not path.is_file():
        raise SnapshotHeadlineError(f"required file is missing: {path}")
    with path.open("r", encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames is None or len(reader.fieldnames) != len(set(reader.fieldnames)):
            raise SnapshotHeadlineError(f"invalid TSV header: {path}")
        rows = list(reader)
    if not rows:
        raise SnapshotHeadlineError(f"TSV contains no rows: {path}")
    return rows


def env_map(rows: Sequence[Mapping[str, str]]) -> dict[str, str]:
    result: dict[str, str] = {}
    for row in rows:
        key = row.get("key", "")
        if not key or key in result:
            raise SnapshotHeadlineError(f"duplicate or empty environment key: {key}")
        result[key] = row.get("value", "")
    required = {
        "snapshot_status": "completed_stages",
        "run_complete": "false",
        "standard_scope": "representative_mixed_only",
        "full_scope": "representative_mixed_only",
        "headline_right_maximal_source": "full/right-maximal",
        "headline_right_maximal_query_repetitions": "3",
        "excluded_stage": "headline/right-maximal",
    }
    for key, value in required.items():
        if result.get(key) != value:
            raise SnapshotHeadlineError(
                f"unexpected snapshot environment value: {key}={result.get(key)}")
    return result


def validate(package: Path) -> tuple[list[dict[str, str]], list[dict[str, str]],
                                     list[dict[str, str]], dict[str, str]]:
    if not package.is_dir():
        raise SnapshotHeadlineError(f"snapshot directory does not exist: {package}")
    readme = (package / "README.md").read_text(encoding="utf-8")
    if "**Snapshot status: completed stages.**" not in readme:
        raise SnapshotHeadlineError("snapshot README lacks completed-stage declaration")
    environment = env_map(read_tsv(package / "environment.tsv"))
    correctness = read_tsv(package / "correctness-summary.tsv")
    for row in correctness:
        if row.get("status") != "ok" or row.get("mismatches") != "0":
            raise SnapshotHeadlineError(
                f"non-ok correctness row: {row.get('workload')}/{row.get('check')}")
    builds = read_tsv(package / "headline" / "headline-build.tsv")
    exact = read_tsv(package / "headline" / "headline-exact.tsv")
    right = read_tsv(package / "headline" / "headline-right-maximal.tsv")
    if len(builds) != 3 or len(exact) != 4 or len(right) != 2:
        raise SnapshotHeadlineError("headline tables have unexpected row counts")
    for label, rows, repetitions in (("build", builds, "3"),
                                      ("exact", exact, "5"),
                                      ("right-maximal", right, "3")):
        for row in rows:
            if row.get("status") != "ok" or row.get("raw_repetitions") != repetitions or \
                    row.get("expected_repetitions") != repetitions:
                raise SnapshotHeadlineError(f"invalid {label} headline evidence")
    if not (package / "figures" / "headline-performance.svg").is_file():
        raise SnapshotHeadlineError("headline SVG is missing")
    return builds, exact, right, environment


def fmt(value: str, digits: int = 2) -> str:
    return f"{float(value):,.{digits}f}"


def managed_region(package: Path, builds: Sequence[Mapping[str, str]],
                   exact: Sequence[Mapping[str, str]], right: Sequence[Mapping[str, str]],
                   env: Mapping[str, str]) -> str:
    relative = "results/" + package.name
    build_lines = [
        "| Index | Builder | Threads | Build time | Peak RSS | Index size | bits/base |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for row in builds:
        build_lines.append(
            f"| {row['index']} | {row['builder']} | {row['threads']} | "
            f"{fmt(row['build_time_seconds'])} s | {fmt(row['peak_rss_mb'])} MiB | "
            f"{int(float(row['index_size_bytes'])) / 2**20:,.2f} MiB | "
            f"{fmt(row['bits_per_base'])} |")
    exact_lines = [
        "| Index | Operation | Throughput | ns/query | Query peak RSS |",
        "|---|---:|---:|---:|---:|",
    ]
    for row in exact:
        exact_lines.append(
            f"| {row['index']} | {row['operation']} | "
            f"{float(row['queries_per_second']) / 1e6:.4f} M queries/s | "
            f"{fmt(row['nanoseconds_per_query'])} | {fmt(row['query_peak_rss_mb'])} MiB |")
    right_lines = [
        "| Method | Query throughput | Matches/s | Speedup | Repetitions |",
        "|---|---:|---:|---:|---:|",
    ]
    for row in right:
        right_lines.append(
            f"| {row['method']} | {float(row['query_bases_per_second']) / 1e6:.3f} M bases/s | "
            f"{fmt(row['matches_per_second'])} | {fmt(row['speedup_vs_baseline'])}x | "
            f"{row['raw_repetitions']} |")
    return "\n".join([
        strict.START_MARKER,
        "## Latest completed-stage server snapshot",
        "",
        "256 MiB synthetic `mixed` genome, 4 contigs, seed `20260822`; exact 100 bp, "
        "forward, locate-1; maximum-right-match query length 256 bp and minimum length 50 bp.",
        "",
        "### Construction",
        "", *build_lines, "",
        "### Exact count and locate-1", "", *exact_lines, "",
        "### Maximum right matches", "", *right_lines, "",
        f"![Headline benchmark]({relative}/figures/headline-performance.svg)", "",
        "Headline exact uses dedicated 3-build/5-query runs; maximum-right-match uses the "
        "audited full/mixed 3-query run because the dedicated headline right-maximal stage "
        "was excluded. Standard and full are representative mixed experiments, not full "
        "six-scenario scans.", "",
        f"[Completed-stage result snapshot]({relative}/README.md)",
        strict.END_MARKER,
    ])


def replace_region(text: str, region: str) -> str:
    start = text.find(strict.START_MARKER)
    end = text.find(strict.END_MARKER)
    if start < 0 or end < start:
        raise SnapshotHeadlineError("benchmark README managed markers are missing or reversed")
    end += len(strict.END_MARKER)
    return text[:start] + region + text[end:]


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        package = args.package_dir.resolve()
        builds, exact, right, environment = validate(package)
        current = args.readme.read_text(encoding="utf-8")
        desired = replace_region(current, managed_region(package, builds, exact, right, environment))
        if args.check:
            if current != desired:
                raise SnapshotHeadlineError("managed README headline is out of date")
        else:
            args.readme.write_text(desired, encoding="utf-8")
    except (SnapshotHeadlineError, OSError, ValueError) as error:
        print(f"snapshot README update failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
