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

Profiles contain 1 MiB (`smoke`), 64 MiB (`quick`), or 1 GiB (`standard`) of
deterministic mixed genomic sequence. A user FASTA can replace the profile:

```bash
./build/release/sufkit_sa_build_bench \
  --reference reference.fa.gz \
  --methods div32,caps32,caps64 \
  --threads 1,2,4,8,16,32 \
  --sampling-rates 1,2,4,8 \
  --acceleration none \
  --output-dir results/sa-build
```

Every repetition runs in a child process. Core timing excludes FASTA parsing,
SA checksumming, serialization, loading, and TSV output; peak RSS is the child
process peak and therefore includes reference storage and builder working
memory. `acceleration=none` isolates SA construction, while `full` measures
compatibility with the existing ISA/LCP/CHILD/right-maximal exact match pipeline. divsufsort phase
timing includes its fused sampled ISA/generalized-Kasai adapter; CaPS directly
retains merge-built LCP when requested.

`--sampling-rates` defaults to 1. For K>1, summary and raw rows record K and
`suffix_count`. SA hashes are compared only between methods at the same K,
while exact and right-maximal exact match checksums must agree across every K. Sampling compacts the
final structures after a complete backend suffix order exists, so worker peak
RSS must not be described as direct sparse-SA construction memory.

The output directory contains `run_metadata.tsv`, `raw_repetitions.tsv`,
`build_results.tsv`, and the generated FASTA for synthetic profiles. The
driver compares two independent SA hashes plus exact and optional right-maximal exact match
checksums before reporting success. Raw and summary TSV files are retained if
the correctness gate fails.

See the [sampled-SA result report](results/unreleased-sampled-sa.md) for the
first smoke memory-shape measurement.
