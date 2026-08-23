# ParlayLib source snapshot

- Upstream: https://github.com/cmuparlay/parlaylib
- Commit: `e1f1dc0ccf930492a2723f7fbef8510d35bf57f5`
- License: MIT
- Vendored content: `include/parlay` header tree and `LICENSE`
- Deterministic aggregate header-tree SHA-256:
  `243dba244cfbdc9031b495ae86d08d3bceddfc7a80e5758172cccc5c1b7e38ad`

The snapshot is the same ParlayLib revision audited in the local RaMAx source.
sufkit uses `parlay::execute_with_scheduler()` so a CaPS construction does not
modify `PARLAY_NUM_THREADS` or share a process-global scheduler setting.
