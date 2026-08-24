# Parallel suffix-array construction benchmark

The standalone benchmark compares divsufsort and CaPS without changing the
unified exact/right-maximal exact match benchmark implementation:

```bash
./build/release/sufkit_sa_build_bench \
  --profile smoke \
  --methods div32,div64,caps32,caps64 \
  --threads 1,2 \
  --sampling-rates 1 \
  --acceleration none \
  --output-dir build/bench/sa-smoke
```

The standalone driver uses the same four scale definitions as the exact and
right-maximal benchmarks:

| Profile | Reference bases | Default build repetitions |
|---|---:|---:|
| `smoke` | 16 KiB | 1 |
| `quick` | 4 MiB | 3 |
| `standard` | 32 MiB | 3 |
| `full` | 256 MiB | 1 |

Each profile creates one deterministic mixed genomic reference. The six-scenario
sweep belongs to the exact and right-maximal drivers; it is not silently
multiplied into this builder-only benchmark. A user FASTA can replace the
profile:

```bash
./build/release/sufkit_sa_build_bench \
  --reference reference.fa.gz \
  --methods div32,caps32,caps64 \
  --threads 1,2,4,8,16,32 \
  --sampling-rates 1,2,4,8 \
  --acceleration none \
  --output-dir results/sa-build
```

Every build, save, and load repetition runs in an isolated phase process. Core
build timing excludes FASTA parsing, SA checksumming, serialization, loading,
and TSV output. Peak RSS is the whole phase-process high-water mark and its
scope is recorded separately for build, save, and load. The build value
therefore includes reference storage and builder working memory; it is not an
incremental allocation attributed only to the SA algorithm.

`acceleration=none` isolates SA construction, `default` persists the normal
ISA/LCP layout, `full` adds CHILD, and `sapling` adds the learned lookup model.
divsufsort phase timing includes its fused sampled ISA/generalized-Kasai
adapter; CaPS directly retains merge-built LCP when requested.

`--sampling-rates` defaults to 1. For K>1, summary and raw rows record K and
`suffix_count`. SA hashes are compared only between methods at the same K,
while exact and right-maximal exact match checksums must agree across every K. Sampling compacts the
final structures after a complete backend suffix order exists, so worker peak
RSS must not be described as direct sparse-SA construction memory.

The output directory contains `run_metadata.tsv`, `raw_repetitions.tsv`,
`build_results.tsv`, and the generated FASTA for synthetic profiles. Raw rows
record backend signature, effective coordinate width, builder thread count,
sampling rate, acceleration layout, phase timing, phase RSS, logical and
allocated file size, and correctness checksums. The driver compares two
independent SA hashes plus exact and optional right-maximal exact match
checksums before reporting success. Raw and summary TSV files are retained if
the correctness gate fails.

In the controlled server suite, divsufsort rows run on one pinned physical CPU;
CaPS scaling rows use one physical CPU per core from NUMA node 0. The runner
records this affinity and the observed host configuration. It does not modify
the CPU governor, Turbo, swap, or any other system setting.

See the [sampled-SA result report](results/unreleased-sampled-sa.md) for the
first smoke memory-shape measurement.
