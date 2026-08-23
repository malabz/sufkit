# Performance tuning

Always measure the operation that dominates the application. Build, load,
count, locate, and MEM have different bottlenecks.

## Construction

- Use divsufsort for small and medium references or when peak memory matters.
- Consider CaPS for large references with several available cores. CaPS is a
  shared-memory constructor and may use much more RSS.
- `threads>1` does not make divsufsort itself parallel. Threads can still help
  selected auxiliary construction work.
- 32-bit SA storage is preferable when the effective backend can represent the
  logical text. Public coordinates are unaffected.
- Building ISA/LCP/CHILD or PWL adds phase time after the SA is constructed.

The dedicated `sufkit_sa_build_bench` isolates constructor timing and reports
method-local peak RSS. Use a representative FASTA before changing automatic
routing thresholds.

## Exact search

- Count avoids position recovery and result materialization.
- LCP-aware binary can reduce repeated character comparisons.
- PWL is most relevant for patterns at least model k. Repeat-rich prefixes can
  enlarge prediction error and local windows.
- A larger PWL model is not guaranteed to be faster; the current 1% raw-SA
  budget was better than larger tested budgets on one synthetic sweep.
- CHILD traversal is explicit because current exact benchmarks show negative
  optimization.
- Locate with many hits is dominated by row recovery, coordinate mapping,
  sorting, and output; range acceleration may barely affect total time.

## MEM

Suffix-link reuse is the default because it avoids restarting from the root at
most query positions. Record suffix-link success and root/fallback lookup
counts before adding another lookup accelerator. PWL can improve the remaining
lookups but has workload-dependent end-to-end impact.

Use `for_each_mem` when results can be consumed online. Use `find_mems` for a
deterministic vector and apply `max_matches` to bound retained result memory.
The full match count still requires complete traversal.

## FM-index

- Huffman is the default space/query compromise.
- EPR improves small-alphabet rank throughput in current experiments, but its
  serialized size and load time are much larger.
- Balanced is retained as an explicit comparison backend and is slower in the
  current DNA quick workload.
- Batch count can help EPR by interleaving independent search chains. Width 0
  chooses 16; benchmark candidate widths rather than assuming more states are
  always better.
- All current FM builders use in-memory `construct_im`; large construction
  needs enough RAM for SDSL's temporary data.

## Measurement discipline

- Use Release builds, not sanitizer builds.
- Pin method, width, threads, acceleration, seed, scenario, pattern lengths,
  strands, and result limits.
- Separate warm-up and measured repetitions.
- Compare full counts and stable result checksums before interpreting timing.
- Do not compare an external `load+query` process directly with an in-process
  query-only call.
- Treat synthetic quick results as hypotheses for real references, not final
  deployment thresholds.

See [benchmark methodology](../benchmarks/methodology.md) and the
[concise results](../benchmarks/README.md).
