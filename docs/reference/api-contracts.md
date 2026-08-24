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

### Reference normalization

Plain and gzip FASTA are parsed with kseq/zlib. Record order defines
zero-based `SequenceId`. The first header token is the unique name and the
remaining header text is the description. Zero records, empty or duplicate
names, and empty sequences are invalid. `GenomeReference::FromRecords()` uses
the same validation.

Reference A/C/G/T is upper-cased. N, U, other IUPAC symbols, and every other
byte normalize to N. Ambiguous counts are recorded per contig and globally.
The normalized encoded buffer, not the original ASCII, is authoritative.

The logical alphabet is:

```text
0 sentinel, 1 separator, 2 A, 3 C, 4 G, 5 T, 6 N
```

One separator follows every contig. SDSL input contains no zero and
`construct_im(...,1)` appends the unique sentinel; standalone SA constructors
receive the same logical text with that zero appended explicitly. N,
separator, and sentinel are unmatchable boundaries.

## Ranges and coordinates

`SuffixRange` is half-open and `Size()` is `end-begin`. Empty searches return
`[0,0)` and do not expose a lexical insertion point.

For complete standalone SA, `EqualRange` returns one SA interval. For sampled
SA (`sampling_rate>1`) complete exact results span residue-specific intervals,
so `EqualRange` explicitly returns `unsupported_backend`; use `Count` or
`Locate` instead.

`Match::position` and `RightMaximalMatch::reference_position` are zero-based,
contig-local positions. `RightMaximalMatch::query_position` is zero-based in the
original forward query. Sequence IDs follow reference input order.

`SequenceInfo::global_offset` exposes the logical-text mapping for inspection,
but ordinary applications should use contig-local result positions. Every
located extent is checked against its contig before it becomes public. CLI
exact-search end coordinates are exclusive.

The normalized-content FNV-1a-64 fingerprint supports deterministic dataset
identity and learned-model consistency. It is not a cryptographic integrity
digest.

## Ordering and truncation

Exact vector results are ordered by sequence ID, position, length, and strand.
Both-strand palindromic exact hits are merged with strand `Strand::kBoth`.

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

| C++ value | `ToString()` value | Meaning |
|---|---|---|
| `ErrorCode::kInvalidInput` | `invalid_input` | Invalid FASTA, pattern, option, size, or coordinate |
| `ErrorCode::kIoError` | `io_error` | Open, read, write, rename, or stream failure |
| `ErrorCode::kUnsupportedBackend` | `unsupported_backend` | Recognized capability is absent or not built |
| `ErrorCode::kCorruptIndex` | `corrupt_index` | Container or payload violates persisted invariants |
| `ErrorCode::kVersionMismatch` | `version_mismatch` | Format/SDSL version cannot be interpreted safely |
| `ErrorCode::kBuildFailure` | `build_failure` | Backend construction or benchmark correctness failure |

Callers should branch on the error code rather than parsing diagnostic text.

## Backend discovery

`AvailableSaBackends()` and `AvailableFmBackends()` report compiled
availability and implementation signatures. A reserved name can be reported
unavailable. Availability is runtime-visible build metadata, not permission
to silently replace an explicit request.

## Sampled standalone SA

`SuffixArrayBuildOptions::sampling_rate=K` keeps text positions divisible by
K. `SuffixArray::SamplingRate()` reports the effective K. `IndexInfo` keeps
logical text length and stored row count separate:

- `text_symbols`: encoded text plus sentinel;
- `suffix_count`: retained SA rows;
- `sa_sampling_rate`: K.

`SuffixAt(row)` addresses `[0,suffix_count)`. Exact `Count`/`Locate` preserve
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
