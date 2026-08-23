# Changelog

All notable changes to sufkit are documented in this file.

## Unreleased

- Changed the default SA acceleration to SA+ISA+LCP and changed MEM auto
  selection to suffix-link; CHILD and full remain explicit capabilities.
- Added an optional, clean-room Sapling-style piecewise-linear learned SA index
  with deterministic integer interpolation and correctness-preserving local
  search.
- Added explicit binary, LCP-aware binary, PWL, and CHILD exact-search control,
  plus PWL initialization/fallback control for suffix-link MEM search.
- Extended `.sufidx` to format 1.2 for the optional learned-model section while
  retaining 1.0/1.1 compatibility and the unchanged SDSL FM payload.
- Added learned-index construction/query metrics and exact/MEM benchmark
  ablations. No Python, PyTorch, CUDA, or Sapling runtime dependency was added.
- Added bundled CaPS-SA 32/64 shared-memory parallel suffix-array construction
  with ParlayLib, configurable build availability, stable persisted backend
  identities, conservative large-input auto-selection, and a dedicated
  isolated construction benchmark.
- Added optional text-position sampled standalone suffix arrays with complete
  exact count/locate and MEM recovery, format 1.3 persistence, CLI/inspection
  controls, and sampled-SA correctness/benchmark coverage.
- Avoided redundant LCP construction: divsufsort now returns sampled ISA/LCP
  through a private fused adapter, while CaPS directly retains its merge-built
  LCP and compacts it by interval minima when sampling is enabled.
- Added fixed SDSL balanced and DNA EPR `csa_wt` backends while retaining
  Huffman as the default and Sada as a reserved unavailable identity.
- Added ordered FM `equal_range_batch` and `count_batch` APIs with fixed-width
  interleaving and scalar-equivalent correctness gates.
- Added a layered English documentation set, concise Chinese onboarding,
  contributor architecture/extension contracts, and local Doxygen API
  generation.

## 0.1.1

- Added full-SA ESA auxiliary construction: ISA, Kasai LCP, and a persisted
  Abouelhoda-style CHILD table.
- Added streaming and deterministic vector MEM APIs with forward,
  reverse-complement, both-strand, hard-break, and bounded-result semantics.
- Added baseline, LCP-assisted, CHILD, suffix-link, and combined MEM query
  modes; suffix-link reuse is implemented with ISA/LCP interval expansion.
- Extended suffix-array `.sufidx` files to format 1.1 while retaining 1.0
  loading and 1.0 output for SA-only indexes.
- Added `sufkit mem`, `--sa-acceleration`, auxiliary inspection fields, MEM
  benchmark profiles, internal ablation, and optional MUMmer4 4.0.1
  black-box comparison.
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
