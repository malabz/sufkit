# Exact and maximal-match benchmark methodology

The benchmark is a correctness-gated, deterministic comparison of the naive
scanner, standalone suffix arrays, and SDSL FM-index backends. It records
construction, serialization, loading, count, and locate separately.
Performance numbers are descriptive; sufkit does not impose absolute speed
thresholds across machines.

Exact-search ablation methods additionally include `sa32-binary`,
`sa32-lcp-binary`, `sa32-sapling`, and explicit `sa32-child`. Learned-model
parameters are controlled with `--learned-k`, `--learned-memory-bp`, and
`--learned-bucket-bits`.

## Profiles

The exact, standalone SA-construction, and right-maximal drivers use one shared
profile definition. Scenario sweeps apply to the exact and right-maximal
drivers; the standalone SA-construction driver uses one deterministic mixed
reference at the selected profile size.

| Profile | Total reference | Queries | Server-suite scenarios | Default methods | Build/query repetitions |
|---|---:|---:|---|---|---|
| `smoke` | 16 KiB | 100 | mixed | naive, SA32, SA64, FM | 1 / 3 |
| `quick` | 4 MiB | 1,000 | all six | naive, SA32, SA64, FM | 3 / 5 indexed, 1 naive |
| `standard` | 32 MiB per scenario | 5,000 | all six | SA32, SA64, FM | 3 / 7 |
| `full` | 256 MiB per scenario | 10,000 | mixed, repeat-rich, many-contig | SA32, SA64, FM | 1 / 5 |

Every profile performs one query warm-up by default, except that the `quick`
naive baseline uses one measured repetition and no warm-up. This keeps the
interactive profile near its intended scale while still scanning every query,
strand, and operation and participating in the complete correctness gate.
Explicit `--query-repetitions` or `--warmups` values override this policy for
all methods. The six selectable scenarios are `mixed`, `balanced`,
`gc-skewed`, `repeat-rich`, `n-islands`, and `many-contig`. The controlled
server suite runs all six for `quick` and `standard`, and the three listed
large scenarios for `full`. Scenarios can be selected explicitly for every
synthetic profile. The legacy single-file `--quick --output` compatibility
path remains a one-scenario `mixed` run.

```bash
sufkit bench \
  --profile quick \
  --scenarios mixed,repeat-rich \
  --methods naive,sa32,sa64,fm \
  --pattern-lengths 20,50,100,200,500 \
  --locate-limits 1,10,1000,all \
  --seed 20260822 \
  --output-dir results/quick
```

The compatibility commands still emit the original single-table summary:

```bash
sufkit bench --smoke --output smoke.tsv
sufkit bench --quick --output quick.tsv
```

FM backend and batched-count comparisons use:

```bash
sufkit bench --profile quick \
  --methods fm-huff,fm-balanced,fm-epr \
  --fm-query-modes scalar,batch \
  --fm-batch-widths 1,4,8,16,32 \
  --fm-batch-widths-for fm-balanced:16,32 \
  --fm-batch-widths-for fm-epr:16,32 \
  --output-dir results/fm-quick
```

`fm` remains an alias for `fm-huff`; selecting both in one run is rejected.
Scalar mode records count and locate. Batch mode records count only and keeps
one row per batch width. Query summaries append the mode, width, processed
query bases, bases/s, and speedup relative to the matching Huffman scalar row.
`--fm-batch-widths` supplies the default width set, while each repeatable
`--fm-batch-widths-for METHOD:...` replaces that set for the named FM backend.
The strict server matrix measures Huffman at widths 1, 4, 8, 16, and 32, and
balanced and EPR at widths 16 and 32. The selected default and per-backend
overrides are retained in `run_metadata.tsv`.

## Synthetic datasets and query groups

The generator uses SplitMix64 and derives all choices from the selected seed.
The same profile, scenario, pattern lengths, and seed therefore produce the
same normalized reference fingerprint and query set. Generated metadata
records GC fraction, ambiguous-base fraction, repeat fraction, contig count,
total length, selected methods, pattern lengths, locate limits, and repetition
policy; generated FASTA files are not stored in the repository.

Queries are grouped as:

- `exact_unique`: sampled from low-frequency reference sequence;
- `exact_repetitive`: sampled from the deterministic repeat template;
- `mutated_low_hit`: one fixed-position substitution in a sampled window;
- `random_no_hit`: generated and independently checked to have zero hits;
- `n_boundary`: joins canonical sequence across an N island and must not match;
- `contig_boundary`: joins adjacent contig ends and must not match;
- `reverse_complement`: includes ordinary and reverse-complement-palindromic patterns.

The default pattern lengths are 20, 50, 100, 200, and 500 bp. A group/length
combination that cannot be generated remains in the output with
`not_applicable`; it is never silently replaced with a shorter pattern.

Count and locate are measured for `forward`, `reverse-complement`, and `both`.
Locate limits may contain numeric limits and `all`. For complete locate, the
driver pre-counts each query independently. A query with more than 100,000
hits is omitted from that complete-locate measurement while lower-frequency
queries in the same group still run. `query_count` and `query_bases` describe
the measured subset and `skipped_high_frequency_queries` records the omitted
cardinality. Only when every query in a slice is omitted does the slice have
status `skipped_high_frequency`. Count and bounded-locate measurements remain
complete.

## User references

```bash
sufkit bench \
  --reference reference.fa.gz \
  --queries queries.fa.gz \
  --methods sa32,sa64,fm \
  --output-dir results/genome
```

When `--queries` is present, input order is retained inside every query group.
Queries are grouped by pattern length and actual forward hit count: 0, 1,
2-10, 11-1,000, and above 1,000. If `--queries` is omitted, exact, mutated,
and independently verified no-hit queries are generated deterministically from
the reference. No data is downloaded.

`--profile` and `--reference` are mutually exclusive. A 32-bit input exceeding
the divsufsort32 limit is recorded as `unsupported_input_size`; other selected
methods continue.

## Isolation, timing, and correctness

Build, save, load, count, and each locate limit run in separate phase workers,
so peak RSS from an earlier phase or method cannot contaminate a later one.
The controller uses `fork()` only as the short transition to `exec()`; each
phase therefore starts from a clean process image and does not inherit the
controller's resident dataset heap. Build workers include reference loading
and construction, save workers include index loading and saving, and query
workers include index/query loading plus the query operation. The exact raw
output names this boundary in `peak_rss_scope`; it is a whole-worker
high-water mark for that declared scope, not the incremental RSS of the index
alone. Comparisons remain meaningful only between rows produced by the same
worker protocol and dataset scope.

Timed query loops exclude TSV formatting and output. Wall time and user/system
CPU time are retained for every raw repetition; summary tables use medians and
also report min/max wall time. Query warm-ups execute in the same phase worker
before its measured repetitions.

Before the command reports success, it checks:

- every measured repetition is deterministic;
- all selected methods agree on count, reported hits, and coordinate checksum;
- N-boundary and contig-boundary queries have zero hits;
- `standard` and `full` additionally compare a deterministic sample from each
  group against the naive implementation when naive is not a timed method.

Result files are written before the final cross-method gate. Consequently, a
correctness mismatch returns a nonzero status but preserves diagnostic TSV
files and does not print the successful-completion message.

## Output files

Exact `--output-dir` runs create four result files:

- `run_metadata.tsv`: dataset, generator, toolchain, OS, CPU, and composition;
- `build_results.tsv`: build/save/load medians, RSS, size, and bits per base;
- `query_results.tsv`: group/length/strand/operation summaries;
- `raw_repetitions.tsv`: every measured wall/CPU time, hit count, checksum, and
  declared RSS scope, plus ordered query-definition rows containing the query
  ID and synthetic source coordinate/template offset when available.

Maximal-match, MEM, and MAM workloads also create
`correctness_summary.tsv`, which records their independent small-oracle gate.

Exact raw rows also carry backend provenance (`backend`,
`backend_signature`, `sdsl_version`, legacy construction width, SA sampling
rate, and builder `threads`), explicit construction/stored coordinate widths,
the SA resource profile, LCP encoding, component/resident bytes,
storage-compaction time, and a deterministic post-build/load query canary
(`canary_total_hits`, `canary_reported_hits`, and `canary_checksum`), and the
phase-local number of query threads (`query_threads`: zero for non-query rows,
one for the current scalar/batch query workers). These fields let the packager
reject a headline assembled from a mislabeled backend, incompatible index, or
different query-execution contract.

For controlled Low-memory raw-versus-byte-coded LCP experiments, maintainers
may configure a separate build with the advanced, developer-only option
`SUFKIT_INTERNAL_FORCE_RAW_LCP=ON`. The option is rejected unless
`SUFKIT_BUILD_BENCHMARKS=ON`, does not change the public API or CLI, and makes
all retained LCP arrays use the raw codec. Fast already keeps raw LCP after the
byte-coded representation failed its pinned quick three-percent regression
gate. Compare Low-memory from the override build with default Low-memory from
the same source revision, compiler flags, dataset, and CPU pinning; the
recorded `lcp_encoding` field distinguishes the two artifacts. This switch is
an A/B instrument, not a supported end-user storage policy.

Metadata deliberately omits the hostname and user-specific input/output paths.
Existing result files are not overwritten. Use a new or empty result directory
for every run.

The non-installed `sufkit_query_memory_bench` provides a complementary
resident-memory view. It runs build, load, and query in separate clean-exec
workers and records both RSS and `/proc/self/smaps_rollup` PSS. Its
`index_ready_pss_mb` is the loaded-process measurement; `peak_rss_mb` remains
the worker lifetime high-water mark. It supports Fast, Low-memory, forced
32/40/48/64 storage, and FM Huffman methods. PSS values are Linux-specific and
must not be silently substituted with RSS on other platforms.

Learned exact rows additionally record SA/ISA/LCP/CHILD/model construction
times, model bytes, suffix and character comparisons, gallop probes, local
window sizes, prediction counts/errors, and full binary fallbacks. Prediction
statistics describe performance only; cross-method range, hit, coordinate,
and checksum equality remains mandatory.

The `standard` and `full` profiles are opt-in for ordinary developer builds and
are not part of the routine local CTest suite. They are nevertheless mandatory
stages of the complete server acceptance used for this result package: the
server runner executes and audits smoke, quick, standard, full, and the fixed
headline sequentially.

For a time-bounded server run, set `SUFKIT_BENCH_SUITE_VARIANT=representative`.
Smoke and quick remain the complete matrix; standard is reduced to the mixed
32 MiB dataset with `sa32-binary`, `sa64-binary`, sampled K=4, and FM Huffman,
three query repetitions, pattern lengths 50/100/200, and locate limits 1/1000.
Full is reduced to mixed 256 MiB with SA32, CaPS32 at 64 threads, and FM
Huffman, three query repetitions, length 100, and locate(1). The corresponding
SA-build canaries are divsufsort32/64, sampled K=4, and CaPS32 for standard,
and divsufsort32 plus CaPS32 for full. Right-maximal representative stages
retain baseline, suffix-link, and (standard only) full. The runner records
`suite_variant=representative` in `manifest/execution-scope.tsv`; the packager
audits the declared reduced contract rather than treating omitted methods or
scenarios as successful measurements. This variant is suitable for a concise
scale-sensitivity check, not for claiming a complete standard/full sweep.

## right-maximal exact match workload (0.1.1)

```bash
sufkit bench \
  --workload right-maximal \
  --profile quick \
  --scenarios mixed,repeat-rich \
  --methods right-maximal-baseline,right-maximal-lcp,right-maximal-child,right-maximal-suffix-link,right-maximal-full,mummer4 \
  --min-lengths 20,50,100 \
  --mummer4 /path/to/mummer \
  --mummer4-runtime /path/to/.libs/mummer \
  --output-dir results/right-maximal-quick
```

The five internal methods build exactly the auxiliary structures their names
require. Query timing includes no TSV formatting. One warm-up precedes three
smoke or five quick repetitions. The optional MUMmer4 row uses full SA
(`K=1`), `skip=1`, no k-mer table, one query thread, `-save` for construction,
and `-load` for measured queries. Its reported query time therefore includes
external process startup and index loading and must not be interpreted as an
in-process query-only comparison.

`right-maximal-suffix-link-binary` and `right-maximal-suffix-link-sapling` build the same
SA+ISA+LCP layout and differ only in initialization/fallback lookup. Their
outputs report suffix-link success rate, previous-empty states, learned versus
binary lookup counts, character/row accesses, prediction errors, search
windows, and learned model space. `right-maximal-full` remains an explicit CHILD
ablation and is not an automatic default.

The right-maximal exact match workload accepts all four shared profiles. Its
server matrix uses the same scenario selection shown above and measures four
separate APIs: streaming callback, unbounded vector, `max_matches=0`, and
`max_matches=1000`. Before the unbounded vector operation, the streaming result
provides a full match count. If that count exceeds 1,000,000, the vector row is
recorded as `skipped_high_frequency` with `vector_skipped=1`; streaming and the
bounded operations still run. This gate prevents a high-frequency workload
from turning a performance run into an uncontrolled result-materialization
allocation.

All internal and MUMmer4 rows are normalized to the same zero-based,
query-first tuple checksum. A mismatch preserves diagnostic TSV files and
returns nonzero. See the
[archived 0.1.1 right-maximal report](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.1.1-right-maximal.md)
for the historical release run.

## Formal MEM and reference-MAM smoke

```bash
sufkit bench --workload mem --profile smoke \
  --methods mem-baseline,mem-lcp,mem-child,mem-suffix-link,mem-full,mummer4 \
  --min-lengths 20,50 --mummer4 /path/to/mummer-4.0.1/mummer \
  --mummer4-runtime /path/to/mummer-4.0.1/.libs/mummer \
  --output-dir results/mem-smoke

sufkit bench --workload mam --profile smoke \
  --methods mam-baseline,mam-lcp,mam-child,mam-suffix-link,mam-full,mummer4 \
  --min-lengths 20,50 --mummer4 /path/to/mummer-4.0.1/mummer \
  --mummer4-runtime /path/to/mummer-4.0.1/.libs/mummer \
  --output-dir results/mam-smoke
```

MEM and MAM runs retain the four performance TSV files described above and add
`correctness_summary.tsv` as a fifth file for the independent small-oracle
gate. Run metadata records the selected `workload`. MUMmer4 uses `-maxmatch`
for MEM and `-mumreference` for MAM. Its timed row covers external process
startup, index loading, querying, and output-file writing; parsing and checksum
calculation happen afterward and are not timed. Sufkit rows measure the
preloaded in-process query kernel. Every method must produce the same total and
checksum for each comparison group.

## Generalized SMEM and strict MUM smoke

```bash
sufkit bench --workload smem --profile smoke \
  --methods smem-baseline,smem-lcp,smem-child,smem-suffix-link,smem-full,minibwa \
  --min-lengths 20,50 --min-occurrences 1,2 \
  --minibwa /path/to/pinned/minibwa \
  --output-dir results/smem-smoke

sufkit bench --workload mum --profile smoke \
  --methods mum-baseline,mum-lcp,mum-child,mum-suffix-link,mum-full,mummer4 \
  --min-lengths 20,50 --mummer4 /path/to/mummer-4.0.1/mummer \
  --mummer4-runtime /path/to/mummer-4.0.1/.libs/mummer \
  --output-dir results/mum-smoke
```

SMEM rows append `min_occurrences`, `total_smems`, and
`smems_per_second` to the established query schema. `total_smems` counts
directional query intervals; `total_matches` counts expanded reference
coordinates. Raw repetition and correctness output append
`min_occurrences` and `total_smems`; `smems_per_second` is a derived summary
field in `query_results.tsv`. Existing columns retain their positions.

MUMmer4 `-mum` is the strict-MUM black-box comparator. Run metadata records
separate `mummer_launcher_sha256` and `mummer_runtime_sha256` fields. When
`--mummer4` already names an ELF binary, it is also the runtime. When it names
a script such as an uninstalled libtool launcher, pass the actual ELF through
`--mummer4-runtime`; the benchmark validates ELF magic and never guesses by
parsing launcher text.

MiniBWA `fastmap` is an optional SMEM timing/provenance row selected with
`--minibwa PATH`; because its
FMD index counts both reference orientations, it is labeled
`external_fmd_scope` and is not pooled into the forward-only internal checksum
group. Build MiniBWA from a recorded source commit in an isolated directory;
the benchmark records the resulting executable version and SHA-256. Its
`index` and `fastmap` subprocesses include load/startup and are not presented
as in-process sufkit kernel times. Measured `external_fmd_scope` repetitions
retain wall time, CPU time, and peak RSS in `raw_repetitions.tsv`. A `fastmap`
`*` coordinate list is labeled `external_high_frequency`; a `.` coordinate or
any mismatch between the declared occurrence count and reported coordinates is
labeled `external_incomplete_coordinates`. These external rows retain their
measured wall time, CPU time, and peak RSS. The labels describe output
completeness only: MiniBWA remains a separate FMD timing/provenance peer and is
not a coordinate-correctness differential. Capability placeholders such as
`not_supported` and skipped operations keep `NA` timing.

Run metadata records the MiniBWA version after replacing tabs, newlines, and
path-like tokens, so external tool output cannot break the TSV schema or leak
a local build path. `command_line_redacted` similarly replaces every supplied
reference, query, output, MUMmer4, and MiniBWA path with `<path>`. The
`git_commit` and `git_dirty` fields are captured when CMake configures the
benchmark binary; `git_provenance_scope=cmake_configure_time` makes that scope
explicit. Reconfigure before a release measurement. The executable SHA-256 is
the authoritative identity of the binary that actually produced the row.
Compiler-flag metadata conservatively replaces every token containing a path
separator with `<path-flag>`, including quoted include paths and definition
values; this can also redact harmless relative paths by design.

MUMmer4 query rows use `operation=external-load+query`: their wall time and
peak RSS include process startup, loading the saved MUMmer index, and querying.
They must not be compared as if they were the same scope as sufkit's preloaded
in-process `streaming` or `vector` kernels.

Before accepting performance results, every internal SMEM/MUM method must
match the independent small oracle for each `(min_length,min_occurrences)`
group. A published MUMmer4 differential must use an ACGT-only single-contig
reference so input-normalization differences cannot masquerade as an algorithm
difference. Standard/full runs are opt-in and are not part of routine
validation.

## Headline and server execution scopes

The visible full/mixed headline deliberately separates construction from
query timing:

- `headline/exact-build` constructs `sa32-binary`, `caps32`, and `fm-huff`
  with one exact-benchmark worker protocol. It uses three build repetitions;
  CaPS receives 64 builder threads while the other two builders use one.
- `headline/exact-query` runs only `sa32-binary` and `fm-huff` on one pinned
  physical CPU, after one warm-up and with five measured repetitions.

The two scopes are never pooled. Their deterministic dataset/query identity is
audited using the seed, scenario, reference fingerprint, base count, query-set
checksum, query count, and query bases. The SA build canary additionally checks
that divsufsort and CaPS produced query-equivalent indexes.

The server runner executes stages and methods sequentially and applies CPU/NUMA
affinity when the host provides `numactl` or `taskset`. It records the CPU
topology, governor, memory, swap, compiler, and affinity in the manifest, but
does not change the CPU governor, Turbo state, swap configuration, or other
system parameters.
