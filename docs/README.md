# sufkit documentation

The English pages are authoritative for the released 0.3.0 interface. The
concise [Chinese guide](zh-CN/README.md) covers initial use and index
selection.

## Users

Start with:

1. [installation and CMake integration](getting-started/installation.md);
2. [five-minute quick start](getting-started/quickstart.md); and
3. [choosing an index](getting-started/choosing-an-index.md).

Then use the maintained guides for:

- [CLI commands](user-guide/cli-reference.md);
- [C++ workflows](user-guide/cpp-workflows.md);
- [exact, sampled-SA, right-maximal, MEM, MAM, SMEM, and MUM search](user-guide/search.md);
- [persistence and inspection](user-guide/persistence-and-inspection.md); and
- [troubleshooting](user-guide/troubleshooting.md).

Stable contracts are collected in the [API reference](reference/api-contracts.md),
[backend matrix](reference/backends.md), [compatibility policy](reference/compatibility.md),
and [`.sufidx` format](reference/index-format-v1.md). The
[algorithm overview](concepts/algorithm-overview.md) explains how the pieces
fit without implementation-level detail.

## Contributors

Read these pages in order:

1. [architecture](development/architecture.md);
2. [algorithm internals and invariants](development/algorithm-internals.md);
3. [extension guide](development/extending-sufkit.md); and
4. [testing, documentation, and releases](development/testing-release-and-docs.md).

The [C++ style guide](development/cpp-style.md) defines source conventions.
Consumers migrating from 0.1.x should use the
[0.2.0 naming guide](development/api-naming-migration-0.2.0.md).

## Performance evidence

Use the [benchmark summary](benchmarks/README.md) for current conclusions and
the [methodology](benchmarks/methodology.md) for reproducible measurements.
