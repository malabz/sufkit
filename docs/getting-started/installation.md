# Installation and integration

## Supported environment

The validated development platform is Linux or WSL on x86_64 with GCC or
Clang. The x86_64 build requires an SSE4.2- and POPCNT-capable processor.
sufkit also requires:

- CMake 3.20 or newer;
- a C++17 compiler;
- a C compiler for bundled libdivsufsort;
- ZLIB development headers and library; and
- POSIX threads when the CaPS backend is enabled.

SDSL 3.0.3, libdivsufsort 2.0.2, kseq, CaPS-SA, and the required ParlayLib
snapshot are vendored. SeqPro is an optional git submodule used only for local
contract tests. A normal build does not download dependencies.

Native Windows/MSVC and non-x86_64 platforms are not release-tested. The
public interface is portable C++17, but do not treat an untested platform as a
compatibility promise.

## Top-level build

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

Top-level defaults enable the CLI, tests, benchmarks, examples, and CaPS. The
available options are:

| Option | Top-level default | Subproject default | Effect |
|---|---:|---:|---|
| `SUFKIT_BUILD_CLI` | ON | OFF | Build the `sufkit` executable |
| `SUFKIT_BUILD_TESTS` | ON | OFF | Build CTest targets |
| `SUFKIT_BUILD_BENCHMARKS` | ON | OFF | Build benchmark commands, SA construction benchmark, and developer microbenchmark |
| `SUFKIT_BUILD_EXAMPLES` | ON | OFF | Compile consumer examples |
| `SUFKIT_ENABLE_CAPS` | ON | ON | Compile the bundled CaPS-SA constructor |
| `SUFKIT_ENABLE_SEQPRO` | OFF | OFF | Build optional SeqPro coordinate/reference contract checks |
| `SUFKIT_BUILD_DOCS` | OFF | OFF | Build local Doxygen API HTML |

`SUFKIT_BUILD_DOCS` requires a local Doxygen installation only when enabled.
The low-level benchmark is developer-only and is not installed.

On x86_64 GCC/Clang builds, CMake verifies `-msse4.2` support and applies it
privately to the library implementation. Consumers linking
`sufkit::sufkit` do not inherit this compiler option.

To build only a static library:

```bash
cmake -S . -B build/library \
  -DSUFKIT_BUILD_CLI=OFF \
  -DSUFKIT_BUILD_TESTS=OFF \
  -DSUFKIT_BUILD_BENCHMARKS=OFF \
  -DSUFKIT_BUILD_EXAMPLES=OFF
cmake --build build/library -j
```

Use `-DBUILD_SHARED_LIBS=ON` for a shared library. On ELF platforms the shared
build hides private symbols, including vendored implementation symbols.

## Optional CaPS support

CaPS is enabled by default. Disable it when binary size, compile time, or a
non-POSIX environment matters more than parallel SA construction:

```bash
cmake -S . -B build/no-caps -DSUFKIT_ENABLE_CAPS=OFF
cmake --build build/no-caps -j
```

Such a build cannot construct new CaPS indexes. It can still load, inspect,
query, and re-save `.sufidx` files whose stored backend is `caps32` or
`caps64`, because the persisted payload is the generic integer SA rather than
private CaPS state.

## Optional SeqPro contract checks

Initialize the submodule and enable the option only when validating SeqPro
interoperation:

```bash
git submodule update --init third_party/seqpro
cmake -S . -B build/seqpro -DSUFKIT_ENABLE_SEQPRO=ON \
  -DSUFKIT_BUILD_BENCHMARKS=OFF
cmake --build build/seqpro -j
ctest --test-dir build/seqpro --output-on-failure
```

This does not change public headers, install dependencies, `.sufidx`, or the
maximal-match query path. SeqPro is used only to cross-check FASTA base and
sequence-text coordinate semantics.

## `add_subdirectory`

```cmake
cmake_minimum_required(VERSION 3.20)
project(consumer LANGUAGES CXX)

add_subdirectory(path/to/sufkit)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE sufkit::sufkit)
target_compile_features(consumer PRIVATE cxx_std_17)
```

Auxiliary sufkit targets default to OFF as a subproject. Set any desired
option before `add_subdirectory`.

## Install and `find_package`

```bash
cmake --preset release
cmake --build --preset release -j
cmake --install build/release --prefix /opt/sufkit
```

Consumer CMake:

```cmake
find_package(sufkit 0.3 CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE sufkit::sufkit)
```

Configure the consumer with `-DCMAKE_PREFIX_PATH=/opt/sufkit` if the prefix is
not in CMake's normal search path. ZLIB is a public package dependency. A
package built with CaPS enabled also records the Threads dependency.

The install contains the public headers, library, CMake package files, CLI and
SA construction benchmark when built, licenses, Markdown documentation, and
the standalone `examples/smem_mum.cpp` source.

## Sanitizer build

```bash
cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure
```

The preset enables AddressSanitizer and UndefinedBehaviorSanitizer. It is for
local validation rather than representative timing.
