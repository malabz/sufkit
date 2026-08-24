# kseq source snapshot

- Upstream lineage: https://github.com/attractivechaos/klib
- Vendored file: `kseq.h`
- Header date marker: `Last Modified: 30JAN2023`
- License: MIT
- `kseq.h` SHA-256:
  `9279bfe0f8bfcfe507d2852b838d2156d793f70b76ae9c63e3f6d381239ba901`
- Aggregate snapshot SHA-256, including `LICENSE`:
  `6ab466245068650bfda25c48ea16a807c95988cb0bea4db1c1cb203e595d14a7`

The vendored header is byte-identical to the RaMAx `include/kseq.h` snapshot
from which sufkit was assembled. Neither sufkit nor that copied header records
an immutable upstream commit or release tag. The date marker and hashes above
therefore identify the current snapshot; no klib commit is guessed here.

## Local modifications

No sufkit-specific edit to `kseq.h` is recorded, and the file retains its
original MIT notice. Because the immutable upstream revision is unknown,
byte-for-byte equivalence to a particular current or historical klib revision
cannot be asserted. The standalone `LICENSE` reproduces the header's MIT
terms.

Do not reformat the macro-heavy header. sufkit instantiates it privately and
uses zlib for both plain and gzip FASTA streams.

## Reproducing the aggregate hash

Run from the repository root with GNU `find`, `sort`, `xargs`, and `sha256sum`:

```bash
cd third_party/kseq
find . -type f ! -name SOURCE.md -printf '%P\n' \
  | LC_ALL=C sort \
  | xargs sha256sum \
  | sha256sum
```

`SOURCE.md` is excluded from the recorded third-party content identity.

## Update procedure

1. Select an immutable upstream commit and record the exact source URL.
2. Compare parser return codes and gzip error behavior with the current file.
3. Preserve the upstream MIT notice without formatting the header.
4. Recompute the file and aggregate hashes.
5. Update `THIRD_PARTY_NOTICES.md` and rerun plain/gzip FASTA tests.
