# Troubleshooting

## Input errors

### Reference FASTA is rejected

Check that it contains at least one record, every record has a non-empty unique
name, and every sequence is non-empty. A damaged gzip stream is an I/O/input
failure rather than an empty reference.

### Exact pattern is rejected

Exact patterns accept only A/C/G/T after case normalization. N, U, IUPAC,
whitespace, and empty strings are invalid. Split ambiguous query regions or use
a maximal-match search, where non-ACGT symbols are hard breaks.

### Query FASTA is rejected

CLI query record names must be non-empty and unique. The first non-empty FASTA
header token is the name; the remainder is the description.

## Construction errors

### `unsupported_backend` for CaPS

The library was configured with `SUFKIT_ENABLE_CAPS=OFF`. Reconfigure with
CaPS enabled or explicitly select divsufsort. An existing CaPS `.sufidx` can
still be loaded because it stores a generic SA payload.

### CaPS fails on a tiny reference

Explicit CaPS requires at least 16 logical symbols. Use divsufsort for tiny
inputs. Automatic selection already does so.

### 32-bit input is too large

Use `CoordinateWidth::kAutoSelect`/`kBits64`, or CLI `--sa-width auto|64`, for
the constructor. divsufsort32 uses a signed limit; CaPS32 uses an unsigned
limit. Final `storage_width` is independent and may remain narrower after a
validated 64-bit construction when the largest logical position fits.

### `threads` appears to have no effect

divsufsort is serial. Threads affect CaPS and selected auxiliary work. Confirm
the effective backend with `inspect` or `IndexInfo::backend`.

### Output already exists

Save is non-overwriting by default. Choose a new path or explicitly use CLI
`--force` / `SaveOptions::overwrite=true`.

## Load and format errors

### `version_mismatch`

FM payloads require the exact recorded SDSL 3.0.3 version. A newer or older
native SDSL serialization is not guessed compatible. A format minor newer
than the current reader also requires a newer sufkit.

### `corrupt_index`

The file failed magic, CRC, section bounds, metadata, SA/auxiliary invariants,
learned-model, or SDSL payload validation. Rebuild from the original reference;
do not bypass validation.

### Wrong index kind

Maximal-match search requires a standalone SA. Exact query supports SA and FM.
Load with the class matching `InspectIndex(...).kind`.

## Search behavior

### No match crosses N or contig boundaries

This is intentional. Reference N, separators, and the sentinel are hard
boundaries. Exact patterns cannot contain their internal codes.

### Explicit PWL or CHILD search fails

The loaded index lacks the required learned or CHILD section. Rebuild with
`--learned-index` or `--sa-acceleration child|full`, or use `auto`/binary.

### SMEM or MUM rejects an otherwise valid SA

Generalized SMEM, reference-MAM, and strict MUM require a complete SA
(`sampling_rate=1`). Rebuild without SA sampling. A Low-memory complete SA is
supported; automatic search then uses its LCP path. FM indexes do not expose
any maximal-match API.

### Locate or maximal-match search uses too much memory

Use `Count` when coordinates are unnecessary. Set `max_hits` or `max_matches`,
use the relevant `ForEach*` callback API, and avoid complete `Locate` or SMEM
coordinate expansion on high-frequency patterns. Accurate totals still require
traversing the complete logical result set.

## Build-system issues

### CMake cannot find ZLIB

Install the platform ZLIB development package or provide a CMake prefix that
defines `ZLIB::ZLIB`.

### A consumer cannot find sufkit

Point `CMAKE_PREFIX_PATH` at the installation prefix or set `sufkit_DIR` to
the directory containing `sufkitConfig.cmake`. Link `sufkit::sufkit`, not an
internal target.

### Doxygen is missing

Normal builds do not require it. Install Doxygen only when configuring with
`SUFKIT_BUILD_DOCS=ON`.

## Benchmark issues

The output directory must not already contain the four result files. A method
mismatch deliberately returns non-zero and retains diagnostic TSVs. Complete
locate is skipped for unsafe high-frequency groups. Standard/full profiles and
real references are user-triggered and can require substantial time and RAM.
