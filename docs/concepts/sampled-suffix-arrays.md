# Text-position sampled suffix arrays

This page describes sufkit's optional standalone-SA sampling. It is distinct
from SDSL's internal CSA sampling and from a constructor that directly builds a
sparse suffix array.

## Definition

For a positive sampling rate `K`, sufkit retains complete-SA entries whose
original logical-text position satisfies:

```text
position mod K = 0
```

The retained suffixes remain in the same lexicographic order as the complete
SA. `K=1` is the complete-SA default. For logical text length `n`, including
separators and the final sentinel, the stored row count is:

```text
suffix_count = ceil(n / K)
```

Configure it in C++ with `SuffixArrayBuildOptions::sampling_rate` or in the
CLI with `--sa-sampling-rate K`.

## Construction and LCP

Both constructors first establish the complete suffix order. Sampling is an
in-place compaction step, so it reduces the resident and serialized final
index, but it does not remove the constructor's full-SA peak-memory
requirement.

The two backend paths avoid a redundant post-construction LCP pass:

- The private divsufsort adapter compacts the returned SA, builds sampled ISA,
  and runs the generalized Kasai scan in one backend result. When scanning
  adjacent sampled text positions, the retained common prefix decreases by
  `K`, not one.
- CaPS already computes a complete LCP vector during its merge stages. sufkit
  retains it when requested and, while compacting the complete SA, assigns the
  LCP between adjacent retained suffixes as the minimum complete LCP over the
  removed-row interval.

ISA, LCP, CHILD, and the optional learned model are then defined over the
stored sampled row order. This means their row count scales approximately as
`n/K`.

## Exact search recovery

A match can begin in any residue class modulo `K`, so one sampled-SA interval
cannot represent all exact results.

For a pattern of length at least `K`, `count()` and `locate()` search up to K
anchors. For offset `r`, the search uses `pattern[r..]`, subtracts `r` from
each sampled candidate, verifies the omitted left prefix, and checks the
contig boundary. The union recovers the complete-SA result.

Patterns shorter than `K` can contain no sampled reference position. They use
a direct per-contig scan to preserve exact semantics. This is correct but may
be slower than a complete SA for short-pattern workloads.

`equal_range()` intentionally returns `unsupported_backend` for `K>1` because
the complete result is a union of residue-specific intervals rather than one
public half-open interval.

## Right-maximal exact match recovery

Sampled right-maximal exact match search requires:

```text
min_length >= K
```

Each canonical query run is searched by residue class. Candidates are anchored
at a sampled reference position, mapped back to a candidate start, extended
right to maximality, and emitted only from the first eligible anchor. This
preserves the complete-layout right-maximal result set without duplicate
anchors. It does not add a left-maximality guarantee.

Suffix-link reuse advances by K query characters on the sampled path. Its
ISA/LCP interval expansion and CHILD navigation operate entirely in the
sampled row domain.

## API and persistence

- `SuffixArray::sampling_rate()` returns K.
- `IndexInfo::sa_sampling_rate` records K.
- `IndexInfo::suffix_count` records the number of stored rows.
- `suffix_at(row)` accepts rows in `[0, suffix_count)` and returns the original
  global logical-text position, which is divisible by K.
- Format 1.3 adds the `sa_sampling` section. Formats 1.0-1.2 imply K=1.

The learned PWL model is compatible with sampling and is trained on the stored
sampled suffix order. Its memory budget is therefore relative to the sampled
SA payload.

## When to use it

Sampling is primarily a final-index memory/size trade-off. It is most suitable
when patterns and right-maximal exact match thresholds are not shorter than K and construction has
enough memory for the complete backend SA. It is not a substitute for SDSL FM
compression, disk-backed construction, or a direct sparse-SA algorithm.

See the [exact-search guide](../user-guide/exact-search.md),
[right-maximal exact match guide](../user-guide/right-maximal-search.md), and
[sampled-SA benchmark](../benchmarks/results/unreleased-sampled-sa.md).
