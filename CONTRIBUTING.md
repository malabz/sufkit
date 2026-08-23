# Contributing to sufkit

Thank you for improving sufkit. This project treats correctness, stable index
interpretation, and reproducible evidence as release requirements.

## Development setup

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure

cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure
```

Linux or WSL with GCC/Clang is the validated environment. C++17, CMake 3.20+
and ZLIB are required. SDSL, libdivsufsort, kseq, CaPS-SA, and ParlayLib are
vendored with source and license records.

Use a focused branch and keep unrelated local changes out of a contribution.
Do not add generated build trees, benchmark scratch data, Doxygen HTML, or
local plans to Git.

All first-party C++ changes follow the project
[C++ style guide](docs/development/cpp-style.md). Use clang-format 18 and
clang-tidy 18, keep third-party snapshots unformatted, and add
`// SPDX-License-Identifier: MIT` to new first-party C++ files. Functional and
mechanical formatting changes should be separate commits whenever possible.

## Architectural rules

- Public headers live in `include/sufkit` and must not expose SDSL,
  divsufsort, CaPS-SA, ParlayLib, kseq, or zlib implementation types.
- `GenomeReference`, `SuffixArray`, and `FmIndex` remain move-only PIMPL
  objects. Query operations on a built or loaded index are immutable.
- Public ranges are half-open. Public positions are zero-based and
  contig-local. Do not change these semantics in an optimization.
- A backend ID denotes one permanent payload interpretation. Never reuse an
  existing ID for a different type, sampling density, or integer width.
- Explicitly requested unavailable features return `unsupported_backend`.
  They must not silently fall back to another implementation.
- Prediction, CHILD navigation, suffix-link reuse, and batching may change
  work performed, but never the result set.
- The outer container validates versions, section bounds, CRCs, metadata, and
  backend-specific invariants before exposing an index.

See [architecture](docs/development/architecture.md) and
[internal invariants](docs/development/internal-invariants.md) before changing
an index or query path.

## Adding functionality

The [extension guide](docs/development/extending-sufkit.md) contains complete
checklists for SA backends, FM backends, exact algorithms, right-maximal exact match algorithms, and
new `.sufidx` sections. At minimum, a functional change needs:

1. a public or private capability decision;
2. deterministic unit and differential tests;
3. save/load and inspection coverage when data is persisted;
4. corruption and unavailable-backend behavior;
5. benchmark registration when performance is part of the claim;
6. updates to API, CLI, backend, compatibility, and changelog documentation.

New algorithms must be implemented clean-room from papers and public
specifications. Record upstream URLs, fixed revisions, licenses, and any
behavior used only for black-box comparison. Do not copy source from a tool
with an incompatible license into the MIT core.

## Correctness and performance

Performance work is accepted only after result equivalence. Use naive or
brute-force oracles on small inputs, cross-backend checks on larger inputs,
stable checksums, boundary cases, and save/load comparisons. A faster result
with different coordinates, exactness, or right maximality is a bug. Left
maximality is reserved for the future MEM contract.

Report build, load, count, locate, and right-maximal timings separately.
Preserve raw repetitions locally and write evidence-bounded Markdown reports containing the
command, commit, environment, seed, fingerprints, checksums, aggregation rule,
and known limits. External-process measurements such as MUMmer4 `load+query`
must not be presented as in-process query-kernel timings.

## Documentation contract

- Public API change: use Google-style names and update Doxygen, API contracts,
  examples, the naming migration guide, and CHANGELOG.
- CLI change: update `--help`, CLI reference, README examples, and exit/error
  behavior.
- Backend change: update backend matrix, compatibility, persistence, and
  benchmark documentation.
- Format change: update the format reference, loader tests, compatibility
  matrix, and inspection output.
- Performance claim: update the concise summary and a versioned detailed
  report with reproducible provenance.

See [testing, releases, and documentation](docs/development/testing-release-and-docs.md)
for the final local checklist.
