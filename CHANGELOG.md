# Changelog

All notable changes to sufkit are documented in this file.

## Unreleased

### Planned for 0.3.0

- Added formal two-sided `Mem*` APIs and the `sufkit mem` command. MEM search
  uses SA interval traversal, LCP-assisted candidate recovery, ISA+LCP
  suffix-link reuse, and bounded left recovery for sampled suffix arrays.
- Added reference-unique `Mam*` APIs and `sufkit mam`, matching MUMmer4
  `-mumreference` semantics: uniqueness is required across the combined
  reference, but repeated occurrences in the query remain valid.
- Kept the existing `RightMaximal*` interface and output behavior unchanged.
  CHILD remains explicit and is never selected automatically.
- Added independent MEM/MAM oracles, MUMmer4 4.0.1 black-box differential
  tests, and correctness-gated `mem`/`mam` smoke benchmark workloads.
- Kept `.sufidx` formats 1.0-1.3 unchanged; MEM/MAM use the normalized text
  and existing SA/ISA/LCP/CHILD/PWL sections and do not depend on SeqPro.

## 0.2.0 - 2026-08-24

### Source interface and terminology

- Standardized the C++ source interface on Google-style `PascalCase` functions
  and `kPascalCase` enumerators. This is intentionally source-incompatible
  with 0.1.x and no old-name forwarding aliases are provided. Public include
  paths, the `sufkit::sufkit` CMake target, CLI syntax, serialized IDs, and
  `.sufidx` compatibility remain unchanged.
- Corrected the former MEM terminology without changing query logic. The
  implementation guarantees right maximality but not left maximality, so its
  public types, APIs, CLI command, and benchmark workload now use
  `RightMaximal` and `right-maximal` names. MEM names are reserved for future
  two-sided maximality support.

### Suffix-array capabilities

- Changed the default SA acceleration to SA+ISA+LCP and the automatic
  right-maximal search path to suffix-link reuse. CHILD and the combined path
  remain available through explicit selection.
- Added bundled CaPS-SA 32/64 shared-memory parallel construction with
  ParlayLib, stable persisted backend identities, configurable availability,
  and conservative large-input auto-selection.
- Added optional text-position sampled standalone suffix arrays with complete
  exact count/locate and right-maximal recovery. Format 1.3 persists sampled
  indexes while retaining readers for formats 1.0 through 1.2.
- Reused builder-produced LCP data: divsufsort exposes sampled ISA/LCP through
  a private fused adapter, and CaPS retains and compacts its merge-built LCP.
- Added an optional clean-room Sapling-style piecewise-linear SA predictor with
  deterministic integer interpolation, verified local search, explicit
  binary/LCP/PWL/CHILD control, and format 1.2 persistence. It adds no Python,
  PyTorch, CUDA, or Sapling runtime dependency.

### FM-index capabilities

- Added fixed SDSL balanced and DNA EPR `csa_wt` backends while retaining
  Huffman as the default and Sada as a reserved unavailable identity.
- Added ordered FM `EqualRangeBatch` and `CountBatch` APIs with interleaved
  query processing and scalar-equivalent correctness gates.

### Performance and project quality

- Optimized existing SA, FM, sampled-SA, Sapling, and right-maximal paths with
  complete-SA count and zero-retention locate fast paths, lazy bounded heaps,
  encoded views and lookup tables, compact auxiliary arrays, delayed coordinate
  mapping, SSE4.2 comparison, active-lane FM batch workspaces, and lower-copy
  construction and persistence.
- Added developer-only low-level, allocation, phase-RSS, sampled-query,
  mixed-length FM batch, suffix-link-scan, learned-index, and construction
  measurements with correctness checks and explicit evidence boundaries.
- Added a layered English documentation set, concise Chinese onboarding,
  contributor architecture and extension contracts, Google C++ style tooling,
  local Doxygen API generation, and expanded local tests.

## 0.1.1

Terminology note: 0.1.1 originally described this query as MEM search. The
current contract corrects it to right-maximal exact matching because the
implementation does not guarantee left maximality.

- Added full-SA ESA auxiliary construction: ISA, Kasai LCP, and a persisted
  Abouelhoda-style CHILD table.
- Added streaming and deterministic vector query APIs, originally named
  `Mem*`, with forward, reverse-complement, both-strand, hard-break, and
  bounded-result semantics.
- Added baseline, LCP-assisted, CHILD, suffix-link, and combined query modes;
  suffix-link reuse is implemented with ISA/LCP interval expansion.
- Extended suffix-array `.sufidx` files to format 1.1 while retaining 1.0
  loading and 1.0 output for SA-only indexes.
- Added the command originally named `sufkit mem`, `--sa-acceleration`,
  auxiliary inspection fields, benchmark profiles, internal ablation, and an
  optional MUMmer4 4.0.1 black-box comparison.
- Kept the SDSL FM-index implementation and payload format unchanged.

## 0.1.0

- Added genome-oriented plain/gzip FASTA ingestion and A/C/G/T/N normalization.
- Added standalone libdivsufsort32 and libdivsufsort64 suffix arrays.
- Added the fixed SDSL 3.0.3 `csa_wt<wt_huff<>,32,64>` FM-index backend.
- Added forward, reverse-complement, and both-strand exact search.
- Added versioned CRC-protected `.sufidx` persistence.
- Added CLI build, query, inspect, and benchmark commands.
- Added layered synthetic and user-reference benchmark support, including raw
  repetitions, multi-dimensional summaries, six synthetic scenarios, and
  correctness-gated count/locate workloads.

The project uses pinned vendored SDSL, libdivsufsort, CaPS-SA, ParlayLib, and
kseq snapshots. Disk-backed construction, strict query-and-reference-unique
MUM, direct sparse-SA construction, r-index/RLBWT, and BigBWT/PFP remain
future work.
