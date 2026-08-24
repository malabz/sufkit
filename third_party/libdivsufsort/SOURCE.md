# libdivsufsort source snapshot

- Upstream project: https://github.com/y-256/libdivsufsort
- Reported project version: `2.0.2`
- License: MIT
- Vendored content: four C implementation files, generated/configured public
  headers, the private implementation header, and `LICENSE`
- Aggregate snapshot SHA-256:
  `ec20b66cad833851a34a65210f91eda73ddd3f09913bc44fade561280c044ff1`

`include/config.h` reports project version 2.0.2. The exact y-256 upstream
commit was not retained in the sufkit repository and must not be inferred from
that version string.

The available vendoring provenance is more specific: the four implementation
files and private header match the RaMAx libdivsufsort submodule revision
`22e6b23e619ff50fd086844b6e618d53ca9d53bd`, whose configured source was the
`simongog/libdivsufsort` fork. The public and configuration headers match the
headers generated for that same RaMAx checkout. This records the origin of the
current bytes; it does not claim that the commit object belongs to the y-256
repository.

## Local modifications

No changes to `divsufsort.c`, `sssort.c`, `trsort.c`, or `utils.c` were found
relative to the recorded RaMAx snapshot. sufkit vendors only the implementation
and headers needed for its private static integration; upstream CMake files,
examples, package metadata, and utilities are omitted. Configured headers are
stored directly so the build remains offline.

No separate sufkit algorithm patch is recorded. Do not apply the first-party
C++ formatter to this C source.

## Reproducing the aggregate hash

Run from the repository root with GNU `find`, `sort`, `xargs`, and `sha256sum`:

```bash
cd third_party/libdivsufsort
find . -type f ! -name SOURCE.md -printf '%P\n' \
  | LC_ALL=C sort \
  | xargs sha256sum \
  | sha256sum
```

`SOURCE.md` is excluded from the recorded third-party content identity.

## Update procedure

1. Select and record an explicit y-256 upstream commit or immutable tag.
2. Regenerate public/configuration headers for both 32-bit and 64-bit APIs.
3. Review and document every delta from upstream.
4. Retain the upstream MIT license.
5. Recompute the aggregate hash and update `THIRD_PARTY_NOTICES.md`.
6. Revalidate SA32, SA64, exact search, persistence, and consumer builds.
