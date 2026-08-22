# Benchmark methodology

`sufkit bench --quick --output quick.tsv` generates four deterministic contigs
totalling approximately 4 MiB and 1,000 A/C/G/T patterns of length 20, 50, or
100.  The fixed generator seed is 20260822.  The reference contains random
base composition, repeat copies, and N islands.

The compared methods are `naive`, `sa32`, `sa64`, and `fm`.  Each runs in a
separate child process.  `wait4` supplies per-worker peak RSS, so one method's
high-water mark does not contaminate another.  One warm-up precedes five query
measurements; median count and locate throughput is reported.  Locate
materializes at most 1,000 hits per query while full counts remain unbounded.

Before writing a successful table, the driver requires every selected method
to have identical total hits, reported hits, and a checksum over sorted
coordinates.  Performance values are descriptive and are not cross-machine
acceptance thresholds.

For fast automated validation:

```bash
sufkit bench --smoke --output smoke.tsv
```

For user data:

```bash
sufkit bench \
  --reference reference.fa.gz \
  --queries queries.fa.gz \
  --methods naive,sa32,sa64,fm \
  --output genome.tsv
```

Large real-genome runs are deliberately not part of automated V1 validation.

