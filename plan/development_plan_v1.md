# sufkit 0.1.0 development plan

Status: V1 implementation completed and locally validated on 2026-08-22.

## Objective

Build a reusable C++17 library for genome-oriented suffix arrays and exact
pattern search.  The first release provides a standalone libdivsufsort suffix
array and an FM-index implemented entirely by SDSL's compressed suffix array:

```cpp
sdsl::csa_wt<sdsl::wt_huff<>, 32, 64>
```

The FM-index wrapper may normalize FASTA input, map multi-contig coordinates,
handle reverse complements, sort/deduplicate results, persist metadata, and
translate errors.  It must not implement its own BWT, C table, Occ checkpoints,
rank/select, LF mapping, or sampled suffix array.

All implementation writes are confined to `/mnt/d/code/sufkit`.  RaMAx and
MUMmer4 are read-only references and must not be modified.  Git commits and
pushes are outside this plan.

## V1 behavior contract

- Platform: Linux/WSL x86_64, GCC/Clang, C++17.
- License: MIT with complete third-party notices.
- CMake target: `sufkit::sufkit`.
- Integration: both `add_subdirectory` and installed `find_package`.
- FASTA: plain or gzip through kseq and zlib.
- Reference normalization: A/C/G/T are upper-cased; every other base symbol is
  normalized to N.
- Query validation: only non-empty A/C/G/T patterns, case-insensitive.
- Coordinates: 0-based, contig-local, half-open intervals in CLI output.
- Search: forward, reverse-complement, and both strands.  Palindromic queries
  are returned once per coordinate with strand `both`.
- Standalone SA: libdivsufsort32 when possible, libdivsufsort64 when requested
  or required.  CaPS is an unavailable, explicit V1 enum value.
- FM-index: only `sdsl_csa_wt_huff` is available in V1.  Balanced csa_wt and
  csa_sada enum values are reserved and fail with `unsupported_backend`.
- Persistence: one self-contained `.sufidx` file with a versioned outer header,
  contig metadata, CRC32-protected sections, and either a custom SA payload or
  an SDSL-native CSA payload.

## Text encoding

The SDSL construction input contains no zero byte:

```text
SEPARATOR=1, A=2, C=3, G=4, T=5, N=6
contig0 + SEP + contig1 + SEP + ... + contigN + SEP
```

SDSL validates the absence of zero and appends the unique zero sentinel.  The
standalone suffix array explicitly appends the same single zero sentinel before
calling divsufsort.  Consequently both indexes sort exactly the same logical
text.  Separators and N are hard query boundaries.

## Implementation milestones

1. Create the CMake/package skeleton, MIT license, notices, presets, examples,
   and pinned vendored dependencies.
2. Implement `GenomeReference`, kseq/zlib RAII, normalized byte encoding,
   metadata, fingerprinting, and global-to-local coordinate mapping.
3. Implement the PIMPL `SuffixArray`, divsufsort32/64 adapters, binary-search
   intervals, count, locate, strand handling, and index information.
4. Implement the PIMPL `FmIndex` with `sdsl::construct_im(..., 1)`,
   `sdsl::backward_search`, SDSL CSA position access, and no custom FM core.
5. Implement deterministic `.sufidx` save/load, atomic publication, outer
   metadata and CRC validation, and SDSL-native serialization.
6. Implement `sufkit build/query/inspect/bench`, documentation, consumer
   examples, CTest coverage, and the deterministic quick benchmark.

## Test and benchmark acceptance

- FASTA plain/gzip, multiline, CRLF, case normalization, ambiguous bases,
  duplicate/empty records, and damaged inputs.
- SA permutation/order invariants and 32/64 equivalence.
- SDSL `size == encoded_size + 1`, one sentinel, correct alphabet, and correct
  conversion from SDSL's closed interval to sufkit's half-open range.
- Naive, SA32, SA64, and SDSL FM count/locate equivalence on known and
  deterministic randomized multi-contig inputs.
- No cross-contig or cross-N hits; reverse-complement and palindrome behavior;
  deterministic `max_hits` truncation.
- SA and SDSL save/load round trips; corrupt magic/version/CRC/section bounds;
  overwrite refusal and atomic output publication.
- Static/shared builds, ASan/UBSan, CLI integration, `add_subdirectory`, and
  installed `find_package` consumers.
- Quick benchmark: deterministic seed 20260822, about 4 MiB and 1,000 patterns,
  with a smaller automated smoke profile.  Performance is descriptive; any
  result mismatch is a hard failure.  Large real-genome runs are not automatic.

## Deferred work

- V1.1: optional CaPS SA; `csa_wt<wt_blcd<>,32,64>`; `csa_sada<>`; disk-backed
  SDSL construction with an explicit cache directory.
- V1.2: LCP/ISA/CHILD/k-mer interval support and independently implemented
  MEM/MUM search, using MUMmer4 only as a black-box differential baseline.
- Later: BigBWT/PFP, mmap, out-of-core SA, SIMD, bindings, and RaMAx adapters.

Experimental backends remain disabled until correctness and benchmark evidence
show a useful trade-off.

## Audited source provenance

RaMAx source snapshot:

- Repository: `/mnt/d/code/RaMAx`
- Branch/commit at audit: `dev-pan` / `853080164fe4d1a36181b80bf127405cdc6957fe`
- `include/index.h`: `1961cfea8a7977c47b2983204a1e050b5b2f7facd869a35eff2d0b4655f8e4ed`
- `src/index/fm_index.cpp`: `2f01eadccc48363f57acd52bfe4455ec635c1b31e9950cecf9949320c896c34f`
- `include/kseq.h`: `9279bfe0f8bfcfe507d2852b838d2156d793f70b76ae9c63e3f6d381239ba901`
- `include/sdsl/version.hpp`: `fe91283876b3404b3dfc646aaee9495d162e9b9c473998b4bc36ad8887f318d6`

MUMmer4 reference snapshot:

- Directory: `/mnt/d/code/Another_project/mummer-4.0.1`
- Release: 4.0.1
- `include/mummer/sparseSA.hpp`: `035f5ff6ce1bba67ef83b2b8538690ec81a40bff405a1b64ec6a6681200dda13`
- `include/mummer/sparseSA_imp.hpp`: `51abd87e60025359c4165ac5cab32509ceeceb162ed703a1f5897df11e4652be`

Upstream dependency sources:

- libdivsufsort 2.0.2: <https://github.com/y-256/libdivsufsort>, MIT.
- SDSL 3.0.3: <https://github.com/xxsds/sdsl-lite>, BSD-3-Clause.
- kseq: <https://github.com/attractivechaos/klib>, MIT.
- Future CaPS-SA: <https://github.com/jamshed/CaPS-SA>, MIT.
- MUMmer4 comparison only: <https://github.com/mummer4/mummer>, Artistic-2.0.

Vendored-file fingerprints:

- `third_party/libdivsufsort/include/divsufsort.h`:
  `0b6b3a20b3c1009ca97312d53aca228d4a7c28fd8e59bcce533a71ad50016b64`
- `third_party/libdivsufsort/include/divsufsort64.h`:
  `134e0076544ed9b8cb340e5de149a1c20bbccbc5871f384ce28068a836a2a43f`
- `third_party/libdivsufsort/lib/divsufsort.c`:
  `d25839d4efe43c8aa60139fab833138471bfa296244bc5601d5c6127fcac6aaa`
- `third_party/kseq/kseq.h`:
  `9279bfe0f8bfcfe507d2852b838d2156d793f70b76ae9c63e3f6d381239ba901`
- `third_party/sdsl/include/sdsl/version.hpp`:
  `fe91283876b3404b3dfc646aaee9495d162e9b9c473998b4bc36ad8887f318d6`
- `third_party/sdsl/include/sdsl/csa_wt.hpp`:
  `cd1bd143350e3080f29a13fc8042af84b4da9ec8618b28a81b0cfff33013b575`
- `third_party/sdsl/include/sdsl/construct.hpp`:
  `8c6499bbddd9c7ac70c6ebb585895e05e497b1822c8fc8fb06adb0a4b4d94933`
- `third_party/sdsl/LICENSE`:
  `8a02b0c7dd5690ffc5433a5834ab2fe4f77cd19bfbad67659d38680b9dfb3421`

## Detailed public API plan

The stable V1 public surface consists of move-only PIMPL classes so dependency
types do not escape through installed headers:

```cpp
using SequenceId = std::uint32_t;
using Position = std::uint64_t;

struct SuffixArrayBuildOptions {
    SaBackend backend = SaBackend::auto_select;
    CoordinateWidth coordinate_width = CoordinateWidth::auto_select;
    std::uint32_t threads = 1;
};

struct FmIndexBuildOptions {
    FmBackend backend = FmBackend::sdsl_csa_wt_huff;
};

struct LocateOptions {
    StrandMode strands = StrandMode::forward;
    std::optional<std::uint64_t> max_hits;
};
```

`GenomeReference` provides `from_fasta`, `from_records`, aggregate base and
ambiguity counts, a deterministic normalized-content fingerprint, and
per-contig metadata. `SuffixArray` and `FmIndex` provide `build`, `load`,
`save`, `equal_range`, `count`, `locate`, `sequence_info`, and `info`;
`SuffixArray` additionally exposes `suffix_at` for algorithm development and
validation. `available_sa_backends` and `available_fm_backends` report V1
availability without silently substituting reserved backends.

All public search ranges are half-open. An absent pattern is represented as
`[0,0)` rather than an insertion point. `total_hits` is the complete result
count, while `hits` contains at most `max_hits` entries and `truncated` states
whether any entries were omitted. Limited locate scans retain only the
coordinate-sorted top N candidates, so temporary hit storage is O(N).

## Detailed implementation stages and gates

### Stage 0: plan and read-only source audit

Deliverables are this document, exact reference revisions, key-file hashes,
dependency sources/licenses, scope decisions, and an explicit read-only rule
for RaMAx and MUMmer4. No RaMAx adapter and no MUMmer-derived implementation is
part of V1.

Completion gate: the document exists before library implementation, all source
revisions are reproducible, and subsequent writes stay within the sufkit tree.

Implementation status: complete.

### Stage 1: repository and CMake packaging

The repository layout is:

```text
include/sufkit/   installed dependency-free public API
src/              private implementation
apps/             CLI and benchmark driver
tests/            unit, randomized differential, persistence tests
benchmarks/       benchmark documentation/extension point
examples/         direct and external CMake consumers
cmake/            package configuration
third_party/      pinned offline dependencies and licenses
docs/             API, format, backend, and benchmark contracts
plan/             development plan and provenance
```

Top-level builds enable CLI, tests, benchmark, and examples; subproject builds
disable them by default. Static or shared output follows `BUILD_SHARED_LIBS`.
The installed target is `sufkit::sufkit`; only zlib remains a public link
dependency. Vendored SDSL and divsufsort are implementation details.

Completion gate: Debug/Release static builds, a shared build, installation,
external `find_package`, and `add_subdirectory` consumers all compile and run.

Implementation status: complete for the bundled dependency modes used by V1.
The OFF forms of the bundled-dependency switches fail explicitly rather than
performing an unversioned or ambiguous system fallback.

### Stage 2: reference ingestion and encoding

kseq reads both uncompressed and gzip FASTA through zlib. The first non-empty
header token is the name; the remaining header text is the description.
Records preserve input order. Zero records, empty names, duplicate names, and
empty sequences are invalid.

ASCII A/C/G/T is normalized to uppercase. Every other reference symbol,
including N, U, IUPAC ambiguity codes, whitespace embedded in a sequence, or
non-biological bytes, becomes N and contributes to the ambiguity count. Query
patterns follow the stricter non-empty A/C/G/T-only contract.

The normalized encoded byte buffer is authoritative. Contig offsets and
lengths describe its A/C/G/T/N spans, with one separator byte after every
contig. No zero byte enters SDSL construction. Coordinate mapping accepts only
hits wholly contained in one contig and rejects separators, N-crossing false
positives, and corrupt out-of-range locations.

Completion gate: plain/gzip equivalence, CRLF and multiline input, normalization
counts, offsets, fingerprints, and boundary behavior pass tests.

Implementation status: complete.

### Stage 3: standalone suffix arrays

The standalone logical text is the encoded reference plus exactly one zero
sentinel. `auto_select` uses divsufsort32 when the full logical length fits
`saidx_t`, otherwise divsufsort64. Explicit 32-bit overflow is rejected;
explicit 64-bit always selects divsufsort64. V1 never selects CaPS.

Search performs two allocation-free suffix-versus-pattern binary searches.
The suffix array owns either 32-bit or 64-bit entries and retains the logical
text for search and self-contained persistence. Locate maps every global
suffix position back to contig-local coordinates before returning it.

Completion gate: permutation/sortedness on known cases, 32/64 equivalence,
naive randomized differential results, strand handling, and save/load
round-trips.

Implementation status: complete.

### Stage 4: SDSL FM-index

The only FM core is permanently identified by this backend signature:

```cpp
using SdslFmIndex = sdsl::csa_wt<sdsl::wt_huff<>, 32, 64>;
```

Construction calls `sdsl::construct_im(index, encoded_text, 1)` and verifies
that SDSL added one sentinel. Search calls `sdsl::backward_search` over the full
CSA interval and converts the returned closed range to a public half-open
range. Locate reads SDSL CSA rows with `operator[]`; persistence uses the CSA's
native `serialize` and `load`. sufkit neither accesses the wavelet tree nor
implements BWT, C, Occ, rank/select, LF, or sampled-SA logic.

Completion gate: text size/alphabet checks, SA/FM/naive equivalence, concurrent
const queries, bounded limited-locate storage, deterministic serialization,
and static audits of sources and public headers.

Implementation status: complete.

### Stage 5: `.sufidx` persistence

Format 1.0 begins with the eight bytes `SUFKIDX\0`, major/minor version,
little-endian marker, kind/backend/coordinate width/normalization IDs, sufkit
and SDSL versions, counts, fingerprint, and a section table. Metadata, SA text,
SA values, and SDSL CSA data are separate CRC32-protected sections; the header
also has CRC32 protection.

SDSL serializes directly to its payload section. The writer closes and
self-validates the temporary file before atomically publishing it. The target
is not replaced without explicit overwrite permission. A bounded input stream
prevents SDSL `load` from reading outside its section. Load validates magic,
format, endianness, section existence/bounds/non-overlap, CRCs, backend,
coordinate width, normalization, versions, record metadata, text fingerprint,
sentinel/alphabet, and payload consumption.

Completion gate: deterministic round-trip, moved-index independence from the
FASTA, overwrite semantics, no residual partial file, and safe rejection of
bad magic, kind, backend, version, truncation, CRC, and section layout.

Implementation status: complete for all implemented format sections.

### Stage 6: CLI, documentation, and benchmark

CLI commands are:

```text
sufkit build --type sa|fm --input REF --output INDEX [backend options]
sufkit query --index INDEX (--pattern P | --query FASTA)
             --strand forward|reverse|both [--count-only] [--max-hits N]
sufkit inspect --index INDEX
sufkit bench --smoke|--quick --output RESULT.tsv
```

SA-only arguments are rejected for FM builds and vice versa. Machine-readable
query results go to stdout; diagnostics and errors go to stderr. Hit TSV uses
query ID, sequence ID/name, zero-based start, exclusive end, and strand.
Exit codes distinguish input (2), I/O (3), corrupt/versioned index (4), and
backend/build errors (5).

The benchmark uses isolated worker processes for peak-RSS separation. The
fixed quick data generator uses seed 20260822, four contigs, about 4 MiB, and
1,000 patterns of 20/50/100 bp. Smoke uses the same logic at test scale.
Methods are naive, divsufsort32, divsufsort64, and the fixed SDSL FM backend.
Each measured query operation has one warm-up and five timed repetitions; the
median is reported. Any count, sorted locate prefix, or checksum mismatch makes
the benchmark fail before a successful performance result is accepted.

The TSV schema is:

```text
dataset dataset_fingerprint total_bases contigs method backend
backend_signature sdsl_version coordinate_width threads build_seconds
peak_rss_mb serialized_bytes load_seconds query_count count_qps locate_qps
total_hits reported_hits result_checksum
```

Implementation status: complete. The smoke profile is part of local
validation; the 4 MiB quick run and real-genome inputs remain user-triggered.

## Test matrix

### Reference and query contract

- Plain and gzip FASTA, multiline sequence, CRLF, mixed case.
- N/U/IUPAC normalization, ambiguity counts, zero byte exclusion.
- Empty file/record/name/sequence, duplicate names, damaged gzip.
- Multi-contig separators, N hard breaks, patterns longer than contigs.
- Empty/N/IUPAC/whitespace query rejection.
- Forward, reverse-complement, both, palindrome merging and stable ordering.

### SA and FM correctness

- Known repetitive strings and high-frequency/no-hit patterns.
- SA32/SA64 equality and deterministic small randomized differential tests.
- SDSL size, alphabet, closed-to-half-open interval conversion.
- Naive/SA32/SA64/FM count and locate equivalence.
- `max_hits=0`, one, bounded N, and unbounded locate.
- Complete `total_hits`, exact truncation flag, and deterministic top-N.
- Concurrent immutable FM queries.
- Installed public headers contain no SDSL or divsufsort include/type.

### Persistence and build integration

- SA and native SDSL CSA round-trip and deterministic bytes.
- Magic/kind/backend/version/CRC/truncation/section corruption.
- Default overwrite refusal and explicit atomic replacement.
- Debug, Release, ASan/UBSan, static/shared library builds.
- Standalone installed consumer and nested subdirectory consumer.
- CLI build/query/inspect/bench and benchmark-disabled failure path.

## V1 acceptance record

The local acceptance record must include:

- Release, Debug, and ASan/UBSan CTest passing.
- Static and shared output passing the core tests.
- Both CMake consumption forms compiling and running.
- Benchmark smoke returning identical `total_hits`, `reported_hits`, and
  `result_checksum` for all four methods.
- Public-header and custom-FM static audits returning no violations.
- RaMAx `git status --short` remaining empty and the audited MUMmer hashes
  remaining unchanged.
- No commit, push, remote configuration, or large genome benchmark.

## Post-V1 roadmap

V1.1 may add CaPS only to standalone `SuffixArray`, never to SDSL's internal
CSA construction. It may also add separately versioned
`csa_wt<wt_blcd<>,32,64>` and `csa_sada<>` backend IDs, plus explicit
SDSL disk-cache construction and a caller-specified cache directory. Every new
backend requires naive/SA/Huffman equivalence and separate benchmark evidence.

V1.2 may add Kasai LCP, compressed LCP, ISA, CHILD, k-mer intervals,
suffix-link interval reuse, MEM, MUM, and optionally MAM in that order. These
algorithms must be independently implemented, define multi-contig and strand
semantics, and use MUMmer4 only as a black-box differential oracle. Incorrect
or non-beneficial optimizations stay disabled or are abandoned.

BigBWT/PFP, mmap, out-of-core SA, parallel batch query, SIMD, incremental
indexes, C/Python APIs, and a RaMAx adapter are later work gated by real usage
and profiling rather than V1 feature count.

## 2026-08-22 local validation snapshot

Completed checks:

- GCC 13.3 Debug static build and CTest: passed.
- GCC 13.3 Release static build and CTest: passed.
- GCC 13.3 ASan/UBSan build and CTest: passed. LeakSanitizer is disabled in
  this WSL environment because ptrace-based leak checking is unsupported;
  address and undefined-behavior instrumentation remain enabled.
- Release shared-library build and CTest: passed.
- Installed `find_package(sufkit CONFIG REQUIRED)` consumer: compiled, linked,
  and ran.
- External `add_subdirectory` consumer: compiled, linked, and ran.
- CLI SA/FM build, query, and inspect workflow: passed.
- Benchmark smoke: naive, SA32, SA64, and SDSL FM each reported 75 complete and
  materialized hits with checksum `50b0fd2b02fb0683`.
- Public-header audit: no SDSL include/type and no divsufsort include/type.
- FM source audit: the only FM operations are SDSL construction, backward
  search, CSA row access, native serialization, and native load.
- RaMAx Git worktree: clean after implementation.
- MUMmer reference file hashes: unchanged from the audited snapshot.
- Local Git repository: initialized on `main`; no commit and no remote.

Environment-limited checks:

- Clang was not installed in the current WSL image, so the Clang build remains
  a CI/future-environment check.
- TSan was not run; the deterministic four-thread immutable query stress test
  passed, while sanitizer coverage in this environment is ASan/UBSan.
- The full 4 MiB `--quick` benchmark and large real-genome benchmark were not
  run automatically; use the documented command when performance measurement
  is desired.
