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
`PARLAY_NUM_THREADS`. CaPS's internal merge LCP is discarded; sufkit's common
ISA/Kasai LCP/CHILD pipeline determines persisted auxiliary data.

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
| Baseline MEM | SA | Fallback for old/minimal index |
| LCP MEM | SA+LCP | Auto after suffix-link is unavailable |
| Suffix-link MEM | SA+ISA+LCP | Default SA build and MEM auto choice |
| CHILD/full MEM | CHILD combinations | Explicit only |

See [index selection](../getting-started/choosing-an-index.md) and the
[benchmark summary](../benchmarks/README.md) for policy evidence.
