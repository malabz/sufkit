# Benchmark summary

This page preserves the historical, evidence-bounded local development
measurements. It reports controlled synthetic smoke/quick measurements from one
WSL2 workstation, not universal hardware or real-genome guarantees. Every
reported method passed its result-equivalence gate before timing was
interpreted. See the [latest reproducible server headline and complete result
package](../../benchmarks/README.md) for the current full-suite entry point.

## Environment and evidence boundary

These historical reports use GCC 13.3.0, CMake 3.28.3, Linux/WSL2, and an AMD Ryzen
9 7940HX. Deterministic generators use seed 20260822. Detailed pages record
dataset/query fingerprints, checksums, repetitions, timing scope, and exact
commands.

These local reports do not establish human-genome wall time, NUMA behavior, a
universal CaPS crossover, disk-backed construction, or production performance
on another machine. Standard/full were not run for the historical numbers
below; they are separate mandatory stages of the current complete server
acceptance. Large real-genome experiments remain user-triggered.

## SA construction: divsufsort and CaPS

Unreleased CaPS result on the 64 MiB quick profile:

| Builder | Threads | Build median | Peak RSS | Relative to div32 |
|---|---:|---:|---:|---:|
| divsufsort32 | 1 | 5.24 s | 388 MiB | 1.00x |
| CaPS32 | 1 | 23.31 s | 1158 MiB | 0.22x |
| CaPS32 | 4 | 6.66 s | 1162 MiB | 0.87x |
| CaPS32 | 8 | 3.66 s | 1169 MiB | 1.48x |

CaPS scaled 6.36x from one to eight threads but used about 3.01x divsufsort32
peak RSS. At 1 MiB it was much slower because setup dominated. This supports a
conservative automatic policy—large text and explicit parallel resources—not
a 64 MiB global threshold. The current auto threshold remains 1 GiB.

See [CaPS construction results](results/unreleased-caps.md) and
[methodology](sa-construction-methodology.md).

## Sampled standalone SA

Unreleased 1 MiB smoke evidence with `acceleration=full`:

| Builder | K | Stored rows | Peak RSS | Serialized size |
|---|---:|---:|---:|---:|
| divsufsort32 | 1 | 1,048,581 | 40.74 MiB | 17.83 MB |
| divsufsort32 | 8 | 131,073 | 9.06 MiB | 3.15 MB |
| CaPS32 | 1 | 1,048,581 | 40.87 MiB | 17.83 MB |
| CaPS32 | 8 | 131,073 | 24.80 MiB | 3.15 MB |

K=8 reduced the serialized full-ESA index by 82.3%. The divsufsort worker RSS
fell 77.8% because sampled auxiliaries are built after compaction; CaPS still
must own complete SA/LCP and working data first. This one-repetition smoke run
is correctness and memory-shape evidence, not a stable build-time claim.

See [sampled-SA results](results/unreleased-sampled-sa.md) and the
[algorithm contract](../concepts/sampled-suffix-arrays.md).

## Exact suffix-array lookup

Unreleased 4 MiB quick results; QPS aggregates selected pattern lengths and
strands:

| Scenario | Operation | Binary | LCP-aware | PWL | CHILD |
|---|---|---:|---:|---:|---:|
| mixed | count | 702k | 823k (1.17x) | 776k (1.11x) | 435k (0.62x) |
| repeat-rich | count | 613k | 667k (1.09x) | 724k (1.18x) | 482k (0.79x) |
| mixed | locate(1) | 484k | 548k (1.13x) | 523k (1.08x) | 350k (0.72x) |
| repeat-rich | locate(1) | 250k | 259k (1.04x) | 282k (1.13x) | 228k (0.91x) |

LCP-aware binary is the stronger mixed baseline; PWL is competitive and helps
repeat-rich lookup, but remains opt-in until real-reference evidence exists.
CHILD is an explicit research capability and a negative optimization here.

See [Sapling results](results/unreleased-sapling.md).

## right-maximal exact match

Released 0.1.1 established suffix-link reuse as the main acceleration. The
subsequent isolated-worker PWL comparison measured:

| Scenario | Min length | Suffix-link binary | Suffix-link PWL | PWL speed |
|---|---:|---:|---:|---:|
| mixed | 20 | 0.0787 s | 0.0498 s | 1.58x |
| mixed | 100 | 0.1047 s | 0.0559 s | 1.87x |
| repeat-rich | 20 | 0.4061 s | 0.4239 s | 0.96x |
| repeat-rich | 100 | 0.3118 s | 0.2345 s | 1.33x |

PWL improved five of six recorded combinations but regressed repeat-rich at
minimum length 20 by about 4.2%. Suffix-link remains the default; PWL fallback
and full CHILD remain explicit.

See the archived
[0.1.1 right-maximal evidence](../archive/benchmarks/0.1.1-right-maximal.md)
and [Sapling evidence](../archive/benchmarks/0.2.0-sapling.md).

## MEM and reference-MAM

The Unreleased `mem` and `mam` workloads call the formal `FindMems()` and
`FindMams()` APIs. MEM rows are checked against MUMmer4 `-maxmatch`;
reference-MAM rows use `-mumreference`. Baseline, LCP, CHILD, suffix-link, and
full methods must agree on total matches and sorted-result checksum before a
run is accepted.

Smoke validation uses seed 20260822, a 64 KiB synthetic reference, 100
queries, minimum lengths 20 and 50, one warm-up, and three measured
repetitions. MUMmer4 time is external load+query time, not an in-process
query-kernel measurement.

## FM-index

Unreleased quick averages across four 4 MiB scenarios:

| Backend | Build | Load | Serialized | Count throughput | locate(1000) QPS |
|---|---:|---:|---:|---:|---:|
| Huffman | 0.338 s | 0.079 s | 2.37 MB | 39.24 Mbase/s | 10,366 |
| Balanced | 0.353 s | 0.084 s | 2.93 MB | 30.72 Mbase/s | 7,755 |
| EPR | 0.296 s | 0.239 s | 7.21 MB | 51.58 Mbase/s | 12,980 |

EPR scalar count was 1.31x and locate(1000) 1.25x Huffman in aggregate, but
used about 3.04x serialized space and 3.03x load time. EPR batch count gained
another 1.13x over its scalar path at its best tested width. Huffman remains
the default; balanced is retained as an explicit comparison backend.

See [FM backend results](results/unreleased-fm.md).

## Interpretation rules

- Correctness checksum disagreement makes a run fail; it is never summarized
  as successful performance.
- Build, save, load, count, locate, and right-maximal exact match are separate operations.
- Method-local worker processes isolate peak RSS.
- Medians follow warm-up and preserve measured repetition counts in local run
  artifacts.
- MUMmer4 is a black-box result comparator. Its process startup and
  `load+query+output+parse` time is not a query-kernel comparison.
- Historical development runs below are retained as Markdown reports. New
  versioned server packages also retain repository-sized aggregate and raw
  repetition TSV evidence, manifests, and SVG; generated FASTA, indexes,
  scratch files, and logs remain on the experiment server.
