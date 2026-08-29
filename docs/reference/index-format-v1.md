# `.sufidx` format 1.x reference

All outer-container multibyte integers are little-endian. The eight-byte magic
is `SUFKIDX\0`. The fixed header records format, index kind, backend,
construction coordinate width, normalization, sufkit/SDSL versions, aggregate
counts, fingerprint, header CRC32, and a section table. For format 1.4 SA
files, the formerly reserved profile byte records Fast (0) or Low-memory (1).

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
- section 3: complete or sampled SA integer vector.

Format 1.1 may add:

- section 5: ISA;
- section 6: LCP;
- section 7: CHILD.

Legal persisted combinations are SA, SA+LCP, SA+LCP+CHILD, SA+ISA+LCP, and
SA+ISA+LCP+CHILD. In legacy formats 1.1--1.3, ISA and CHILD use the constructor
width. Legacy LCP uses 32 bits when logical-text length fits `uint32_t`, and 64
bits otherwise. These legacy integer sections record count and width before
values. Format 1.4 replaces that implicit rule with the codec headers below;
raw32 may be valid for a large sampled index because an LCP value is bounded
by a suffix extent, while byte-coded32 additionally requires its text-ordered
anchor positions and values to fit the 32-bit codec domain.

Format 1.2 may add section 8 (`learned_sa`) independently of a legal ESA
combination. It stores model ID `sapling_pwl_v1`, k, bucket bits, memory budget
in basis points, coordinate width, anchor count, reference fingerprint,
64-bit keys, and width-matched SA-row anchors. Expected count is
`2^bucket_bits+1`; anchors are monotonic and terminate at key `2^(2k)` and row
stored suffix count.

Format 1.3 adds section 9 (`sa_sampling`) when K>1. It stores:

```text
uint32 sampling_rate
uint64 suffix_count
```

K must be greater than one and `suffix_count` must equal
`ceil(text_symbols/K)`. Every stored suffix position must be in range,
divisible by K, and together form the permutation of sampled text positions.
ISA satisfies `ISA[p/K]=row`; LCP and CHILD refer to adjacent sampled rows.
Learned anchors are bounded by `suffix_count` rather than `text_symbols`.

These layouts describe legacy 1.0–1.3 files. Absence of section 9 means K=1
and `suffix_count=text_symbols`, preserving their interpretation.

## Format 1.4 SA codecs

All newly saved standalone SAs use 1.4. Section IDs and backend IDs do not
change. The outer `coordinate_width` continues to identify the constructor as
32 or 64 bit; each coordinate-bearing section independently declares its
physical codec.

Coordinate codec IDs are permanent:

| ID | Codec | Payload |
|---:|---|---|
| 0 | `native32` | one little-endian `uint32` plane |
| 1 | `native64` | one little-endian `uint64` plane |
| 2 | `split40_low32_high8` | `uint32 low[count]`, then `uint8 high[count]` |
| 3 | `split48_low32_high16` | `uint32 low[count]`, then `uint16 high[count]` |

Each coordinate codec header contains codec-header version, codec ID,
coordinate domain (`symbol_count`), element count, low/high plane byte counts,
and reserved fields. A reconstructed value is:

```text
low[i] | (uint64(high[i]) << 32)
```

The domains are logical text symbols for SA, stored suffix rows for ISA,
stored suffix rows plus the one-past marker for CHILD, and stored suffix rows
plus the terminal anchor for learned rows. Plane lengths must exactly match
the codec and element count; every decoded value must remain inside its
section domain.

Format 1.4 embeds the same coordinate codec after the fixed learned-SA keys,
so learned row width no longer has to equal constructor width.

LCP codec IDs are:

| ID | Codec | Payload |
|---:|---|---|
| 0 | `raw` | one 32- or 64-bit value per stored row |
| 1 | `bytecoded_lcp` | one primary byte per row plus position/value anchor planes |

The LCP codec header records codec-header version, 32/64-bit anchor width,
sampling rate, the fixed 4096-symbol guide block size, logical text symbols,
row count, primary bytes, anchor count, and both anchor-plane byte counts. The
derived guide is rebuilt after load and is not serialized. Values below 255
are stored directly; 255 identifies a value reconstructed from the covering
text-ordered anchor and the generalized-PLCP decrement. The loader validates
all decoded values against adjacent suffix bounds.

The production profile policy uses raw LCP for Fast and byte-coded LCP for
Low-memory. A developer-only benchmark build can force raw LCP for a
Low-memory A/B comparison; that switch does not add a codec or public profile.

Unknown codec versions/IDs produce `version_mismatch`. Malformed domains,
counts, plane lengths, overflows, trailing bytes, or decoded values produce
`corrupt_index`.

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
complete/sampled SA permutation and auxiliary invariants or CSA size/alphabet.
Section-limited streams prevent a backend loader from reading following data.
