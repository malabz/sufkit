# CLI reference

The CLI writes machine-readable results to stdout and diagnostics to stderr.
Successful commands return 0.

## `sufkit build`

```text
sufkit build --type sa|fm --input PATH --output PATH [--force]
```

Common options:

| Option | Meaning |
|---|---|
| `--type sa|fm` | Required index kind |
| `--input PATH` | Plain or gzip FASTA |
| `--output PATH` | Target `.sufidx` |
| `--force` | Permit replacement of an existing target |

SA options:

| Option | Values | Default |
|---|---|---|
| `--sa-backend` | `auto`, `divsufsort`, `caps` | `auto` |
| `--sa-width` | `auto`, `32`, `64` | `auto` |
| `--threads` | Positive uint32 | `1` |
| `--sa-sampling-rate` | Positive uint32 K | `1` (complete SA) |
| `--sa-acceleration` | `none`, `lcp`, `child`, `suffix-link`, `full` | `suffix-link` |
| `--learned-index` | Flag | Off |
| `--learned-k` | 1–31 | `20`; specifying it enables the model |
| `--learned-memory-bp` | Positive uint32 basis points | `100` |
| `--learned-bucket-bits` | 0–31 and no more than `2*k` | Automatic |

`auto` chooses CaPS only at at least 1 GiB of logical symbols, with more than
one thread and a CaPS-enabled build. Explicit CaPS requires at least 16
logical symbols. SA options are rejected for FM builds.

For K>1, the stored SA retains text positions divisible by K. C++ `Count` and
`Locate` recover complete results; `EqualRange` is unavailable and
right-maximal exact match requires `min_length>=K`.

FM option:

| Option | Values | Default |
|---|---|---|
| `--fm-backend` | `sdsl-csa-wt-huff`, `sdsl-csa-wt-balanced`, `sdsl-csa-wt-epr` | Huffman |

FM options are rejected for SA builds. Reserved `sdsl-csa-sada` is recognized
by the library model but not accepted as an available CLI backend.

## `sufkit query`

```text
sufkit query --index PATH (--pattern ACGT | --query Q.fa[.gz])
  [--strand forward|reverse|both]
  [--count-only]
  [--max-hits N]
  [--algorithm auto|binary|lcp-binary|sapling-pwl|child]
```

Exactly one of `--pattern` and `--query` is required. Query FASTA names must be
non-empty and unique. `--algorithm` applies only to standalone SA indexes;
non-auto selection is rejected for FM indexes.

Count-only TSV:

```text
query_id	total_hits
```

Locate TSV:

```text
query_id	sequence_id	sequence_name	start	end	strand
```

Coordinates are zero-based and contig-local; `end` is exclusive. A truncation
warning on stderr reports returned and total hits.

## `sufkit right-maximal`

```text
sufkit right-maximal --index PATH --query Q.fa[.gz]
  [--min-length N]
  [--strand forward|reverse|both]
  [--algorithm auto|baseline|lcp|child|suffix-link|full]
  [--lookup-algorithm auto|binary|lcp-binary|sapling-pwl|child]
  [--max-matches N]
```

`--min-length` defaults to 20 and must be positive. The index must be a
standalone SA. `--max-matches` is applied independently to each query record.

Output:

```text
query_id	sequence_id	sequence_name	reference_start	query_start	length	strand
```

Both reference and query positions are zero-based. Reverse matches use the
original forward-query coordinate system. Unlike exact both-strand search,
forward and reverse right-maximal exact matches remain orientation-distinct.

## `sufkit inspect`

```text
sufkit inspect --index PATH
```

Output is a two-column `key/value` TSV. Common fields include format and
library versions, kind, backend signature, SDSL version, coordinate width,
sequence/base/symbol counts, fingerprint, and serialized bytes. SA indexes
also report `suffix_count`, `sa_sampling_rate`, acceleration, auxiliary bytes,
lookup acceleration, learned model size, k, bucket bits, and requested budget.

## `sufkit bench`

Exact workload:

```text
sufkit bench --profile smoke|quick|standard|full --output-dir DIR [options]
sufkit bench --reference REF.fa[.gz] [--queries Q.fa[.gz]] --output-dir DIR
```

Main controls include `--scenarios`, `--methods`, `--pattern-lengths`,
`--locate-limits`, `--seed`, repetitions, warmups, learned-model parameters,
`--fm-query-modes`, and `--fm-batch-widths`.

right-maximal exact match workload:

```text
sufkit bench --workload right-maximal --profile smoke|quick|standard|full \
  --output-dir DIR [options]
```

See [benchmark methodology](../benchmarks/methodology.md) for profiles,
methods, schemas, correctness gates, and timing boundaries.

Parallel SA construction has a separate executable:

```text
sufkit_sa_build_bench --profile smoke|quick|standard \
  --methods div32,div64,caps32,caps64 \
  --threads 1,2,4,8 --sampling-rates 1,2,4,8 \
  --acceleration none|full --output-dir DIR
```

## Exit codes

| Code | Meaning |
|---:|---|
| 0 | Success |
| 2 | Invalid CLI or input |
| 3 | I/O failure |
| 4 | Corrupt index or incompatible format/version |
| 5 | Unavailable backend, construction failure, or unexpected failure |
