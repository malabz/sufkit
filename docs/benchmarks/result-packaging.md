# Server benchmark result packaging

The benchmark packager converts one additive server run into a reviewable,
repository-sized result directory. It is a reporting boundary, not another
benchmark implementation: it never runs an index method and never changes the
input run.

## Provenance rule

Every headline performance value is recomputed from a discovered
`raw_repetitions.tsv`. `build_results.tsv` and `query_results.tsv` are included
in the input manifest but are not numeric fallbacks for headline cells. This
prevents a stale summary or a changed aggregation rule from silently entering
the most visible table.

Required raw evidence is discovered by column names rather than a fixed column
position:

| Headline value | Required raw fields |
|---|---|
| Headline build time | unified exact-worker `method`, `phase=build`, `threads`, `repetition`, `seconds` |
| Build peak RSS | `build_peak_rss_mb` or phase-local `peak_rss_mb` |
| Index size | `serialized_bytes` |
| bits/base | `serialized_bytes`, `total_bases` |
| Exact throughput | `query_group`, `pattern_length`, `strand`, `operation`, `query_count`, `seconds` |
| Exact bases/s | exact fields above plus `query_bases` |
| Right-maximal bases/s | `operation=streaming`, `min_length`, `seconds`, `query_bases` |
| Right-maximal matches/s | fields above plus `total_matches` |

If a benchmark producer does not place a required value in raw evidence, the
packager emits `NA` and marks that row and package `partial`. It does not borrow
the value from a summary TSV. `--require-complete` converts this condition to a
non-zero exit after all diagnostics have been written.

## Aggregation

- The three headline builders are read from one unified exact-worker scope:
  `sa32-binary` at one thread, `caps32` at 64 threads, and `fm-huff` at one
  thread. `headline/exact-build` is preferred, with `headline/exact` accepted
  as the same protocol. Standalone `headline/sa-build-*` rows are never pooled
  into this table, even if they remain in an older run directory.
- Headline count and locate throughput is read only from
  `headline/exact-query/raw_repetitions.tsv`. Query-shaped rows accidentally
  present in `exact-build` are ignored rather than pooled into the five query
  repetitions.
- The build and query scopes are not assumed identical merely because both are
  named `full/mixed`. Their metadata must agree on seed, scenario, reference
  fingerprint, total bases, query-set checksum, query count, and query bases.
- Build values are medians across matching raw build repetitions.
- Exact headline throughput pools the four ordinary query groups within each
  repetition by summing query counts, bases, and elapsed seconds; it then takes
  the median repetition throughput. Boundary and reverse-complement validation
  groups are excluded.
- Exact query RSS is the largest phase-local group RSS in a repetition, then
  the median across repetitions.
- Detailed exact aggregates retain `skipped_high_frequency_queries` and a
  `safety_status` derived from the raw safety/status field. Correctness tuples
  include query count, skipped-query count, query bases, hit counts, and result
  checksum, so equal hit checksums cannot hide different measured workloads.
  Complete locate applies its 100,000-hit limit per query: unsafe queries are
  removed while safe queries in the same slice remain measured. A whole slice
  is `skipped_high_frequency` only when no safe query remains.
- Right-maximal headline rows use only the streaming callback operation at
  minimum length 50.
- Detailed right-maximal aggregates contain only `streaming`, `vector`,
  `max_matches=0`, and `max_matches=1000`. Construction, save, and load rows
  remain available in the raw union but are never mixed into algorithm
  ablation tables or query-rate figures.
- An unbounded right-maximal vector row is `skipped_high_frequency` when the
  streaming preflight reports more than 1,000,000 matches. Its raw evidence
  carries `materialization_match_threshold=1000000` and `vector_skipped=1`;
  streaming and the two bounded operations still run.
- Checksums must remain stable. Unexpected statuses and cross-method result
  mismatches block headline generation.

Exact raw rows include backend name/signature, SDSL version, coordinate width,
SA sampling rate, builder thread count, `query_threads`, and a deterministic
build/load canary (`canary_total_hits`, `canary_reported_hits`, and
`canary_checksum`). The headline gate uses these fields to reject mixed
backend or execution provenance and to compare divsufsort/CaPS query canaries.
`query_threads` is phase-local: non-query rows record zero and the current
query workers record one.

Phase RSS is interpreted together with the producer's `peak_rss_scope`.
Exact and right-maximal workers are forked after the controller has prepared
the current dataset, so their high-water marks include inherited controller
dataset pages plus the named build/load/query work. The packager preserves
that scope; it does not subtract a controller baseline or relabel the value as
index-only memory.

The tool uses only the Python standard library and accepts additive schema
extensions. Unknown columns are retained in the workload-specific raw union.
Current exact, SA-build, and right-maximal schemas are distinguished by their
semantic columns, so they may coexist under one run directory.

## Output

```text
benchmarks/results/server-REVISION-YYYYMMDD/
├── README.md
├── environment.tsv
├── commands.md
├── manifest.tsv
├── correctness-summary.tsv
├── headline/
├── build/
├── exact/
├── right-maximal/
├── scenarios/
└── figures/
    ├── headline-performance.svg
    ├── build-scaling.svg
    ├── memory-and-size.svg
    ├── count-locate-details.svg
    ├── right-maximal-ablation.svg
    └── caps-thread-scaling.svg
```

`environment.tsv` intentionally omits user paths and hostnames. The manifest
contains paths relative to the run root, byte sizes, SHA-256 hashes, and TSV
schema dimensions. The SVG has exactly three panels: build time, peak RSS/index
size, and exact count/locate-1 throughput. Five detailed SVGs are also generated
directly from aggregate raw evidence. If their selected data is absent they show
an explicit `NA / partial` annotation rather than a fabricated zero bar.

The package description always states the evidence boundary: controlled
synthetic results do not establish universal or real-genome performance.

## Stage audit interface

The server runner can stop between scales on a stable, read-only audit command:

```bash
python3 -B benchmarks/package_benchmark_results.py audit \
  --run-dir RUN_DIR --profile smoke|quick|standard|full|headline
```

The audit prints `profile`, `check`, `status`, and `details` as TSV. It checks
required workload directories and companion TSVs, required semantic columns,
row shape, expected/non-expected statuses, repetition checksum stability, and
cross-method checksum equivalence. For `smoke`, `quick`, `standard`, and
`full`, it additionally enforces the closed matrix in
`run_server_suite.sh`: expected scenarios; every exact method, group, pattern
length, strand, scalar count/locate slice and FM batch-count width; each named
SA-build submatrix with its exact method/thread/sampling/acceleration tuple;
all internal right-maximal methods; and all four right-maximal query APIs.
The strict FM batch matrix is backend-specific: Huffman uses widths 1, 4, 8,
16, and 32, while balanced and EPR use widths 16 and 32. The latter two are
passed as per-backend overrides, not appended to the global Huffman width set.
For SA construction, requested acceleration `default` and its persisted
canonical label `suffix-link` are the same layout. A requested thread point
above the machine's logical CPU count may be represented by exactly one
`not_applicable:threads_exceed_logical_cpus` raw row; no other reason-qualified
`not_applicable:*` status is accepted.
Every supported query and build slice must contain the profile's exact measured
repetition set. A deterministic `skipped_high_frequency` complete-locate row,
an empty-group `not_applicable` row, or an FM right-maximal `not_supported`
capability row may represent its expected slice. Skipped and not-applicable
query slices must still carry the complete profile repetition set; only the
single FM capability row is exempt. An arbitrary non-ok row may not hide
missing supported measurements. Therefore a profile cannot pass by containing
only one method or one scenario.

The audit also imports the right-maximal
`correctness_summary.tsv` naive-oracle status into the packaged correctness
gate. The `headline` audit additionally requires
the fixed three build repetitions, five query repetitions, and all raw fields
needed by the visible tables. The `sa32-binary`, `caps32`, and `fm-huff`
builds must come from the same exact-worker source scope and the same dataset
fingerprint; their thread settings must be 1, 64, and 1 respectively. When
both SA build rows expose their required canary hit counts and checksum, those
values must agree. Backend signature, coordinate width, sampling rate, SDSL
version, build threads, and query-thread provenance must be stable across all
three build repetitions. The `exact-build` and `exact-query` metadata must
agree on seed, scenario, total bases, dataset fingerprint, query-set checksum,
query count, and query bases. It never writes or removes run artifacts.

The `headline_fixed_workload_contract` check also prevents a smoke or quick run
from being labelled as the fixed headline. Both exact scopes must report
`profile=full`, `scenario=mixed`, 268,435,456 reference bases, four contigs,
and 10,000 queries. The query scope must contain the complete length-100,
forward count/locate-1 evidence. Right-maximal metadata must report the same
full reference and query count, 2,560,000 query bases, and five measured
streaming repetitions for baseline and suffix-link at minimum length 50.

## Runner recovery and producer partials

The runner publishes each exact, SA-build, and right-maximal producer directory
through a hidden sibling named
`.OUTPUT.partial.<attempt>.<slot>`. It first validates the workload's mandatory
TSV set, writes a `.sufkit-producer-complete` marker, and then renames the whole
directory into place. A failed or structurally incomplete producer partial is
retained on the server for diagnosis; it is never treated as a completed
result directory.

On restart, every stage `.complete` marker is paired with its validator before
the stage may be skipped. Prior failed markers and logs are archived by attempt,
and an existing final producer directory is reused only when its payload and
producer marker validate. An existing but invalid final directory is preserved
and blocks the retry instead of being overwritten. Existing result packages
are likewise re-audited across all five profiles, checked for the complete file
tree, and passed through the headline package validator before reuse.

Producer partials are distinct from the packager's
`.OUTPUT.partial.<pid>.<nonce>` sibling described above. Both forms, together
with stage logs and failed markers, remain in the server experiment tree and
must not be copied into `benchmarks/results` as a completed package.

The supplied server runner records CPU topology, memory and swap state, the
observed governor, compiler versions, and exact affinity commands. It pins
query work to one physical CPU and parallel CaPS work to physical cores from
NUMA node 0 when supported, but does not change the governor, Turbo state,
swap, or any other system parameter.
