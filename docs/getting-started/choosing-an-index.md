# Choosing an index

No single representation is best for every workload. Choose by required
operation first, then by build time, query throughput, and memory budget.

## Decision table

| Primary need | Recommended starting point | Why |
|---|---|---|
| Exact count with a compressed index | Huffman FM | Smallest available FM backend and stable default |
| Faster FM count/locate, space is secondary | EPR FM | Faster in current DNA quick runs, but much larger and slower to load |
| right-maximal exact match search | SA+ISA+LCP | Suffix-link right-maximal exact match is the current default and strongest general path |
| Lowest standalone-SA resident memory | Low-memory SA | Complete SA+byte-coded LCP; no resident ISA, CHILD, or PWL |
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

Low-memory is a resident-index policy, not a low-peak CaPS constructor. CaPS
still owns complete native-width SA/LCP arrays, two full work arrays, and
partition metadata during construction; the wrapper then copies the final SA
out of the non-owning CaPS interface. Use divsufsort when construction memory
is the primary constraint, or measure CaPS's isolated build peak before
selecting it.

An explicit `caps` request never silently falls back. A build with
`SUFKIT_ENABLE_CAPS=OFF` returns `unsupported_backend` for construction.

## Construction width and storage width

These are deliberately separate decisions:

- `CoordinateWidth` / `--sa-width` selects the constructor integer type.
  divsufsort32 uses signed `saidx_t` and is limited near `INT32_MAX`; CaPS32
  uses `uint32_t` and can cover a larger range. Both have 64-bit alternatives.
- `CoordinateStorageWidth` / `--sa-storage-width` selects the resident and
  persisted SA layout and preferred auxiliary width after construction. It
  supports native 32/64-bit arrays and split 40/48-bit structure-of-arrays.
  An auxiliary with a one-past row marker may promote independently when that
  marker cannot fit the primary SA width.

Selection uses the logical symbol count:

```text
biological bases + one separator per contig + one sentinel
```

It does not use biological bases alone. A 64-bit constructor can therefore
build safely and then down-pack to native32 when the maximum position still
fits `uint32_t`. Every coordinate and the SA permutation are validated before
the wider build representation is released.

Automatic storage differs by profile: Fast selects native32 when possible and
otherwise native64; Low-memory selects the narrowest of 32, 40, 48, and 64.
Explicit widths are rejected when too narrow. Split40 stores low32/high8
planes (5 bytes per coordinate); split48 stores low32/high16 planes (6 bytes).

The 40/48-bit implementations and persistence are correctness-tested, but
real references above `2^32` logical symbols have not yet completed the full
release benchmark matrix. Treat those ranges, and any claim of outperforming
MUMmer4 there, as experimental rather than established.

## Fast and Low-memory profiles

| Profile | Resident structures | Automatic width | MEM auto | MAM auto |
|---|---|---|---|---|
| `fast` | Requested layout; preset defaults to complete SA+ISA+raw LCP | native32, then native64 | LCP + auto-skip | Suffix-link |
| `low-memory` | Complete SA+LCP only | Narrowest 32/40/48/64 | LCP + auto-skip | LCP |

Low-memory overrides acceleration to LCP, disables the learned model, and
currently requires `sampling_rate=1`. The ISA needed to construct LCP is
temporary. Low-memory LCP is stored byte-coded; rare long values use compact
anchors and a derived in-memory guide. Fast keeps raw LCP because byte coding
did not satisfy its three-percent per-workload regression gate in pinned quick
measurements.

Fast remains the default because its retained ISA provides a substantial MAM
advantage on adjacent query positions. MEM does not automatically consume that
ISA: sparse query anchors made LCP plus MUMmer-style auto-skip the more robust
path in mixed and repeat-rich quick measurements. Explicit algorithm choices
remain available for controlled workloads and benchmark ablations.

For K=1, these first-order payload budgets exclude overflow anchors and other
metadata. Fast includes native raw LCP; Low-memory includes a one-byte primary
LCP plane:

| Stored width | Fast SA+ISA+LCP | Low-memory SA+LCP |
|---:|---:|---:|
| 32 | about 13 bytes/symbol | about 6 bytes/symbol |
| 40 | about 19 bytes/symbol | about 7 bytes/symbol |
| 48 | about 21 bytes/symbol | about 8 bytes/symbol |
| 64 | about 25 bytes/symbol | about 10 bytes/symbol |

These are layout estimates, not measured RSS: long-LCP anchors, metadata,
allocators, results, construction buffers, and process runtime add memory.

Public reference coordinates remain `uint64_t` regardless of constructor or
storage width.

## Complete or sampled standalone SA

Sampling rate K is independent of constructor and storage width. K=1 is the
complete-SA default. K>1 retains suffix positions divisible by K, so final SA
and row-based auxiliaries use approximately 1/K the entries.

Use sampling when loaded memory or serialized size matters more than the extra
residue recovery work. It does not reduce the complete-SA construction peak.
Exact count/locate remain complete; patterns shorter than K use a direct
contig scan, `EqualRange` is unavailable, and right-maximal exact match requires `min_length>=K`.
See the [search guide](../user-guide/search.md) for sampled query behavior.

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

## Workload-level tuning

- Use `Count()` when coordinates are not needed; high-hit `Locate()` is often
  dominated by recovery, mapping, sorting, and output.
- LCP-aware binary can reduce repeated comparisons. PWL is relevant only when
  patterns reach model k and its root/fallback lookups are frequent enough.
- Suffix-link reuse is the default right-maximal path. Use the streaming API
  when matches can be consumed online, or `max_matches` to bound retained
  vector memory.
- FM Huffman is the space-oriented default. EPR can improve DNA rank/query
  throughput at substantially greater serialized size and load time. Batch
  count should be measured at several widths rather than assumed faster.
- Sampling rate K reduces final SA-based structures by roughly K but adds
  residue recovery and does not reduce complete-SA construction peak memory.
- Use `--sa-profile low-memory` when resident memory is the priority; use
  `fast` when suffix-link throughput is the priority. Do not infer large-genome
  superiority from the storage byte formula alone.

The validated x86_64 build requires SSE4.2 and POPCNT; private compiler flags
do not propagate to consumers. Use Release builds and representative reference
and query distributions. Compare complete counts and stable result checksums
before timing, keep build/load/count/locate/right-maximal measurements
separate, and treat synthetic quick results as hypotheses rather than
deployment thresholds.

See the [benchmark summary](../benchmarks/README.md) for current measurements
and the [methodology](../benchmarks/methodology.md) for reproducible runs.
