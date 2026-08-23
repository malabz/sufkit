# Compatibility policy

## Version status

The released version is 0.1.1. The current `main` development binary still
reports 0.1.1 while CaPS, sampled SA, multiple FM types, FM batch count, and
Sapling PWL are classified as Unreleased. Documentation preserves this
distinction until a new release changes the project version and changelog.

## Source and binary compatibility

sufkit follows semantic-version intent, but the 0.x series does not promise
ABI compatibility across minor releases. `SOVERSION` is 0. Rebuild consumers
when upgrading a development or minor version.

Public headers are C++17 and hide third-party types. Source compatibility is a
goal, but experimental APIs and options may change before their first release.
Released coordinate, strand, error-category, and no-silent-fallback semantics
should be treated as stable contracts.

## `.sufidx` compatibility

| Reader on current `main` | 1.0 | 1.1 | 1.2 | 1.3 |
|---|---:|---:|---:|---:|
| SA | Yes | Yes | Yes | Yes |
| FM | Yes | Header minor accepted; no SA-only sections | Header minor accepted; no SA-only sections | Header minor accepted; no SA-only sections |

Output selection:

- SA only: 1.0;
- SA plus ISA/LCP/CHILD: 1.1;
- any SA with a learned section: 1.2;
- any SA with sampling rate greater than one: 1.3, including sampled learned
  and ESA combinations;
- FM alternatives: 1.0 outer layout, backend identity in the existing byte.

Readers reject a newer major or unsupported minor. Unknown required sections,
backend IDs, normalization IDs, or illegal section combinations are not
ignored.

## Backend compatibility

Stored backend IDs are permanent. A build without CaPS can load CaPS32/64
generic SA payloads. A build without an FM template cannot infer or convert its
SDSL-native payload.

SDSL does not guarantee cross-version native serialization. FM load requires
the exact recorded bundled version 3.0.3. A future upgrade must use an explicit
compatibility decision, new tests, and a new backend identity if the payload
interpretation changes.

## Platform support

Linux and WSL x86_64 with GCC/Clang are tested. Shared-library symbol hiding is
implemented for ELF GNU/Clang builds. Native Windows/MSVC, macOS, other CPU
architectures, big-endian hosts, and unusual filesystems are not currently
release-validated.

The outer format is explicitly little-endian and checked on load. Atomic
publication assumes rename semantics within the target directory.
