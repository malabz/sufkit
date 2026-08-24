# Contributing to sufkit

sufkit treats result equivalence, stable index interpretation, and reproducible
evidence as release requirements. Linux or WSL with GCC/Clang is the validated
development environment; C++17, CMake 3.20+, and ZLIB are required.

## Local setup

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure

cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure
```

Keep generated builds, local plans, raw benchmark output, and Doxygen HTML out
of Git. Do not reformat vendored dependencies. New first-party C++ follows the
[style guide](docs/development/cpp-style.md) and carries
`// SPDX-License-Identifier: MIT`.

## Before changing code

Read the documents relevant to the change:

1. [architecture](docs/development/architecture.md) for module ownership and
   dependency direction;
2. [algorithm internals](docs/development/algorithm-internals.md) for
   correctness, layout, persistence, and concurrency invariants;
3. [extension guide](docs/development/extending-sufkit.md) for backend,
   algorithm, format, CLI, and benchmark checklists; and
4. [testing and release maintenance](docs/development/testing-release-and-docs.md)
   for the required local validation layers.

Public headers must not expose SDSL, divsufsort, CaPS-SA, ParlayLib, kseq, or
zlib implementation types. Public ranges stay half-open; positions stay
zero-based and contig-local. Backend and section IDs are permanent. Explicitly
unavailable capabilities must fail rather than silently select another path.

## Correctness and evidence

An optimization may change work, never results. Use small brute-force oracles,
cross-backend and width comparisons, boundary cases, stable checksums, and
save/load round trips. Right-maximal results do not imply left maximality or
MEM semantics.

Report build, load, count, locate, and right-maximal time separately. Preserve
raw repetitions locally. A tracked performance claim needs the command,
commit, environment, seed, fingerprints, checksums, aggregation rule, and
known limits; external process time must not be labeled in-process query time.

## Documentation with a change

- Public API: update Doxygen, API contracts, examples, migration notes, and
  CHANGELOG.
- CLI: update `--help`, CLI reference, examples, and exit behavior.
- Backend or format: update backend, compatibility, persistence, inspection,
  and corruption documentation.
- Performance: update the concise benchmark summary and archive one
  reproducible versioned evidence report.

Keep each fact in its canonical document. Do not add a new redirect page or a
second explanation when a link to the maintained source is sufficient.
