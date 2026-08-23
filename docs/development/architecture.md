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
    SA --> ESA[ISA / LCP / CHILD / PWL]
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
| Suffix-array layer | Constructor dispatch, SA storage, ISA/LCP/CHILD/PWL, exact and MEM | SDSL CSA internals |
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
    C -->|CaPS| P[Parallel complete SA]
    D --> A[Common auxiliary pipeline]
    P --> A
    A --> E[Optional ISA / LCP / CHILD / PWL]

    K -->|FM| S[SDSL construct_im adds zero sentinel]
    S --> W[Fixed Huffman / balanced / EPR CSA]

    E --> O[Versioned .sufidx writer]
    W --> O
```

CaPS and divsufsort differ only in complete-SA construction. CaPS's internal
merge LCP is discarded so persisted auxiliary semantics remain constructor
independent.

## Exact query flow

```mermaid
flowchart TD
    P[Pattern] --> V[Validate and encode A/C/G/T]
    V --> R{Strand mode}
    R --> Q1[Forward encoded query]
    R --> Q2[Reverse complement]
    Q1 --> X[Backend range search]
    Q2 --> X
    X --> C{count or locate}
    C -->|count| T[Complete interval sizes]
    C -->|locate| L[Recover global positions]
    L --> B[Contig-boundary verification]
    B --> M[Map to contig-local coordinates]
    M --> S[Sort, merge strand duplicates, retain first N]
```

SA range search dispatches among binary, LCP-aware binary, eligible PWL, or
explicit CHILD. FM range search uses SDSL `backward_search`. Backend paths must
converge before public result finalization.

## MEM query flow

```mermaid
flowchart TD
    Q[Raw query] --> H[Encode canonical runs and hard breaks]
    H --> O[Forward / reverse orientation]
    O --> I[Initialize min-length interval]
    I --> E[Extend and enumerate candidate rows]
    E --> M[Check left and right maximality]
    M --> C[Map valid reference coordinates]
    C --> SL{Next query position}
    SL -->|ISA+LCP reusable| U[Suffix-link interval reuse]
    SL -->|invalid / empty / hard break| I
    U --> E
    C --> F[Stream callback or bounded sorted vector]
```

The five algorithm modes share maximality and coordinate rules. Only interval
discovery/reuse differs.

## Load and inspection flow

```mermaid
flowchart TD
    F[.sufidx] --> H[Read fixed header and table]
    H --> V[Validate magic, versions, bounds, overlap, CRC]
    V --> M[Read and validate common metadata]
    M --> K{Index kind / backend ID}
    K -->|SA| S[Read text + generic integer SA]
    S --> A[Validate permutation and auxiliary sections]
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
- Public index objects are move-only, preventing accidental expensive copies.
- Const queries are concurrent. Mutable caller-owned statistics are outside
  that guarantee.
- Synchronous MEM callbacks execute on the calling thread.

## Dependency direction

Allowed direction is public API → internal model/query/serialization → backend
implementation. Backends may use common encoding and metadata but must not
invent different public coordinate or strand semantics. CLI and benchmarks
consume the library; the library never depends on application-layer code.

Read [internal invariants](internal-invariants.md) before changing any arrow in
these diagrams.
