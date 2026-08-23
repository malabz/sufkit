# C++ style guide

## Policy

sufkit follows the
[Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) for
first-party C++ code. The repository pins clang-format 18 and clang-tidy 18 so
that formatting does not depend on a contributor's system default.

The project deliberately keeps four compatibility exceptions:

- C++17 remains the language baseline;
- public operations continue to report failures with `sufkit::Error`;
- source and header files retain their `.cpp` and `.hpp` extensions; and
- public headers retain `#pragma once` and the existing include paths.

These exceptions are project policy. A style-only change must not convert the
error model, rename files, change public include paths, or require C++20.

## Names

Use the following conventions in new first-party code:

| Entity | Convention | Example |
|---|---|---|
| Class, struct, enum, alias | `PascalCase` | `SuffixArrayBuildOptions` |
| Function and method | `PascalCase` | `EqualRange()` |
| Enum value | `kPascalCase` | `SaBackend::kDivsufsort` |
| Constant | `kPascalCase` | `kSeparator` |
| Local variable and parameter | `snake_case` | `pattern_length` |
| Public structure field | `snake_case` | `total_hits` |
| Private data member | `snake_case_` | `impl_` |
| Macro | `UPPER_SNAKE_CASE` | `SUFKIT_API` |

Do not add old-name forwarding aliases for the 0.2.0 API migration. A public
rename must update the declaration, definition, Doxygen references, tests,
examples, user documentation, and the
[0.2.0 naming migration guide](api-naming-migration-0.2.0.md) together.

Names required by an external C API, an operator overload, a generated macro,
or the C++ `main()` entry point keep the spelling required by that interface.
CLI options, TSV field names, persisted strings, backend IDs, and `.sufidx`
section IDs are compatibility data rather than C++ identifiers and must not be
renamed as part of source formatting.

## Formatting and includes

The repository `.clang-format` is based on the Google style with C++17, an
80-column limit, and comment reflow disabled. Use two spaces in C++ source.
Do not manually align code in a way that clang-format will immediately undo.

Include blocks use this order, with one blank line between groups:

1. the corresponding project header for a `.cpp` file;
2. C system headers;
3. C++ standard-library headers;
4. third-party headers; and
5. other sufkit headers.

Every public header must include what it directly uses and compile on its own.
Do not place `using namespace` directives in headers. Preserve the PIMPL
boundary: public headers must not expose SDSL, divsufsort, CaPS-SA, ParlayLib,
kseq, or zlib types.

## Comments and licenses

New first-party C++ files start with:

```cpp
// SPDX-License-Identifier: MIT
```

Public declarations use concise Doxygen comments for contracts: accepted
input, coordinates, ownership, returned ordering, thread safety, and errors.
Implementation comments explain a reason or invariant that is not evident from
the code. Useful examples include sentinel/separator boundaries, sampled-SA
coordinate recovery, suffix-link fallback, PWL correctness fallback, and
serialization validation.

Do not narrate getters, assignments, ordinary loops, or obvious branches. Keep
comments accurate when code changes. In particular, the current
`RightMaximalMatch` contract guarantees right maximality only and must not be
described as a MEM implementation.

Third-party files keep their original formatting and license notices. Never
run a first-party formatter over `third_party/`.

## Local checks

Configure developer tooling explicitly:

```bash
cmake -S . -B build/style \
  -DSUFKIT_ENABLE_DEVELOPER_TOOLS=ON \
  -DSUFKIT_BUILD_BENCHMARKS=OFF
```

Then run:

```bash
cmake --build build/style --target sufkit-format-check
cmake --build build/style --target sufkit-tidy-check
git diff --check
```

Use `sufkit-format` only when you intend to rewrite the explicit first-party
formatting list. The format targets exclude vendored code, benchmark sources,
generated build trees, and benchmark result documents.

Before submitting a change:

1. keep behavioral changes separate from repository-wide formatting;
2. confirm no third-party snapshot was reformatted;
3. update all public API call sites and documentation;
4. build both add-subdirectory and installed-package consumers when public
   headers change; and
5. run the relevant non-benchmark tests before making a compatibility claim.
