# Backend matrix

| Backend | V1 status | Purpose |
|---|---|---|
| libdivsufsort32 | available | Standalone suffix array up to the 32-bit length limit |
| libdivsufsort64 | available | Standalone 64-bit suffix array |
| CaPS-SA | available by default | Parallel standalone SA; directly supplies its merge-built LCP |
| SDSL csa_wt + wt_huff, SA/ISA 32/64 | available, default | Compatible FM-index baseline |
| SDSL csa_wt + wt_blcd, SA/ISA 32/64 | available | Balanced wavelet-tree comparison backend |
| SDSL csa_wt + wt_epr<8>, SA/ISA 32/64 | available | DNA small-alphabet rank optimization candidate |
| SDSL csa_sada | reserved, unavailable | Later comparison candidate |
| BigBWT/PFP | out of scope | Possible later external-memory route |
| Sapling-style PWL | available, opt-in | Predicts SA rows and narrows exact binary search |
| ESA CHILD | available, explicit | Suffix-tree-style interval navigation; not automatic |

`auto_select` uses CaPS for logical texts of at least 1 GiB when more than one
thread is requested; otherwise it uses divsufsort. Coordinate width is chosen
after the effective backend.

The default standalone SA layout is SA+ISA+LCP. CHILD is retained for explicit
experiments and future suffix-tree-style algorithms. The PWL learned model is
orthogonal to that layout and adds no runtime dependency.

Both SA backends support text-position sampling through
`SuffixArrayBuildOptions::sampling_rate`. CaPS reuses its native full LCP when
compacting; the divsufsort adapter constructs sparse ISA/LCP immediately after
the SA has been compacted. Sampling lowers resident and serialized index
memory, not the peak memory needed by the underlying full-SA constructor.

FM-index construction never auto-selects an experimental backend. The default
remains `sdsl-csa-wt-huff`; balanced and EPR must be requested explicitly.
