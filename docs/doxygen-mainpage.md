# sufkit public C++ API

sufkit provides move-only C++17 objects for normalized genome references,
standalone suffix arrays, and fixed SDSL compressed suffix arrays. Public API
types deliberately hide third-party implementations.

Start with:

- `sufkit::GenomeReference` to validate and normalize FASTA or in-memory
  records;
- `sufkit::SuffixArray` for exact search, direct SA access, and right-maximal
  exact match search;
- `sufkit::FmIndex` for compressed exact range/count/locate and batched count;
- `sufkit::inspect_index` and backend-discovery functions for persisted/build
  capabilities; and
- `sufkit::Error` for stable error categories.

All public coordinates are zero-based and contig-local. Exact patterns accept
only A/C/G/T after case normalization. Right-maximal query non-ACGT symbols
are hard breaks. This operation does not yet guarantee left maximality and is
not a MEM API. Built or loaded indexes are immutable and support concurrent const
queries; caller-owned statistics outputs require independent synchronization.

For tutorials, algorithm explanations, persistence details, and compatibility
policy, read `docs/README.md` in the source repository.
