# Changelog

All notable changes to sufkit are documented in this file.

## Unreleased

- Optimized existing SA, FM, sampled-SA, Sapling, and right-maximal query
  paths with no public API or index-format change: complete-SA count and
  zero-retention locate fast paths, lazy bounded heaps, encoded views/LUTs,
  compact auxiliary arrays, delayed coordinate mapping, SSE4.2 comparison,
  active-lane FM batch workspaces, and lower-copy construction/persistence.
- Added developer-only low-level, allocation, phase-RSS, sampled-query,
  mixed-length FM batch, and suffix-link-scan measurements with correctness
  checks, source/toolchain provenance, explicit RSS scopes, and a documented
  WSL evidence boundary.
- Standardized the planned 0.2.0 C++ source interface on Google-style
  `PascalCase` functions and `kPascalCase` enumerators. This is intentionally
  source-incompatible: no old-name forwarding aliases are provided. Public
  include paths, the `sufkit::sufkit` CMake target, CLI syntax, serialized IDs,
  and `.sufidx` compatibility are unchanged.
- Corrected the former MEM terminology without changing query logic. The
  implementation guarantees right maximality but not left maximality, so
  `Mem*`, `find_mems()`, `for_each_mem()`, `sufkit mem`, and `--workload mem`
  were renamed to `RightMaximal*`, `FindRightMaximalMatches()`,
  `ForEachRightMaximalMatch()`, `sufkit right-maximal`, and
  `--workload right-maximal`. MEM names are reserved for future two-sided
  maximality support.
- Changed the default SA acceleration to SA+ISA+LCP and changed right-maximal
  search auto selection to suffix-link; CHILD and full remain explicit.
- Added an optional, clean-room Sapling-style piecewise-linear learned SA index
  with deterministic integer interpolation and correctness-preserving local
  search.
- Added explicit binary, LCP-aware binary, PWL, and CHILD exact-search control,
  plus PWL initialization/fallback control for suffix-link right-maximal search.
- Extended `.sufidx` to format 1.2 for the optional learned-model section while
  retaining 1.0/1.1 compatibility and the unchanged SDSL FM payload.
- Added learned-index construction/query metrics and exact/right-maximal
  benchmark ablations. No Python, PyTorch, CUDA, or Sapling runtime dependency
  was added.
- Added bundled CaPS-SA 32/64 shared-memory parallel suffix-array construction
  with ParlayLib, configurable build availability, stable persisted backend
  identities, conservative large-input auto-selection, and a dedicated
  isolated construction benchmark.
- Added optional text-position sampled standalone suffix arrays with complete
  exact count/locate and right-maximal recovery, format 1.3 persistence,
  CLI/inspection controls, and sampled-SA correctness/benchmark coverage.
- Avoided redundant LCP construction: divsufsort now returns sampled ISA/LCP
  through a private fused adapter, while CaPS directly retains its merge-built
  LCP and compacts it by interval minima when sampling is enabled.
- Added fixed SDSL balanced and DNA EPR `csa_wt` backends while retaining
  Huffman as the default and Sada as a reserved unavailable identity.
- Added ordered FM `EqualRangeBatch` and `CountBatch` APIs with fixed-width
  interleaving and scalar-equivalent correctness gates.
- Added a layered English documentation set, concise Chinese onboarding,
  contributor architecture/extension contracts, and local Doxygen API
  generation.

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

The 0.1.x development line uses pinned vendored SDSL, libdivsufsort, CaPS-SA,
ParlayLib, and kseq snapshots. Disk-backed construction, MUM/MAM, direct
sparse-SA construction, r-index/RLBWT, and BigBWT/PFP remain future work.
