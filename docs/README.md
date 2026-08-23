# sufkit documentation

This directory is the documentation hub for users, C++ consumers,
contributors, and performance researchers. The English pages are the
authoritative technical documentation. A smaller Chinese guide is available
under [`zh-CN/`](zh-CN/README.md).

## Choose a reading path

| You want to… | Start here | Continue with |
|---|---|---|
| Run the CLI | [Quick start](getting-started/quickstart.md) | [CLI reference](user-guide/cli-reference.md), [troubleshooting](user-guide/troubleshooting.md) |
| Embed sufkit in C++ | [Installation](getting-started/installation.md) | [C++ workflows](user-guide/cpp-workflows.md), [API contracts](reference/api-contracts.md) |
| Choose SA, CaPS, or an FM backend | [Choosing an index](getting-started/choosing-an-index.md) | [Backend reference](reference/backends.md), [performance tuning](user-guide/performance-tuning.md) |
| Understand the algorithms | [Algorithm overview](concepts/algorithm-overview.md) | [Algorithm contracts](development/algorithm-contracts.md) |
| Reduce standalone-SA resident size | [Sampled suffix arrays](concepts/sampled-suffix-arrays.md) | [Exact search](user-guide/exact-search.md), [sampled-SA results](benchmarks/results/unreleased-sampled-sa.md) |
| Add a backend or search algorithm | [Architecture](development/architecture.md) | [Extending sufkit](development/extending-sufkit.md) |
| Interpret performance claims | [Benchmark summary](benchmarks/README.md) | [Methodology](benchmarks/methodology.md), versioned result reports |
| Understand `.sufidx` | [Persistence guide](user-guide/persistence-and-inspection.md) | [Format reference](reference/index-format-v1.md), [compatibility](reference/compatibility.md) |

## Documentation layers

### Getting started

- [Installation and integration](getting-started/installation.md)
- [Five-minute quick start](getting-started/quickstart.md)
- [Choosing an index](getting-started/choosing-an-index.md)

### User guides

- [C++ workflows](user-guide/cpp-workflows.md)
- [CLI reference](user-guide/cli-reference.md)
- [Exact search](user-guide/exact-search.md)
- [right-maximal exact match search](user-guide/right-maximal-search.md)
- [Persistence and inspection](user-guide/persistence-and-inspection.md)
- [Performance tuning](user-guide/performance-tuning.md)
- [Troubleshooting](user-guide/troubleshooting.md)

### Concepts and reference

- [Genome data model](concepts/genome-data-model.md)
- [Algorithm overview](concepts/algorithm-overview.md)
- [Text-position sampled suffix arrays](concepts/sampled-suffix-arrays.md)
- [API contracts](reference/api-contracts.md)
- [Backend reference](reference/backends.md)
- [Compatibility policy](reference/compatibility.md)
- [`.sufidx` 1.x format](reference/index-format-v1.md)
- [Glossary](reference/glossary.md)

### Contributor documentation

- [Architecture](development/architecture.md)
- [Internal invariants](development/internal-invariants.md)
- [Detailed algorithm contracts](development/algorithm-contracts.md)
- [Extending sufkit](development/extending-sufkit.md)
- [Testing, releases, and documentation maintenance](development/testing-release-and-docs.md)

### Benchmarks

- [Concise benchmark summary](benchmarks/README.md)
- [Benchmark methodology](benchmarks/methodology.md)
- [CaPS construction results](benchmarks/results/unreleased-caps.md)
- [Sampled-SA development results](benchmarks/results/unreleased-sampled-sa.md)
- [right-maximal exact match 0.1.1 results](benchmarks/results/v0.1.1-right-maximal.md)
- [Sapling development results](benchmarks/results/unreleased-sapling.md)
- [FM backend development results](benchmarks/results/unreleased-fm.md)

## Version labels

`0.1.1` refers only to the released ESA/right-maximal exact match baseline. Features merged into
`main` afterward are labeled **Unreleased**, even though the development
binary still reports `0.1.1`. Historical benchmark reports retain their
original measurement scope and are not rewritten as release claims.
