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
| `--sa-storage-width` | `auto`, `32`, `40`, `48`, `64` | `auto` |
| `--sa-profile` | `fast`, `low-memory` | `fast` |
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

`--sa-width` controls the construction backend. divsufsort32 is limited by a
signed 32-bit coordinate, while CaPS32 uses unsigned 32-bit coordinates.
`--sa-storage-width` independently controls the resident and persisted SA
layout and preferred auxiliary width after construction. An auxiliary domain
that includes a one-past row marker may promote independently. Auto selection
uses the complete logical text (bases + separators + sentinel):

- Fast chooses native32 when possible, otherwise native64;
- Low-memory chooses the narrowest of native32, split40, split48, and
  native64.

Explicitly requesting a width that cannot represent the largest logical-text
position is an input error. A 64-bit build may validly store as 32/40/48 bits
after full coordinate and permutation validation.

Low-memory requires a complete SA (`--sa-sampling-rate 1`), forces the LCP
layout, and omits resident ISA, CHILD, and learned data. The CLI rejects an
explicit non-LCP acceleration, a sampled SA, or learned-index options together
with this profile. Fast keeps the requested acceleration and defaults to
SA+ISA+raw LCP. Low-memory persists byte-coded LCP. CHILD/full construction
needs raw LCP while constructing CHILD; Fast retains that native
representation.

The profile describes the final resident index. It does not change the peak
working set of the selected constructor: CaPS still creates its complete SA,
complete LCP, work arrays, and partition metadata before sufkit can retain the
compact result.

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

## `sufkit mem` and `sufkit mam`

```text
sufkit mem --index PATH --query Q.fa[.gz]
  [--min-length N] [--strand forward|reverse|both]
  [--algorithm auto|baseline|lcp|child|suffix-link|full]
  [--lookup-algorithm auto|binary|lcp-binary|sapling-pwl|child]
  [--skip N] [--max-matches N]

sufkit mam --index PATH --query Q.fa[.gz]
  [--min-length N] [--strand forward|reverse|both]
  [--algorithm auto|baseline|lcp|child|suffix-link|full]
  [--lookup-algorithm auto|binary|lcp-binary|sapling-pwl|child]
  [--max-matches N]
```

`mem` reports two-sided maximal exact matches. `mam` additionally requires
the matched string to occur exactly once across the combined reference; query
uniqueness is not required. `--skip` is MEM-only. MAM requires a complete SA,
and both commands reject FM indexes. Output columns and coordinate conventions
are identical to `right-maximal`.

## `sufkit inspect`

```text
sufkit inspect --index PATH
```

Output is a two-column `key/value` TSV. Common fields include format and
library versions, kind, backend signature, SDSL version, sequence/base/symbol
counts, fingerprint, and serialized bytes. `coordinate_width` is retained as
the construction-width compatibility field; `construction_coordinate_width`
makes that meaning explicit, while `stored_coordinate_width` reports the
physical SA layout. `construction_backend` is the explicit constructor
provenance key; `backend` is retained as its compatibility alias.

SA indexes also report `sa_resource_profile`, `lcp_encoding`,
`suffix_count`, `sa_sampling_rate`, acceleration, lookup capability, and
learned-model metadata. Memory breakdown fields are `text_bytes`, `sa_bytes`,
`isa_bytes`, `lcp_bytes`, `child_bytes`, and `resident_core_bytes`.
Byte-coded LCP further reports its primary bytes, overflow-anchor count/bytes,
and derived guide bytes.

## `sufkit bench`

Exact workload:

```text
sufkit bench --profile smoke|quick|standard|full --output-dir DIR [options]
sufkit bench --reference REF.fa[.gz] [--queries Q.fa[.gz]] --output-dir DIR
```

Main controls include `--scenarios`, `--methods`, `--pattern-lengths`,
`--locate-limits`, `--seed`, repetitions, warmups, learned-model parameters,
`--fm-query-modes`, and `--fm-batch-widths`.

maximal-match workloads:

```text
sufkit bench --workload right-maximal --profile smoke|quick|standard \
  --strands forward,reverse-complement,both \
  --output-dir DIR [options]
```

The right-maximal compatibility default is `--strands forward`. Internal
methods emit an independent summary/raw row for every explicitly selected
orientation. MUMmer4 timed rows retain their historical forward-only scope.

Formal workloads use `--workload mem` or `--workload mam` and method prefixes
`mem-*` or `mam-*`. MUMmer4 uses `-maxmatch` for MEM and `-mumreference` for
reference-MAM; all measured methods must pass the same checksum gate.

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
