# Packaged benchmark results

Versioned server result directories are generated here by
[`package_benchmark_results.py`](../package_benchmark_results.py). Do not copy
headline numbers by hand.

The controlled suite has four shared scales:

| Profile | Reference per scenario | Queries | Build/query repetitions | Scenarios in the server suite |
|---|---:|---:|---:|---|
| `smoke` | 16 KiB | 100 | 1 / 3 | mixed |
| `quick` | 4 MiB | 1,000 | 3 / 5 | all six |
| `standard` | 32 MiB | 5,000 | 3 / 7 | all six |
| `full` | 256 MiB | 10,000 | 1 / 5 | mixed, repeat-rich, many-contig |

The six selectable scenarios are `mixed`, `balanced`, `gc-skewed`,
`repeat-rich`, `n-islands`, and `many-contig`. The standalone SA-build workload
uses one deterministic mixed reference at each profile size; exact and
right-maximal use the scenario matrix above. The separate full/mixed headline
overrides construction to three repetitions while retaining five measured
query repetitions.

The server runner also supports a declared time-bounded variant:
`SUFKIT_BENCH_SUITE_VARIANT=representative`. In that mode smoke and quick keep
the complete matrices, while standard and full use only the mixed scenario and
the explicitly recorded representative method/query subsets. The variant is
stored in `manifest/execution-scope.tsv` and the strict packager validates its
own method, scenario, length, locate-limit, and repetition contract. Omitted
methods are not silently interpreted as successful measurements; representative
results must be described as scale checks rather than complete profile sweeps.

```bash
python3 -B benchmarks/package_benchmark_results.py \
  package \
  --run-dir /path/to/complete/server-run \
  --output-dir benchmarks/results/server-REVISION-YYYYMMDD \
  --revision REVISION \
  --server-label server \
  --require-complete
```

Audit each staged profile before starting the next one:

```bash
python3 -B benchmarks/package_benchmark_results.py audit \
  --run-dir /path/to/server-run --profile smoke
```

Valid audit profiles are `smoke`, `quick`, `standard`, `full`, and `headline`.
The command writes a stable four-column TSV report to standard output and
returns non-zero for a missing workload/file/required column, an unexpected
status, unstable checksums, or a cross-method result mismatch. Headline audit
also requires all fixed three-build and five-query raw repetitions and every
raw field needed by the visible tables.

The strict exact-query matrix uses FM batch widths 1, 4, 8, 16, and 32 for
Huffman, and widths 16 and 32 for balanced and EPR. The balanced/EPR sets are
per-backend overrides of the global Huffman set, not additional widths.

The headline construction table is drawn only from one unified
`headline/exact-build` worker scope containing `sa32-binary`, `caps32`, and
`fm-huff`. The independent `headline/exact-query` scope supplies only the
single-core SA/FM count and locate rows. The audit requires the two scopes to
agree on seed, scenario, dataset fingerprint, base count, query-set checksum,
query count, and query bases. It also verifies backend signatures, coordinate
width, sampling rate, builder threads, query-thread provenance, SDSL version,
and the divsufsort/CaPS build canary. Rows from standalone SA-build scopes are
not pooled into the headline construction table.

Complete exact locate applies the 100,000-hit safety limit per query. Safe
queries remain timed; raw and aggregate rows retain the measured query count
and `skipped_high_frequency_queries`. Right-maximal streaming first obtains the
complete count, and unbounded vector materialization is skipped above
1,000,000 matches while streaming, `max_matches=0`, and `max_matches=1000`
remain available. Both thresholds and skip flags are preserved in packaged
TSV evidence.

Peak RSS must be read with each raw row's `peak_rss_scope`. Exact and
maximal-match phases now start through `exec()` so they do not inherit the
controller's previously resident heap. Build workers include reference loading
and construction, query workers include query loading, index loading, and the
named operation, and save/load scopes remain separate. MUMmer4's external
`load+query` scope is still reported separately from sufkit's in-process core
query time; the scope label, not an unqualified RSS number, defines a fair
comparison.

The output is intentionally small enough for Git: TSV summaries and raw
repetition evidence, Markdown, file manifests, and SVG. Generated references,
indexes, scratch files, and logs stay on the experiment server.

Before packaging, the server runner writes each benchmark producer into a
hidden sibling named `.OUTPUT.partial.<attempt>.<slot>`, validates its mandatory
TSVs, adds a `.sufkit-producer-complete` marker, and atomically publishes the
directory. On restart it revalidates completed stages and existing published
outputs before reuse. Failed producer partials, attempt logs, and archived state
markers stay on the server; an existing invalid final directory is never
overwritten and none of these diagnostic artifacts is a publishable package.

The packager refuses to overwrite an existing result directory. It first writes
to a unique hidden sibling named
`.OUTPUT.partial.<pid>.<nonce>` and publishes the requested `--output-dir` only
after every gate succeeds. A correctness mismatch or incomplete package returns
non-zero and retains diagnostic evidence in the exact hidden partial sibling
reported on stderr; the requested output directory is not created. Missing
producer fields remain `NA`, and a blocked package does not receive a headline
directory. Do not transfer or publish a hidden partial directory as a completed
result package.

The server runner is additive and executes stages/methods sequentially. It
records hardware, memory/swap state, the observed CPU governor, compiler, and
affinity; it pins query and CaPS work to declared physical CPUs but does not
change the governor, Turbo state, swap, or other system parameters. If a final
package already exists, the runner re-audits all five profiles, its required
file tree, and its headline semantics before accepting it as recovered.

Run its small standard-library regression test with:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -B \
  tests/test_package_benchmark_results.py -v
```
