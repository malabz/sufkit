# sufkit

`sufkit` 0.3.0 is a C++17 library and CLI for genome-oriented suffix arrays,
SDSL compressed suffix arrays (FM-indexes), exact pattern search, and maximal
exact-match enumeration.

Version 0.3.0 provides the `Mem*` API for two-sided maximal exact matches,
`Mam*` for reference-unique MEMs, generalized `(l,c)-SMEM` search, and strict
query-and-reference-unique `Mum*` search. The existing
`RightMaximal*` API remains available under its weaker right-only public
contract.

## Capabilities

- FASTA/FASTA.gz input, multi-contig A/C/G/T/N normalization, and stable
  contig-local coordinates;
- divsufsort32/64 and optional CaPS-SA32/64 construction;
- complete or text-position sampled standalone suffix arrays;
- independent SA construction and resident storage widths, including native
  32/64-bit and split 40/48-bit coordinate layouts;
- optional ISA, LCP, CHILD, suffix-link reuse, and Sapling-style PWL lookup;
- Fast SA with raw LCP and Low-memory SA with byte-coded LCP;
- fixed SDSL Huffman, balanced, and DNA EPR CSA backends;
- scalar and batched FM count, exact locate, right-maximal compatibility,
  formal MEM, reference-MAM, generalized SMEM, and strict MUM search;
- CRC-protected, self-contained `.sufidx` persistence and inspection; and
- `add_subdirectory` and installed `find_package` CMake integration.

Huffman FM and the Fast SA profile (complete SA+ISA+raw LCP) are the
conservative defaults. The Low-memory profile keeps a complete SA and
byte-coded LCP while dropping resident ISA, CHILD, and PWL data. Both profiles
use LCP traversal with MUMmer-style query skipping for automatic MEM search;
Fast additionally uses its retained ISA for automatic suffix-link MAM search.
CHILD, Sapling PWL, sampled SA, balanced FM, and EPR FM remain explicit
choices. The
[index-selection guide](docs/getting-started/choosing-an-index.md) explains
their trade-offs.

## Build and query

Requirements are Linux or WSL x86_64 with SSE4.2 and POPCNT, GCC or Clang,
CMake 3.20+, C++17, and ZLIB. Other dependencies are vendored.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

```bash
./build/release/sufkit build --type fm \
  --input reference.fa.gz --output reference.sufidx

./build/release/sufkit query --index reference.sufidx \
  --pattern ACGTACGT --strand both
```

For a standalone SA, construction width and final storage width are separate:

```bash
./build/release/sufkit build --type sa --input reference.fa.gz \
  --output reference.sa.sufidx --sa-profile low-memory \
  --sa-width auto --sa-storage-width auto
```

This permits a safe 64-bit constructor followed by validated 32/40/48-bit
storage. Width selection uses the complete logical symbol count (bases,
contig separators, and sentinel), not the FASTA base count alone.

See the [five-minute quick start](docs/getting-started/quickstart.md) and
[CLI reference](docs/user-guide/cli-reference.md) for SA construction,
right-maximal/MEM/MAM/SMEM/MUM search, batch queries, and inspection.

## C++ integration

```cmake
add_subdirectory(path/to/sufkit)
target_link_libraries(my_program PRIVATE sufkit::sufkit)
```

or install the project and use:

```cmake
find_package(sufkit 0.3 CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE sufkit::sufkit)
```

```cpp
#include <sufkit/sufkit.hpp>

auto reference = sufkit::GenomeReference::FromFasta("reference.fa.gz");
auto index = sufkit::FmIndex::Build(reference);
index.Save("reference.sufidx");

auto loaded = sufkit::FmIndex::Load("reference.sufidx");
auto hits = loaded.Locate("ACGTACGT");
```

Public coordinates are zero-based and contig-local. Exact patterns accept only
A/C/G/T after case normalization; maximal-match queries treat every other
symbol as a hard break.

The standalone [SMEM/MUM example](examples/smem_mum.cpp) demonstrates
generalized reference-coordinate SMEM output and strict MUM output using only
the public API.

## Compatibility and documentation

Version 0.3.0 retains the 0.2 public names and adds MEM, reference-MAM,
generalized SMEM, and strict MUM APIs. Version 0.2.0 was source-incompatible
with 0.1.x because public functions and enumerators adopted Google-style names
without compatibility wrappers; affected consumers should use the
[migration guide](docs/development/api-naming-migration-0.2.0.md). Public
include paths, the `sufkit::sufkit` target, main CLI interface, enum underlying
values, and `.sufidx` 1.0–1.4 reading remain stable. New standalone SAs use
format 1.4 codecs; FM indexes continue to use their 1.0 outer layout.

Split 40/48-bit layouts are implemented and correctness-tested, but real-scale
references above `2^32` logical symbols and end-to-end memory/performance
superiority over MUMmer4 have not yet been validated. Those ranges remain
experimental; the project does not claim a measured advantage there yet.

[Documentation](docs/README.md) · [中文说明](README.zh-CN.md) ·
[Contributing](CONTRIBUTING.md) ·
[Benchmark summary](docs/benchmarks/README.md) ·
[Representative benchmark results](https://github.com/malabz/sufkit/blob/main/benchmarks/README.md)
