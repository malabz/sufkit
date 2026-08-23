# Sampled SA smoke benchmark

The local smoke run used GCC 13.3 on WSL2 and the deterministic 1 MiB synthetic
reference. Each row is one isolated worker process with `acceleration=full`.

```bash
./build/release/sufkit_sa_build_bench \
  --profile smoke \
  --methods div32,caps32 \
  --threads 1 \
  --sampling-rates 1,2,4,8 \
  --acceleration full \
  --repetitions 1 \
  --output-dir build/bench/sa-sampling-smoke
```

| Backend | K | Retained suffixes | Build seconds | Peak RSS MiB | Serialized bytes |
|---|---:|---:|---:|---:|---:|
| divsufsort32 | 1 | 1,048,581 | 0.0785 | 40.74 | 17,826,376 |
| divsufsort32 | 2 | 524,291 | 0.0623 | 22.61 | 9,437,780 |
| divsufsort32 | 4 | 262,146 | 0.0545 | 13.62 | 5,243,460 |
| divsufsort32 | 8 | 131,073 | 0.0468 | 9.06 | 3,146,292 |
| CaPS32 | 1 | 1,048,581 | 0.2604 | 40.87 | 17,826,376 |
| CaPS32 | 2 | 524,291 | 0.2526 | 24.80 | 9,437,780 |
| CaPS32 | 4 | 262,146 | 0.2463 | 24.80 | 5,243,460 |
| CaPS32 | 8 | 131,073 | 0.2649 | 24.80 | 3,146,292 |

All rows produced the same exact checksum `15589384678678967352` and MEM
checksum `17567547818709499406`. SA checksums differed by K, as expected, but
matched between divsufsort and CaPS at each K.

K=8 reduced the serialized full-ESA index by 82.3%. The divsufsort worker peak
RSS fell by 77.8% because it compacts the SA before constructing sparse
ISA/LCP/CHILD. CaPS peak RSS plateaued near 24.8 MiB for K>=2 because CaPS must
first own its complete SA/LCP and working structures. This smoke dataset is a
correctness and memory-shape check, not a stable large-genome performance
claim; quick/standard and real-genome runs remain user-triggered.
