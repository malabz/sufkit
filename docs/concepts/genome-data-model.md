# Genome data model

All sufkit indexes share one normalized reference model. Constructor and query
algorithms are interchangeable only because they operate on the same logical
text, boundary rules, and coordinate mapping.

## FASTA records

Plain and gzip FASTA are parsed with kseq/zlib. Input record order defines
`SequenceId`, starting at zero. The first non-empty header token is the unique
sequence name; the rest of the header is retained as description.

References with zero records, empty or duplicate names, or empty sequences are
invalid. `GenomeReference::FromRecords` applies the same validation as FASTA
input.

## Normalization

Reference bases are normalized as follows:

| Input | Stored biological symbol |
|---|---|
| A/a | A |
| C/c | C |
| G/g | G |
| T/t | T |
| N, U, IUPAC, and every other byte | N |

Ambiguous counts are retained per contig and globally. The normalized encoded
buffer, not the original ASCII, is the indexing authority.

Exact patterns are stricter: after case normalization they must contain only
A/C/G/T. right-maximal exact match queries accept lowercase bases and convert every other character
to a hard break.

## Internal alphabet

```text
0  SENTINEL
1  SEPARATOR
2  A
3  C
4  G
5  T
6  N
```

The reference input used for SDSL is:

```text
contig0 SEPARATOR contig1 SEPARATOR ... contigN SEPARATOR
```

It contains no zero. SDSL `construct_im(..., 1)` appends the unique zero
sentinel. Standalone SA constructors receive the same text with one explicit
zero appended. Therefore divsufsort, CaPS, and all SDSL CSA types sort the same
logical text.

N, separator, and sentinel cannot occur in a legal exact pattern. They are
hard boundaries for right-maximal exact match. This prevents cross-contig and cross-ambiguous-region
matches by construction.

## Coordinates

Internal SA/CSA values are global positions in the encoded logical text.
Public results contain:

- a zero-based `SequenceId`; and
- a zero-based position local to that contig.

CLI exact `end` is exclusive. `SequenceInfo::global_offset` is an internal
logical-text mapping aid exposed for inspection; applications should use
contig-local match positions unless they deliberately reconstruct global
coordinates.

Every locate candidate is checked to ensure the full pattern fits inside the
mapped contig. Positions on separators, N, or sentinel are never exposed.

## Strands

Exact `both` searches the original pattern and reverse complement. If they are
identical, each coordinate appears once with strand `both`; other coincident
orientations are merged deterministically.

right-maximal exact match forward and reverse-complement results stay orientation-distinct, even for
palindromic queries. A reverse right-maximal exact match's query position is converted back to the
original forward query coordinate system.

## Fingerprint

The FNV-1a-64 fingerprint is computed from normalized encoded content. It is
used for deterministic dataset identity and persisted-model consistency, not
as a cryptographic authenticity guarantee. Applications needing security or
strong provenance should record an external cryptographic digest as well.
