# Exact search

Exact search accepts case-insensitive A/C/G/T patterns. Empty patterns, N,
IUPAC symbols, whitespace, and every other character are invalid. This strict
contract differs from right-maximal exact match queries, where non-ACGT characters are hard breaks.

## Range, count, and locate

`EqualRange(pattern)` returns a half-open row range `[begin,end)`. Range size
is the number of forward occurrences. Empty results are normalized to
`[0,0)`; no insertion-position meaning is promised for an empty result.

`Count(pattern, strands)` performs range searches without materializing
coordinates. `Locate(pattern, options)` additionally resolves SA rows, maps
global positions to contigs, sorts, merges strand duplicates where required,
and optionally retains only the first N results.

For high-hit queries, `Count` can be much cheaper than `Locate`. A learned range
prediction only accelerates range discovery; it cannot accelerate the cost of
materializing and sorting a very large result set.

## Sampled standalone SA

For sampling rate K>1, the stored suffixes cover only text positions divisible
by K. `Count` and `Locate` still return the complete-SA result by searching up
to K pattern offsets and verifying the omitted left prefix. Patterns shorter
than K use a direct per-contig scan because no sampled position is guaranteed
inside a match.

`EqualRange` is deliberately unsupported for sampled SA: the result is a
union of residue-specific intervals rather than one `SuffixRange`. Explicit
binary, LCP-binary, PWL, and CHILD selectors apply to each sampled anchor
lookup, but do not change recovery semantics.

## Strand semantics

| Mode | Search |
|---|---|
| `StrandMode::kForward` | Original pattern |
| `StrandMode::kReverseComplement` | Reverse-complement pattern only |
| `StrandMode::kBoth` | Union of the two orientations |

For a reverse-complement palindrome in `kBoth` mode, a coordinate is returned
once with `Strand::kBoth`. Otherwise matches at the same coordinate are merged
to `kBoth` only when both orientations genuinely produce that coordinate.

Results are ordered by sequence ID, position, length, and strand. Coordinates
are zero-based and contig-local.

## SA algorithms

| Algorithm | Required data | Behavior |
|---|---|---|
| `SaSearchAlgorithm::kBinary` | SA | Two full suffix binary searches |
| `SaSearchAlgorithm::kLcpBinary` | SA | Reuses common-prefix lengths computed at the current binary-search boundaries |
| `SaSearchAlgorithm::kSaplingPwl` | Learned section | Predicts a row, brackets safely, then performs local LCP-aware search |
| `SaSearchAlgorithm::kChild` | LCP+CHILD | Explicit ESA top-down traversal |
| `SaSearchAlgorithm::kAutoSelect` | Any | PWL only if present and pattern length ≥ k; otherwise binary |

Auto selection never chooses CHILD. An explicit algorithm whose data is not
present throws `unsupported_backend` rather than silently degrading.

PWL prediction does not define the answer. Exponential expansion finds bounds
that contain the true lower or upper boundary; verified local search completes
the range. Patterns shorter than model k use full binary fallback.

`SaSearchStatistics` is optional mutable output for suffix/character
comparisons, prediction error, local-window rows, gallop probes, and fallback
counts. Use one object per concurrent query.

## FM search

FM scalar search uses SDSL `backward_search`. The SDSL closed interval is
converted to the public half-open range. Locate reads CSA values; sufkit does
not access wavelet-tree internals or implement LF.

`EqualRangeBatch` and `CountBatch` preserve pattern order. The batch
implementation interleaves independent backward-search states in fixed-width
chunks. Width zero means 16; explicit width is 1–256. Every pattern is encoded
before the first search, so invalid input rejects the whole call. There is no
batch locate API.

## Boundaries and damaged indexes

N, contig separators, and the unique sentinel have codes that cannot occur in
a legal pattern. Every located global position is still checked against
contig bounds before it becomes a public match. A malformed position from a
corrupt payload is rejected rather than returned.

See [text-position sampled suffix arrays](../concepts/sampled-suffix-arrays.md)
for construction, persistence, and memory trade-offs.
