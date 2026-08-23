# `.sufidx` format 1.x reference

All outer-container multibyte integers are little-endian. The eight-byte magic
is `SUFKIDX\0`. The fixed header records format, index kind, backend,
coordinate width, normalization, sufkit/SDSL versions, aggregate counts,
fingerprint, header CRC32, and a section table.

Each section descriptor contains type/required flags, byte offset, byte length,
and CRC32. Section ranges must be in-file and non-overlapping. Unknown required
sections are rejected.

## Common metadata

Metadata stores each sequence ID, global offset, length, ambiguous-base count,
UTF-8 name, and description. Loading validates sequential IDs, unique non-empty
names, exact contig offsets, aggregate counts, and logical text length.

## Backend IDs

| ID | Interpretation |
|---:|---|
| 1 | divsufsort32 |
| 2 | divsufsort64 |
| 3 | CaPS-SA `uint32_t` generic SA payload |
| 4 | CaPS-SA `uint64_t` generic SA payload |
| 10 | SDSL `csa_wt<wt_huff<>,32,64>` |
| 11 | SDSL `csa_wt<wt_blcd<>,32,64>` |
| 12 | Reserved SDSL `csa_sada` identity |
| 13 | SDSL `csa_wt<wt_epr<8>,32,64>` |

Constructor identity is recorded even when payload layout is generic. A
CaPS-disabled build can interpret IDs 3/4 because their section is an ordinary
integer SA.

## Suffix-array sections

Required:

- section 1: common metadata;
- section 2: encoded text ending in one zero sentinel;
- section 3: full SA integer vector.

Format 1.1 may add:

- section 5: ISA;
- section 6: LCP;
- section 7: CHILD.

Legal persisted combinations are SA, SA+LCP, SA+LCP+CHILD, SA+ISA+LCP, and
SA+ISA+LCP+CHILD. ISA and CHILD use SA row width. LCP uses 32 bits when logical
text length fits `uint32_t`, otherwise 64 bits. Integer sections record count
and width before values.

Format 1.2 may add section 8 (`learned_sa`) independently of a legal ESA
combination. It stores model ID `sapling_pwl_v1`, k, bucket bits, memory budget
in basis points, coordinate width, anchor count, reference fingerprint,
64-bit keys, and width-matched SA-row anchors. Expected count is
`2^bucket_bits+1`; anchors are monotonic and terminate at key `2^(2k)` and row
logical-text length.

SA-only output is 1.0; auxiliary output is 1.1; any learned output is 1.2.

## FM sections

FM files contain metadata and one bounded SDSL-native CSA section. Header
backend and exact SDSL 3.0.3 version determine interpretation. SDSL
`serialize(std::ostream&)` and `load(std::istream&)` own the payload format.

Alternative fixed FM backends add no outer fields or sections and therefore
continue to write format 1.0. A reserved or uncompiled backend cannot be
guessed from payload bytes.

## Atomic write and validation

The writer streams to `target.sufidx.partial.<pid>.<nonce>`, computes CRCs,
backfills the header, closes and validates the complete temporary container,
then renames within the target directory. Overwrite is opt-in.

The reader validates outer structure before backend payload load, then checks
SA permutation and auxiliary invariants or CSA size/alphabet. Section-limited
streams prevent a backend loader from reading following data.
