# Detailed algorithm contracts

This page defines the internal behavior an alternative implementation must
preserve. Pseudocode uses half-open intervals even when a third-party API does
not.

## Suffix comparison and binary range

Compare encoded `pattern[0..m)` with `text[SA[row]..)` without constructing a
suffix string. Sentinel, separator, or N compares by its encoded byte and
cannot equal legal pattern symbols.

Lower bound finds the first suffix not less than the pattern. Upper bound finds
the first suffix whose first m symbols are greater than the pattern. If the
interval is empty, normalize it to `[0,0)` rather than exposing insertion rows.

```text
lo=0, hi=n
while lo<hi:
    mid=(lo+hi)/2
    cmp=compare_suffix_pattern(SA[mid], pattern)
    update lo/hi according to lower or upper policy
```

LCP-aware binary carries known matched prefix lengths at lo and hi. The next
comparison starts at their minimum, records additional character comparisons,
and updates the matching boundary length with the selected row.

## CaPS constructor dispatch

Effective backend policy:

```text
if requested != auto: requested
else if caps_available and threads>1 and symbols>=2^30: caps
else: divsufsort
```

Width auto policy uses the effective backend: CaPS32 is limited by `uint32_t`,
divsufsort32 by signed `saidx_t`; otherwise use 64 bits. Explicit backend or
width violations fail. CaPS subproblems are deterministic from symbols and
threads and do not affect suffix order.

After any constructor, the common permutation/sortedness tests and common
auxiliary pipeline apply.

## ISA and Kasai LCP

Build ISA by assigning each SA row to its text position. Parallel chunks may
write disjoint positions.

Kasai scans text positions in order. For suffix at position p with row r>0,
compare against `SA[r-1]` from the previous retained common-prefix length;
store at `LCP[r]`, then decrement the retained length by one when positive.
Hard encoded symbols participate in suffix ordering but MEM code treats them
as boundaries.

## CHILD table

CHILD is built deterministically from LCP with the Abouelhoda-style linear
stack procedure for up/down/next-L links. Persisted values use SA coordinate
width and one out-of-band convention representable by the validated table.

Loading checks ranges and rebuilds the deterministic CHILD table from LCP for
structural equality. A future compact validation may replace full rebuild only
after equivalent corruption coverage is demonstrated.

CHILD traversal must return the same range as binary search for all legal
patterns. Verification/narrowing remains in the current path because removing
it has not passed the complete randomized differential contract.

## PWL construction

For every contig, scan legal A/C/G/T k-mers with rolling 2-bit encoding. Do not
generate k-mers containing N, crossing a separator, or crossing contigs.
Map a k-mer text position to row with ISA.

Choose `bucket_bits` explicitly or as the largest power-of-two anchor table
whose serialized size stays within the requested raw-SA basis-point budget.
Store anchor key (`uint64_t`) and row (SA width). Fill empty buckets with
neighboring monotonic anchors and append the terminal domain anchor.

Interpolate between adjacent anchors with checked unsigned arithmetic and a
wide integer intermediate. Floating point is not part of persisted/query
semantics.

## PWL exact lookup

```text
if pattern length < k: full binary
key = encode first k bases
pred = integer_interpolate(bucket anchors)
compare pattern/prefix at pred
gallop 1,2,4,... toward required boundary
binary-search inside proven bracket
verify boundary
fallback to full binary if a valid bracket cannot be established
```

Find both lower and upper k-prefix boundaries. If pattern is longer than k,
continue remaining comparisons only inside that interval. Statistics count
predictions, absolute row error, gallop probes, local rows, comparisons, and
full fallbacks; they never steer correctness.

## FM backward search

Encode pattern to bytes 2–5. Start with SDSL's complete closed interval and
call `sdsl::backward_search` from the last pattern byte to the first. Convert a
non-empty `[lb,rb]` into `[lb, rb+1)`; normalize empty output to `[0,0)`.

Batch search stores one independent SDSL interval and reverse cursor per
pattern. For a chunk of at most width states, advance each active state by one
character per round via the same public single-character call. Do not read a
wavelet tree directly. Scalar and every width must return identical ranges.

## Locate and strand finalization

For each range row, read SA/CSA global position. Reject separator/sentinel/N or
a pattern extent outside the mapped contig. Convert to `Match`, combine strand
queries, sort, merge exact orientation duplicates, retain first N, and preserve
the complete count.

FM `operator[]` may be used for bounded materialization; unbounded operations
may use SDSL locate facilities if result semantics remain identical.

## MEM baseline

Split a query orientation into maximal canonical A/C/G/T runs. For each start
that can support `min_length`:

1. find the min-length prefix range;
2. compute right LCE for candidate suffixes;
3. reject non-left-maximal candidates;
4. emit valid right- and left-maximal reference/query pairs; and
5. enforce contig and query-run bounds.

Avoid constructing suffix substrings. Duplicate generation within a path must
be normalized before public vector comparison.

## LCP and CHILD MEM paths

LCP mode uses adjacent common-prefix information to expand/enumerate candidate
intervals without repeating all suffix characters. CHILD mode navigates a
query start top-down from the root. Both must call the same maximality and
coordinate emission logic as baseline.

## Suffix-link reuse

For the previous interval representing query substring `Q[i..i+l)`, derive the
row/interval for `Q[i+1..i+l)` through ISA positions and LCP interval expansion.
Retain only an interval proven to represent the deleted-first-character
substring. Extend for the next query symbols by binary/LCP/PWL lookup as
configured.

Failure conditions include empty previous interval, hard break, insufficient
length, invalid shifted text position, degenerate expansion, or failed
verification. Any failure resets to a root lookup. `full` combines explicit
CHILD navigation with the same reuse contract.

## MEM maximality and coordinates

A candidate `(r,q,l)` is valid only if all l canonical symbols equal. It is
left maximal when either side is at a boundary or preceding symbols differ;
right maximal is analogous for following symbols. Reference contig bounds and
query canonical-run bounds are the authority.

Reverse orientation output position is:

```text
original_query_length - (rc_position + match_length)
```

All five algorithm modes, all lookup algorithms, constructors, widths, save/
load states, and thread counts must produce the same normalized tuple set.
