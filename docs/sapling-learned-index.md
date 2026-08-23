# Sapling-style learned suffix-array lookup

## Scope and provenance

sufkit implements a small piecewise-linear learned index inspired by
[“Sapling: accelerating suffix array queries with learned data
models”](https://academic.oup.com/bioinformatics/article/37/6/744/5941464)
(Bioinformatics 37(6):744-749, DOI 10.1093/bioinformatics/btaa911). The
implementation is clean-room and uses the paper, the authors'
[public manual](https://github.com/mkirsche/sapling/wiki/Sapling-Manual), and
[MIT-licensed repository](https://github.com/mkirsche/sapling) as algorithmic
references. No Sapling source is copied, compiled, linked, or vendored.

The learned model is an optional lookup accelerator. It does not replace the
suffix array, ISA, LCP, suffix-link reuse, or CHILD. It adds no runtime
dependency and does not use a neural network.

## Construction

For each contig, sufkit streams canonical A/C/G/T k-mers with rolling 2-bit
encoding. A window is reset at N and at every contig boundary, so no model key
can cross an ambiguous base, separator, or sentinel. `ISA[position]` supplies
the corresponding SA row.

Keys are partitioned by their high `bucket_bits`. Every bucket records its
minimum observed key and SA row; empty buckets are filled deterministically
from neighboring anchors. A terminal anchor represents the end of the k-mer
key domain and the end of the SA. Anchor keys are 64-bit integers and anchor
rows use the SA coordinate width.

With no explicit bucket count, the largest power-of-two bucket count is chosen
whose serialized model, including its header, fits
`memory_overhead_basis_points` of the raw SA storage. The default is 100 basis
points, or 1%. Explicit `bucket_bits` overrides this budget and is intended for
ablation and very small references.

## Query correctness

The first k query bases form the model key. Integer linear interpolation
predicts an SA row; arithmetic uses an unsigned 128-bit intermediate to avoid
overflow and floating-point nondeterminism. The prediction is never trusted as
an answer.

For both lower and upper bounds, sufkit compares the predicted suffix and
gallops in powers of two until it brackets the true boundary. It then performs
LCP-aware binary search in that local interval. If the pattern is longer than
k, its full range is resolved inside the verified k-prefix interval. Empty
ranges are normalized to `[0,0)`.

Patterns shorter than k use ordinary full-SA binary search. Missing models,
invalid explicit algorithms, and corrupt model sections fail explicitly. A
bad prediction can degrade to a wide search, but cannot produce a false
negative or a wrong interval.

## Interaction with MEM and CHILD

The default SA auxiliary layout is SA+ISA+LCP. MEM auto-selection uses the
suffix-link path and does not consume CHILD. CHILD remains persisted and
available when explicitly requested for suffix-tree-style interval traversal
or later MUM/MAM/maximal-repeat work.

For suffix-link MEM search, learned lookup is used only for the first
minimum-length prefix of a canonical run and when the previous/suffix-link
interval cannot be reused. Consequently, exact lookup can improve strongly
while end-to-end MEM improves only modestly. Benchmark counters report lookup
calls, suffix-link success, previous-empty states, fallback counts, lookup time
proxies, prediction error, and local search windows so this Amdahl-law boundary
is visible.

## Environment

The production implementation needs only the existing sufkit C++17 build:

- Linux or WSL x86_64;
- GCC or Clang;
- CMake;
- bundled libdivsufsort and SDSL;
- zlib and kseq.

Python, PyTorch, CUDA, a GPU, training data, network access, and the Sapling
Makefile are not required. Linux `perf` is optional for external cache and
branch-counter experiments.

## Commands

Build and inspect a learned index:

```bash
sufkit build --type sa \
  --input reference.fa.gz \
  --output reference.learned.sufidx \
  --sa-acceleration suffix-link \
  --learned-index \
  --learned-k 20 \
  --learned-memory-bp 100

sufkit inspect --index reference.learned.sufidx
```

Run exact and MEM ablations:

```bash
sufkit bench --profile quick \
  --scenarios mixed,repeat-rich \
  --methods sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child,fm \
  --pattern-lengths 16,20,21,50,100,200,500 \
  --output-dir results/sapling-exact-quick

sufkit bench --workload mem --profile quick \
  --scenarios mixed,repeat-rich \
  --methods mem-suffix-link-binary,mem-suffix-link-sapling,mem-full \
  --min-lengths 20,50,100 \
  --output-dir results/sapling-mem-quick
```

The learned model remains opt-in until correctness, space, build-cost, and
real-genome performance gates justify a later default change.
