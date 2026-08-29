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

Every build, save, and load repetition forks and then execs the current
benchmark executable in a private worker mode. The exec boundary discards the
controller address space, so phase RSS cannot inherit the controller's prior
high-water mark. Workers return a versioned fixed-size binary record through a
private result file; the public benchmark options are unchanged.

Core build timing excludes FASTA parsing, SA checksumming, serialization,
loading, and TSV output. The reported build peak is sampled immediately after
`SuffixArray::Build()` returns in the clean process, before the internal
handoff save; it includes that worker's reference parsing/normalization and
builder working memory. The save worker first loads the source index, so its
peak scope explicitly includes source load. The load peak is sampled when the
index becomes ready, before validation checksums. Raw and summary outputs use
the following literal scopes:

```text
clean_exec_build_worker_until_index_ready
clean_exec_save_worker_including_source_load
clean_exec_load_worker_until_index_ready
```

These are process high-water marks, not incremental allocations attributed
only to the requested operation.

`acceleration=none` isolates SA construction, `default` persists the normal
ISA/LCP layout, `full` adds CHILD, and `sapling` adds the learned lookup model.
divsufsort phase timing includes its fused sampled ISA/generalized-Kasai
adapter. CaPS always computes merge-built LCP internally; Fast and CHILD copy
the required raw rows, while Low-memory creates byte-coded LCP after the CaPS
object is released. CaPS build RSS still includes its complete SA/LCP, work
arrays, and partition metadata regardless of the final resource profile.

`--sampling-rates` defaults to 1. For K>1, summary and raw rows record K and
`suffix_count`. SA hashes are compared only between methods at the same K,
while exact and right-maximal exact match checksums must agree across every K. Sampling compacts the
final structures after a complete backend suffix order exists, so worker peak
RSS must not be described as direct sparse-SA construction memory.

The output directory contains `run_metadata.tsv`, `raw_repetitions.tsv`,
`build_results.tsv`, and the generated FASTA for synthetic profiles. Raw and
summary rows record construction/stored coordinate widths, resource profile,
LCP encoding, storage-compaction time, the three phase RSS scopes, backend
signature, builder threads, sampling, acceleration, phase timing, logical and
allocated file size, and correctness checksums. The legacy
`coordinate_width` column remains a construction-width alias. The driver
compares two independent SA hashes plus exact and optional right-maximal exact
match checksums before reporting success. Raw and summary TSV files are
retained if the correctness gate fails.

In the controlled server suite, divsufsort rows run on one pinned physical CPU;
CaPS scaling rows use one physical CPU per core from NUMA node 0. The runner
records this affinity and the observed host configuration. It does not modify
the CPU governor, Turbo, swap, or any other system setting.

See the archived
[sampled-SA result report](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.2.0-sampled-sa.md)
for the
first smoke memory-shape measurement.
