# Algorithm overview

This page explains what each algorithm contributes and how the pieces fit. The
[algorithm contracts](../development/algorithm-contracts.md) define the exact
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

sufkit provides two complete-SA constructors:

- libdivsufsort32/64: serial, mature, and generally preferable for small or
  medium references;
- CaPS-SA32/64: shared-memory parallel construction for large references.

Both receive identical encoded text and produce the same suffix order. CaPS
internally computes LCP information during merging, but sufkit intentionally
discards it and runs the common post-SA pipeline. Constructor choice therefore
does not change exact or MEM semantics.

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

`locate` visits the matched SA rows, maps them to contigs, and sorts results,
so its lower bound includes O(z) output work regardless of range-search speed.

## ISA and LCP

The inverse suffix array satisfies:

```text
ISA[SA[row]] = row
```

LCP stores the common-prefix length of adjacent suffixes, with `LCP[0]=0`.
sufkit constructs it with the linear Kasai algorithm using SA and ISA.

LCP supports interval reasoning and reduces repeated comparisons. ISA enables
moving from a text position back to the row of its suffix, which is central to
suffix-link interval reuse.

## CHILD and enhanced suffix arrays

The CHILD table encodes selected parent/child and sibling relationships among
LCP intervals. Together, SA+LCP+CHILD can simulate suffix-tree-style top-down
navigation without storing pointer-heavy tree nodes.

CHILD is a capability rather than the default. Current exact and MEM paths
include verification/narrowing work that can make CHILD slower. It is retained
for explicit interval navigation, future MUM/MAM and maximal-repeat work, and
algorithm research.

## MEM and suffix-link reuse

A MEM is exact and cannot be extended with the same canonical base on either
side. A baseline can search from every query position, but repeatedly starting
from the root wastes work on related suffixes of the query.

Suffix-link reuse deletes the first character of the previous matched query
substring and derives the corresponding SA interval using ISA and LCP. The
search then extends downward for the new character sequence.

```text
query[i ...] interval
       │ delete query[i]
       ▼
query[i+1 ...] reused interval → extend → verify maximality
```

Invalid, empty, or hard-boundary states return to a correct root lookup. The
optimization changes how an interval is found, not which MEMs are emitted.

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

Primary provenance and fixed dependency revisions are recorded in
`THIRD_PARTY_NOTICES.md`, the backend reference, and detailed benchmark pages.
