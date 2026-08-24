# Historical development evidence

This directory preserves versioned benchmark reports and implementation notes
that support past performance claims. These files are non-normative: current
behavior is defined by the public headers and the active documentation under
`docs/`.

The archive is retained in the source repository for provenance, but is not
installed with the library. Commands, measurements, checksums, fingerprints,
dates, and evidence boundaries in archived benchmark reports remain unchanged.

## Benchmark reports

| Release | Topic | Report |
|---|---|---|
| 0.1.1 | Right-maximal query baseline | [0.1.1-right-maximal.md](benchmarks/0.1.1-right-maximal.md) |
| 0.2.0 | CaPS-SA construction | [0.2.0-caps.md](benchmarks/0.2.0-caps.md) |
| 0.2.0 | FM backends and batch count | [0.2.0-fm.md](benchmarks/0.2.0-fm.md) |
| 0.2.0 | Sampled standalone SA | [0.2.0-sampled-sa.md](benchmarks/0.2.0-sampled-sa.md) |
| 0.2.0 | Sapling PWL lookup | [0.2.0-sapling.md](benchmarks/0.2.0-sapling.md) |
| 0.2.0 | Low-level performance work | [0.2.0-low-level-performance.md](benchmarks/0.2.0-low-level-performance.md) |

## Development notes

- [0.2.0 low-level performance notes](development/0.2.0-low-level-performance-notes.md)

The concise, current interpretation is maintained in the
[benchmark summary](../benchmarks/README.md). Git history remains the source
for deleted redirect pages and superseded documentation layouts.
