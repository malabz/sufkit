# Persistence and inspection

`.sufidx` is a self-contained file. Loading does not require the original
FASTA path. The file stores normalized contig metadata, the exact backend
identity, format and library versions, a normalized-content fingerprint, and
backend payload sections.

## Save behavior

Save refuses to overwrite an existing target unless `SaveOptions::overwrite`
or CLI `--force` is explicit. The writer:

1. creates `target.sufidx.partial.<pid>.<nonce>` in the target directory;
2. writes a placeholder header and streams each section;
3. computes section and header CRC32 values;
4. closes and rereads the temporary file through the normal parser;
5. publishes it in the same directory: overwrite uses atomic rename; the
   default no-replace path uses a same-directory hard link and removes the
   private temporary name.

An exception removes only the temporary file created by that call. It does not
delete an existing target.

## Format versions

| Format | Contents |
|---|---|
| 1.0 | SA-only or FM container using existing header fields |
| 1.1 | Optional ISA, LCP, and CHILD SA sections |
| 1.2 | Optional learned-SA section, independently combinable with legal ESA layouts |
| 1.3 | Text-position sampled SA metadata and sampled row payload |
| 1.4 | Per-section coordinate/LCP codecs and persisted SA resource profile |

The current reader accepts 1.0 through 1.4. New standalone SAs write 1.4;
legacy 1.0–1.3 files retain their original interpretation when loaded. FM
alternatives continue to use the 1.0 outer layout because backend identity is
already stored in the header.

Format 1.4 separates constructor provenance from physical layout. The base
header records the backend and its 32/64-bit construction width. SA, ISA,
CHILD, and learned row payloads each carry a native32, native64, split40, or
split48 codec header. LCP carries a raw or byte-coded codec header. Section IDs
and backend IDs are unchanged.

Backend IDs are permanent. divsufsort32/64 are 1/2, CaPS32/64 are 3/4, SDSL
Huffman/balanced/reserved Sada/EPR are 10/11/12/13.

## Validation

Before returning an index, loading checks:

- magic, major/minor version, endianness, kind, normalization, and backend;
- header and section CRCs;
- section bounds, overlap, required/unknown sections, and integer overflow;
- metadata IDs, names, offsets, lengths, ambiguous counts, and fingerprint;
- text/sentinel length and SA permutation;
- sampled rate/count and divisibility/permutation constraints;
- legal ISA/LCP/CHILD combinations and internal consistency;
- 1.4 codec ID/version, plane lengths, element/domain counts, and value range;
- raw/byte-coded LCP sampling metadata, anchors, and decoded LCP bounds;
- learned model ID, widths, monotonic anchors, terminal anchor, and budget
  metadata; and
- exact SDSL 3.0.3 payload version, CSA size, and alphabet.

SDSL payload input is bounded to its section. A payload cannot intentionally
consume following sections through the provided stream.

## Inspection

```bash
sufkit inspect --index reference.sufidx
```

Important fields:

| Field | Meaning |
|---|---|
| `kind` | `suffix_array` or `fm_index` |
| `format_version` | Outer `.sufidx` version |
| `library_version` | Writer library version |
| `backend` / `construction_backend` | Compatibility and explicit names for constructor provenance |
| `backend_signature` | Exact constructor implementation signature |
| `sdsl_version` | Required native payload version for FM indexes |
| `coordinate_width` | Legacy name for construction backend width |
| `construction_coordinate_width` | Explicit construction backend width |
| `stored_coordinate_width` | Resident/persisted SA coordinate width |
| `total_bases` / `text_symbols` | Biological bases and logical encoded symbols |
| `suffix_count` / `sa_sampling_rate` | Stored SA rows and text-position sampling rate |
| `fingerprint` | FNV-1a-64 normalized-content fingerprint |
| `auxiliary_bytes` | Resident ISA/LCP/CHILD payload bytes, including the derived LCP guide |
| `sa_resource_profile` | `fast` or `low-memory` |
| `lcp_encoding` | `none`, `raw`, or `byte-coded` |
| `text_bytes` / `sa_bytes` / `isa_bytes` / `lcp_bytes` / `child_bytes` | Resident payload breakdown |
| `resident_core_bytes` | Resident text + SA + auxiliaries + learned payload |
| `lcp_primary_bytes` | One-byte primary plane for byte-coded LCP |
| `lcp_overflow_anchors` / `lcp_overflow_bytes` | Long-LCP anchor count and payload |
| `lcp_guide_bytes` | Derived, non-persisted lookup guide |
| `lookup_acceleration` | Binary or Sapling PWL capability |
| `learned_index_bytes` | Serialized learned section size |

Inspection validates container-level structure but is not a replacement for
application-level reference provenance records.

The byte fields report logical payload bytes, not allocator overhead, and are
more suitable for comparing index layouts than process RSS. A storage formula
alone is not evidence of end-to-end superiority. Native32 Fast/Low-memory and
MUMmer4 have a controlled microbial comparison in the benchmark summary;
split40/48 still lack representative real references above `2^32` symbols and
remain experimental.
