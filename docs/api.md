# API and sequence semantics

## Reference input

`GenomeReference::from_fasta` reads plain or gzip FASTA through kseq and zlib.
`from_records` accepts in-memory records.  Names must be non-empty and unique;
the reference must contain at least one non-empty sequence.

Reference A/C/G/T is upper-cased.  N, U, other IUPAC codes, and every other
symbol are converted to N.  Query patterns are stricter: after upper-casing,
every character must be A/C/G/T.

Contigs are encoded in input order and separated by byte 1.  A pattern cannot
cross a separator or an N.  Sequence IDs and positions are zero-based.

## Query results

`equal_range` returns a half-open index-row range.  Empty results are `[0,0)`;
no insertion-point meaning is promised for an empty SDSL range.

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

## Errors

Public operations throw `sufkit::Error` with one of:

- `invalid_input`
- `io_error`
- `unsupported_backend`
- `corrupt_index`
- `version_mismatch`
- `build_failure`

Unavailable reserved backends fail explicitly and never fall back silently.
