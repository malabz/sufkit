# MEM search

A maximal exact match (MEM) is an exact reference/query match that cannot be
extended by the same canonical base on both sides. sufkit reports every
reference/query MEM pair whose length is at least `min_length`.

## Sequence and coordinate semantics

- Lowercase A/C/G/T is normalized.
- Every other query symbol is a hard break.
- Reference N, contig separators, the sentinel, and query hard breaks cannot
  be crossed.
- Reference positions are zero-based and contig-local.
- Query positions are zero-based in the original forward query.
- Reverse-complement matches are transformed back with
  `query_length - (rc_position + length)`.
- Forward and reverse MEMs remain distinct, including palindromic queries.

An empty query, an all-break query, or a query shorter than `min_length`
returns no matches. `min_length=0` is invalid.

## Sampled standalone SA

For sampling rate K>1, MEM requires `min_length>=K`. The query is divided into
K residue classes, candidates are anchored on sampled reference positions,
then extended to the true left and right maximal boundaries. Duplicate anchors
inside one MEM are suppressed, so the normalized result remains identical to
the complete SA.

Suffix-link steps advance by K positions on this path and the stored ISA/LCP/
CHILD structures refer to sampled rows. The restriction is checked explicitly;
there is no silent fallback to a different MEM definition.

## Algorithms

| Mode | Persisted requirement | Main strategy |
|---|---|---|
| `baseline` | SA | Start each query position from a root SA lookup |
| `lcp` | SA+LCP | Use LCP interval information for candidate expansion |
| `child` | SA+LCP+CHILD | Start each position with ESA top-down traversal |
| `suffix_link` | SA+ISA+LCP | Reuse the previous interval after deleting one query character |
| `full` | SA+ISA+LCP+CHILD | Combine explicit CHILD navigation and suffix-link reuse |

All modes must return the same normalized MEM set. Optimized interval reuse is
discarded when it becomes invalid, crosses a hard break, or cannot be safely
derived.

`auto_select` chooses suffix-link, then LCP, then baseline according to stored
data. It does not automatically choose CHILD or full.

`MemOptions::lookup_algorithm` controls root initialization and suffix-link
fallback. Binary is always available; learned PWL is useful only when a model
exists and enough root/fallback work remains. Explicit unavailable choices
throw `unsupported_backend`.

## Streaming and bounded vector APIs

`for_each_mem` invokes a callback synchronously for every match. It provides
low result-storage overhead but does not promise enumeration order across
algorithms. A callback exception propagates unchanged and stops the search.

`find_mems` returns a deterministic order:

```text
query_position, sequence_id, reference_position, length, strand
```

When `max_matches=N`, the implementation still traverses the full result to
compute `total_matches`, but retains only the sorted first N with bounded
storage. N=0 is a count-only MEM operation. `truncated` indicates whether any
matches were omitted from the vector.

## Statistics

`MemSearchStatistics` reports:

- root/fallback lookup calls;
- binary and learned lookup counts;
- suffix-link attempts, successes, and fallbacks;
- positions where the previous interval was empty; and
- nested exact-search comparison, prediction, and window statistics.

Statistics help explain why an optimization changes end-to-end time. They are
caller-owned mutable output and must not be shared unsafely between threads.

## MUMmer4 comparison boundary

MUMmer4 is used only as a black-box correctness comparator on compatible
ACGT-only datasets. Its Artistic-2.0 source is not copied or linked into the
MIT library. External process startup, index load, output, and parsing must be
kept separate from sufkit in-process query-only measurements.
