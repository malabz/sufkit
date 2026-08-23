# Choosing an index

No single representation is best for every workload. Choose by required
operation first, then by build time, query throughput, and memory budget.

## Decision table

| Primary need | Recommended starting point | Why |
|---|---|---|
| Exact count with a compressed index | Huffman FM | Smallest available FM backend and stable default |
| Faster FM count/locate, space is secondary | EPR FM | Faster in current DNA quick runs, but much larger and slower to load |
| right-maximal exact match search | SA+ISA+LCP | Suffix-link right-maximal exact match is the current default and strongest general path |
| Direct SA row access or ESA research | Standalone SA | Exposes `SuffixAt` and supports optional auxiliary structures |
| Large, multithreaded SA construction | CaPS-SA | Shared-memory parallel construction; measure peak memory first |
| Smaller resident/serialized standalone SA | Text-position sampled SA | Complete exact/right-maximal exact match results with extra query work and constraints |
| Experimental exact lookup acceleration | SA + Sapling PWL | Can narrow binary search for patterns at least model k |
| Suffix-tree-style interval research | SA+LCP+CHILD | Explicit capability; not an automatic performance choice |

## SA constructor: divsufsort or CaPS

`SaBackend::kAutoSelect` uses CaPS only when all conditions hold:

- CaPS was compiled in;
- more than one build thread was requested; and
- the logical text, including separators and sentinel, has at least `2^30`
  symbols.

Otherwise it uses divsufsort. The 1 GiB rule is conservative and is not a
universal measured crossover. In the current 64 MiB quick benchmark CaPS32 at
eight threads was 1.48x faster than divsufsort32 but used about 3.01x peak RSS;
at 1 MiB it was much slower because setup dominated. Run the dedicated SA
construction benchmark on representative hardware before changing the policy.

An explicit `caps` request never silently falls back. A build with
`SUFKIT_ENABLE_CAPS=OFF` returns `unsupported_backend` for construction.

## SA coordinate width

- divsufsort32 uses signed 32-bit `saidx_t` and therefore has the stricter
  representable-length limit.
- CaPS32 uses `uint32_t`.
- 64-bit variants support larger texts at higher SA memory cost.
- `CoordinateWidth::kAutoSelect` chooses the smallest width legal for the
  effective constructor.

The coordinate width affects the stored SA/ISA/CHILD rows. Public reference
coordinates remain `uint64_t`.

## Complete or sampled standalone SA

Sampling rate K is independent of constructor and coordinate width. K=1 is the
complete-SA default. K>1 retains suffix positions divisible by K, so final SA
and row-based auxiliaries use approximately 1/K the entries.

Use sampling when loaded memory or serialized size matters more than the extra
residue recovery work. It does not reduce the complete-SA construction peak.
Exact count/locate remain complete; patterns shorter than K use a direct
contig scan, `EqualRange` is unavailable, and right-maximal exact match requires `min_length>=K`.
See [sampled suffix arrays](../concepts/sampled-suffix-arrays.md).

## SA acceleration layout

| `SaAcceleration` | Persisted data | Main purpose |
|---|---|---|
| `none` | SA | Smallest standalone SA and baseline search |
| `lcp` | SA+LCP | LCP-assisted search and right-maximal exact match ablation |
| `lcp_child` | SA+LCP+CHILD | Explicit ESA interval navigation |
| `lcp_suffix_link` | SA+ISA+LCP | Default suffix-link right-maximal exact match path |
| `full` | SA+ISA+LCP+CHILD | Combined research/ablation capability |

CHILD remains useful for future suffix-tree-style algorithms, repeat
enumeration, MUM/MAM research, and explicit interval traversal. It is not
automatically selected for exact search or right-maximal exact match because current benchmarks
show negative or inconsistent speed effects.

## Learned SA lookup

Sapling PWL is orthogonal to the acceleration layout. It predicts an SA row
from the first k bases, then uses exponential bracketing and local LCP-aware
binary search. Prediction is only a hint, so correctness does not depend on
model accuracy.

Use it when exact patterns are at least k or suffix-link right-maximal exact match has enough root
lookups/fallbacks to amortize the model. It is disabled by default. The current
default model uses k=20 and a budget of 1% of the raw SA payload.

## FM backends

| Backend | Relative role | Current policy |
|---|---|---|
| `sdsl-csa-wt-huff` | Compressed baseline | Default |
| `sdsl-csa-wt-balanced` | Balanced wavelet-tree comparison | Explicit; slower in current DNA runs |
| `sdsl-csa-wt-epr` | Small-alphabet rank speed | Explicit; faster but substantially larger |
| `sdsl-csa-sada` | Reserved ID | Unavailable |

All use fixed SDSL SA/ISA sampling densities 32/64. A backend name maps to one
permanent template signature. SDSL-native payloads require the exact recorded
SDSL 3.0.3 version when loaded.

See the [benchmark summary](../benchmarks/README.md) for measured evidence and
[performance tuning](../user-guide/performance-tuning.md) for workload-level
guidance.
