# CaPS-SA construction benchmark results (Unreleased)

## Environment

```text
OS: Linux 5.15.167.4-microsoft-standard-WSL2 x86_64
CPU: AMD Ryzen 9 7940HX, 16 cores / 32 logical CPUs
Compiler: GCC 13.3.0
CMake: 3.28.3
Build: Release, portable flags (no forced -march=native or -mavx2)
Seed: 20260822
```

CaPS-SA was built from commit `2597b373` with ParlayLib `e1f1dc0`. Core build
timing excludes FASTA loading, SA checksumming, save/load validation, and TSV
formatting. Peak RSS is the complete worker-process peak. Every row passed the
same two-part SA checksum and exact-query checksum gate.

The sufkit implementation commit is
`0d8a4a949873ab360ab825c16312c99936138e06`; integration with the existing
ESA/Sapling/FM mainline is `cebe058dc7044e2684ab3d9512a245e97ceb7413`.

## Commands

```bash
./build/release/sufkit_sa_build_bench \
  --profile smoke \
  --methods div32,div64,caps32,caps64 \
  --threads 1,2 \
  --acceleration none \
  --output-dir build/bench/sa-smoke-final

./build/release/sufkit_sa_build_bench \
  --profile smoke \
  --methods div32,caps32 \
  --threads 1,2 \
  --acceleration full \
  --output-dir build/bench/sa-smoke-full

./build/release/sufkit_sa_build_bench \
  --profile quick \
  --methods div32,caps32 \
  --threads 1,2,4,8 \
  --acceleration none \
  --output-dir build/bench/sa-quick
```

Datasets:

| Profile | Logical symbols | Fingerprint |
|---|---:|---:|
| smoke | 1,048,581 | 12806052656604154757 |
| quick | 67,108,869 | 17810958779957406579 |

## Smoke results

The 1 MiB smoke profile is dominated by parallel setup overhead. It is a
correctness test rather than evidence that CaPS should be selected for small
inputs.

| Method | Threads | Build median (s) | Peak RSS (MiB) | Speedup vs same-width divsufsort |
|---|---:|---:|---:|---:|
| div32 | 1 | 0.0455 | 8.89 | 1.00x |
| div32 | 2 | 0.0438 | 8.89 | 1.00x |
| caps32 | 1 | 0.2502 | 21.96 | 0.18x |
| caps32 | 2 | 0.1502 | 22.96 | 0.29x |
| div64 | 1 | 0.0448 | 13.14 | 1.00x |
| caps64 | 1 | 0.2843 | 38.97 | 0.16x |
| caps64 | 2 | 0.1672 | 40.73 | 0.24x |

With `acceleration=full`, div32 and caps32 produced the same SA checksum,
exact checksum, and right-maximal exact match checksum `17567547818709499406`. CaPS still pays for
its internal LCP. This original result predates commit `0162c34`, which changed
the integration to retain CaPS merge-built LCP directly instead of running a
second Kasai pass. The historical timings above must not be used to quantify
the newer LCP path.

## Quick results

Each 64 MiB quick row is the median of three independent worker processes.

| Method | Threads | Build median (s) | Peak RSS (MiB) | vs div32 at same thread | CaPS parallel efficiency |
|---|---:|---:|---:|---:|---:|
| div32 | 1 | 5.2401 | 388.13 | 1.00x | — |
| div32 | 2 | 5.4131 | 388.13 | 1.00x | — |
| div32 | 4 | 5.8113 | 388.14 | 1.00x | — |
| div32 | 8 | 5.4124 | 388.14 | 1.00x | — |
| caps32 | 1 | 23.3054 | 1157.50 | 0.22x | 100.0% |
| caps32 | 2 | 12.5261 | 1158.63 | 0.43x | 93.0% |
| caps32 | 4 | 6.6615 | 1161.62 | 0.87x | 87.5% |
| caps32 | 8 | 3.6618 | 1168.71 | 1.48x | 79.6% |

CaPS32 scales from 23.31 s at one thread to 3.66 s at eight threads, a 6.36x
self-speedup. At eight threads it is 1.48x faster than divsufsort32 on this
profile, while using about 3.01x the peak RSS. The result supports keeping
automatic CaPS selection restricted to large, explicitly multithreaded
inputs; it does not establish that 64 MiB is a universal crossover point.

All 24 quick repetitions used identical:

```text
SA checksum:    601412ef90896aab:b22884b0df11904d
exact checksum: 12074627964983466961
status:         ok
```

## Boundaries

The 1 GiB `standard` profile and real-genome FASTA benchmark were not run in
this implementation pass. They remain explicit user-triggered commands. No
claim is made about human-genome wall time, NUMA scaling, external-memory
behavior, or a crossover threshold on other CPUs. The 1 GiB automatic rule is
a conservative routing policy selected before these measurements, not a
threshold fitted from the 64 MiB result.

Sampling/LCP follow-up evidence is reported separately in
[the sampled-SA smoke benchmark](unreleased-sampled-sa.md).
