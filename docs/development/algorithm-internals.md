# Algorithm internals and invariants

This page is the contributor contract for index construction and query
implementation. It combines algorithm behavior with the invariants that every
backend and optimization must preserve. Pseudocode uses half-open intervals
even when a third-party API does not.

## Global invariants

- Stored biological symbols are A/C/G/T/N encoded as 2–6. Separator is 1;
  construction input contains no zero; the logical indexed text ends with
  exactly one zero sentinel.
- N, separator, sentinel, query hard breaks, and contig boundaries cannot be
  crossed by a public match.
- Constructor, construction width, storage width, resource profile, sampling,
  lookup algorithm, batching, and acceleration may change work and layout,
  never normalized results.
- Explicit unavailable capabilities fail with `unsupported_backend`; damaged
  persisted data is not silently ignored.
- Built or loaded indexes are immutable. Const queries need no shared mutable
  cache, lock, atomic counter, or memory fence.

For K=1, SA is a complete permutation of `[0,n)`. For K>1, it is the sorted
permutation of positions divisible by K and has `ceil(n/K)` rows. ISA is the
inverse of the stored SA domain, `LCP[0]=0`, and CHILD/PWL row references must
stay within that domain. CaPS and divsufsort must produce the same stored
suffix order and public results.

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

Construction width is not storage width. After construction and optional
sampling, resolve storage from the complete logical symbol count and profile:

```text
Fast auto:       native32 if max_position<=UINT32_MAX, else native64
Low-memory auto: native32 -> split40 -> split48 -> native64
```

An explicit storage width must represent `symbol_count-1`. Validate every
coordinate, the complete/sampled permutation, and any ISA inverse relation
before releasing a wider build representation. Backend ID and outer header
width continue to describe the constructor, not the codec.

On Linux/WSL, divsufsort64 writes into a page-backed live `int64_t` array.
Down-packing copies typed values forward into ordinary vector planes and uses
`madvise(MADV_DONTNEED)` only on fully consumed source pages. This keeps RSS
near the 8-byte source plane without overlapping object lifetimes. The
non-POSIX fallback is correct but can temporarily retain source plus target;
do not apply the Linux peak-memory claim to that fallback.

After any constructor, common order/result tests apply. CaPS supplies its
native-width merge-built LCP only while a raw row array is needed for CHILD;
the common non-CHILD path avoids copying that extra plane and builds compact
LCP after the CaPS object is released. These backend phase results must
satisfy the same persisted invariants.

CaPS itself still allocates complete SA, complete LCP, same-width SA/LCP work
arrays, and `p`-dependent partition tables. Because the public CaPS accessors
return borrowed pointers, the wrapper cannot adopt those allocations and must
copy the final SA while the object remains alive. Low-memory must therefore be
described as a final-layout policy, not a low-peak CaPS construction mode.

## Sampling, ISA, and LCP

For sampling rate K, retain complete-SA rows whose suffix position is divisible
by K, preserving lexicographic order. Build ISA by assigning row to
`suffix_position/K`. Parallel chunks may write disjoint positions.

The generalized Kasai scan visits sampled positions `p=sample*K`. For row r>0,
compare against `SA[r-1]` from the previous retained common-prefix length,
store `LCP[r]`, then reduce retained length by K with saturation at zero. K=1
is ordinary Kasai. Hard encoded symbols participate in suffix ordering but right-maximal exact match
code treats them as boundaries.

CaPS produces complete LCP during merging. When K>1, the LCP between adjacent
retained rows is the minimum complete LCP over the intervening row interval;
this equals the common prefix of the retained suffix pair.

Raw LCP uses 32 bits when `text_symbols-1` fits `uint32_t`, otherwise 64. The
byte-coded alternative has these invariants:

- one primary byte per stored row;
- values 0..254 are stored directly;
- 255 marks a long value covered by a text-ordered anchor;
- each anchor records `(sampled_text_position, LCP_value)` in 32/64 bits;
- consecutive marked positions whose value decreases by exactly K share the
  same anchor;
- a 4096-symbol guide is derived after build/load and is not persisted.

`Exact(row,suffix_position)` must reconstruct the raw LCP value. For
`AtLeast(...,target<=255)`, the marker proves the predicate without anchor
lookup. Low-memory retains byte coding so the primary plane never expands to a
full-width array; Fast retains raw LCP after byte coding failed its query
regression gate. Validation checks `LCP[0]=0`, every
decoded adjacent-suffix bound, every anchor's ownership, and generalized
sampled-run decrement.

Exact sampled recovery searches pattern suffixes for every residue r in
`[0,K)`, maps each candidate sampled position back by r, and verifies the
omitted prefix and contig boundary. If pattern length is below K, direct contig
scan is the correctness fallback. `EqualRange` is rejected for K>1.

Sampled right-maximal search uses anchor length `min_length-K+1`, iterates query
residue classes in K steps, maps sampled anchors to candidate starts, extends
right to maximality, and rejects duplicate anchors for the same output match.

## CHILD table

CHILD is built deterministically from logical LCP values, independent of raw
or byte-coded storage, with the Abouelhoda-style linear
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
Store anchor key (`uint64_t`) and row using the resolved coordinate codec; the
row domain includes the terminal one-past anchor and may therefore promote
independently from SA positions. Fill empty buckets with neighboring monotonic
anchors and append the terminal domain anchor.

Interpolate between adjacent anchors with checked unsigned arithmetic and a
wide integer intermediate. Floating point is not part of persisted/query
semantics.

## Fast exact-prefix directory

Complete Fast indexes that retain ISA and do not contain a Sapling model may
derive a canonical-DNA prefix directory. Construction scans each contig with a
rolling 2-bit key, resets at N and every contig boundary, maps valid text
positions through ISA, and verifies the inverse SA mapping. It therefore never
creates a key that crosses N, separator, sentinel, or contig boundaries.

For native32 row domains, `(begin,end)` is packed into one 64-bit word so one
dependent load obtains the complete interval. Wider row domains retain two
64-bit endpoint planes. The largest `k<=10` fitting within 25% of the existing
resident core is selected; references below the minimum size do not pay the
fixed table cost.

A lookup result is an exact interval, not a prediction. If the pattern is
longer than k, LCP-aware lower/upper-bound refinement starts with k already
matched and remains inside the returned interval. Short or non-canonical
prefixes fall back to the ordinary path. The directory is immutable,
thread-safe, derived after build/load, excluded from `.sufidx`, and included in
runtime resident-memory accounting.

This uses the same high-level latency-reduction idea as
[MiniBWA](https://github.com/lh3/minibwa)'s exact k-mer interval cache, but the
implementation is independent and specialized for sufkit's standalone SA,
generalized-contig boundaries, and exact range API. MiniBWA's compact
request-state and prefetch design also informs performance experiments; no
MiniBWA source is copied or linked into sufkit.

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

## Right-maximal exact match baseline

Split a query orientation into maximal canonical A/C/G/T runs. For each start
that can support `min_length`:

1. find the min-length prefix range;
2. compute right LCE for candidate suffixes;
3. extend and verify the current right boundary;
4. emit valid right-maximal reference/query pairs; and
5. enforce contig and query-run bounds.

Avoid constructing suffix substrings. Duplicate generation within a path must
be normalized before public vector comparison.

## LCP and CHILD right-maximal exact match paths

LCP mode uses adjacent common-prefix information to expand/enumerate candidate
intervals without repeating all suffix characters. CHILD mode navigates a
query start top-down from the root. Both must call the same exactness,
right-maximality, and coordinate emission logic as baseline.

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

## Right maximality and coordinates

A candidate `(r,q,l)` is valid only if all l canonical symbols equal. It is
right maximal when either side is at its right boundary or the following
symbols differ. Reference contig bounds and query canonical-run bounds are the
authority. No left-maximality guarantee is made by this API.

Consequently, callers that require the left-side guarantee must use the
separate `Mem*` API, not infer it from the legacy contract.

Reverse orientation output position is:

```text
original_query_length - (rc_position + match_length)
```

All five algorithm modes, all lookup algorithms, constructors, widths, save/
load states, and thread counts must produce the same normalized tuple set.

## Formal MEM candidate recovery

The implementation is clean-room. MUMmer4 4.0.1 is used only as an external
behavioral reference and differential executable; no MUMmer source is copied,
linked, or modified. The audited local source snapshot had SHA-256
`035f5ff6ce1bba67ef83b2b8538690ec81a40bff405a1b64ec6a6681200dda13`
for `sparseSA.hpp` and
`7488df12f9671c07e3e2767454aeba99b5240c1ddb9b02251597ff46bab81280`
for `sparseSA.cpp`.

MEM search shares the verified interval traversal and LCE machinery but has an
independent two-sided emission policy. Let K be the SA sampling rate, s the
skip multiplier, W=`s*K`, L the minimum length, and T=`L-W+1`. Each canonical
query run is visited in every reference residue class, anchors advance by W,
and the right search only needs to reach T before bounded left recovery.

For K=1 and s=1, `IsLeftMaximal()` is one guarded predecessor comparison. For
sampled search, `RecoverSampledMemStart()` compares at most W preceding bases
without crossing a contig, N, separator, sentinel, or query hard break. If W
bases all match, the preceding anchor owns the MEM; suppressing the current
candidate prevents duplicate residue/anchor emission. Otherwise the recovered
start is left maximal, and `left+right>=L` is required before emission.

Suffix-link reuse shifts both interval endpoints by W in text coordinates,
maps them with ISA, and expands through LCP while the interval depth remains
valid. Empty intervals, invalid sampled residues, insufficient depth, hard
breaks, and failed verification reset to a root lookup. This optimization may
reduce work but is never a correctness precondition.

The baseline path searches the T-prefix interval and computes a right LCE per
candidate row. LCP paths reuse adjacent prefix information to obtain the same
set; CHILD/full remain explicit. Anchor/residue ownership guarantees that the
streaming kernel emits each directional tuple once. Unlimited vector results
retain a final sort/unique correctness guard; bounded results can therefore
count exactly while keeping only an N-element heap. Independent brute-force
tests, rather than MUMmer4 source, define the public result set.

## Reference-MAM uniqueness

Reference-MAM is supported only for a complete SA. After a two-sided MEM is
identified, its full matched string must have a complete-SA interval of size
one across all contigs. Query occurrence count is intentionally ignored. This
matches MUMmer4 `-mumreference`, not strict MUM semantics.

MEM/MAM do not call SeqPro and add no dedicated persisted section. They use the
encoded reference text and contig metadata already inside `.sufidx`; old
1.0-1.3 SA files therefore require no conversion.

## Persistence and backend identities

- Backend and section IDs are permanent and are never reused for a different
  payload interpretation.
- The outer container is little-endian, bounds every section, validates CRCs
  and legal combinations, and publishes only a self-validated temporary file.
- Format 1.3 sampling metadata must agree with row counts, stored suffixes,
  auxiliaries, and learned anchors. Format 1.4 adds independent coordinate/LCP
  codecs and the resource profile; codec domains, plane sizes, counts, decoded
  values, and trailing bytes are strictly validated. Older supported formats
  imply their documented defaults.
- FM payload loading requires the recorded SDSL 3.0.3 type and version. sufkit
  does not implement alternative C/Occ/LF/rank/select structures.
- Loading is self-contained and never depends on the original FASTA path.

## Current low-level design

The private x86_64 implementation is compiled for SSE4.2 and POPCNT without
exporting that flag to consumers. Its comparison/LCE kernel uses a short
scalar prefix, bounded 16-byte unaligned loads, movemask/first-mismatch
selection, and scalar tails. The scalar and SIMD paths must agree on order,
logical matched length, and every boundary case; no load may cross a validated
buffer extent.

SA, ISA, CHILD, and learned rows use private native32, split40, split48, or
native64 storage selected by their representable domain and profile. Split
storage is structure-of-arrays (`low32[]` plus `high8[]` or `high16[]`), never
a padded proxy struct. Random probes decode one pair; sequential consumers use
`DecodeSpan`. LCP independently uses raw32/raw64 or byte coding. Values are
promoted to public `uint64_t` only at the boundary, and variant dispatch
belongs outside typed hot loops.

Fast automatically uses native storage above 32 bits until packed access has
passed the per-workload regression gate. Low-memory intentionally trades ISA
suffix-link reuse for narrow coordinates and LCP traversal. Query decode
workspaces are per call; the immutable index has no adaptive shared cache.

Query workspaces are call-owned. FM batch uses structure-of-arrays state and
an active-lane list; SA/right-maximal queries use non-owning encoded views and
compact global coordinates until final mapping. No workspace or statistics
object is stored in the immutable index, and no large thread-local buffer or
library-global thread pool is allowed.

Performance changes are accepted only after scalar/optimized, constructor,
width, sampling, strand, and save/load checksums agree. A target workload must
improve materially without a stable regression in primary non-target paths.
Benchmark instrumentation is run separately from timed passes and cannot
steer query behavior.
