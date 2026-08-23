# `.sufidx` format 1.x

All multibyte outer-container integers are little-endian.  The file starts
with the eight-byte magic `SUFKIDX\0`, followed by format, index kind, backend,
coordinate width, normalization, sufkit/SDSL versions, aggregate counts,
fingerprint, header CRC32, and a section table.

Each section descriptor contains:

- type and required flags;
- byte offset and byte length;
- CRC32.

Common metadata stores each sequence ID, global offset, length, ambiguous-base
count, UTF-8 name, and description.  The loader checks sequential IDs, unique
names, exact contig offsets, total bases, ambiguous bases, and logical text
length before exposing an index.

A suffix-array file has metadata, encoded text, and SA sections.  The text ends
in one zero sentinel, and the SA is validated as a permutation.

Format 1.1 optionally adds section 5 (ISA), section 6 (LCP), and section 7
(CHILD). The only valid combinations are SA only, SA+LCP, SA+LCP+CHILD,
SA+ISA+LCP, and SA+ISA+LCP+CHILD. ISA and CHILD use the SA coordinate width;
LCP uses 32 bits when the logical text fits that width and 64 bits otherwise.
Every integer-vector section begins with its element count and width byte.

SA-only indexes continue to be written as format 1.0. Any auxiliary structure
causes format 1.1 output. The 0.1.1 reader accepts both versions and treats a
1.0 SA as `SaAcceleration::none`, enabling correct baseline MEM search.

An FM-index file has metadata and an SDSL-native CSA section.  Its header also
records SDSL 3.0.3 and the exact backend ID.  CRC and section bounds are checked
before a bounded stream is passed to SDSL `load`.

Save writes `target.sufidx.partial.<pid>.<nonce>` in the target directory,
calculates CRCs, rereads the completed temporary container for validation, and
then renames it atomically.  Existing targets are refused unless overwrite was
explicitly enabled.

The V1 reader requires format major 1 and a minor version no newer than it
supports.  Unknown normalization, backend, kind, required section, bounds,
CRC, or SDSL version is rejected.
