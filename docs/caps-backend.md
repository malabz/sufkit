# CaPS-SA backend

CaPS-SA is sufkit's shared-memory parallel constructor for a complete suffix
array. It is selected explicitly with `SaBackend::caps` or automatically for
logical texts of at least 1 GiB when more than one thread is requested.

```bash
sufkit build --type sa \
  --input reference.fa.gz \
  --output reference.sufidx \
  --sa-backend caps \
  --sa-width auto \
  --threads 16
```

The backend receives the same byte text as divsufsort, including contig
separators and sufkit's unique zero sentinel. Bounded-context construction is
not used. CaPS internally constructs an LCP array as part of its merge
algorithm, but this integration deliberately discards that result: existing
ISA, Kasai LCP, CHILD, suffix-link, exact-search, and MEM code remains the sole
post-SA pipeline.

## Threads and subproblems

Each build executes inside a private Parlay scheduler. sufkit does not set or
read `PARLAY_NUM_THREADS`. The internal subproblem count is:

```text
min(8192, max(1, symbols / 4096), max(1, 128 * threads))
```

This setting is private construction tuning and does not affect the resulting
suffix order or the persisted payload. Explicit CaPS construction requires at
least 16 logical symbols. Runtime failure is reported as `build_failure`; an
explicit request is never silently replaced by divsufsort.

## Compatibility

The `.sufidx` payload remains the existing generic integer-vector suffix
array. Backend IDs 3 and 4 identify `caps32` and `caps64`, respectively. A
build configured with `SUFKIT_ENABLE_CAPS=OFF` cannot construct new CaPS
indexes but can load, inspect, search, and re-save an existing CaPS index.
