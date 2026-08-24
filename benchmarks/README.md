# Benchmarks

The current low-level optimization measurements and their correctness gates
are summarized in
[`docs/benchmarks/results/v0.2.0-low-level-performance.md`](../docs/benchmarks/results/v0.2.0-low-level-performance.md).

The V1 benchmark is compiled into the `sufkit bench` command so the same
installed executable can measure generated or user-supplied genome data.
Its implementation is split across `apps/benchmark*.cpp`; the benchmark
contract, TSV schema, correctness gate, and invocation examples are documented in
`docs/benchmarks/methodology.md`.

Use the smoke profile for a fast correctness check:

```bash
./build/release/sufkit bench \
  --profile smoke \
  --output-dir build/bench/smoke
```

Run the fixed approximately 4 MiB profile explicitly when performance numbers
are needed:

```bash
./build/release/sufkit bench \
  --profile quick \
  --output-dir build/bench/quick
```

`standard` and `full` are opt-in large profiles. User FASTA input is accepted
with `--reference` and optional `--queries`; the benchmark never downloads a
dataset automatically.

The exact benchmark exposes sampled SA K=2/4/8 methods explicitly for both
SA32 and SA64, with `binary` and `lcp-binary` lookup variants. For example:

```bash
./build/release/sufkit bench \
  --profile smoke \
  --methods sa32-sampled-k2-binary,sa32-sampled-k4-lcp-binary \
  --output-dir build/bench/sampled-smoke
```

FM batch runs keep their equal-length rows and can add a deterministic
mixed-length group with `--fm-query-modes scalar,batch,batch-mixed`. The
right-maximal workload accepts `--query-repetitions N` to override its profile
default and `--strands forward,reverse-complement,both` for independent
orientation rows. Its compatibility default remains `forward`; MUMmer4 timed
rows remain explicitly forward-only.

Unified benchmark metadata appends the Git/source state, complete configured
compiler flags, CPU flags, executable checksum, actual CPU affinity,
SSE4.2/POPCNT state, and a path-redacted command. Absolute path-bearing flag
values are redacted as well. Its `peak_rss_mb` is the
high-water mark of the complete method worker process (build, save, load, and
query), not query-only RSS; this is recorded as
`peak_rss_scope=method_process_lifetime`.

Parallel suffix-array construction has a separate executable so CaPS work does
not couple to the unified query benchmark:

```bash
./build/release/sufkit_sa_build_bench \
  --profile quick \
  --methods div32,caps32 \
  --threads 1,2,4,8 \
  --acceleration none \
  --output-dir build/bench/sa-quick
```

See `docs/benchmarks/sa-construction-methodology.md` for timing boundaries and
TSV fields, and `docs/benchmarks/results/unreleased-caps.md` for the measured
smoke/quick results. Sampled-SA methodology and measurements are documented in
`docs/benchmarks/results/unreleased-sampled-sa.md`.

Instruction-level comparison kernels have a separate developer-only harness:

```bash
./build/release/sufkit_low_level_bench --verify-only

taskset -c 0 ./build/release/sufkit_low_level_bench \
  --profile smoke --repetitions 7 \
  --output build/bench/low-level-smoke.tsv
```

The executable is built by `SUFKIT_BUILD_BENCHMARKS=ON` but is not installed.
See `docs/development/low-level-performance.md` for its correctness matrix,
TSV semantics, assembly checks, and WSL evidence boundary.

Query-time allocation observations use a separate non-timed developer target:

```bash
./build/release/sufkit_query_allocation_bench --verify-only

./build/release/sufkit_query_allocation_bench \
  --output build/bench/query-allocations.tsv
```

The target builds its small reference, SA, and FM-index before enabling its
executable-local allocation counter. It reports one logical query pass and is
not installed. Allocation counts are implementation observations, not timing
results or a substitute for RSS measurements.

Phase-isolated RSS observations use another non-installed developer target:

```bash
./build/release/sufkit_query_memory_bench \
  --reference reference.fa.gz \
  --method sa32 \
  --output build/bench/sa32-phase-rss.tsv

./build/release/sufkit_query_memory_bench \
  --reference reference.fa.gz \
  --method fm-huff \
  --output build/bench/fm-huff-phase-rss.tsv
```

The parent process executes independent build, load, and query workers and
records both `/proc` current RSS and `wait4` peak RSS with explicit scope
labels. This complements the unified benchmark's whole-method peak; it does
not replace timing, allocation, or native-Linux profiler evidence. Paths are
not written to the TSV, and temporary index/report files are confined to the
output directory and removed after the run.
