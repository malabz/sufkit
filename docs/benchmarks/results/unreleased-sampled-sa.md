# sufkit 0.2.0 sampled-SA smoke benchmark

**Status:** 0.2.0 development benchmark evidence. The historical filename is
retained to preserve existing links. This result validates correctness and the
expected memory shape; it is not a release-wide or large-genome speed claim.

The measured implementation commit was `0162c34e9d26a364af25f1b088e61f80be0307db`
before integration with the documentation branch. The run used GCC 13.3 on
WSL2, seed `20260822`, the deterministic 1 MiB synthetic reference, one
isolated worker per row, and `acceleration=full`.

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

All rows produced exact checksum `15589384678678967352` and right-maximal exact match checksum
`17567547818709499406`. SA checksums necessarily changed with K but agreed
between divsufsort and CaPS at the same K.

At K=8, the serialized full-ESA index was 82.3% smaller. The divsufsort worker
peak RSS was 77.8% lower because compaction happens before sampled ISA/LCP/CHILD
construction. CaPS RSS plateaued near 24.8 MiB for K>=2 because the backend
must first own the complete SA/LCP and working structures.

The single repetition and 1 MiB input are insufficient for a stable build-time
conclusion. Quick/standard and real-genome runs remain user-triggered. In
particular, this result does not show that sampling reduces the peak memory of
the underlying full-SA constructor.
