# sufkit server benchmark: completed-stage snapshot

**Snapshot status: completed stages.**

This snapshot was generated automatically from audited raw TSV files. It is not a claim that every originally planned stage completed. The dedicated headline right-maximal stage was unfinished and is excluded. Standard and full are representative `mixed` experiments, not six-scenario scans.

## Headline conditions

256 MiB synthetic mixed genome, 4 contigs, seed `20260822`; exact patterns are 100 bp, forward, and locate uses `max_hits=1`. Maximum-right-match queries are 256 bp with `min_length=50`.

## Construction

| Index | Builder | Threads | Build time (s) | Peak RSS (MiB) | Index size (bytes) | bits/base | Speedup |
|---|---|---|---|---|---|---|---|
| SA / divsufsort | divsufsort | 1 | 39.930388 | 7939.484375 | 3489661471 | 104.000016 | 1 |
| SA / CaPS | CaPS | 64 | 9.727652 | 7880.953125 | 3489661471 | 104.000016 | 4.104833 |
| FM / SDSL Huffman | SDSL Huffman | 1 | 37.076965 | 2479.613281 | 158544588 | 4.724997 | 1.076959 |

## Exact search

| Index | Operation | queries/s | ns/query | Peak RSS (MiB) |
|---|---|---|---|---|
| SA32 default | Count | 33078.459509 | 30231.153894 | 5669.242188 |
| SA32 default | Locate-1 | 5201.207871 | 192263.032879 | 5669.347656 |
| FM Huffman | Count | 85753.492212 | 11661.332667 | 431.539062 |
| FM Huffman | Locate-1 | 87.497846 | 11428852.731591 | 431.546875 |

## Maximum right matches

| Method | query bases/s | matches/s | Speedup | Repetitions |
|---|---|---|---|---|
| SA baseline | 189304.950605 | 1352.864872 | 1 | 3 |
| SA suffix-link default | 1123298.214175 | 8027.633136 | 5.933803 | 3 |

Exact headline values use dedicated 3-build/5-query runs. Maximum-right-match values use the audited full/mixed run with 3 measured query repetitions.
