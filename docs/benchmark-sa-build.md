# Parallel suffix-array construction benchmark

The standalone benchmark compares divsufsort and CaPS without changing the
unified exact/MEM benchmark implementation:

```bash
./build/release/sufkit_sa_build_bench \
  --profile smoke \
  --methods div32,div64,caps32,caps64 \
  --threads 1,2 \
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
  --acceleration none \
  --output-dir results/sa-build
```

Every repetition runs in a child process. Core timing excludes FASTA parsing,
SA checksumming, serialization, loading, and TSV output; peak RSS is the child
process peak and therefore includes reference storage and builder working
memory. `acceleration=none` isolates SA construction, while `full` measures
compatibility with the existing ISA/Kasai LCP/CHILD/MEM pipeline.

The output directory contains `run_metadata.tsv`, `raw_repetitions.tsv`,
`build_results.tsv`, and the generated FASTA for synthetic profiles. The
driver compares two independent SA hashes plus exact and optional MEM
checksums before reporting success. Raw and summary TSV files are retained if
the correctness gate fails.

Add `--sampling-rates 1,2,4,8` to compare complete and text-position sampled
SA layouts. Raw and summary rows include `sampling_rate` and `suffix_count`.
SA checksums are compared between methods only at the same K; exact and MEM
checksums must agree across every K.
