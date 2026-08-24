# Exact and right-maximal exact match benchmark methodology

The benchmark is a correctness-gated, deterministic comparison of the naive
scanner, standalone divsufsort32/64 suffix arrays, and the fixed SDSL FM-index.
It records construction, serialization, loading, count, and locate separately.
Performance numbers are descriptive; sufkit does not impose absolute speed
thresholds across machines.

Exact-search ablation methods additionally include `sa32-binary`,
`sa32-lcp-binary`, `sa32-sapling`, and explicit `sa32-child`. Learned-model
parameters are controlled with `--learned-k`, `--learned-memory-bp`, and
`--learned-bucket-bits`.

Sampled-SA lookup is selected explicitly so it cannot be confused with a
complete SA result. Both coordinate widths support sampling rates 2, 4, and 8
with either the ordinary binary or legal LCP-assisted search path:

```text
sa32-sampled-k2-binary       sa32-sampled-k2-lcp-binary
sa32-sampled-k4-binary       sa32-sampled-k4-lcp-binary
sa32-sampled-k8-binary       sa32-sampled-k8-lcp-binary
sa64-sampled-k2-binary       sa64-sampled-k2-lcp-binary
sa64-sampled-k4-binary       sa64-sampled-k4-lcp-binary
sa64-sampled-k8-binary       sa64-sampled-k8-lcp-binary
```

The build table appends `sa_sampling_rate`, which is 2, 4, or 8 for these
methods. Sampled lookup still participates in the same complete range, hit,
coordinate, and checksum correctness gate as every other selected method.

## Standalone-SA construction workload

The dedicated constructor driver isolates divsufsort/CaPS, coordinate width,
thread count, sampling rate, and auxiliary layout:

```bash
./build/release/sufkit_sa_build_bench \
  --profile quick \
  --methods div32,div64,caps32,caps64 \
  --threads 1,2,4,8 \
  --sampling-rates 1,2,4,8 \
  --acceleration none \
  --output-dir results/sa-build
```

Synthetic construction profiles contain 1 MiB (`smoke`), 64 MiB (`quick`),
or 1 GiB (`standard`) of deterministic mixed sequence; `--reference` accepts a
user FASTA. Every repetition uses a child process. Core timing excludes FASTA
parsing, checksumming, persistence, and TSV formatting, while peak RSS covers
the complete child lifetime.

`acceleration=none` isolates the SA constructor. Other layouts include their
ISA/LCP/CHILD work; divsufsort timing includes its fused sampled ISA/Kasai
adapter and CaPS retains merge-built LCP. Sampling compacts only after the
complete suffix order exists, so it must not be described as sparse-SA
construction memory.

The driver compares two independent SA hashes at the same K and exact plus
right-maximal checksums across every K before success. It writes metadata, raw
repetitions, build summaries, and the generated synthetic FASTA. Diagnostic
files remain available if the correctness gate fails.

## Profiles

| Profile | Total reference | Queries | Default scenarios | Default methods | Build/query repetitions |
|---|---:|---:|---|---|---|
| `smoke` | 16 KiB | 100 | mixed | naive, SA32, SA64, FM | 1 / 3 |
| `quick` | 4 MiB | 1,000 | mixed | naive, SA32, SA64, FM | 3 / 5 indexed, 1 naive |
| `standard` | 32 MiB per scenario | 5,000 | all six | SA32, SA64, FM | 3 / 7 |
| `full` | 256 MiB | 10,000 | mixed | SA32, SA64, FM | 1 / 5 |

Every profile performs one query warm-up by default, except that the `quick`
naive baseline uses one measured repetition and no warm-up. This keeps the
interactive profile near its intended scale while still scanning every query,
strand, and operation and participating in the complete correctness gate.
Explicit `--query-repetitions` or `--warmups` values override this policy for
all methods. `standard` covers
`mixed`, `balanced`, `gc-skewed`, `repeat-rich`, `n-islands`, and
`many-contig`; scenarios can be selected explicitly for every synthetic
profile.

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
  --fm-query-modes scalar,batch,batch-mixed \
  --fm-batch-widths 1,4,8,16,32 \
  --output-dir results/fm-quick
```

`fm` remains an alias for `fm-huff`; selecting both in one run is rejected.
Scalar mode records count and locate. Batch mode records count only and keeps
one row per batch width for every existing equal-length group. The optional
`batch-mixed` mode adds a separate `mixed_length`/`mixed` group containing the
dataset's queries in their deterministic order when at least two lengths are
available; when scalar mode is also selected, the same mixed group gets a
scalar baseline for a meaningful speedup ratio. It does not replace or change
the equal-length rows. Query
summaries append the mode, width, processed query bases, bases/s, and speedup
relative to the matching Huffman scalar row.

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
Locate limits may contain numeric limits and `all`. Complete locate is skipped
with status `skipped_high_frequency` if any query in that result group has more
than 100,000 hits, preventing accidental high-frequency materialization.

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

Each method runs in a separate worker process, so peak RSS from an earlier
method cannot contaminate a later one. Timed query loops exclude TSV formatting
and output. Per-logical-pass wall time and user/system CPU time are retained
for every raw repetition; summary tables use medians and also report min/max
wall time.

`peak_rss_mb` is the operating system's high-water mark for the complete
method worker lifetime, which includes build, save, load, and query phases. It
is not query-only RSS. `run_metadata.tsv` records this boundary explicitly as
`peak_rss_scope=method_process_lifetime`. MUMmer4 is the documented external
exception: each reported process measurement covers the corresponding
external build or load-plus-query invocation rather than an in-process sufkit
worker.

Very short query groups repeat the same deterministic logical pass inside one
timed interval. Smoke does not repeat a pass for calibration. Quick targets at
least 10 ms per measured interval; standard, full, and user-reference runs
target at least 100 ms. Output `seconds`, CPU time, query counts, query bases,
hits, checksums, and search statistics are normalized back to one logical pass,
so methods remain directly comparable even when their internal calibration
iteration counts differ. Search statistics come from a separate untimed
single-pass execution whose checksum and hit totals must match the timed pass.

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

`--output-dir` creates exactly four result files:

- `run_metadata.tsv`: dataset, generator, toolchain, OS, CPU, and composition;
- `build_results.tsv`: build/save/load medians, RSS, size, and bits per base;
- `query_results.tsv`: group/length/strand/operation summaries;
- `raw_repetitions.tsv`: every normalized logical-pass wall/CPU time, hit count,
  and checksum, plus ordered query-definition rows containing the query ID and
  synthetic source coordinate/template offset when available.

Metadata deliberately omits the hostname and user-specific input/output paths.
Existing result files are not overwritten. Use a new or empty result directory
for every run.

For reproducibility, the metadata schema appends the configured Git commit and
dirty state, complete configured compiler flags for the library and benchmark
CLI, CPU flags, executable SHA-256, actual process CPU affinity, compiled and
runtime SSE4.2/POPCNT state, a command line with path-valued arguments redacted,
and the peak-RSS scope. Absolute path-bearing compiler flag values are also
redacted. The command remains sufficient to reproduce option choices without
recording private filesystem locations.

Learned exact rows additionally record SA/ISA/LCP/CHILD/model construction
times, model bytes, suffix and character comparisons, gallop probes, local
window sizes, prediction counts/errors, and full binary fallbacks. Prediction
statistics describe performance only; cross-method range, hit, coordinate,
and checksum equality remains mandatory.

The `standard` and `full` profiles are intended for explicit local runs and are
not part of the normal release acceptance commands.

## Right-maximal exact-match workload

```bash
sufkit bench \
  --workload right-maximal \
  --profile quick \
  --scenarios mixed,repeat-rich \
  --methods right-maximal-baseline,right-maximal-lcp,right-maximal-child,right-maximal-suffix-link,right-maximal-full,mummer4 \
  --min-lengths 20,50,100 \
  --strands forward,reverse-complement,both \
  --mummer4 /path/to/mummer \
  --output-dir results/right-maximal-quick
```

The five internal methods build exactly the auxiliary structures their names
require. Query timing includes no TSV formatting. One warm-up precedes three
synthetic smoke or five quick/standard/user-reference repetitions. Synthetic
smoke performs no timing amplification, quick targets 10 ms, and
standard/user-reference internal rows target 100 ms before normalization to
one logical pass. `--query-repetitions N` overrides the default measured
repetition count for every internal and MUMmer4 row; `N` must be positive. The
optional MUMmer4 row uses full SA
(`K=1`), `skip=1`, no k-mer table, one query thread, `-save` for construction,
and `-load` for measured queries. Its reported query time therefore includes
external process startup and index loading and must not be interpreted as an
in-process query-only comparison.

`--strands` accepts a comma-separated subset of `forward`,
`reverse-complement`, and `both`. Its compatibility default is `forward`.
Internal methods build one index per method and then independently warm up,
calibrate, measure, and instrument every selected strand. `strand` is appended
to both `query_results.tsv` and `raw_repetitions.tsv`; existing column positions
remain unchanged. Correctness baselines are keyed by dataset, minimum length,
and strand, so a forward checksum is never compared with a reverse or combined
checksum. `query_bases` continues to mean input query bases; it is not doubled
for the `both` orientation.

MUMmer4 remains the documented exception. Its timed row retains the existing
forward-only `-load` measurement and is labeled `strand=forward`, even when
additional internal strands are requested. The separately executed MUMmer4
reverse-complement path remains a correctness check rather than a timed result
row. The CLI prints a warning when a MUMmer4 run requests any non-forward
strand, so this difference cannot be mistaken for full strand coverage.

`right-maximal-suffix-link-binary` and `right-maximal-suffix-link-sapling` build the same
SA+ISA+LCP layout and differ only in initialization/fallback lookup. Their
outputs report suffix-link success rate, previous-empty states, learned versus
binary lookup counts, character/row accesses, prediction errors, search
windows, and learned model space. `right-maximal-full` remains an explicit CHILD
ablation and is not an automatic default.

The right-maximal exact match workload also accepts `--profile standard`. It generates 32 MiB per
scenario with 5,000 queries of 256 bp and five measured query repetitions.
When comparing the standard profile across the same six scenarios as the main
benchmark, pass them explicitly with
`--scenarios mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig`.

All internal and MUMmer4 rows are normalized to the same zero-based,
query-first tuple checksum. A mismatch preserves diagnostic TSV files and
returns nonzero.
