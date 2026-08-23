# Changelog

All notable changes to sufkit are documented in this file.

## 0.1.0 - release candidate

- Added genome-oriented plain/gzip FASTA ingestion and A/C/G/T/N normalization.
- Added standalone libdivsufsort32 and libdivsufsort64 suffix arrays.
- Added the fixed SDSL 3.0.3 `csa_wt<wt_huff<>,32,64>` FM-index backend.
- Added forward, reverse-complement, and both-strand exact search.
- Added versioned CRC-protected `.sufidx` persistence.
- Added CLI build, query, inspect, and benchmark commands.
- Added layered synthetic and user-reference benchmark support, including raw
  repetitions, multi-dimensional summaries, six synthetic scenarios, and
  correctness-gated count/locate workloads.

The 0.1.x series uses only the pinned vendored SDSL and libdivsufsort snapshots.
CaPS, additional SDSL CSA types, disk-backed construction, LCP, MEM, and MUM
remain later-version work.
