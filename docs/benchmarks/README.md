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
on another machine. Most historical sections below did not run standard/full.
The current adaptive-storage evidence includes one synthetic standard run and
one four-genome microbial reference, but not a reference above `2^32` logical
symbols.

## Adaptive SA profiles

The 0.3.0 implementation was measured on 2026-08-29 with clean
phase workers pinned to one logical CPU. Fast stores native32 SA+ISA+raw LCP;
Low-memory stores native32 SA+byte-coded LCP and does not retain ISA. All paired
exact, MEM, and reference-MAM result checksums agreed.

The latest synthetic standard `mixed` run used 33,554,432 bases, 5,000 queries,
three builds, one warm-up, and seven query repetitions:

| Metric | Fast | Low-memory |
|---|---:|---:|
| Build median | 4.046 s | 4.362 s |
| Build peak RSS | 868.74 MiB | 520.67 MiB |
| Save median | 3.875 s | 1.856 s |
| Load median | 4.339 s | 2.037 s |
| Query-worker peak RSS | 429.37 MiB | 205.41 MiB |
| Serialized index | 436.21 MB | 201.46 MB |
| Resident core | 436.21 MB | 201.49 MB |
| Bits/base | 104.000 | 48.031 |

Low-memory reduced retained/index space and query-worker peak RSS by about 54%
and 52%, respectively. Summing query-group median seconds across all strands,
its throughput relative to Fast was 0.946x for count, 0.944x/0.974x/0.993x for
locate limits 1/10/1000. This is the intended profile trade-off, and is why
Fast retains native ISA and raw LCP.

A real-reference run used four complete *E. coli* genomes (18,533,783 bases;
FASTA SHA-256
`254e5d916a985de331794db95d74316279e0cd768b7bce79aa591dc13f715940`),
1,000 deterministic 256-base queries, and MUMmer4 4.0.1. The MEM and
reference-MAM timings reported below are forward-strand only:

| Metric | Fast | Low-memory | MUMmer4 |
|---|---:|---:|---:|
| Serialized index | 240.94 MB | 111.23 MB | 166.86 MB |
| Bits/base | 104.000 | 48.012 | 72.023 |
| Build peak RSS | 465.76 MiB | 272.60 MiB | 454.71 MiB |
| Query-process peak RSS | 240.13 MiB | 116.30 MiB | 190.85 MiB |
| Index-ready PSS | 232.54 MiB | 110.94 MiB | not measured |

The index-ready PSS values come from an independent clean-exec query-memory
worker using the same reference, not from the timed MEM/MAM workers. Build RSS
also has different phase boundaries: each sufkit build worker includes
reference loading and construction but saves in a separate worker, whereas the
external MUMmer4 build process includes startup, reference loading,
construction, and index saving. The table therefore reports each tool's
declared whole-worker high-water mark rather than identical build subphases.

Thus Low-memory used about 33% less serialized space and 39% less query-process
peak RSS than MUMmer4 on this reference. Its preloaded MEM kernel took
0.069/0.014/0.0067 s at minimum lengths 20/50/100; preloaded reference-MAM took
0.398/0.333/0.236 s. MUMmer4's timed external process, including startup,
index loading, querying, and output-file writing, took approximately 1.05-1.15
s for MEM and 1.02-1.03 s for reference-MAM; result parsing and checksum
calculation happened afterward and were not timed. These are deliberately
different timing scopes: they establish fast repeated queries, not a cold-start
victory. Adding sufkit's separate load median shows that its one-shot load plus
query path was still slower on this small query batch. Faster `.sufidx` loading
is therefore the next clear optimization target.

These profile comparisons combine layout and algorithm choices: Fast uses
suffix-link reuse while Low-memory uses the LCP path. They must not be read as
an isolated raw-versus-byte-coded LCP microbenchmark. Split40/split48 remain
experimental until a representative reference above `2^32` symbols is run.

## SA construction: divsufsort and CaPS

0.2.0 development evidence for CaPS on the 64 MiB quick profile:

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

See the archived
[CaPS construction results](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.2.0-caps.md)
and
[methodology](sa-construction-methodology.md).

## Sampled standalone SA

0.2.0 development evidence on the 1 MiB smoke profile with
`acceleration=full`:

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

See the archived
[sampled-SA results](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.2.0-sampled-sa.md)
and the
[algorithm overview](../concepts/algorithm-overview.md).

## Exact suffix-array lookup

0.2.0 development evidence on the 4 MiB quick profile; QPS aggregates selected
pattern lengths and
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

See the archived
[Sapling results](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.2.0-sapling.md).

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
[0.1.1 right-maximal evidence](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.1.1-right-maximal.md)
and
[Sapling evidence](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.2.0-sapling.md).

## MEM, reference-MAM, SMEM, and MUM

The 0.3.0 `mem` and `mam` workloads call the formal `FindMems()` and
`FindMams()` APIs. MEM rows are checked against MUMmer4 `-maxmatch`;
reference-MAM rows use `-mumreference`. Baseline, LCP, CHILD, suffix-link, and
full methods must agree on total matches and sorted-result checksum before a
run is accepted.

Version 0.3.0 also provides `smem` and `mum` workloads. Generalized
SMEM rows report both distinct query-interval seeds and expanded reference
coordinates for each occurrence threshold. Strict MUM rows can be checked
against MUMmer4 `-mum`. MiniBWA is optional and is kept in a separately
labeled FMD `load+query` scope; it is not treated as an in-process or
forward-only timing peer. No broad SMEM/MUM performance claim is made here;
the available smoke/quick runs are correctness-gated development evidence.

Smoke validation uses seed 20260822, a 16 KiB synthetic reference, 100
queries, minimum lengths 20 and 50, one warm-up, and three measured
repetitions. MUMmer4 time includes external process startup, index loading,
querying, and output-file writing; output parsing and checksum calculation are
not timed. It is therefore not an in-process query-kernel measurement.
For an uninstalled libtool build, benchmark commands pass both the launcher
(`--mummer4`) and the actual ELF (`--mummer4-runtime`); run metadata records
their SHA-256 values separately.

## FM-index

0.2.0 development evidence averaged across four 4 MiB quick scenarios:

| Backend | Build | Load | Serialized | Count throughput | locate(1000) QPS |
|---|---:|---:|---:|---:|---:|
| Huffman | 0.338 s | 0.079 s | 2.37 MB | 39.24 Mbase/s | 10,366 |
| Balanced | 0.353 s | 0.084 s | 2.93 MB | 30.72 Mbase/s | 7,755 |
| EPR | 0.296 s | 0.239 s | 7.21 MB | 51.58 Mbase/s | 12,980 |

EPR scalar count was 1.31x and locate(1000) 1.25x Huffman in aggregate, but
used about 3.04x serialized space and 3.03x load time. EPR batch count gained
another 1.13x over its scalar path at its best tested width. Huffman remains
the default; balanced is retained as an explicit comparison backend.

See the archived
[FM backend results](https://github.com/malabz/sufkit/blob/main/docs/archive/benchmarks/0.2.0-fm.md).

## Interpretation rules

- Correctness checksum disagreement makes a run fail; it is never summarized
  as successful performance.
- Build, save, load, count, locate, and right-maximal exact match are separate operations.
- Method-local worker processes isolate peak RSS.
- Medians follow warm-up and preserve measured repetition counts in local run
  artifacts.
- MUMmer4 is a black-box result comparator. Its process startup, index loading,
  querying, and output-file writing are timed; parsing and checksum calculation
  happen afterward and are excluded. This is not a query-kernel comparison.
- Historical development runs below are retained as Markdown reports. New
  versioned server packages also retain repository-sized aggregate and raw
  repetition TSV evidence, manifests, and SVG; generated FASTA, indexes,
  scratch files, and logs remain on the experiment server.
