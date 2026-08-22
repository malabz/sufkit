# Benchmarks

The V1 benchmark is compiled into the `sufkit bench` command so the same
installed executable can measure generated or user-supplied genome data.
Its implementation is in `apps/benchmark.cpp`; the benchmark contract, TSV
schema, correctness gate, and invocation examples are documented in
`docs/benchmark.md`.

Use the smoke profile for a fast correctness check:

```bash
./build/release/sufkit bench --smoke --output build/bench/smoke.tsv
```

Run the fixed approximately 4 MiB profile explicitly when performance numbers
are needed:

```bash
./build/release/sufkit bench --quick --output build/bench/quick.tsv
```
