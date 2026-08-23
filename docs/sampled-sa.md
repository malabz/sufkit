# Sampled suffix arrays

`SuffixArrayBuildOptions::sampling_rate=K` retains suffixes beginning at text
positions `0,K,2K,...`. K=1 is the existing complete suffix array. The CLI
equivalent is:

```bash
sufkit build --type sa --input reference.fa.gz --output reference.sufidx \
  --sa-backend divsufsort --sa-sampling-rate 4 --sa-acceleration full
```

The implementation first lets divsufsort or CaPS construct a complete suffix
order. It then compacts that array in place, so the loaded index, ISA, LCP,
CHILD, learned model, and serialized payload scale with approximately `n/K`.
The current implementation does not lower the full-SA constructor's peak
memory; this distinction must be preserved in benchmark claims.

CaPS already constructs LCP during its merge. sufkit retains that result and
computes sparse-neighbour LCP by range minima while compacting. Upstream
libdivsufsort has no LCP-returning API, so sufkit's private divsufsort adapter
compacts the returned SA and immediately constructs sparse ISA/LCP in the same
backend build result. The later ESA stage does not repeat that work.

Exact `count` and `locate` search up to K query offsets and verify the left
prefix. Patterns shorter than K use a direct contig scan because some matches
contain no sampled reference position. `equal_range` is unavailable for K>1:
the complete result is a union of sparse-SA intervals, not one interval.

MEM search requires `min_length >= K`. Each candidate is anchored at a sampled
reference position, extended to both maximal boundaries, and emitted only when
the anchor is the first sampled position in the MEM. This preserves the full
SA MEM set without duplicate anchors. Contig boundaries, N, separators, the
sentinel, strand coordinates, sorting, and truncation semantics are unchanged.
