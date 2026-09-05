# CaPS-SA source snapshot

- Upstream: https://github.com/jamshed/CaPS-SA
- Commit: `2597b37306542cf7c25a8d2f4ee89ec1579b71ba`
- License: MIT
- Original `include/Suffix_Array.hpp` SHA-256:
  `0fad37d6d3da2905a98a9785b612b05efd2fdcdd74d2012ea47ae8656064e8fd`
- Original `src/Suffix_Array.cpp` SHA-256:
  `a1a8704b8d27e3c954ff0f33f3f79740d0a626c57e2e2f46b866ba92f89063a3`

The upstream build system is intentionally not vendored because it downloads
ParlayLib at configure time and applies machine-specific compiler flags.

## sufkit portability and library patches

- Guard AVX2 intrinsics with `__AVX2__` and provide a scalar 32-byte comparison.
- Throw on allocation failure instead of continuing with a null pointer.
- Replace the constructor's process exit with `std::invalid_argument`.
- Disable progress/timing output unless `CAPS_SA_ENABLE_DIAGNOSTICS` is defined.
- Treat a zero-pivot sampling request as an empty operation, avoiding the
  upstream `n / 0` failure when a valid small input uses one subproblem.

The construction patch adds caller-owned output buffers with exception cleanup,
an immutable directory of exact equal-symbol runs (at least 256 symbols), and
optional synchronous stage notifications. Run skipping preserves complete LCP
values and every input symbol; it never truncates context or drops suffixes.
Partitioning, lexicographic ordering, and SA/LCP output semantics are unchanged.
The patched file hashes are:

- `include/Suffix_Array.hpp`:
  `4ee040c365ec421e765aa44382b9a7d546759695f504f678902c91c9269e610b`
- `src/Suffix_Array.cpp`:
  `a3fb88bb44de4067d02cdf0747cd48579cc4f015b009ecce3052f0d404b3aede`
