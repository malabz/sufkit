# sufkit

`sufkit` is a C++17 library for genome-oriented suffix arrays and exact
pattern and maximal-exact-match search. Version 0.1.1 contains:

- libdivsufsort32/64 standalone suffix arrays;
- fixed SDSL Huffman, balanced, and DNA EPR compressed suffix-array backends;
- plain/gzip FASTA input through kseq and zlib;
- multi-contig, forward, reverse-complement, and both-strand queries;
- versioned, self-contained `.sufidx` files;
- optional ISA, Kasai LCP, and ESA CHILD structures for suffix arrays;
- baseline, LCP, CHILD, suffix-link, and combined MEM search paths;
- a CLI, tests, and layered deterministic performance benchmarks.

The FM-index is not reimplemented by sufkit.  Construction, backward search,
suffix-array sampling, position recovery, and payload serialization are SDSL
operations.  SDSL types remain private implementation details.

## Build

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

The default top-level build includes the library, CLI, tests, benchmark, and
example.  When sufkit is added as a subdirectory, those auxiliary targets are
off by default.

## CMake integration

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

## Library example

```cpp
#include <sufkit/sufkit.hpp>

auto reference = sufkit::GenomeReference::from_fasta("reference.fa.gz");
auto index = sufkit::FmIndex::build(reference);
index.save("reference.sufidx");

auto loaded = sufkit::FmIndex::load("reference.sufidx");
auto hits = loaded.locate("ACGTACGT");

std::vector<std::string_view> patterns{"ACGT", "GATTACA", "TTTT"};
sufkit::FmBatchOptions batch;
batch.strands = sufkit::StrandMode::both;
batch.batch_width = 16;
auto counts = loaded.count_batch(patterns, batch);
```

MEM search uses a suffix-array index:

```cpp
auto reference = sufkit::GenomeReference::from_fasta("reference.fa.gz");
auto index = sufkit::SuffixArray::build(reference); // full ESA by default

sufkit::MemOptions options;
options.min_length = 20;
options.strands = sufkit::StrandMode::both;
auto mems = index.find_mems(query_sequence, options);
```

References are normalized to A/C/G/T/N.  Query patterns are case-insensitive
but must contain only A/C/G/T.  Returned positions are zero-based and local to
the reported contig.

## CLI

```bash
sufkit build --type sa --input reference.fa.gz --output reference.sa.sufidx
sufkit build --type fm --input reference.fa.gz --output reference.fm.sufidx
sufkit build --type fm --fm-backend sdsl-csa-wt-epr \
  --input reference.fa.gz --output reference.epr.sufidx

sufkit query --index reference.fm.sufidx --pattern ACGTACGT
sufkit query --index reference.fm.sufidx --query queries.fa --strand both
sufkit query --index reference.fm.sufidx --pattern ACGT --count-only
sufkit mem --index reference.sa.sufidx --query queries.fa --min-length 20 --algorithm full

sufkit inspect --index reference.fm.sufidx
sufkit bench --profile quick --output-dir build/bench/quick
sufkit bench --reference reference.fa.gz --queries queries.fa.gz --output-dir build/bench/real
sufkit bench --workload mem --profile quick --output-dir build/bench/mem-quick
```

Existing index files are not overwritten unless `--force` is supplied.

## Current boundaries

- Linux/WSL with GCC or Clang is the validated platform.
- V1 FM construction uses SDSL's in-memory `construct_im` path.
- CaPS, balanced `csa_wt`, `csa_sada`, disk-backed construction, MUM/MAM,
  sparse SA, and BigBWT are roadmap items rather than silent fallbacks.
- Synthetic benchmark profiles are `smoke`, `quick`, `standard`, and `full`,
  with six selectable genome-structure scenarios. Large and real-genome runs
  are always user-triggered.

See [API semantics](docs/api.md), [index format](docs/index-format-v1.md),
[SDSL backend](docs/sdsl-backend.md), [benchmark methodology](docs/benchmark.md),
and the [0.1.1 MEM benchmark results](docs/benchmark-mem-v0.1.1.md).
