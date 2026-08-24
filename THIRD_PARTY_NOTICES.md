# Third-party notices

## SeqPro

- Source: https://github.com/malabz/seqpro
- Submodule commit: `d593846864b151309a35edb0bf1048302c7cde59`
- License: MIT
- Optional source tree: `third_party/seqpro`

SeqPro is disabled by default and is not used by the MEM/MAM query hot path.
When `SUFKIT_ENABLE_SEQPRO=ON`, it is built only for coordinate/reference
contract validation. Its original license and notices remain in the submodule.
The submodule tracks its upstream `main` branch so maintainers can update it
independently and record the new gitlink commit.

## CaPS-SA

- Source: https://github.com/jamshed/CaPS-SA
- Commit: `2597b37306542cf7c25a8d2f4ee89ec1579b71ba`
- License: MIT
- Vendored files: `third_party/caps-sa`

The original license is reproduced in `third_party/caps-sa/LICENSE`. The small
portability and library-integration patch set is documented in
`third_party/caps-sa/SOURCE.md`.

## ParlayLib

- Source: https://github.com/cmuparlay/parlaylib
- Commit: `e1f1dc0ccf930492a2723f7fbef8510d35bf57f5`
- License: MIT
- Vendored files: `third_party/parlaylib/include/parlay`

The original license is reproduced in `third_party/parlaylib/LICENSE`.

## SDSL 3.0.3

- Source: https://github.com/xxsds/sdsl-lite
- License: BSD-3-Clause
- Vendored headers: `third_party/sdsl/include/sdsl`

The bundled snapshot is the SDSL 3.0.3 header set audited in RaMAx. Its
license and author list are reproduced in `third_party/sdsl/LICENSE` and
`third_party/sdsl/AUTHORS` from the upstream `v3.0.3` tag. Subset identity,
tag commit, aggregate hash, and update rules are recorded in
`third_party/sdsl/SOURCE.md`.

## libdivsufsort 2.0.2

- Source: https://github.com/y-256/libdivsufsort
- License: MIT
- Vendored files: `third_party/libdivsufsort`

The original license is reproduced in `third_party/libdivsufsort/LICENSE`.
The exact upstream commit was not retained; snapshot provenance, content hash,
and that limitation are recorded in `third_party/libdivsufsort/SOURCE.md`.

## kseq

- Source: https://github.com/attractivechaos/klib
- License: MIT
- Vendored header: `third_party/kseq/kseq.h`

The MIT notice is contained at the top of the vendored header and reproduced
in `third_party/kseq/LICENSE`. The immutable upstream revision is not known;
the header date marker and hashes are recorded in `third_party/kseq/SOURCE.md`.
