# Backend reference

This matrix describes the released 0.3.0 capabilities. Experimental labels
describe validation scope, not a different source-tree version.

## Suffix-array constructors

| Public backend | Stored backend | Availability | Threads | Implementation |
|---|---|---:|---:|---|
| `auto_select` | Effective backend | Always | Policy input | CaPS at ≥1 GiB symbols with threads>1 when available; otherwise divsufsort |
| `divsufsort` + 32 | `divsufsort32` ID 1 | Yes | SA build is serial | libdivsufsort 2.0.2 `saidx_t` |
| `divsufsort` + 64 | `divsufsort64` ID 2 | Yes | SA build is serial | libdivsufsort 2.0.2 `saidx64_t` |
| `caps` + 32 | `caps32` ID 3 | Configurable | Yes | CaPS-SA 2597b373 `uint32_t`, ParlayLib e1f1dc0 |
| `caps` + 64 | `caps64` ID 4 | Configurable | Yes | CaPS-SA 2597b373 `uint64_t`, ParlayLib e1f1dc0 |

`SUFKIT_ENABLE_CAPS=ON` is the default. Explicit construction when disabled
throws `unsupported_backend`. Generic stored SA payloads remain readable in a
CaPS-disabled build.

CaPS requires at least 16 logical symbols. Its subproblem count is private
tuning:

```text
min(8192, max(1, symbols/4096), max(1, 128*threads))
```

Each build uses a private Parlay scheduler and does not read or modify
`PARLAY_NUM_THREADS`. CaPS always computes a complete merge-built LCP
internally. Fast and CHILD construction copy the required native-width rows;
with sampling, that array is compacted by the interval minimum between
adjacent retained rows. Low-memory avoids the extra wrapper-side raw-LCP copy
and constructs byte-coded LCP after the CaPS object has been released. ISA and
CHILD otherwise remain part of the common auxiliary pipeline.

The Low-memory profile reduces the final common index, not CaPS's construction
working set. During `construct()`, the vendored implementation owns complete
SA and LCP arrays, same-width SA/LCP work arrays, and partition metadata that
includes a `p*(p+1)` table. The wrapper must also copy the final SA before the
CaPS object can be destroyed because the vendored API exposes only borrowed
pointers. Consequently, do not infer a low CaPS build peak from a 6--8
byte/symbol final Low-memory index; measure the constructor phase separately.

The private divsufsort adapter constructs the complete SA, compacts it when
requested, and returns sampled ISA/LCP phase data to the common builder. This
avoids rebuilding ISA/LCP after backend dispatch. divsufsort itself remains
serial; threads apply only to safe auxiliary work.

## Construction and physical storage

The stored backend ID records constructor provenance, not section layout.
`CoordinateWidth` chooses backend32/backend64; `CoordinateStorageWidth`
chooses native32, split40, split48, or native64 for the primary SA after
construction and acts as the preferred auxiliary width. Legal examples include
divsufsort64 -> native32 and CaPS64 -> split40. An auxiliary one-past marker
may require an independent promotion. A narrow request is accepted only after
all logical positions and permutation invariants fit.

| Storage | Layout | Coordinate bytes | Auto policy |
|---|---|---:|---|
| native32 | `uint32_t[]` | 4 | Fast and Low-memory when valid |
| split40 | `low32[] + high8[]` | 5 | Low-memory for positions above `UINT32_MAX` and below `2^40` |
| split48 | `low32[] + high16[]` | 6 | Low-memory for positions from `2^40` through `2^48-1` |
| native64 | `uint64_t[]` | 8 | Fast above native32; Low-memory above split48 |

Fast defaults to complete SA+ISA+raw LCP and native random-access storage.
Low-memory requires K=1, stores SA+byte-coded LCP, and omits persistent ISA,
CHILD, and PWL. Explicit CHILD/full builds retain raw LCP while constructing
CHILD; Fast also persists that native representation. The 40/48-bit
large-reference
performance envelope is still experimental; this table describes
representation, not a measured MUMmer4 advantage.

## Standalone-SA sampling

`SuffixArrayBuildOptions::sampling_rate=K` is orthogonal to constructor,
construction/storage widths, ESA acceleration, and PWL lookup. K=1 stores
every suffix.
K>1 retains text positions divisible by K and reduces stored SA/ISA/LCP/CHILD
rows to `ceil(n/K)`.

Both backends still form the complete suffix order before compaction. Sampling
therefore reduces loaded/serialized size, not the fundamental full-SA
constructor peak. Exact `Count`/`Locate`, right-maximal compatibility search,
and MEM recover complete results; direct `EqualRange` is unavailable because
the result is not one row interval. Reference-MAM, SMEM, and MUM require K=1.
See the [search guide](../user-guide/search.md).

## FM-index backends

| Public backend | Stored ID | Status | Fixed SDSL type |
|---|---:|---|---|
| `sdsl_csa_wt_huff` | 10 | Available, default | `csa_wt<wt_huff<>,32,64>` |
| `sdsl_csa_wt_balanced` | 11 | Available, explicit | `csa_wt<wt_blcd<>,32,64>` |
| `sdsl_csa_sada` | 12 | Reserved, unavailable | Reserved interpretation |
| `sdsl_csa_wt_epr` | 13 | Available, explicit | `csa_wt<wt_epr<8>,32,64>` |

Templates are private `.cpp` instantiations. Runtime sampling parameters are
not exposed. A template or sampling-density change requires a new backend ID.

## Search accelerations

| Capability | Stored data | Default selection |
|---|---|---|
| Binary exact lookup | SA | Default without eligible PWL |
| LCP-aware binary | SA | Explicit; reuses comparison-boundary LCP values computed during the lookup |
| Sapling PWL | Learned section | Auto only when present and pattern length ≥ k |
| CHILD exact traversal | LCP+CHILD | Explicit only |
| Baseline right-maximal exact match | SA | Fallback for old/minimal index |
| LCP right-maximal exact match | SA+LCP | Auto after suffix-link is unavailable |
| Suffix-link right-maximal exact match | SA+ISA+LCP | Default SA build and right-maximal exact match auto choice |
| CHILD/full right-maximal exact match | CHILD combinations | Explicit only |
| Baseline MEM | SA | Fallback for old/minimal index |
| LCP MEM | SA+LCP | Automatic MEM path with MUMmer-style query skipping |
| Suffix-link MEM | SA+ISA+LCP | Explicit MEM path; automatic Fast MAM path |
| CHILD/full MEM | CHILD combinations | Explicit only |
| Reference-MAM | Complete SA; optional ESA data | Fast auto uses suffix-link; Low-memory auto uses LCP; K=1 only |
| Generalized SMEM | Complete SA; optional ESA data | Suffix-link, then LCP, then baseline; K=1 only |
| Strict MUM | Complete SA; optional ESA data | Reference-MAM plus query-uniqueness cleanup; K=1 only |

For a sampled SA, all row-based accelerations operate over sampled order.
Sampled right-maximal and MEM search additionally require `min_length >= K`.
FM indexes do not expose maximal-match operations.

See [index selection](../getting-started/choosing-an-index.md) and the
[benchmark summary](../benchmarks/README.md) for policy evidence.
