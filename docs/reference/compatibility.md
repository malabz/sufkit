# Compatibility policy

## Version status

The current released version is 0.2.0.

## Source and binary compatibility

sufkit follows semantic-version intent, but the 0.x series does not promise
ABI compatibility across minor releases. `SOVERSION` is 0. Rebuild consumers
when upgrading a development or minor version.

Public headers are C++17 and hide third-party types. Version 0.2.0 is source
incompatible with 0.1.x because public functions and enumerators were renamed
to the Google-style convention without compatibility wrappers. Consumers must
update their source using the naming migration guide and recompile. The public
include paths, `sufkit::sufkit` CMake target, main CLI interface, enum underlying
values, coordinate and strand semantics, error categories, and
no-silent-fallback contract remain unchanged.

## `.sufidx` compatibility

The Unreleased MEM/reference-MAM APIs add no sections or identifiers. Existing
1.0-1.3 standalone SA files can run the baseline path; stored ISA/LCP/CHILD/PWL
data enables the corresponding accelerations. Reference-MAM requires a
complete SA. FM payloads and behavior are unchanged.

| 0.2.0 reader | 1.0 | 1.1 | 1.2 | 1.3 |
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
implemented for ELF GNU/Clang builds. The current optimized x86_64 binary
requires SSE4.2 and POPCNT; the private compiler option is not exported through
the CMake package. Native Windows/MSVC, macOS, other CPU architectures,
big-endian hosts, and unusual filesystems are not currently release-validated.

The outer format is explicitly little-endian and checked on load. Atomic
publication stays within the target directory: overwrite publication uses an
atomic rename, while no-replace publication uses a same-directory hard link
followed by removal of the private temporary name.
