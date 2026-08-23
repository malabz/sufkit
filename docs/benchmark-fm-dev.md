# FM-index backend and batched-count benchmark

## Scope and correctness

This development benchmark compares three fixed SDSL 3.0.3 compressed suffix
arrays without changing the standalone suffix-array implementation:

```text
fm-huff      csa_wt<wt_huff<>,32,64>
fm-balanced  csa_wt<wt_blcd<>,32,64>
fm-epr       csa_wt<wt_epr<8>,32,64>
```

The source branch starts at `e5c57364a5d0704e01eef178885c34eb5c159ce6`.
Tests and benchmarks were run in `/mnt/d/code/sufkit-fmindex`; the path is not
stored in result metadata. Huffman remains the default backend.

Both smoke and quick completed their correctness gates. SA32, all three FM
backends, scalar count, and batch widths 1, 4, 8, 16, and 32 produced identical
hit counts and checksums. Release and ASan/UBSan CTest both passed. There were
no non-`ok` build or query rows and no cross-method checksum mismatch.

## Environment and datasets

```text
Compiler:       GCC 13.3.0
CMake:          3.28.3
Build type:     Release
OS:             Linux 5.15.167.4-microsoft-standard-WSL2
Architecture:   x86_64
CPU:            AMD Ryzen 9 7940HX with Radeon Graphics
Logical CPUs:   32
SDSL:           3.0.3
Seed:           20260822
```

Smoke used two 16 KiB datasets:

| Scenario | Fingerprint |
|---|---|
| balanced | `b19d546862724f95` |
| mixed | `58b167a81ddb78e3` |

Quick used four 4 MiB, four-contig datasets and 1,000 queries per scenario.
The timed forward count workload contains 172,240 query bases per scenario.

| Scenario | Fingerprint | Ambiguous fraction | Repeat fraction |
|---|---|---:|---:|
| mixed | `c06bdfecb0dd3f1e` | 0.0100 | 0.1500 |
| gc-skewed | `40c9f4cc9fd50b80` | 0.0005 | 0.0100 |
| repeat-rich | `72e633da51d3d5e1` | 0.0005 | 0.4000 |
| n-islands | `336bc2dea399d472` | 0.0500 | 0.0100 |

Each quick build has three repetitions, and each query row has one warm-up and
five measured repetitions. Pattern lengths are 20, 50, 100, 200, and 500 bp.
Locate limits are 1, 10, and 1,000.

## Commands and artifacts

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure

cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure
```

```bash
./build/release/sufkit bench \
  --profile smoke \
  --scenarios balanced,mixed \
  --methods naive,sa32,fm-huff,fm-balanced,fm-epr \
  --fm-query-modes scalar,batch \
  --fm-batch-widths 1,4,8,16,32 \
  --pattern-lengths 20,50,100 \
  --locate-limits 1,10,all \
  --output-dir build/bench/fm-smoke-final
```

```bash
./build/release/sufkit bench \
  --profile quick \
  --scenarios mixed,gc-skewed,repeat-rich,n-islands \
  --methods sa32,fm-huff,fm-balanced,fm-epr \
  --fm-query-modes scalar,batch \
  --fm-batch-widths 1,4,8,16,32 \
  --pattern-lengths 20,50,100,200,500 \
  --locate-limits 1,10,1000 \
  --output-dir build/bench/fm-quick-final
```

Each result directory contains `run_metadata.tsv`, `build_results.tsv`,
`query_results.tsv`, and `raw_repetitions.tsv`. Raw repetitions, rather than
only the summaries below, are the source of record.

## Build, load, and space

The table reports the mean of the four per-scenario medians to keep the
time/space trade-off visible. Index size is stable across scenarios except for
the distribution-sensitive Huffman and balanced wavelet trees.

| Backend | Build s | Load s | Peak RSS MiB | Bytes | bits/base |
|---|---:|---:|---:|---:|---:|
| Huffman | 0.3379 | 0.0788 | 72.72 | 2,372,406 | 4.525 |
| Balanced | 0.3533 | 0.0842 | 74.74 | 2,932,390 | 5.593 |
| EPR | 0.2961 | 0.2391 | 81.87 | 7,207,119 | 13.746 |

Relative to Huffman, balanced takes 1.24x the space and has no build or load
advantage. EPR builds in 0.876x the time, but uses 3.04x the serialized space,
1.13x peak RSS, and 3.03x the load time. EPR is therefore a speed-oriented
backend, not a compressed-space replacement for Huffman.

## Scalar count and batched count

The following throughput aggregates all forward count groups and pattern
lengths. Ratios use the matching run's Huffman scalar measurement.

| Backend | Scalar Mbase/s | vs Huffman | Best batch width | Best batch Mbase/s | Batch vs own scalar |
|---|---:|---:|---:|---:|---:|
| Huffman | 39.244 | 1.000x | 4 | 39.398 | 1.004x |
| Balanced | 30.715 | 0.783x | 16 | 31.392 | 1.022x |
| EPR | 51.584 | 1.314x | 8 | 58.396 | 1.132x |

EPR scalar count is faster than Huffman in all four scenarios. The gain ranges
from 1.14x on mixed and repeat-rich to 1.70x on gc-skewed. Batch processing is
materially useful only for EPR in this run. The automatic width 16 reaches
57.901 Mbase/s, 1.122x EPR scalar and only about 0.8% below the global best
width 8, so the fixed automatic default remains reasonable.

| Scenario | EPR scalar Mbase/s | EPR vs Huffman | Best width | EPR batch Mbase/s | Batch vs EPR scalar |
|---|---:|---:|---:|---:|---:|
| mixed | 47.844 | 1.141x | 16 | 55.741 | 1.165x |
| gc-skewed | 51.461 | 1.696x | 16 | 60.776 | 1.181x |
| repeat-rich | 51.369 | 1.143x | 32 | 57.915 | 1.127x |
| n-islands | 56.361 | 1.289x | 16 | 61.492 | 1.091x |

The first batch implementation allocated state storage per chunk and copied
the forward encoded patterns. Its preliminary result was slower than scalar.
Using a fixed 256-state stack buffer and moving the forward encoded batch
removed those avoidable costs; only the final result directories above should
be used for conclusions.

## Locate

SA sampling remains 32/64 for every FM backend, so locate differences primarily
reflect the cost of repeated LF/rank navigation rather than a sampling-density
trade-off. Across all scenarios, the combined forward `locate(max_hits=1000)`
results are:

| Backend | QPS | vs Huffman |
|---|---:|---:|
| Huffman | 10,366 | 1.000x |
| Balanced | 7,755 | 0.748x |
| EPR | 12,980 | 1.252x |

Across every scenario and the 1/10/1,000 limits, EPR ranges from 1.16x to
1.70x Huffman, while balanced ranges from 0.71x to 0.89x. SA32 remains much
faster for locate but uses the standalone full-ESA representation; it is not a
like-for-like compressed-index space comparison.

## Interpretation and limits

The useful Movi 2 connection is methodological, not structural. EPR reduces
the small-alphabet rank path and working set inside an SDSL-provided structure;
batch count interleaves independent backward-search chains. The implementation
does not contain move rows, thresholds, RLBWT, PML, custom rank/select, custom
LF, or private-layout prefetching.

Balanced wavelet-tree traversal is a negative optimization on these DNA
workloads and should remain an explicit comparison backend. EPR provides a
real query-speed gain but pays a large size and load-time cost. For that reason
Huffman remains the default; callers may explicitly select EPR when count and
locate throughput matter more than serialized size.

`perf` is not installed in this WSL environment, so no cache-miss, instruction,
or cycle counters were collected. The benchmark supports conclusions about
wall time, CPU time, peak RSS, and serialized size only; it does not claim a
measured cache-miss reduction.

No standard/full profile, large real genome, RLBWT, r-index, PML, disk-cached
construction, alternative SA sampling density, or batch locate experiment was
run in this development stage.
