# Local release checklist

The 0.1.0 release candidate is validated locally. No command in this checklist
creates a commit, tag, push, or remote release.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure

./build/release/sufkit bench \
  --profile smoke \
  --output-dir build/bench/smoke

./build/release/sufkit bench \
  --profile quick \
  --output-dir build/bench/quick
```

Before publication, also verify:

- static and shared builds succeed;
- install plus `find_package(sufkit CONFIG REQUIRED)` succeeds;
- installed data contains `LICENSE` and `THIRD_PARTY_NOTICES.md`;
- public headers do not include SDSL or divsufsort headers;
- FM-index code still delegates construction, search, location, and payload
  serialization to the fixed SDSL backend;
- `.sufidx` format remains version 1.0;
- `plan/` remains local and ignored;
- the worktree contains only intended release-candidate changes.
