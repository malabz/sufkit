# sufkit

`sufkit` 0.2.0 is a C++17 library and CLI for genome-oriented suffix arrays,
SDSL compressed suffix arrays (FM-indexes), exact pattern search, and
right-maximal exact-match enumeration.

The right-maximal API is **not** MEM search: matches are exact and cannot
extend to the right, but left maximality is not guaranteed. MEM names are
reserved for a future two-sided implementation.

## Capabilities

- FASTA/FASTA.gz input, multi-contig A/C/G/T/N normalization, and stable
  contig-local coordinates;
- divsufsort32/64 and optional CaPS-SA32/64 construction;
- complete or text-position sampled standalone suffix arrays;
- optional ISA, LCP, CHILD, suffix-link reuse, and Sapling-style PWL lookup;
- fixed SDSL Huffman, balanced, and DNA EPR CSA backends;
- scalar and batched FM count, exact locate, and right-maximal streaming;
- CRC-protected, self-contained `.sufidx` persistence and inspection; and
- `add_subdirectory` and installed `find_package` CMake integration.

Huffman FM and SA+ISA+LCP are the conservative defaults. CHILD, Sapling PWL,
sampled SA, balanced FM, and EPR FM are explicit choices. The
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

See the [five-minute quick start](docs/getting-started/quickstart.md) and
[CLI reference](docs/user-guide/cli-reference.md) for SA construction,
right-maximal search, batch queries, and inspection.

## C++ integration

```cmake
add_subdirectory(path/to/sufkit)
target_link_libraries(my_program PRIVATE sufkit::sufkit)
```

or install the project and use:

```cmake
find_package(sufkit 0.2 CONFIG REQUIRED)
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
A/C/G/T after case normalization; right-maximal queries treat every other
symbol as a hard break.

## Compatibility and documentation

Version 0.2.0 is source-incompatible with 0.1.x because public functions and
enumerators adopted Google-style names without compatibility wrappers. Public
include paths, the `sufkit::sufkit` target, main CLI interface, enum values,
and `.sufidx` 1.0–1.3 reading remain stable. See the
[migration guide](docs/development/api-naming-migration-0.2.0.md).

[Documentation](docs/README.md) · [中文说明](README.zh-CN.md) ·
[Contributing](CONTRIBUTING.md) ·
[Benchmark summary](docs/benchmarks/README.md)
