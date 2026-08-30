# Architecture

sufkit separates a small, stable C++ surface from backend-heavy construction,
query, and serialization code. Third-party templates and headers stay behind
PIMPL boundaries.

## Component map

```mermaid
flowchart LR
    U[User application] --> API[Public C++ API]
    CLI[sufkit CLI] --> API
    BENCH[Benchmark drivers] --> API

    API --> REF[GenomeReference]
    API --> SA[SuffixArray]
    API --> FM[FmIndex]
    API --> INSPECT[Index inspection]

    REF --> MODEL[Normalized reference model]
    SA --> DIV[libdivsufsort]
    SA --> CAPS[CaPS-SA / ParlayLib]
    SA --> SAMPLE[Optional text-position sampling]
    SAMPLE --> PACK[Validated 32 / 40 / 48 / 64 storage]
    PACK --> ESA[ISA / raw or byte-coded LCP / CHILD / PWL]
    FM --> SDSL[SDSL 3.0.3 CSA]

    SA --> QUERY[Common result semantics]
    FM --> QUERY
    SA --> FORMAT[.sufidx container]
    FM --> FORMAT
    INSPECT --> FORMAT
```

The public API depends on standard C++ types. `SuffixArray` and `FmIndex`
choose private implementations after construction/load. The common semantics
layer owns encoding, strand expansion, coordinate mapping, sorting, merging,
and truncation rules shared across backends.

## Module responsibilities

| Module | Responsibility | Must not own |
|---|---|---|
| `include/sufkit` | Stable declarations, options, results, errors | Third-party implementation types |
| Reference layer | FASTA, normalization, metadata, encoded text, fingerprint | Index-specific structures |
| Query utilities | Exact pattern encoding, reverse complement, deterministic result finalization | SA/FM traversal |
| Suffix-array layer | Constructor dispatch, adaptive coordinate/LCP storage, profiles, ISA/LCP/CHILD/PWL, exact and maximal matches | SDSL CSA internals |
| FM layer | Fixed SDSL variants, scalar/batch backward search, CSA locate | Custom C/Occ/LF/rank/select |
| Serialization | Outer container, CRC, bounded sections, atomic publication | Algorithm policy |
| Inspection | Validate/report persisted metadata and compiled backend availability | Query construction |
| CLI | Argument validation, FASTA query ingestion, TSV output, exit mapping | Hidden alternative semantics |
| Benchmark | Deterministic data, process isolation, correctness gates, summaries | Production API changes |

## Index build data flow

```mermaid
flowchart TD
    F[FASTA / records] --> N[Validate and normalize A/C/G/T/N]
    N --> M[Contig metadata + encoded bytes 1..6]
    M --> K{Index kind}

    K -->|SA| Z[Append explicit zero sentinel]
    Z --> C{Constructor policy}
    C -->|divsufsort| D[Complete SA]
    C -->|CaPS| P[Parallel complete SA + internal merge LCP]
    D --> Q[Optional position sampling]
    P --> Q
    Q --> R[Validate and repack final coordinates]
    R --> A{SA resource profile}
    A -->|Fast| E[Requested persistent ISA / raw LCP]
    A -->|Low-memory| L[Temporary ISA -> compressed LCP]
    E --> X[Optional explicit CHILD / PWL]
    L --> Y[Release temporary ISA]

    K -->|FM| S[SDSL construct_im adds zero sentinel]
    S --> W[Fixed Huffman / balanced / EPR CSA]

    X --> O[Versioned .sufidx writer]
    Y --> O
    W --> O
```

CaPS and divsufsort first produce the same complete suffix order. Fast retains
raw LCP; Low-memory constructs byte-coded LCP after the backend result has been
retained. CaPS supplies its merge-built raw LCP to Fast and to explicit
CHILD/full construction.
Optional sampling compacts rows before the remaining auxiliary pipeline.
Persisted invariants and query results remain constructor independent even
though phase ownership differs.

Construction width is resolved against the selected backend's signed or
unsigned limit. Storage width is resolved separately from the largest logical
position and resource profile. Fast selects native32 then native64;
Low-memory selects the narrowest native32/split40/split48/native64 layout.
Down-packing is allowed only after value, permutation, and inverse checks.
The low-RSS divsufsort64 conversion uses POSIX page-discard hints after typed
values have been copied; non-POSIX builds use the same correct target layout
but may retain both source and target during conversion.

This low-peak down-pack path is specific to the private divsufsort64 adapter.
The vendored CaPS object owns full SA/LCP plus full work arrays and
`p`-dependent partition metadata while constructing. Its API exposes borrowed
result pointers, so the wrapper also needs a separate destination SA before
the object is released. Fast/Low-memory therefore controls retained layout but
does not bound CaPS construction peak RSS.

The LCP layer exposes one immutable logical interface over raw or byte-coded
storage. Byte coding keeps a one-byte row plane, text-ordered overflow anchors,
and a derived guide. Low-memory does not retain the ISA used during LCP
construction.

## Query data flow

Queries are encoded and expanded by strand before backend dispatch. Complete
SA exact search chooses binary, LCP-aware binary, eligible PWL, or explicit
CHILD; sampled SA recovers residue classes; FM search calls the selected SDSL
CSA. Every path converges on common boundary validation, contig mapping,
sorting, strand merging, and retention logic.

Right-maximal search exists only on standalone SA. It splits a query into
canonical runs, initializes or reuses a suffix interval, extends candidates,
verifies exactness and right maximality, and emits through the common stream or
bounded-vector layer. Sampled recovery changes the anchor domain, not output
semantics. Detailed traversal rules are maintained in
[algorithm internals](algorithm-internals.md).

## Load and inspection flow

```mermaid
flowchart TD
    F[.sufidx] --> H[Read fixed header and table]
    H --> V[Validate magic, versions, bounds, overlap, CRC]
    V --> M[Read and validate common metadata]
    M --> K{Index kind / backend ID}
    K -->|SA| S[Read text + legacy integer or 1.4 coordinate codec]
    S --> P[Read optional sampling metadata]
    P --> A[Decode profile/coordinate/LCP codecs and validate invariants]
    K -->|FM| D[Instantiate exact fixed SDSL type]
    D --> B[Bounded native load + CSA validation]
    A --> I[Immutable SuffixArray]
    B --> J[Immutable FmIndex]
    M --> R[Inspection metadata]
```

Inspection shares the container validator but does not construct query-facing
third-party state. Unknown or damaged required data is never silently ignored.

## Ownership and concurrency

- `GenomeReference` owns normalized input during construction.
- An index copies required metadata/payload and does not retain a FASTA path.
- Build temporaries such as construction input or temporary ISA are released
  after the final structure is formed.
- Split40/48 coordinate planes and the LCP guide are immutable after load;
  per-call decode workspaces are not stored in the shared index.
- Public index objects are move-only, preventing accidental expensive copies.
- Const queries are concurrent. Mutable caller-owned statistics are outside
  that guarantee.
- Synchronous maximal-match callbacks execute on the calling thread. Strict
  MUM may buffer MAM candidates before callback delivery because query
  uniqueness is a whole-record property.

## Dependency direction

Allowed direction is public API → internal model/query/serialization → backend
implementation. Backends may use common encoding and metadata but must not
invent different public coordinate or strand semantics. CLI and benchmarks
consume the library; the library never depends on application-layer code.

Read [algorithm internals](algorithm-internals.md) before changing any arrow in
these diagrams.
