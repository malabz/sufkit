# Local release checklist

The 0.1.1 release candidate is validated locally. No command in this checklist
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

./build/release/sufkit bench \
  --workload mem --profile smoke \
  --methods mem-baseline,mem-lcp,mem-child,mem-suffix-link,mem-full,mummer4 \
  --min-lengths 20,50 --mummer4 /path/to/mummer \
  --output-dir build/bench/mem-smoke
```

Before publication, also verify:

- static and shared builds succeed;
- install plus `find_package(sufkit CONFIG REQUIRED)` succeeds;
- installed data contains `LICENSE` and `THIRD_PARTY_NOTICES.md`;
- public headers do not include SDSL or divsufsort headers;
- FM-index code still delegates construction, search, location, and payload
  serialization to the fixed SDSL backend;
- SA-only `.sufidx` remains 1.0, auxiliary SA indexes use 1.1, and FM payload
  compatibility remains unchanged;
- all MEM algorithms agree with the brute-force oracle and MUMmer4 comparison;
- `plan/` remains local and ignored;
- the worktree contains only intended release-candidate changes.
