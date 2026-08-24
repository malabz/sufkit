# Low-level performance work

This page defines the measurement and compatibility boundary for low-level
optimizations. It is a development contract, not a performance result report.

## CPU baseline

The optimized x86_64 build requires SSE4.2 and POPCNT. With GCC or Clang,
CMake checks support for `-msse4.2` and applies it only to the private `sufkit`
translation units. SDSL 3.0.3 selects its hardware popcount implementation
through `__SSE4_2__`, so its FM rank code is compiled under the same baseline.

The option is deliberately `PRIVATE`:

- consumers of `sufkit::sufkit` do not inherit `-msse4.2`;
- public headers contain no intrinsics or target-specific types;
- bundled libdivsufsort, CaPS-SA, and ParlayLib are not rebuilt with a new
  architecture flag; and
- the project does not use `-march=native`, AVX2, AVX-512, or
  `-ffast-math`.

Running an x86_64 binary on a processor without SSE4.2 and POPCNT is not
supported. Native Windows/MSVC, non-x86 platforms, and runtime multiversioning
remain outside the currently validated release configuration.

## Low-level microbenchmark

`SUFKIT_BUILD_BENCHMARKS=ON` builds the developer-only
`sufkit_low_level_bench` executable. It is not installed and is not part of
the public CLI or library API.

The current harness contains a scalar reference and a 128-bit SSE
compare-and-LCP kernel. It verifies before timing:

- lengths 1--80, 100, 200, 500, and 4096 in the quick profile;
- the 7/8/9, 15/16/17, 31/32/33, and 63/64/65 boundaries;
- mismatch positions 0, 1, 3, 7, 8, 15, 16, the final byte, and equality;
- independent pointer offsets from 0 through 15;
- both comparison directions; and
- unequal input lengths.

Run only the correctness matrix:

```bash
cmake -S . -B build/low-level \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUFKIT_BUILD_BENCHMARKS=ON
cmake --build build/low-level --target sufkit_low_level_bench -j
./build/low-level/sufkit_low_level_bench --verify-only
```

Collect raw smoke or quick repetitions:

```bash
taskset -c 0 ./build/low-level/sufkit_low_level_bench \
  --profile smoke \
  --repetitions 7 \
  --output build/low-level/smoke.tsv

taskset -c 0 ./build/low-level/sufkit_low_level_bench \
  --profile quick \
  --repetitions 7 \
  --output build/low-level/quick.tsv
```

The TSV stores every measured repetition. One warm-up pass is excluded from
timing. Quick measurements use 2,048 rounds per case group; smoke keeps 32
rounds because it is primarily a correctness check. Scalar and production-SSE
measurements swap order on alternating repetitions to reduce fixed ordering
bias. `checksum`, `sse42_compiled`, and `sse42_runtime` make it possible to
reject an invalid comparison before summarizing throughput. The logical-byte
rate counts bytes through the first mismatch rather than all bytes loaded by a
SIMD instruction.

The harness directly instantiates the private production comparison kernel
from `src/sequence_compare.hpp`. Its scalar oracle is an independent
implementation and must not be replaced by a call to production code. The
harness does not construct SA or FM objects and is not part of the installed
public API.

## Query allocation observations

`SUFKIT_BUILD_BENCHMARKS=ON` also builds the non-installed
`sufkit_query_allocation_bench` executable. It provides a deliberately
non-timed view of heap requests made by one logical query pass:

```bash
./build/low-level/sufkit_query_allocation_bench --verify-only

./build/low-level/sufkit_query_allocation_bench \
  --output build/low-level/query-allocations.tsv
```

The executable overrides ordinary, array, and C++17 over-aligned global
`new`/`delete` forms only in this target. The counter is disabled while the
small deterministic reference, SA, FM-index, patterns, and output rows are
constructed. It is enabled separately around one call group for:

- SA count and locate with zero, finite, and unlimited retention;
- bounded and unbounded right-maximal result collection; and
- FM scalar count, batched count, and the same three locate limits.

The TSV columns are `operation`, `allocation_count`, `allocated_bytes`, and
`checksum`. Requested zero-byte allocations are represented by the one byte
actually requested from `malloc`; aligned requests use `posix_memalign` and
the matching delete forms release them with `free`. The checksum gate compares
SA/FM locate results and scalar/batch counts, and separately verifies that the
bounded right-maximal vector is a prefix of the unbounded result.

These values are developer observations for the current compiler, standard
library, bundled dependencies, query, and allocator interception boundary.
They do not measure elapsed time, retained memory, allocator metadata,
fragmentation, stack use, RSS, or allocations performed before/after the
counting scope. A lower requested-byte count does not by itself establish an
end-to-end performance improvement.

## Phase-isolated RSS observations

The unified benchmark's `peak_rss_mb` is intentionally scoped to the complete
method worker lifetime. It cannot by itself distinguish build, load, and query
memory. With `SUFKIT_BUILD_BENCHMARKS=ON`, the non-installed
`sufkit_query_memory_bench` target provides a stricter complementary view:

```bash
./build/low-level/sufkit_query_memory_bench \
  --reference reference.fa.gz \
  --method sa32 \
  --output build/low-level/sa32-phase-rss.tsv

./build/low-level/sufkit_query_memory_bench \
  --reference reference.fa.gz \
  --method fm-huff \
  --output build/low-level/fm-huff-phase-rss.tsv
```

The parent process uses `fork` followed immediately by `exec` of the same
binary for three independent workers. Consequently, a load or query worker
does not inherit the reference, index, allocator arenas, or high-water mark of
the build worker:

- `build` reads and normalizes the reference, builds and saves the index, and
  records current RSS while the reference and index remain alive;
- `load` independently loads the saved index and records `/proc/self/statm`
  current RSS at the `index_ready` boundary; and
- `query` independently loads the index, records `index_ready` RSS, executes a
  fixed count/`locate(0)`/`locate(10)` workload, and records current RSS again
  at `after_queries`.

The parent obtains each worker's Linux `ru_maxrss` with `wait4`. The TSV labels
the scopes as `build_worker_lifetime`, `load_worker_lifetime`, and
`query_worker_lifetime_including_load`; the last value must not be described
as query-only peak RSS. The `index_ready_rss_mb` and `after_rss_mb` columns are
the appropriate current-RSS boundary observations for examining retained
query memory.

The workload is deterministic and the SA32 and Huffman FM-index query rows
must produce the same checksum for the same reference. The output contains a
normalized reference fingerprint but no reference, output, or temporary-file
path. The temporary `.sufidx` and worker reports are created only beside the
requested output and are removed on normal success or failure. This tool is a
developer observation harness: it does not report wall time, allocator
metadata, page residency by mapping, proportional set size, or hardware cache
counters.

`sa32` means a complete divsufsort32 SA with the default SA+ISA+LCP
acceleration; `fm-huff` means the fixed Huffman SDSL CSA. The query worker uses
three fixed 20-base A/C/G/T patterns and runs count, `locate(max_hits=0)`, and
`locate(max_hits=10)` once for each pattern. They are intentionally stable
measurement canaries rather than a claim that the workload represents a
particular genome or application.

## Serialization stream buffers

The private serialization stream buffer is controlled by the advanced CMake
cache variable `SUFKIT_IO_BUFFER_KIB`. Supported development values are 64,
256, and 1024 KiB; the measured default is 1024 KiB. The definition remains
`PRIVATE` to the library target and is not propagated to consumers. It changes
only transient stream buffering, not section boundaries, CRCs, or serialized
bytes.

Integer payloads are still processed in 256 KiB logical blocks. On a
little-endian host, a block whose in-memory and persisted widths match is
written/read directly; the endian-conversion path remains available for other
hosts or widths. Buffer-size selection must be measured separately on `/tmp`
and `/mnt` under WSL. It is not evidence of native-Linux storage throughput.

## Assembly evidence

Compiler flags are necessary but not sufficient evidence that the intended
instructions were emitted. Inspect a Release binary locally:

```bash
objdump -d -C build/low-level/sufkit_low_level_bench |
  grep -E 'pcmpeqb|pmovmskb|popcnt'

objdump -d -C build/low-level/libsufkit.a |
  grep -m 10 -E 'popcnt'
```

Record the compiler version, complete flags, CPU flags, commit, and binary
checksum with any retained measurement. These provenance values are not
automatically embedded in the low-level TSV. The unified exact and
right-maximal benchmark metadata does embed its configured source/toolchain
state, executable checksum, affinity, CPU flags, SSE4.2 state, and path-redacted
command, but those values describe that CLI binary and must not be copied to a
separately built low-level harness. Do not infer instruction use from a CMake
option alone.

Low-level right-maximal comparisons must explicitly pass
`--strands forward,reverse-complement,both`. Each internal strand is a separate
summary/raw dimension and participates in a strand-specific correctness gate;
the historical default remains forward-only for command compatibility.

Suffix-link scan counters are deliberately absent from the default library.
They may be enabled only in a separate static developer build:

```bash
cmake -S . -B build/low-level-diagnostics \
  -DCMAKE_BUILD_TYPE=Release \
  -DSUFKIT_BUILD_CLI=ON \
  -DSUFKIT_BUILD_BENCHMARKS=ON \
  -DSUFKIT_ENABLE_INTERNAL_BENCH_DIAGNOSTICS=ON
```

The advanced option defaults to `OFF`, requires a static CLI/benchmark build,
and is never an installed-interface definition. Timed release measurements
must use a different build directory with the option disabled. Diagnostic wall
time includes a clock call for every suffix-link scan and is suitable only for
deciding whether an additional range-minimum structure is worth investigating.
It must not be reported as ordinary query throughput.

## Temporary-file ownership

Index save acquires its temporary sibling with exclusive creation. On the
supported Linux/WSL platform, `overwrite=false` publishes through a
same-directory hard link, which atomically fails if another writer has already
created the target. Explicit overwrite continues to use atomic replacement.
The final container is fully closed and self-validated before either publish
operation.

The phase-isolated RSS harness owns an atomically created private temporary
directory. Its index, worker reports, and partial TSV never share a predictable
unreserved prefix with another process. Two benchmark processes targeting the
same output are allowed to do their private work, but exactly one can publish;
the loser cannot overwrite or delete the winner's result.

## Evidence limits under WSL

The validated WSL environment supports wall time, CPU time, query throughput,
RSS, serialized sizes, checksums, disassembly, and the separate allocation
observations described above. The comparison-kernel harness itself does not
intercept `operator new` or emit allocation fields. Those measurements can
support claims about end-to-end time, space, and instruction generation on that
environment.

Without a working hardware-counter profiler, WSL measurements do not prove a
reduction in L1/LLC misses, TLB misses, branch misses, NUMA remote access, or
cache-line bouncing. `/mnt` and `/tmp` I/O results must be labeled separately
and must not be presented as native-Linux filesystem throughput. Native Linux
hardware-counter and multi-socket validation is separate future evidence.

## Query concurrency invariants

Built and loaded indexes remain immutable. A low-level optimization must not
introduce a query-time mutex, atomic counter, memory fence, mutable SDSL cache,
shared last-query cache, global thread pool, or large thread-local workspace.
Per-call workspaces are owned by the caller thread. This preserves the current
read-only concurrency model and avoids turning a local microbenchmark gain
into cache-coherence contention in applications.

Measured results and adoption decisions for this implementation are recorded
in [the 0.2.0 low-level performance report](../benchmarks/results/v0.2.0-low-level-performance.md).
