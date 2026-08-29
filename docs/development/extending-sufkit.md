# Extending sufkit

Start with [architecture](architecture.md) and
[algorithm internals](algorithm-internals.md), and follow the
[C++ style guide](cpp-style.md). The checklists below prevent a local
implementation detail from becoming an incompatible public format.

## Add a suffix-array constructor

1. Decide whether it is a new public `SaBackend` or private implementation of
   an existing permanent backend.
2. Define availability and auto-selection policy independently of explicit
   selection. Keep routing deterministic and inspectable.
3. Define construction widths and exact input-size limits independently from
   final coordinate codecs.
4. Keep third-party headers in private source and record fixed revision,
   license, and patches.
5. Feed the common encoded text including one sentinel and produce a complete
   generic SA permutation before any optional text-position compaction.
6. Route into the common ISA/LCP/CHILD/PWL semantics. A backend may return
   validated ISA/LCP phase data to avoid redundant work, but must not fork
   persisted or query semantics.
7. Assign permanent stored backend IDs/signatures if constructor provenance is
   persisted. Confirm whether old builds can read the generic payload.
8. Add availability descriptors, CLI values, inspection, save/load, disabled
   build behavior, and construction/storage-width tests.
9. Differentially compare SA order, exact results, right-maximal exact match results, deterministic
   serialization, and concurrency against divsufsort.
10. Add an isolated constructor benchmark with time, CPU, peak RSS, checksum,
    threads, and explicit auto-routing evidence.
11. Verify K=1 and sampled K>1 paths, including backend-provided LCP, against
    the same exact/right-maximal exact match checksums.

CaPS is the reference example: optional compile-time availability, private
Parlay scheduler, stable IDs 3/4, generic payload, and a conservative 1 GiB
auto threshold.

## Extend coordinate or LCP storage

1. Keep constructor provenance (`SaBackend` and `CoordinateWidth`) independent
   from resident/persisted codec selection.
2. Use `text_symbols-1` for representability. Include every contig separator
   and the sentinel in checked symbol-count arithmetic.
3. Never use a padded packed-coordinate struct. New narrow layouts require
   explicit contiguous planes and exact serialized plane lengths.
4. Provide checked scalar access and contiguous-span decoding. Lift variant
   dispatch outside query hot loops and do not add a shared mutable cache.
5. Validate every down-packed value and complete/sampled permutation before
   releasing the wider construction buffer.
6. Keep Fast conservative: a packed layout must pass the documented query
   regression gate before automatic selection. Low-memory may choose the
   narrowest correct layout.
7. A new persisted codec needs a permanent codec ID, format-minor decision,
   unknown-codec behavior, exact size checks, corruption fixtures, inspect
   fields, and legacy-file tests.
8. LCP encodings must implement exact value reconstruction and threshold
   queries over the same logical LCP. Compare decoded values, CHILD tables,
   exact/MEM/MAM results, and save/load checksums against raw LCP.
9. Report logical payload bytes separately from RSS/PSS and construction peak.
   Do not claim large-reference or MUMmer4 superiority from formulas alone.

## Add an FM backend

1. Freeze one exact SDSL template, sampling densities, public enum value,
   stored ID, name, and signature.
2. Instantiate it only in `src/fm_index.cpp` and add it to private variant
   dispatch for build/range/locate/serialize/load/size.
3. Do not expose SDSL types or add custom C/Occ/LF/rank/select substitutes.
4. Validate construction size/alphabet and exact SDSL version on load.
5. Reject recognized but unimplemented IDs explicitly.
6. Test scalar range/count/locate, strands, palindromes, batch widths,
   save/load, corruption, inspection, and backend cross-equivalence.
7. Benchmark build, load, size, count, locate, batch behavior, and peak RSS.
8. Update backend, compatibility, format, API, CLI, changelog, and benchmark
   documentation.

Changing a published template or sampling density is a new backend, not a
compatible edit.

## Add an exact-search algorithm

1. Define required stored capability separately from constructor backend.
2. Add an explicit selector and a conservative auto-selection rule only after
   benchmark evidence.
3. Use public half-open ranges and normalize empty results.
4. Treat predictions/navigation as hints and provide a correctness-preserving
   fallback.
5. Ensure count and locate share the same exact range.
6. Compare interval and sorted coordinates against binary search over known,
   repetitive, boundary, no-hit, and randomized patterns.
7. Check save/load and concurrent const calls.
8. Add work counters that explain the intended optimization without making
   the index mutable.

## Add a right-maximal exact match algorithm

1. Preserve exactness, right maximality, hard-break, contig, strand, and
   coordinate contracts; do not imply left maximality.
2. Declare required `SaAcceleration` data and explicit unavailable behavior.
3. Share candidate verification and result emission with existing paths.
4. Reset safely when interval reuse or navigation cannot be proved valid.
5. Compare the complete normalized multiset against the brute-force oracle and
   every existing path for random small references and queries.
6. Test streaming callback, vector ordering, retention N=0/1/many, callback
   exceptions, both strands, and concurrency.
7. Use MUMmer4 only as a black-box dataset-specific comparison. It emits MEMs,
   so equality on one dataset does not establish semantic equivalence.
8. Benchmark query bases/s, matches/s, lookup/reuse counters, index size, and
   construction phases separately.

## Extend MEM, MAM, or add strict MUM

1. Preserve the formal two-sided MEM oracle and the weaker legacy
   `RightMaximalMatch` contract independently.
2. Keep complete and sampled MEM equivalent across K, skip, constructors,
   lookup modes, strands, hard breaks, and persistence.
3. Keep reference-MAM uniqueness global across contigs and independent of
   query occurrence count.
4. A future strict MUM API must add an independently tested query-uniqueness
   filter; it must not silently change `MamMatch`.
5. Use MUMmer4 only as a black-box ACGT single-contig differential tool and
   report the first tuple difference on failure.

## Add a `.sufidx` section or format version

1. Determine whether the data is optional, required, or backend-specific.
2. Allocate a never-reused section ID and document element layout, width,
   endianness, counts, and invariants.
3. Increase format minor when an older reader cannot understand a legal new
   section; increase major only for incompatible outer interpretation.
4. Stream payloads without duplicating large sections in memory.
5. Validate size arithmetic before allocation and restrict backend reads to a
   bounded section stream.
6. Add CRC, truncation, duplicate/overlap, width/count, semantic-corruption,
   trailing-byte, and unknown-required-section tests.
7. Preserve atomic non-overwriting publication and old fixture loading.
8. Update inspect, format reference, compatibility matrix, changelog, and
   release checklist.

## Add CLI or benchmark surface

CLI parameters must validate kind-specific conflicts, defaults, numeric bounds,
stdout schema, stderr diagnostics, and exit-code mapping. Update `--help` and
the CLI reference together.

Benchmark methods must run in isolated workers when peak RSS matters, retain
all measured repetitions locally, calculate stable checksums, refuse success
on mismatch, and append schema fields without silently changing old meanings.
Document timing scope and aggregation before publishing performance claims.
