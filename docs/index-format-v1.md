# `.sufidx` format 1.0

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

