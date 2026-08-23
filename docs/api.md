# API and sequence semantics

## Reference input

`GenomeReference::from_fasta` reads plain or gzip FASTA through kseq and zlib.
`from_records` accepts in-memory records.  Names must be non-empty and unique;
the reference must contain at least one non-empty sequence.

Reference A/C/G/T is upper-cased.  N, U, other IUPAC codes, and every other
symbol are converted to N.  Query patterns are stricter: after upper-casing,
every character must be A/C/G/T.

MEM queries differ deliberately: lowercase A/C/G/T is accepted and every
other query symbol is a hard break. A MEM never crosses a query hard break,
reference N, contig separator, or sentinel.

Contigs are encoded in input order and separated by byte 1.  A pattern cannot
cross a separator or an N.  Sequence IDs and positions are zero-based.

## Query results

`equal_range` returns a half-open index-row range.  Empty results are `[0,0)`;
no insertion-point meaning is promised for an empty SA or SDSL range.

`count` and `locate` support:

- `forward`: the supplied pattern;
- `reverse_complement`: only its reverse complement;
- `both`: both orientations.

Both-strand palindromes are returned once per coordinate with strand `both`.
Other results are ordered by sequence ID, position, length, and strand.
`max_hits` selects the first N entries in that order with O(N) temporary hit
storage; `total_hits` remains the complete count and `truncated` reports the
distinction.

Built and loaded indexes are immutable.  Const queries can be issued
concurrently.  Builders are not documented as concurrent operations.

## MEM results

`SuffixArray::for_each_mem` synchronously streams every reference/query MEM to
the callback without promising enumeration order. `find_mems` is the bounded
convenience API. It sorts by query position, sequence ID, reference position,
length, and strand; `total_matches` is complete even when `max_matches`
truncates the retained vector.

MEM coordinates are zero-based. Reference positions are contig-local. Reverse
matches report query positions in the original forward query coordinate
system. Forward and reverse-complement MEMs remain orientation-distinct,
including for reverse-complement palindromes.

The default suffix-array build acceleration is `lcp_suffix_link`
(SA+ISA+LCP). CHILD remains available through `lcp_child` and `full`, but it is
never selected automatically. An explicitly requested MEM algorithm must be supported by
the loaded auxiliary sections; otherwise the query fails with
`unsupported_backend`. MEM `auto_select` chooses suffix-link, then LCP, then
baseline. It does not select CHILD or full.

## Learned suffix-array lookup

`LearnedSaOptions` optionally builds a Sapling-style piecewise-linear model.
It is independent of `SaAcceleration`: a caller may combine it with SA only,
SA+ISA+LCP, or an explicitly requested CHILD index. The default is disabled,
with `k=20` and a model budget of 100 basis points (1% of the SA payload).

Exact SA queries accept `auto_select`, `binary`, `lcp_binary`, `sapling_pwl`,
or explicit `child`. Auto selects PWL only when the index contains a learned
section and the pattern is at least the model k; otherwise it selects ordinary
binary search. Auto never selects CHILD. Explicitly requesting missing PWL or
CHILD data throws `unsupported_backend`.

PWL prediction is only a search hint. Exponential bracketing and local
LCP-aware binary search recover the exact lower and upper bounds. A poor
prediction can therefore increase work but cannot change the result. Patterns
shorter than k use full binary search.

`MemOptions::lookup_algorithm` controls initialization and fallback lookup in
the suffix-link MEM path. `MemSearchStatistics` reports suffix-link success,
empty-previous states, binary/PWL lookup counts, character comparisons,
prediction error, local windows, and full-search fallbacks.
Build and query statistics pointers are optional caller-owned outputs. Do not
share the same statistics object between concurrent calls; the immutable index
itself remains safe for concurrent const queries.

## Errors

Public operations throw `sufkit::Error` with one of:

- `invalid_input`
- `io_error`
- `unsupported_backend`
- `corrupt_index`
- `version_mismatch`
- `build_failure`

Unavailable reserved backends fail explicitly and never fall back silently.
