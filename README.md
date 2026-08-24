# sufkit

`sufkit` is a C++17 library and command-line toolkit for genome-oriented
suffix arrays, SDSL compressed suffix arrays (FM-indexes), exact pattern
search, and right-maximal exact match search.

The current right-maximal search is **not** a MEM implementation: it guarantees
exactness and right maximality but does not yet guarantee left maximality. MEM
names are reserved for a future two-sided implementation.

The library is designed for two audiences:

- applications that need an embeddable, move-only C++ index with no SDSL or
  divsufsort types in the public API; and
- users who want to build, query, inspect, and benchmark self-contained
  `.sufidx` files from FASTA or FASTA.gz input.

[中文说明](README.zh-CN.md) · [Documentation](docs/README.md) ·
[Contributing](CONTRIBUTING.md) · [Benchmark summary](docs/benchmarks/README.md)

## Feature status

The released library version is `0.2.0`.

| Capability | Status in 0.2.0 | Default |
|---|---|---|
| divsufsort32/64 suffix-array construction | Released | Used for ordinary SA builds |
| CaPS-SA 32/64 parallel construction | Released in 0.2.0 | Auto-selected only for at least 1 GiB of symbols with more than one thread |
| SA+ISA+LCP suffix-link right-maximal search | Released | Default SA acceleration |
| ESA CHILD construction and traversal | Released, explicit | Never auto-selected |
| SDSL Huffman CSA | Released | Default FM backend |
| SDSL balanced and DNA EPR CSA | Released in 0.2.0 | Explicit only |
| FM batched count | Released in 0.2.0 | Scalar remains the ordinary API |
| Sapling-style piecewise-linear SA lookup | Released in 0.2.0, experimental | Disabled unless requested |
| Text-position sampled SA | Released in 0.2.0, experimental | Disabled (`sampling_rate=1`) unless requested |
| `.sufidx` 1.0/1.1 read support | Released | Old indexes remain readable |
| `.sufidx` 1.2 learned section | Released in 0.2.0 | Written only when PWL is present |
| `.sufidx` 1.3 sampled-SA section | Released in 0.2.0 | Written only when `sampling_rate > 1` |

All FM data structures are provided by the bundled SDSL 3.0.3 implementation.
`sufkit` does not reimplement BWT rank/select, C/Occ, LF mapping, SA sampling,
or FM locate.

## Five-minute start

Requirements are Linux or WSL on an SSE4.2- and POPCNT-capable x86_64 CPU,
GCC or Clang, CMake 3.20 or newer, a C++17 toolchain, and ZLIB. The target flag
is private to sufkit and is not propagated to CMake consumers. Other
third-party sources are bundled.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

Build and query a compressed index:

```bash
./build/release/sufkit build --type fm \
  --input reference.fa.gz --output reference.fm.sufidx

./build/release/sufkit query --index reference.fm.sufidx \
  --pattern ACGTACGT --strand both
```

Build a suffix array and enumerate right-maximal exact matches:

```bash
./build/release/sufkit build --type sa \
  --input reference.fa.gz --output reference.sa.sufidx

./build/release/sufkit right-maximal --index reference.sa.sufidx \
  --query queries.fa.gz --min-length 20 --strand both
```

All public coordinates are zero-based and contig-local. Exact patterns accept
only A/C/G/T after case normalization. Right-maximal queries treat every other
symbol as a hard break.

## C++ integration

From a source tree:

```cmake
add_subdirectory(path/to/sufkit)
target_link_libraries(my_program PRIVATE sufkit::sufkit)
```

After installation:

```bash
cmake --install build/release --prefix /path/to/prefix
```

```cmake
find_package(sufkit CONFIG REQUIRED)
target_link_libraries(my_program PRIVATE sufkit::sufkit)
```

Minimal exact-search example:

```cpp
#include <sufkit/sufkit.hpp>

auto reference = sufkit::GenomeReference::FromFasta("reference.fa.gz");
auto index = sufkit::FmIndex::Build(reference);
index.Save("reference.sufidx");

auto loaded = sufkit::FmIndex::Load("reference.sufidx");
auto result = loaded.Locate("ACGTACGT");
```

Minimal right-maximal exact match example:

```cpp
auto reference = sufkit::GenomeReference::FromFasta("reference.fa.gz");
auto index = sufkit::SuffixArray::Build(reference);  // SA+ISA+LCP

sufkit::RightMaximalOptions options;
options.min_length = 20;
options.strands = sufkit::StrandMode::kBoth;
auto result =
    index.FindRightMaximalMatches("GGGACGTACGTNNNGATTACA", options);
```

To reduce resident and serialized SA memory, set
`SuffixArrayBuildOptions::sampling_rate` or pass `--sa-sampling-rate K`.
The builder still constructs a complete SA before compacting it, so sampling
does not reduce constructor peak memory. Sampled right-maximal search requires
`min_length >= K`; direct `EqualRange()` is intentionally unavailable because
the sampled rows do not represent the complete suffix order.

See the [installation guide](docs/getting-started/installation.md),
[quick start](docs/getting-started/quickstart.md), and
[index selection guide](docs/getting-started/choosing-an-index.md) before
choosing a production backend.

## Project boundaries

- Linux/WSL x86_64 with GCC and Clang is the validated platform.
- The current x86_64 binary requires SSE4.2 and POPCNT; AVX2 and AVX-512 are
  not required.
- FM construction currently uses SDSL's in-memory `construct_im` path.
- CaPS is a shared-memory builder and can use substantially more peak memory
  than divsufsort; it is not a disk-backed constructor.
- Sampled SA is text-position sampling over an initially complete SA, not a
  direct sparse-SA constructor.
- MUM, MAM, r-index/RLBWT, BigBWT/PFP, approximate matching, and
  disk-cached construction are not implemented.
- Synthetic profiles are useful for controlled comparisons but are not a
  substitute for application-specific real-genome measurements.

The [documentation hub](docs/README.md) separates short user guidance from
API contracts, algorithm descriptions, internal architecture, contributor
instructions, index-format details, and evidence-bounded benchmark reports.

The 0.2.0 source interface uses Google-style `PascalCase` functions and
`kPascalCase` enumerators. Existing 0.1.x callers must update their source
using the
[API naming migration guide](docs/development/api-naming-migration-0.2.0.md).
