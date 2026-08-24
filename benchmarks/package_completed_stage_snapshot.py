#!/usr/bin/env python3
"""Package audited, completed benchmark stages into a reviewable snapshot.

This is intentionally separate from ``package_benchmark_results.py package``.
The latter remains the publication gate for a fully completed run.  This tool
accepts only the explicitly enumerated completed stages and never treats an
unfinished dedicated headline right-maximal run as evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import secrets
import shutil
import statistics
import sys
from typing import Mapping, Sequence

import package_benchmark_results as core


SNAPSHOT_STATUS = "completed_stages"
PROFILES = ("smoke", "quick", "standard", "full")
REQUIRED_MARKERS = (
    "smoke_audit.complete",
    "quick_audit.complete",
    "standard_audit.complete",
    "full_audit.complete",
    "headline_exact_build.complete",
    "headline_exact_query.complete",
)
MANIFEST_COLUMNS = (
    "artifact_kind", "path", "bytes", "sha256", "rows", "columns",
    "schema", "schema_sha256", "workload", "profile",
)


class SnapshotError(RuntimeError):
    pass


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="package only audited, completed stages of a benchmark run")
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--server-label", required=True)
    parser.add_argument("--source-archive-sha256", default="NA")
    return parser.parse_args(argv)


def require_markers(run_dir: Path) -> None:
    missing = [name for name in REQUIRED_MARKERS
               if not (run_dir / "state" / name).is_file()]
    if missing:
        raise SnapshotError("missing completed-stage marker(s): " + ", ".join(missing))


def require_all_ok(rows: Sequence[Mapping[str, str]], label: str,
                   expected_repetitions: int | None = None) -> None:
    if not rows:
        raise SnapshotError(f"{label}: no aggregate rows")
    failures = [row for row in rows if row.get("status") != "ok"]
    if failures:
        raise SnapshotError(f"{label}: contains {len(failures)} non-ok row(s)")
    if expected_repetitions is not None:
        bad = [row for row in rows if
               row.get("raw_repetitions") != str(expected_repetitions) or
               row.get("expected_repetitions") != str(expected_repetitions)]
        if bad:
            raise SnapshotError(
                f"{label}: expected exactly {expected_repetitions} raw repetitions")


def full_right_headline(tables: Sequence[core.Table]) -> list[dict[str, str]]:
    raw = [row for table in tables
           if table.name == "raw_repetitions.tsv" and
           table.workload == "right-maximal" and table.profile == "full" and
           table.scope == "full/right-maximal"
           for row in table.rows]
    specs = (
        ("SA baseline", "right-maximal-baseline"),
        ("SA suffix-link default", "right-maximal-suffix-link"),
    )
    output: list[dict[str, str]] = []
    for display, method in specs:
        selected = [row for row in raw if row.get("method") == method and
                    row.get("operation") == "streaming" and
                    row.get("min_length") == "50" and row.get("status") == "ok"]
        repetitions = {core.integer(row.get("repetition")) for row in selected}
        checksums = {row.get("result_checksum", core.NA) for row in selected}
        count_checksums = {row.get("count_checksum", core.NA) for row in selected}
        totals = {row.get("total_matches", core.NA) for row in selected}
        bases = {row.get("query_bases", core.NA) for row in selected}
        seconds = [core.number(row.get("seconds")) for row in selected]
        complete = (
            len(selected) == 3 and repetitions == {0, 1, 2} and
            len(checksums) == len(count_checksums) == len(totals) == len(bases) == 1 and
            core.NA not in checksums and core.NA not in count_checksums and
            all(value is not None and value > 0 for value in seconds)
        )
        median_seconds = statistics.median(value for value in seconds if value is not None) \
            if complete else None
        total_matches = core.number(next(iter(totals))) if len(totals) == 1 else None
        query_bases = core.number(next(iter(bases))) if len(bases) == 1 else None
        output.append({
            "method": display,
            "query_bases": core.format_integer(query_bases),
            "seconds_median": core.format_number(median_seconds),
            "query_bases_per_second": core.format_number(
                core.ratio(query_bases, median_seconds)),
            "matches_per_second": core.format_number(
                core.ratio(total_matches, median_seconds)),
            "total_matches": core.format_integer(total_matches),
            "speedup_vs_baseline": core.NA,
            "result_checksum": next(iter(checksums)) if len(checksums) == 1 else core.NA,
            "raw_repetitions": str(len(selected)),
            "expected_repetitions": "3",
            "status": "ok" if complete else "unavailable",
        })
    baseline = core.number(output[0]["seconds_median"])
    for row in output:
        row["speedup_vs_baseline"] = core.format_number(
            core.ratio(baseline, row["seconds_median"]))
    return output


def long_headline(builds: Sequence[Mapping[str, str]],
                  exact: Sequence[Mapping[str, str]],
                  right: Sequence[Mapping[str, str]]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for row in builds:
        for metric, unit in (("build_time_seconds", "seconds"),
                             ("peak_rss_mb", "MiB"),
                             ("index_size_bytes", "bytes"),
                             ("bits_per_base", "bits/base")):
            rows.append({"section": "build", "item": row["index"], "metric": metric,
                         "value": row[metric], "unit": unit, "status": row["status"],
                         "provenance": "headline/exact-build/raw_repetitions.tsv"})
    for row in exact:
        rows.append({"section": "exact", "item": f"{row['index']} / {row['operation']}",
                     "metric": "queries_per_second", "value": row["queries_per_second"],
                     "unit": "queries/s", "status": row["status"],
                     "provenance": "headline/exact-query/raw_repetitions.tsv"})
    for row in right:
        rows.append({"section": "right-maximal", "item": row["method"],
                     "metric": "query_bases_per_second",
                     "value": row["query_bases_per_second"], "unit": "bases/s",
                     "status": row["status"],
                     "provenance": "full/right-maximal/raw_repetitions.tsv"})
    return rows


def correctness_rows(tables: Sequence[core.Table], run_dir: Path) -> list[dict[str, str]]:
    rows, blocked = core.summarize_correctness(tables)
    if blocked:
        raise SnapshotError("cross-method correctness summary is blocked")
    output = list(rows)
    for profile in PROFILES:
        checks, failed = core.audit_profile(run_dir, profile)
        for check in checks:
            output.append({
                "workload": "profile-audit",
                "check": f"{profile}:{check['check']}",
                "groups_checked": "1",
                "mismatches": "0" if check["status"] == "ok" else "1",
                "status": check["status"],
                "details": check["details"],
            })
        if failed:
            raise SnapshotError(f"{profile} audit failed")
    custom = (
        ("completed_stage_markers", "all required .complete markers present"),
        ("excluded_unfinished_headline_right_maximal",
         "dedicated headline/right-maximal excluded; full/right-maximal used"),
        ("snapshot_headline_build_repetitions", "three dedicated build repetitions"),
        ("snapshot_headline_exact_repetitions", "five dedicated exact repetitions"),
        ("snapshot_full_right_maximal_repetitions", "three audited full repetitions"),
        ("snapshot_right_maximal_checksum_stability", "stable across three repetitions"),
    )
    output.extend({"workload": "snapshot", "check": name, "groups_checked": "1",
                   "mismatches": "0", "status": "ok", "details": details}
                  for name, details in custom)
    return output


def environment(run_dir: Path, tables: Sequence[core.Table], revision: str,
                server_label: str, archive_sha: str) -> list[dict[str, str]]:
    rows = core.environment_rows(run_dir, tables, revision, server_label)
    overrides = {"run_complete": "false"}
    result = [row | {"value": overrides.get(row["key"], row["value"])} for row in rows]
    result.extend([
        {"key": "snapshot_status", "value": SNAPSHOT_STATUS,
         "source": "completed-stage snapshot contract"},
        {"key": "standard_scope", "value": "representative_mixed_only",
         "source": "suite configuration"},
        {"key": "full_scope", "value": "representative_mixed_only",
         "source": "suite configuration"},
        {"key": "headline_right_maximal_source", "value": "full/right-maximal",
         "source": "snapshot aggregation contract"},
        {"key": "headline_right_maximal_query_repetitions", "value": "3",
         "source": "full/right-maximal/raw_repetitions.tsv"},
        {"key": "excluded_stage", "value": "headline/right-maximal",
         "source": "unfinished stage exclusion"},
        {"key": "source_archive_sha256", "value": archive_sha,
         "source": "downloaded completed-stage archive"},
    ])
    return result


def markdown_table(columns: Sequence[str], rows: Sequence[Mapping[str, str]]) -> str:
    return core.markdown_table(columns, rows)


def write_docs(output: Path, builds: list[dict[str, str]], exact: list[dict[str, str]],
               right: list[dict[str, str]], build_rows: list[dict[str, str]],
               exact_rows: list[dict[str, str]], right_rows: list[dict[str, str]]) -> None:
    build_view = [{
        "Index": row["index"], "Builder": row["builder"], "Threads": row["threads"],
        "Build time (s)": row["build_time_seconds"], "Peak RSS (MiB)": row["peak_rss_mb"],
        "Index size (bytes)": row["index_size_bytes"], "bits/base": row["bits_per_base"],
        "Speedup": row["speedup_vs_divsufsort"],
    } for row in builds]
    exact_view = [{
        "Index": row["index"], "Operation": row["operation"],
        "queries/s": row["queries_per_second"], "ns/query": row["nanoseconds_per_query"],
        "Peak RSS (MiB)": row["query_peak_rss_mb"],
    } for row in exact]
    right_view = [{
        "Method": row["method"], "query bases/s": row["query_bases_per_second"],
        "matches/s": row["matches_per_second"], "Speedup": row["speedup_vs_baseline"],
        "Repetitions": row["raw_repetitions"],
    } for row in right]
    (output / "README.md").write_text(
        "# sufkit server benchmark: completed-stage snapshot\n\n"
        "**Snapshot status: completed stages.**\n\n"
        "This snapshot was generated automatically from audited raw TSV files. It is not a "
        "claim that every originally planned stage completed. The dedicated headline "
        "right-maximal stage was unfinished and is excluded. Standard and full are "
        "representative `mixed` experiments, not six-scenario scans.\n\n"
        "## Headline conditions\n\n"
        "256 MiB synthetic mixed genome, 4 contigs, seed `20260822`; exact patterns are "
        "100 bp, forward, and locate uses `max_hits=1`. Maximum-right-match queries are "
        "256 bp with `min_length=50`.\n\n"
        "## Construction\n\n" + markdown_table(list(build_view[0]), build_view) + "\n"
        "## Exact search\n\n" + markdown_table(list(exact_view[0]), exact_view) + "\n"
        "## Maximum right matches\n\n" + markdown_table(list(right_view[0]), right_view) + "\n"
        "Exact headline values use dedicated 3-build/5-query runs. Maximum-right-match "
        "values use the audited full/mixed run with 3 measured query repetitions.\n",
        encoding="utf-8")
    (output / "headline" / "README.md").write_text(
        "# Headline provenance\n\nDedicated build: `headline/exact-build` (3 repetitions). "
        "Dedicated exact query: `headline/exact-query` (5 repetitions). Maximum right "
        "matches: audited `full/right-maximal` (3 repetitions). The unfinished dedicated "
        "`headline/right-maximal` directory was not imported or aggregated.\n", encoding="utf-8")
    (output / "build" / "README.md").write_text(
        "# Construction results\n\n`build-results.tsv` combines SA construction workers and exact-index "
        "build records. It reports divsufsort, CaPS, 32/64-bit SA, sampled SA and FM "
        "measurements where the adopted stages contain them. `sampling-space.tsv` isolates "
        "sampling rates above 1; `caps-scaling.tsv` isolates CaPS thread configurations.\n\n"
        "Build time excludes FASTA reading and saving when those fields are separately available. "
        "Logical serialized bytes and allocated disk bytes remain separate columns.\n",
        encoding="utf-8")
    (output / "exact" / "README.md").write_text(
        "# Exact count and locate results\n\nThe tables are recomputed from raw repetitions and retain "
        "pattern length, strand, query group, scalar/batch mode, locate limit, checksum and "
        "status. `locate(all)` high-frequency safety skips remain explicit and are not treated "
        "as zero throughput. Standard/full cover representative mixed data only.\n",
        encoding="utf-8")
    (output / "right-maximal" / "README.md").write_text(
        "# Maximum-right-match results\n\nThese are **maximum right matches**, not MEMs: left "
        "maximality is not implied. The table contains baseline, LCP, CHILD, suffix-link, full "
        "and sampled-SA variants present in completed stages. The headline comparison is taken "
        "from audited full/mixed streaming results (3 repetitions); the unfinished dedicated "
        "headline repetition is excluded.\n",
        encoding="utf-8")
    (output / "scenarios" / "README.md").write_text(
        "# Scenario coverage\n\nQuick covers `mixed`, `balanced`, `gc-skewed`, `repeat-rich`, "
        "`n-islands`, and `many-contig`. Smoke covers `mixed`. Standard and full were shortened "
        "to representative `mixed` experiments and must not be interpreted as complete "
        "six-scenario scans. No real-genome experiment is claimed in this snapshot.\n",
        encoding="utf-8")


def generated_manifest(output: Path, source_manifest: list[dict[str, str]]) -> list[dict[str, str]]:
    rows = [{"artifact_kind": "input", **row} for row in source_manifest]
    for path in sorted(output.rglob("*")):
        if not path.is_file() or path.name == "manifest.tsv":
            continue
        relative = str(path.relative_to(output)).replace(os.sep, "/")
        rows.append({"artifact_kind": "generated", "path": relative,
                     "bytes": str(path.stat().st_size), "sha256": core.sha256(path),
                     "rows": core.NA, "columns": core.NA, "schema": core.NA,
                     "schema_sha256": core.NA, "workload": "snapshot-artifact",
                     "profile": "snapshot"})
    return rows


def package(args: argparse.Namespace) -> None:
    run_dir = args.run_dir.resolve()
    output = args.output_dir.resolve()
    if output.exists():
        raise SnapshotError(f"output already exists: {output}")
    if not run_dir.is_dir():
        raise SnapshotError(f"run directory does not exist: {run_dir}")
    require_markers(run_dir)
    tables = core.discover_tables(run_dir)
    builds = core.aggregate_build_headline(tables)
    exact = core.aggregate_exact_headline(tables)
    right = full_right_headline(tables)
    require_all_ok(builds, "headline build", 3)
    require_all_ok(exact, "headline exact", 5)
    require_all_ok(right, "full right-maximal headline", 3)
    correctness = correctness_rows(tables, run_dir)

    staging = output.parent / f".{output.name}.partial.{os.getpid()}.{secrets.token_hex(4)}"
    staging.mkdir(parents=True)
    try:
        for name in ("headline", "build", "exact", "right-maximal", "scenarios", "figures"):
            (staging / name).mkdir()
        build_rows = core.aggregate_sa_build_rows(tables) + core.aggregate_exact_build_rows(tables)
        exact_rows = core.aggregate_exact_rows(tables)
        right_rows = core.aggregate_right_maximal_rows(tables)
        core.write_tsv(staging / "environment.tsv",
                       environment(run_dir, tables, args.revision, args.server_label,
                                   args.source_archive_sha256), ("key", "value", "source"))
        core.write_tsv(staging / "correctness-summary.tsv", correctness,
                       ("workload", "check", "groups_checked", "mismatches", "status", "details"))
        core.write_tsv(staging / "headline" / "headline-build.tsv", builds,
                       core.HEADLINE_BUILD_COLUMNS)
        core.write_tsv(staging / "headline" / "headline-exact.tsv", exact,
                       core.HEADLINE_EXACT_COLUMNS)
        core.write_tsv(staging / "headline" / "headline-right-maximal.tsv", right,
                       core.HEADLINE_RIGHT_COLUMNS)
        core.write_tsv(staging / "headline" / "headline.tsv", long_headline(builds, exact, right),
                       ("section", "item", "metric", "value", "unit", "status", "provenance"))
        core.write_tsv(staging / "build" / "build-results.tsv", build_rows,
                       core.BUILD_AGG_COLUMNS)
        core.write_tsv(staging / "build" / "sampling-space.tsv",
                       [row for row in build_rows if (core.integer(row.get("sampling_rate")) or 1) > 1],
                       core.BUILD_AGG_COLUMNS)
        core.write_tsv(staging / "build" / "caps-scaling.tsv",
                       [row for row in build_rows if "caps" in row.get("method", "").lower() or
                        "caps" in row.get("effective_backend", "").lower()], core.BUILD_AGG_COLUMNS)
        core.write_tsv(staging / "exact" / "count-results.tsv",
                       [row for row in exact_rows if row.get("operation") == "count"],
                       core.EXACT_AGG_COLUMNS)
        core.write_tsv(staging / "exact" / "locate-results.tsv",
                       [row for row in exact_rows if row.get("operation") == "locate"],
                       core.EXACT_AGG_COLUMNS)
        core.write_tsv(staging / "right-maximal" / "algorithm-ablation.tsv", right_rows,
                       core.RIGHT_AGG_COLUMNS)
        core.write_headline_svg(staging / "figures" / "headline-performance.svg", builds, exact)
        write_docs(staging, builds, exact, right, build_rows, exact_rows, right_rows)
        (staging / "commands.md").write_text(
            "# Reproduction commands\n\n```bash\npython3 -B benchmarks/package_completed_stage_snapshot.py \\\n  --run-dir <completed-stage-run> \\\n  --output-dir benchmarks/results/server-205023a-representative-20260824 \\\n  --revision 205023aa8fbaefdbfd7e20f6a84ebbe7a3fad2dd \\\n  --server-label server-205023a\n```\n\nThe source run was "
            "`/mnt/sda/zhangpinglu/exp/sufkit/runs/205023a-representative-20260824T010427Z`. "
            "Personal credentials and host paths are not embedded in metric TSVs.\n",
            encoding="utf-8")
        source_manifest = core.manifest_rows(run_dir, tables)
        core.write_tsv(staging / "manifest.tsv", generated_manifest(staging, source_manifest),
                       MANIFEST_COLUMNS)
        output.parent.mkdir(parents=True, exist_ok=True)
        staging.rename(output)
    except Exception:
        print(f"snapshot staging retained for diagnosis: {staging}", file=sys.stderr)
        raise


def main(argv: Sequence[str] | None = None) -> int:
    try:
        package(parse_args(argv))
    except (SnapshotError, core.PackageError, OSError, ValueError) as error:
        print(f"snapshot packaging failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
