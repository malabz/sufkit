# Backend reference

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
`PARLAY_NUM_THREADS`. When an acceleration needs LCP, sufkit retains the LCP
already produced by CaPS merge stages rather than running a second Kasai pass.
ISA and CHILD remain part of the common auxiliary pipeline. With sampling,
complete CaPS LCP is compacted by taking the interval minimum between adjacent
retained rows.

The private divsufsort adapter constructs the complete SA, compacts it when
requested, and returns sampled ISA/LCP phase data to the common builder. This
avoids rebuilding ISA/LCP after backend dispatch. divsufsort itself remains
serial; threads apply only to safe auxiliary work.

## Standalone-SA sampling

`SuffixArrayBuildOptions::sampling_rate=K` is orthogonal to constructor,
coordinate width, ESA acceleration, and PWL lookup. K=1 stores every suffix.
K>1 retains text positions divisible by K and reduces stored SA/ISA/LCP/CHILD
rows to `ceil(n/K)`.

Both backends still form the complete suffix order before compaction. Sampling
therefore reduces loaded/serialized size, not the fundamental full-SA
constructor peak. Exact count/locate and right-maximal exact match recover complete results; direct
`equal_range` is unavailable for sampled SA because the result is not one row
interval. See [sampled suffix arrays](../concepts/sampled-suffix-arrays.md).

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

For a sampled SA, all row-based accelerations operate over sampled order.
Sampled right-maximal exact match additionally requires `min_length >= K`.

See [index selection](../getting-started/choosing-an-index.md) and the
[benchmark summary](../benchmarks/README.md) for policy evidence.
