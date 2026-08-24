# Construction results

`build-results.tsv` combines SA construction workers and exact-index build records. It reports divsufsort, CaPS, 32/64-bit SA, sampled SA and FM measurements where the adopted stages contain them. `sampling-space.tsv` isolates sampling rates above 1; `caps-scaling.tsv` isolates CaPS thread configurations.

Build time excludes FASTA reading and saving when those fields are separately available. Logical serialized bytes and allocated disk bytes remain separate columns.
