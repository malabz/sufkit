# Testing, releases, and documentation maintenance

## Local validation layers

### Functional suite

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

The suite must cover reference parsing/normalization, exact semantics, every
available constructor/backend, right-maximal exact match differential results, persistence,
corruption rejection, inspection, CLI integration, and benchmark smoke gates.

### Sanitizers

```bash
cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure
```

Do not use sanitizer timings for performance conclusions.

### Low-level kernels

When benchmarks are enabled, build `sufkit_low_level_bench` and run
`--verify-only` as a correctness gate. Timed scalar/SSE comparisons use seven
repetitions pinned to one logical CPU and retain raw TSV rows. Confirm the
expected SSE and POPCNT instructions with `objdump`; a compiler flag alone is
not instruction evidence. Current performance invariants are in
[algorithm internals](algorithm-internals.md).

### Static/shared and consumers

Validate `BUILD_SHARED_LIBS=OFF/ON`, `add_subdirectory`, install to a temporary
prefix, and an installed `find_package(sufkit CONFIG REQUIRED)` consumer. A
shared ELF library must not export vendored private symbols.

### Documentation

```bash
cmake -S . -B build/docs -DSUFKIT_BUILD_DOCS=ON
cmake --build build/docs --target sufkit_docs
```

Acceptance requires zero Doxygen warnings, no private third-party types in the
public reference, valid relative Markdown links, and successful compilation of
every canonical example. Generated HTML is not committed.

### Source style

Configure `SUFKIT_ENABLE_DEVELOPER_TOOLS=ON`, then run
`sufkit-format-check`, `sufkit-tidy-check`, and `git diff --check`. The
format/tidy inputs are an explicit first-party list and must not include
vendored or benchmark source. See the [C++ style guide](cpp-style.md).

## Correctness gates by change

| Change | Required evidence |
|---|---|
| Reference model | Plain/gzip parity, boundaries, normalized fingerprint |
| SA constructor | Full order/permutation, backend-width limits, build64->store32 parity, exact/maximal parity |
| Coordinate storage | native32/64 and split40/48 scalar/span parity, boundary selection, corruption, concurrency |
| SA sampling/LCP | K=1/K>1 order, raw/byte-coded parity, anchor validation, recovery parity, 1.3/1.4 corruption |
| Exact algorithm | Range/count/sorted locate parity for all strands |
| right-maximal exact match algorithm | Brute-force random oracle plus all internal-mode parity |
| MEM/reference-MAM | Independent two-sided oracle, complete/sampled parity where supported, MUMmer4 differential |
| generalized SMEM | Direct substring/occurrence/containment oracle, seed and expanded-coordinate totals |
| strict MUM | Direct overlapping query-occurrence oracle plus MUMmer4 `-mum` differential |
| FM backend/batch | Scalar/width/backend range, count, locate checksum parity |
| Persistence | Round trip, old fixtures, corruption and allocation-bound tests |
| Concurrency | Shared immutable index with thread-local statistics |
| Benchmark | Correctness gate before performance summary |

## Release checklist

1. Set CMake and public macro version intentionally.
2. Move completed items from `Unreleased` to the exact release section.
3. Verify the README capability summary, backend matrix, CLI help, Doxygen
   `since` labels, and compatibility table agree.
4. Verify `.sufidx` output minor for every legal index layout and old-reader
   expectations.
5. Run Release, sanitizer, static/shared, install, and consumer validation.
6. Run smoke benchmarks for correctness. Run quick/real performance only when
   a release claim depends on it.
7. Record final commit, dependency revisions, environment, fingerprints,
   checksums, and measurement limits in each result report.
8. Confirm licenses and `THIRD_PARTY_NOTICES.md` cover every bundled source.
9. Confirm no local paths, hostnames, build products, raw benchmark TSVs,
   generated HTML, or plans are tracked.
10. Run sampled-SA and adaptive-storage differential tests. Confirm formats
    1.0-1.3 remain readable and all newly saved standalone SAs use valid 1.4
    codecs whenever SA sampling, coordinate layout, or LCP construction
    changes.
11. Create commit, tag, push, or release only with explicit authorization.

Large-width release claims require separate evidence. Boundary fixtures can
prove checked selection and codec correctness without allocating billions of
rows, but they cannot prove peak memory, cache behavior, or throughput above
`2^32` symbols. Mark those ranges experimental until representative clean-exec
build/load/query measurements exist, including an apples-to-apples MUMmer4
comparison when such a claim is made.

## Documentation source of truth

| Fact | Authority | Required mirrors |
|---|---|---|
| Public method signature/default | Public header + Doxygen | API contracts, examples, naming migration |
| CLI flag/default | CLI parser and `--help` | CLI reference, relevant tutorial |
| Backend identity/status | Stored enum/signature + discovery API | Backend reference, README, changelog |
| File layout | Serialization reader/writer | Format and compatibility references |
| Algorithm semantics | Tests + algorithm internals | Search guide and Doxygen |
| Performance claim | Archived versioned evidence | Concise benchmark summary |
| Release status | Project version + CHANGELOG | README and compatibility page |

## Documentation review rules

- Keep README and benchmark summary concise; move implementation detail to one
  focused page and link it.
- English is authoritative. Chinese pages explain stable workflows and link to
  English parameter/reference detail rather than duplicating long tables.
- Define core terminology in the algorithm overview and use it consistently.
- Every diagram needs adjacent prose so the document remains understandable
  without Mermaid rendering.
- Code blocks must identify language or use `text` for output.
- Replace personal paths with placeholders such as `/path/to/mummer`.
- Distinguish released, unreleased, experimental, reserved, and unavailable.
- State negative results and unrun workloads explicitly.
- Do not infer real-genome, NUMA, disk, or cross-machine performance from
  synthetic quick runs.
