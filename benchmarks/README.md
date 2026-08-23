# Benchmarks

The V1 benchmark is compiled into the `sufkit bench` command so the same
installed executable can measure generated or user-supplied genome data.
Its implementation is split across `apps/benchmark*.cpp`; the benchmark
contract, TSV schema, correctness gate, and invocation examples are documented in
`docs/benchmark.md`.

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
