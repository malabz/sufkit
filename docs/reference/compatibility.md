# Compatibility policy

## Version status

The current released version is 0.3.0.

## Source and binary compatibility

sufkit follows semantic-version intent, but the 0.x series does not promise
ABI compatibility across minor releases. `SOVERSION` is 0. Rebuild consumers
when upgrading a development or minor version.

Public headers are C++17 and hide third-party types. Version 0.3.0 retains the
0.2 public names and adds MEM, reference-MAM, generalized SMEM, and strict MUM
APIs. Version 0.2.0 was source incompatible with 0.1.x because public functions
and enumerators were renamed to the Google-style convention without
compatibility wrappers. Consumers migrating directly from 0.1.x must update
their source using the naming migration guide. The public include paths,
`sufkit::sufkit` CMake target, main CLI interface, enum underlying values,
coordinate and strand semantics, error categories, and no-silent-fallback
contract remain unchanged.

The 0.x series still makes no cross-minor ABI promise. Consumers should
recompile when moving from 0.2 to 0.3 even when their source uses only APIs
that were already present in 0.2.

## `.sufidx` compatibility

The 0.3.0 MEM/reference-MAM/SMEM/MUM APIs add no sections or identifiers.
Adaptive storage adds format 1.4 section codecs while preserving section and
backend IDs. Existing 1.0-1.3 standalone SA files retain their
legacy 32/64-bit interpretation and can run the same supported searches when
they contain the auxiliary structures required by the selected algorithm.

| Reader | 1.0 | 1.1 | 1.2 | 1.3 | 1.4 |
|---|---:|---:|---:|---:|---:|
| Released 0.2.0 | Yes | Yes | Yes | Yes | No |
| Released 0.3.0 SA | Yes | Yes | Yes | Yes | Yes |
| Released 0.3.0 FM | Yes | Header-only compatibility | Header-only compatibility | Header-only compatibility | Header-only compatibility; writer remains 1.0 |

Released 0.3.0 output selection:

- all newly saved standalone SAs: 1.4, with explicit coordinate and LCP
  codecs plus resource profile;
- FM alternatives: 1.0 outer layout, backend identity in the existing byte.

Format 1.4 keeps `coordinate_width` as construction provenance and records
physical width in each coordinate section. Loading 1.0-1.3 infers storage from
the legacy backend width; it does not rewrite a file unless the caller saves a
new index.

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
implemented for ELF GNU/Clang builds. The current optimized x86_64 binary
requires SSE4.2 and POPCNT; the private compiler option is not exported through
the CMake package. Native Windows/MSVC, macOS, other CPU architectures,
big-endian hosts, and unusual filesystems are not currently release-validated.

The outer format is explicitly little-endian and checked on load. Atomic
publication stays within the target directory: overwrite publication uses an
atomic rename, while no-replace publication uses a same-directory hard link
followed by removal of the private temporary name.
