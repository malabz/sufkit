# SDSL source snapshot

- Upstream: https://github.com/xxsds/sdsl-lite
- Version: `3.0.3`
- Upstream tag: `v3.0.3`
- Release: https://github.com/xxsds/sdsl-lite/releases/tag/v3.0.3
- Tag commit: `d54d38908a14745eb93fd5304fc9b2b9c2542ee9`
- License: BSD-3-Clause
- Vendored content: `include/sdsl`, `AUTHORS`, and `LICENSE`
- Aggregate snapshot SHA-256:
  `95fd85e1d277b598f78eb355eb07b075c026c078956c5e8da640db319b72cd4c`

The version macros in `include/sdsl/version.hpp` report 3.0.3. The source
identity and tag are also recorded in `THIRD_PARTY_NOTICES.md`. The complete
upstream build system, tests, examples, and unrelated repository files are not
vendored.

The full commit object was recovered from the signed upstream release tag,
rather than inferred from the version number. The tag commit and aggregate
content hash are the reproducible identities for this subset.

## Local modifications

No sufkit-specific SDSL source patch is recorded. This is a header-only subset
of the SDSL 3.0.3 snapshot previously audited in RaMAx, with the upstream
authors and license files retained. Integration occurs only in sufkit's own
CMake and `.cpp` files.

Because the upstream archive is not stored beside the subset, future audits
should compare the tag directly before claiming byte-for-byte upstream
identity. Do not format or otherwise normalize these headers.

## Reproducing the aggregate hash

Run from the repository root with GNU `find`, `sort`, `xargs`, and `sha256sum`:

```bash
cd third_party/sdsl
find . -type f ! -name SOURCE.md -printf '%P\n' \
  | LC_ALL=C sort \
  | xargs sha256sum \
  | sha256sum
```

`SOURCE.md` is excluded so provenance text can be updated without changing the
recorded third-party content identity.

## Update procedure

1. Select an explicit upstream tag and record its full commit object.
2. Replace the vendored subset without carrying generated or build output.
3. Retain upstream license and author records.
4. Review all upstream and sufkit-local deltas.
5. Recompute the aggregate hash and update `THIRD_PARTY_NOTICES.md`.
6. Revalidate every fixed SDSL backend and native serialization boundary.
