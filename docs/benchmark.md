# Benchmark methodology

The benchmark is a correctness-gated, deterministic comparison of the naive
scanner, standalone divsufsort32/64 suffix arrays, and the fixed SDSL FM-index.
It records construction, serialization, loading, count, and locate separately.
Performance numbers are descriptive; sufkit does not impose absolute speed
thresholds across machines.

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
and output. Wall time and user/system CPU time are retained for every raw
repetition; summary tables use medians and also report min/max wall time.

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
- `raw_repetitions.tsv`: every measured wall/CPU time, hit count, and checksum,
  plus ordered query-definition rows containing the query ID and synthetic
  source coordinate/template offset when available.

Metadata deliberately omits the hostname and user-specific input/output paths.
Existing result files are not overwritten. Use a new or empty result directory
for every run.

The `standard` and `full` profiles are intended for explicit local runs and are
not part of the normal release acceptance commands.

## MEM workload (0.1.1)

```bash
sufkit bench \
  --workload mem \
  --profile quick \
  --scenarios mixed,repeat-rich \
  --methods mem-baseline,mem-lcp,mem-child,mem-suffix-link,mem-full,mummer4 \
  --min-lengths 20,50,100 \
  --mummer4 /path/to/mummer \
  --output-dir results/mem-quick
```

The five internal methods build exactly the auxiliary structures their names
require. Query timing includes no TSV formatting. One warm-up precedes three
smoke or five quick repetitions. The optional MUMmer4 row uses full SA
(`K=1`), `skip=1`, no k-mer table, one query thread, `-save` for construction,
and `-load` for measured queries. Its reported query time therefore includes
external process startup and index loading and must not be interpreted as an
in-process query-only comparison.

The MEM workload also accepts `--profile standard`. It generates 32 MiB per
scenario with 5,000 queries of 256 bp and five measured query repetitions.
When comparing the standard profile across the same six scenarios as the main
benchmark, pass them explicitly with
`--scenarios mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig`.

All internal and MUMmer4 rows are normalized to the same zero-based,
query-first tuple checksum. A mismatch preserves diagnostic TSV files and
returns nonzero. See `benchmark-mem-v0.1.1.md` for the measured release run.
