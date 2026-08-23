# Public API contracts

Generated Doxygen pages document each public declaration. This page collects
cross-cutting contracts that are easy to miss when reading one method.

## Object model

`GenomeReference`, `SuffixArray`, and `FmIndex` are move-only PIMPL classes.
They own their normalized metadata or index payload. No public header exposes
third-party implementation types.

Build and load return fully initialized objects or throw. There is no partially
usable public index. A built or loaded index is immutable; const query methods
may be called concurrently.

## Inputs

- Reference FASTA and records require non-empty unique names and non-empty
  sequences.
- Reference symbols normalize to A/C/G/T/N.
- Exact patterns must be non-empty A/C/G/T after upper-casing.
- Right-maximal query non-ACGT symbols are hard breaks.
- `min_length=0`, `threads=0`, invalid widths, budgets, batch widths, and
  incompatible explicit algorithms are rejected.
- `sampling_rate=0` is rejected; sampled right-maximal search also rejects
  `min_length < K`.

## Ranges and coordinates

`SuffixRange` is half-open and `size()` is `end-begin`. Empty searches return
`[0,0)` and do not expose a lexical insertion point.

For complete standalone SA, `equal_range` returns one SA interval. For sampled
SA (`sampling_rate>1`) complete exact results span residue-specific intervals,
so `equal_range` explicitly returns `unsupported_backend`; use `count` or
`locate` instead.

`Match::position` and `RightMaximalMatch::reference_position` are zero-based,
contig-local positions. `RightMaximalMatch::query_position` is zero-based in the
original forward query. Sequence IDs follow reference input order.

## Ordering and truncation

Exact vector results are ordered by sequence ID, position, length, and strand.
Both-strand palindromic exact hits are merged with strand `both`.

Right-maximal results are ordered by query position, sequence ID, reference
position, length, and strand. Forward and reverse results remain distinct.

`RightMaximalMatch` guarantees exactness and non-extendability on the right. It
does not guarantee left maximality and is not a MEM contract.

`max_hits` and `max_matches` bound retained output, not the complete count.
`total_hits`/`total_matches` remain accurate and `truncated` reports omission.
Right-maximal N=0 is therefore count-only but still performs full enumeration.

## Callbacks and statistics

`RightMaximalCallback` executes synchronously on the caller thread. Exceptions
propagate unchanged. Streaming order is deliberately unspecified.

Build and query statistics pointers are optional caller-owned mutable outputs.
They are reset/populated by the call and are not part of index immutability.
Use separate objects for concurrent operations.

## Errors

Public failures use `sufkit::Error`:

| `ErrorCode` | Meaning |
|---|---|
| `invalid_input` | Invalid FASTA, pattern, option, size, or coordinate |
| `io_error` | Open, read, write, rename, or stream failure |
| `unsupported_backend` | Recognized capability is absent or not built |
| `corrupt_index` | Container or payload violates persisted invariants |
| `version_mismatch` | Format/SDSL version cannot be interpreted safely |
| `build_failure` | Backend construction or benchmark correctness failure |

Callers should branch on the error code rather than parsing diagnostic text.

## Backend discovery

`available_sa_backends()` and `available_fm_backends()` report compiled
availability and implementation signatures. A reserved name can be reported
unavailable. Availability is runtime-visible build metadata, not permission
to silently replace an explicit request.

## Sampled standalone SA

`SuffixArrayBuildOptions::sampling_rate=K` keeps text positions divisible by
K. `SuffixArray::sampling_rate()` reports the effective K. `IndexInfo` keeps
logical text length and stored row count separate:

- `text_symbols`: encoded text plus sentinel;
- `suffix_count`: retained SA rows;
- `sa_sampling_rate`: K.

`suffix_at(row)` addresses `[0,suffix_count)`. Exact count/locate preserve
complete results through residue recovery; patterns shorter than K use a
correct per-contig scan. Right-maximal search requires `min_length>=K`.

## Complexity notes

Complexity depends on backend and result size. High-level guidance:

- SA binary range: O(m log n) simple character bound;
- LCP-aware/PWL/CHILD: same result with workload-dependent comparison savings;
- FM range: O(m) rank transitions at the abstract level;
- locate: at least O(z) result work plus backend recovery;
- Complete or sampled generalized Kasai LCP and ISA: O(n) construction work,
  with O(ceil(n/K)) persisted rows for sampling rate K;
- Right-maximal search: depends on query positions, interval reuse success,
  repeats, and output size; use statistics and representative queries.

No public timing or memory constant is guaranteed across compiler, hardware,
backend revision, or genome distribution.
