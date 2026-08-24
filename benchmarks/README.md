# Benchmark source layout

The installed `sufkit bench` command owns deterministic exact, FM, sampled-SA,
and right-maximal workloads. The non-installed developer targets are:

| Target | Purpose |
|---|---|
| `sufkit_sa_build_bench` | Isolated divsufsort/CaPS construction, sampling, and auxiliary phases |
| `sufkit_low_level_bench` | Scalar/SSE comparison-kernel correctness and timing |
| `sufkit_query_allocation_bench` | Query allocation observations outside timed passes |
| `sufkit_query_memory_bench` | Separate build/load/query current and peak RSS workers |

Build the benchmark targets with:

```bash
cmake -S . -B build/bench-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUFKIT_BUILD_BENCHMARKS=ON
cmake --build build/bench-release -j
```

The authoritative command profiles, TSV schemas, timing scopes, correctness
gates, and reproducibility rules are in the
[benchmark methodology](../docs/benchmarks/methodology.md). Current conclusions
and the source-only evidence entry point are in the
[benchmark summary](../docs/benchmarks/README.md).

Raw result directories, generated FASTA, Doxygen output, and local profiler
data are development artifacts and must not be committed. Developer benchmark
executables are not installed with the library.
