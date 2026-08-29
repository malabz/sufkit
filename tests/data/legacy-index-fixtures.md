# Legacy `.sufidx` fixtures

The four `legacy-sa-v1.*.fixture` files are deterministic `.sufidx`
compatibility
fixtures produced by the sufkit binary built from commit
`fd1abbdb4a486abcbca3be7d915f43d3638f8b16`. They use
`tests/data/reference.fa` and cover each suffix-array container format that
predates the 1.4 section codecs.

```text
legacy-sa-v1.0.fixture  SA only
legacy-sa-v1.1.fixture  SA + ISA + LCP + CHILD
legacy-sa-v1.2.fixture  SA + ISA + LCP + Sapling PWL
legacy-sa-v1.3.fixture  sampled SA (K=2) + ISA + LCP + CHILD
```

SHA-256:

```text
42deb6ac55b35ba480dedf0568598a19394f0fa61fd0fbf3cdf7304c980a2f7f  legacy-sa-v1.0.fixture
789bafc30f5cacef6bf89ca95d85ba786e2393b5c06cdad8fe47afe5fd888c1b  legacy-sa-v1.1.fixture
781f092eb190a4b70132ee240d6afb0577bd692430060a10428ccb0239a4fc38  legacy-sa-v1.2.fixture
9bd40e838b5c93dfe1dc211de9fefa2df7b638dd85e5aed8bddb1f7b1121574a  legacy-sa-v1.3.fixture
```

These files are test inputs, not benchmark evidence. Do not regenerate them
with the current writer: doing so would replace the historical format under
test instead of testing backward compatibility.
