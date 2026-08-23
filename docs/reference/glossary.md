# Glossary

| Term | Meaning in sufkit |
|---|---|
| SA | Suffix array: text positions sorted by suffix content |
| ISA | Inverse suffix array, `ISA[SA[row]]=row` |
| LCP | Adjacent suffix longest-common-prefix array, with `LCP[0]=0` |
| ESA | Enhanced suffix array: SA with auxiliary structures such as LCP/CHILD |
| CHILD | Compact LCP-interval navigation table with up/down/next-L relations |
| CSA | Compressed suffix array; SDSL's FM-capable indexed representation |
| BWT | Burrows–Wheeler transform used by FM-index search |
| FM-index | Backward-search index based on BWT rank and cumulative counts |
| rank/select | Succinct sequence primitives; supplied by SDSL in FM backends |
| LF mapping | Last-to-first BWT navigation; not implemented independently by sufkit |
| MEM | Maximal exact match: exact and not jointly extendable left or right |
| MUM | Maximal unique match; not currently implemented |
| MAM | Maximal almost-unique match; not currently implemented |
| suffix link | Interval relation after deleting the first character of a matched string |
| PWL | Piecewise-linear learned model used to predict an approximate SA row |
| anchor | PWL key/row endpoint used for deterministic interpolation |
| hard break | N, separator, sentinel, or non-ACGT MEM query symbol that a match cannot cross |
| logical text | Encoded contigs plus separators and one sentinel |
| global position | Offset in logical text; mapped to public contig-local coordinates |
| half-open range | `[begin,end)`, including begin and excluding end |
| locate | Recover reference coordinates for rows in a matching interval |
| backend ID | Permanent persisted byte identifying one exact payload interpretation |
| fingerprint | FNV-1a-64 digest of normalized content; deterministic, not cryptographic |
