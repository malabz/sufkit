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

No partitioning, suffix comparison, merge, SA, or LCP algorithm semantics were
otherwise changed. The patched file hashes are:

- `include/Suffix_Array.hpp`:
  `01de30ec02f2e7b234a5b649073c075b083c87404b8d4b588b004b4761687d68`
- `src/Suffix_Array.cpp`:
  `043521f307d8a9c3fe675741fcd131dd5fba4e5639ae143847a4ada290bf8684`
