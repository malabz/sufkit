# Internal invariants

These properties are compatibility and correctness requirements, not
implementation suggestions.

## Reference and text

1. Stored reference symbols are exactly A/C/G/T/N encoded as 2–6.
2. Separator is 1 and occurs after every contig in SDSL construction input.
3. Construction input contains no zero.
4. The logical indexed text contains exactly one zero sentinel at its end.
5. divsufsort, CaPS, and SDSL sort the same logical text.
6. Contig offsets, lengths, ambiguous counts, total bases, and text symbols
   agree exactly.
7. N, separator, and sentinel never become public exact matches or crossable
   MEM symbols.

## SA and auxiliary structures

1. SA is a complete permutation of `[0,n)`.
2. Adjacent SA suffixes are lexicographically non-decreasing.
3. `ISA[SA[row]]=row` for every row.
4. `LCP[0]=0`; each remaining LCP is bounded by both adjacent suffix lengths.
5. CHILD indices are in range and reproduce the deterministic table derived
   from persisted LCP.
6. Legal section combinations are exactly those represented by
   `SaAcceleration`.
7. Constructor choice cannot alter SA order, auxiliary semantics, exact
   results, or MEM results.
8. CaPS internal LCP is not persisted or substituted for the common Kasai
   implementation without a separate compatibility decision.

## Learned model

1. PWL model identity, k, bucket bits, width, anchor count, fingerprint, and
   memory budget are validated.
2. Anchor count is `2^bucket_bits+1` and allocation arithmetic cannot overflow.
3. Keys and rows are monotonic; terminal key/row cover the complete domain.
4. Interpolation uses checked integer arithmetic.
5. Prediction is never trusted as a result boundary.
6. Exponential bracketing/local search or a full binary fallback establishes
   the exact range.
7. Missing or damaged model data is not silently ignored on load.

## Exact search

1. Public ranges are half-open.
2. Empty results are `[0,0)`.
3. All algorithms produce identical interval, count, and sorted locate output.
4. Explicit unavailable algorithms fail with `unsupported_backend`.
5. Auto search never chooses CHILD.
6. Both-strand exact palindromes are returned once per coordinate with strand
   `both`.
7. `total_hits` is complete even when retained hits are bounded.

## MEM

1. Every emitted match has length at least positive `min_length`.
2. Every emitted match is exact and cannot extend jointly left or right.
3. Matches do not cross reference/query hard boundaries.
4. Reverse query coordinates map to the original forward query.
5. Forward/reverse results remain orientation-distinct.
6. Baseline, LCP, CHILD, suffix-link, and full produce the same normalized
   multiset.
7. Invalid reuse always returns to a correct root state.
8. `total_matches` is complete under output retention limits.

## FM-index

1. Each backend ID maps permanently to one SDSL template/signature.
2. SDSL owns construction, rank-backed backward search, SA sampling, locate,
   and native serialization.
3. sufkit does not implement private C/Occ/LF/rank/select alternatives.
4. CSA size is encoded input size plus one sentinel.
5. FM payload load requires the exact recorded SDSL version.
6. Batch results preserve input order and scalar semantics for every strand.
7. Batch width affects scheduling only, not results.

## Persistence

1. Backend and section IDs are never reused.
2. Format major changes only for incompatible outer interpretation; minor
   increases when readers need new optional/required sections.
3. Required unknown data, CRC errors, bounds errors, overlap, integer overflow,
   and illegal combinations are rejected.
4. Backend payload reads are section-bounded.
5. Save is non-overwriting unless explicit and publishes only a completely
   validated temporary file.
6. Loading never depends on the original FASTA path.

## API and dependencies

1. Public headers expose only standard C++ and sufkit types.
2. Indexes are move-only PIMPL objects.
3. Built/loaded indexes are immutable for const concurrent queries.
4. Statistics objects remain caller-owned mutable output.
5. Third-party exceptions are converted into an appropriate `sufkit::Error`
   unless the documented MEM callback itself throws.
6. Reserved or disabled features never silently route to another backend.

## Benchmark and documentation

1. Performance reporting starts only after complete-count and coordinate or
   MEM checksum agreement.
2. Worker process boundaries and timing scope are recorded.
3. External-process and in-process timings are labeled separately.
4. Every persistent performance claim names commit, environment, dataset,
   seed, fingerprints, repetitions, aggregation, and limitations.
5. A backend/API/format behavior change updates its documentation and
   changelog status in the same contribution.
