# Algorithm overview

This page explains what each algorithm contributes and how the pieces fit. The
[algorithm internals](../development/algorithm-internals.md) define the exact
implementation invariants used by contributors.

Let `n` be logical reference symbols, `m` pattern length, `z` reported hits,
and `q` query length.

## Suffix arrays

A suffix array stores every reference suffix position in lexicographic order.
An exact pattern occupies one contiguous row interval.

```text
reference text → suffixes sorted by content → SA rows
pattern        → lower bound + upper bound → matching SA interval
```

sufkit provides two constructors that first establish a complete suffix order:

- libdivsufsort32/64: serial, mature, and generally preferable for small or
  medium references;
- CaPS-SA32/64: shared-memory parallel construction for large references.

Both receive identical encoded text and produce the same suffix order. The
constructor's 32/64-bit integer type is independent of the final coordinate
layout: a wider constructor result may be validated and down-packed to
native32, split40, or split48 storage. CaPS computes a merge-built LCP
internally. Fast and CHILD construction copy the required raw values while the
CaPS object is alive. Low-memory releases the CaPS object and then builds
byte-coded LCP from the retained SA and a temporary ISA. The divsufsort path
likewise constructs the profile-selected raw or byte-coded auxiliary
representation after sorting. Constructor choice does not change exact or
maximal-match semantics.

Construction is generally O(n) or near-linear in practical suffix sorters;
the public contract promises correctness and width limits rather than one
implementation-independent worst-case bound.

## Exact SA search

Ordinary search performs two binary searches and compares a pattern directly
against selected suffixes. It visits O(log n) rows, with up to O(m log n)
character work in the simple bound.

LCP-aware binary search carries the prefix already known to match at each
boundary. It reduces repeated character comparisons while retaining the same
row probes and result interval.

`Locate` visits the matched SA rows, maps them to contigs, and sorts results,
so its lower bound includes O(z) output work regardless of range-search speed.

## Text-position sampled SA

With sampling rate K, sufkit retains suffixes whose logical-text position is
divisible by K. The stored order is a subsequence of the complete SA and has
approximately n/K rows. ISA, LCP, CHILD, and PWL are defined over this sampled
row domain.

Exact matches are recovered over all K residue classes. Right-maximal and MEM
matches are anchored on sampled reference positions and recovered to their
true boundaries. The trade-off is additional query work and `min_length>=K`
for maximal-match search. Because one exact result becomes a union of intervals, sampled SA
cannot expose a
single `EqualRange`.

Both backends still create a complete SA before compaction. Sampling reduces
the loaded and serialized index, not full-SA constructor peak memory. See the
[search guide](../user-guide/search.md) for its public behavior.

## ISA and LCP

The inverse suffix array satisfies:

```text
ISA[SA[row]] = row
```

LCP stores the common-prefix length of adjacent stored suffixes, with
`LCP[0]=0`. Low-memory constructs byte-coded LCP with a linear generalized
Kasai scan using SA and ISA; Fast retains native raw LCP. For sampled
positions, the retained common prefix is reduced by K per text step. When
CHILD needs raw rows,
divsufsort uses a raw generalized Kasai scan; CaPS can reuse its merge-built
complete LCP, compacting it by range minima between retained rows when K is
greater than one.

LCP supports interval reasoning and reduces repeated comparisons. ISA enables
moving from a text position back to the row of its suffix, which is central to
suffix-link interval reuse.

Most LCP values in genomic indexes are short. Byte-coded LCP stores values
below 255 directly in one byte. A 255 marker identifies a longer value; a
text-ordered anchor stores the start position and value of each maximal run
whose generalized PLCP decreases by the sampling rate. A small derived guide
narrows anchor lookup after load. Low-memory uses this representation
unconditionally. Fast keeps raw LCP because byte coding did not meet the
three-percent per-workload regression gate in pinned quick measurements. Both
representations expose identical row values.

## Adaptive coordinate storage

The largest logical-text position, not constructor choice, determines which
final storage widths are valid:

```text
max_position <= UINT32_MAX  -> native32
max_position < 2^40         -> split40
max_position < 2^48         -> split48
otherwise                   -> native64
```

Split40 uses contiguous low32 and high8 planes; split48 uses low32 and high16.
This avoids padding each coordinate to eight bytes. Random row probes decode
one low/high pair, while interval enumeration can decode a contiguous span.
The public coordinate type remains `uint64_t`.

Fast chooses native32 or native64 automatically because suffix-link lookups
favor native random access. Low-memory chooses the narrowest valid width and
keeps SA+LCP without a resident ISA. This is a policy distinction, not a
change in suffix order or query semantics.

## CHILD and enhanced suffix arrays

The CHILD table encodes selected parent/child and sibling relationships among
LCP intervals. Together, SA+LCP+CHILD can simulate suffix-tree-style top-down
navigation without storing pointer-heavy tree nodes.

CHILD is a capability rather than the default. Current exact and right-maximal exact match paths
include verification/narrowing work that can make CHILD slower. It is retained
for explicit interval navigation, MAM and maximal-repeat research, and
algorithm research.

## Right-maximal exact matches and suffix-link reuse

A right-maximal exact match is exact and cannot be extended with the same
canonical base on the right. The current implementation does not guarantee
left maximality and is therefore not a MEM implementation. A baseline can
search from every query position, but repeatedly starting from the root wastes
work on related suffixes of the query.

Suffix-link reuse deletes the first character of the previous matched query
substring and derives the corresponding SA interval using ISA and LCP. The
search then extends downward for the new character sequence.

```text
query[i ...] interval
       │ delete query[i]
       ▼
query[i+1 ...] reused interval → extend → verify right maximality
```

Invalid, empty, or hard-boundary states return to a correct root lookup. The
optimization changes how an interval is found, not which right-maximal exact
matches are emitted.

## MEM and reference-MAM

A MEM is exact and cannot be extended jointly on either side. From a query
anchor, sufkit first finds a right-side SA interval. LCP information exposes
shorter outer intervals so different reference occurrences with different
right-maximal lengths are not lost. ISA+LCP suffix links reuse the previous
anchor interval; failed reuse returns to a verified root lookup.

For a complete SA, one predecessor comparison proves left maximality. For a
sampled SA with rate K and skip multiplier s, the query is searched in all K
residue classes and each candidate is recovered left by at most `s*K` bases.
If the full window still matches, the preceding anchor owns the same MEM and
the current anchor is suppressed. This is the sparseMEM/essaMEM ownership
principle used by MUMmer4, implemented independently in sufkit.

A reference-MAM is a MEM whose matched string has exactly one occurrence in
the union of all reference contigs. Query uniqueness is deliberately not
required. sufkit therefore checks a complete-SA interval of size one and only
supports MAM with K=1. Strict MUM would additionally require query uniqueness
and is outside the current API.

## Sapling-style PWL lookup

Sapling uses the first k pattern bases as a 2-bit integer key. A compact
piecewise-linear model predicts an approximate SA row. sufkit then:

1. compares at the prediction;
2. expands in powers of two in the required direction;
3. brackets the true boundary;
4. runs local LCP-aware binary search; and
5. verifies the final boundaries.

The model is a hint. Poor prediction increases work but cannot drop a match.
Patterns shorter than k use full binary search. Space is controlled as basis
points of the raw SA payload and anchors use deterministic integer
interpolation.

## FM-index and SDSL CSA

The Burrows–Wheeler transform clusters equal preceding symbols. An FM-index
uses cumulative symbol counts and rank queries to extend a pattern interval
backward one character at a time:

```text
full CSA interval
  ← last pattern character
  ← previous character
  ...
  ← first character
= matching interval
```

sufkit delegates construction, rank-backed backward search, SA sampling,
position recovery, and native payload serialization to SDSL 3.0.3. Three
fixed `csa_wt` types are available:

- Huffman wavelet tree: compressed default;
- balanced wavelet tree: explicit comparison backend;
- EPR small-alphabet wavelet tree: faster rank on current DNA workloads at a
  significant space/load cost.

Count needs O(m) backward transitions in the high-level model. Locate adds
backend-dependent sampled-SA recovery for each reported row.

## Batched backward search

Batch count keeps independent pattern intervals and advances active queries in
fixed-width groups. This can expose instruction and memory-level parallelism
without changing SDSL internals. Every transition still calls SDSL's public
single-character `backward_search`.

Batching preserves input order and scalar semantics. It helps only when the
backend and workload can hide latency; more active states are not guaranteed
to be faster.

## Selection philosophy

Automatic choices are deliberately conservative:

- constructor auto-routing uses CaPS only for large, multithreaded builds;
- SA acceleration defaults to ISA+LCP suffix links;
- exact auto search chooses PWL only when a model exists and the pattern is
  long enough; otherwise binary;
- CHILD/full are explicit;
- FM construction defaults to Huffman;
- experimental speedups are promoted only after cross-method equivalence and
  representative benchmarks.

In particular, the split40/48 layouts are not promoted into Fast auto
selection, and no `>2^32` or MUMmer4 superiority claim is made until real-scale
build, loaded-memory, and query measurements pass the documented gates.

Primary provenance and fixed dependency revisions are recorded in
`THIRD_PARTY_NOTICES.md`, the backend reference, and detailed benchmark pages.
