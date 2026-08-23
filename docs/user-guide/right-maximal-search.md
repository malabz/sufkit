# Right-maximal exact match search

The current suffix-array query reports exact reference/query matches that are
maximal on the **right**. It does not yet guarantee left maximality and must not
be called maximal exact match (MEM) search.

For a match `(reference_start, query_start, length)`, the guaranteed property
is:

```text
reference[reference_start .. reference_start+length)
    == query[query_start .. query_start+length)
```

and either the right boundary is reached or the next canonical symbols differ.
No public contract is currently made about whether equal symbols exist
immediately to the left.

Therefore:

```text
true MEM  => right-maximal exact match
right-maximal exact match =/> true MEM
```

In explanatory prose these results may be called “right-maximal MEM
candidates”. Public API names use `RightMaximalMatch`, which states exactly
what the implementation guarantees. `MemMatch` and MEM-oriented method names
are reserved for a later implementation that verifies both sides.

## Public API

The principal types and methods are:

```cpp
sufkit::RightMaximalOptions
sufkit::RightMaximalMatch
sufkit::RightMaximalResult
sufkit::RightMaximalSearchAlgorithm
sufkit::RightMaximalSearchStatistics

SuffixArray::find_right_maximal_matches()
SuffixArray::for_each_right_maximal_match()
```

The CLI command is:

```bash
sufkit right-maximal \
  --index reference.sufidx \
  --query queries.fa.gz \
  --min-length 20
```

## Sequence and coordinate semantics

- Lowercase A/C/G/T is normalized.
- Every other query symbol is a hard break.
- Reference N, contig separators, the sentinel, and query hard breaks cannot
  be crossed.
- Reference positions are zero-based and contig-local.
- Query positions are zero-based in the original forward query.
- Reverse-complement matches are transformed back with
  `query_length - (rc_position + length)`.
- Forward and reverse results remain distinct, including palindromic queries.

An empty query, an all-break query, or a query shorter than `min_length`
returns no matches. `min_length=0` is invalid.

## Algorithms

| Mode | Persisted requirement | Main strategy |
|---|---|---|
| `baseline` | SA | Start each query position from a root SA lookup |
| `lcp` | SA+LCP | Reuse adjacent-suffix prefix information |
| `child` | SA+LCP+CHILD | Start each position with ESA top-down traversal |
| `suffix_link` | SA+ISA+LCP | Reuse an interval after deleting a query character |
| `full` | SA+ISA+LCP+CHILD | Combine suffix-link reuse and explicit CHILD navigation |

All modes must return the same normalized right-maximal result set. Their
difference is interval discovery and reuse, not public match semantics.

`auto_select` chooses suffix-link, then LCP, then baseline according to stored
data. It does not automatically choose CHILD or full.

`RightMaximalOptions::lookup_algorithm` controls root initialization and
suffix-link fallback. Binary is always available; learned PWL requires a model.
Explicit unavailable choices throw `unsupported_backend`.

## Sampled standalone SA

For sampling rate K>1, the operation requires `min_length>=K`. The query is
processed by residue class and candidate anchors are recovered from sampled
reference positions. The output guarantee remains right maximality only.

The sampled and complete layouts are required to return the same normalized
right-maximal result set. This is not a claim that either result is a MEM set.

## Streaming and bounded vector APIs

`for_each_right_maximal_match()` invokes a callback synchronously for every
match. It has low result-storage overhead but does not promise enumeration
order across algorithms. Callback exceptions propagate unchanged.

`find_right_maximal_matches()` returns matches ordered by:

```text
query_position, sequence_id, reference_position, length, strand
```

When `max_matches=N`, the implementation still traverses the full result to
compute `total_matches`, but retains only the sorted first N. N=0 is count-only.
`truncated` indicates whether vector entries were omitted.

## Statistics

`RightMaximalSearchStatistics` reports root/fallback lookups, binary and
learned lookups, suffix-link attempts/successes/fallbacks, previous-empty
positions, and nested exact-search work counters. Statistics are caller-owned
mutable output and must not be shared unsafely between threads.

## Boundary with future MEM support

Future MEM functionality must add an independently tested left-maximality
condition while preserving exactness, right maximality, contig/hard-break
boundaries, strand mapping, sorting, and persistence compatibility. It should
use MEM-specific public names only after a brute-force two-sided oracle passes.

MUMmer4 produces MEMs, not generic right-maximal candidates. Historical runs
where its normalized output happened to equal sufkit's output are useful
dataset-specific regression evidence, but do not prove that the current API
implements the MEM definition.
