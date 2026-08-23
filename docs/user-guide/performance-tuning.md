# Performance tuning

Always measure the operation that dominates the application. Build, load,
count, locate, and right-maximal exact match have different bottlenecks.

## Construction

- Use divsufsort for small and medium references or when peak memory matters.
- Consider CaPS for large references with several available cores. CaPS is a
  shared-memory constructor and may use much more RSS.
- `threads>1` does not make divsufsort itself parallel. Threads can still help
  selected auxiliary construction work.
- 32-bit SA storage is preferable when the effective backend can represent the
  logical text. Public coordinates are unaffected.
- divsufsort now returns sampled ISA/LCP from a private adapter; CaPS retains
  its merge-built LCP. This avoids a redundant second LCP pass.
- Building CHILD or PWL still adds work after the backend SA/LCP phases.

## Standalone-SA sampling

- K>1 reduces loaded and serialized SA/ISA/LCP/CHILD rows approximately by K.
- The backend still forms a complete SA, so sampling is not a solution for
  constructor peak memory or out-of-core construction.
- Exact searches at least K bases perform up to K anchor lookups; shorter
  patterns use direct contig scan.
- Sampled right-maximal exact match requires `min_length>=K` and may trade more interval/extension
  work for lower resident memory.
- PWL model size is budgeted against the sampled SA payload when both are used.

Benchmark K=1 and intended K values on the real query-length distribution.

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

## right-maximal exact match

Suffix-link reuse is the default because it avoids restarting from the root at
most query positions. Record suffix-link success and root/fallback lookup
counts before adding another lookup accelerator. PWL can improve the remaining
lookups but has workload-dependent end-to-end impact.

Use `for_each_right_maximal_match` when results can be consumed online. Use `find_right_maximal_matches` for a
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
- Record sampling rate and suffix count; never compare SA checksums across
  different K as though the stored row sets were identical.
- Treat synthetic quick results as hypotheses for real references, not final
  deployment thresholds.

See [benchmark methodology](../benchmarks/methodology.md) and the
[concise results](../benchmarks/README.md).
