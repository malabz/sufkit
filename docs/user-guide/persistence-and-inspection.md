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
5. atomically renames it into place.

An exception removes only the temporary file created by that call. It does not
delete an existing target.

## Format versions

| Format | Contents |
|---|---|
| 1.0 | SA-only or FM container using existing header fields |
| 1.1 | Optional ISA, LCP, and CHILD SA sections |
| 1.2 | Optional learned-SA section, independently combinable with legal ESA layouts |
| 1.3 | Text-position sampled SA metadata and sampled row payload |

The current reader accepts all four. SA-only output remains 1.0, an auxiliary
SA without PWL uses 1.1, and a learned SA uses 1.2. Any SA with K>1 uses 1.3.
FM alternatives use the existing 1.0 outer layout because backend identity is
already stored in the header.

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
| `backend` / `backend_signature` | Stable backend ID interpretation and exact implementation |
| `sdsl_version` | Required native payload version for FM indexes |
| `coordinate_width` | Stored SA-style row width |
| `total_bases` / `text_symbols` | Biological bases and logical encoded symbols |
| `suffix_count` / `sa_sampling_rate` | Stored SA rows and text-position sampling rate |
| `fingerprint` | FNV-1a-64 normalized-content fingerprint |
| `auxiliary_bytes` | Persisted ISA/LCP/CHILD payload bytes |
| `lookup_acceleration` | Binary or Sapling PWL capability |
| `learned_index_bytes` | Serialized learned section size |

Inspection validates container-level structure but is not a replacement for
application-level reference provenance records.
