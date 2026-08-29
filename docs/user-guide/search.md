# Search guide

sufkit provides exact pattern search on standalone suffix arrays and SDSL
FM-indexes, plus right-maximal compatibility search, formal MEM search, and
reference-unique MAM search on standalone suffix arrays.

## Exact search

Exact patterns are case-insensitive A/C/G/T strings. Empty patterns, N, IUPAC
symbols, whitespace, and every other character are invalid.

```cpp
auto range = index.EqualRange("ACGT");
auto count = index.Count("ACGT", sufkit::StrandMode::kBoth);

sufkit::LocateOptions options;
options.strands = sufkit::StrandMode::kBoth;
options.max_hits = 100;
auto result = index.Locate("ACGT", options);
```

`EqualRange()` returns a half-open row interval `[begin,end)` for a forward
query. Empty results are normalized to `[0,0)` and do not expose an insertion
position. `Count()` avoids coordinate materialization. `Locate()` resolves
rows, verifies contig boundaries, maps positions to zero-based contig-local
coordinates, sorts results, and applies `max_hits` without changing
`total_hits`.

| Strand mode | Search behavior |
|---|---|
| `kForward` | Search the original pattern |
| `kReverseComplement` | Search only its reverse complement |
| `kBoth` | Search both and merge coincident orientations |

A reverse-complement palindrome in `kBoth` mode is returned once per
coordinate with `Strand::kBoth`.

### Standalone-SA lookup

| Algorithm | Requirement | Role |
|---|---|---|
| `kBinary` | SA | Ordinary lower/upper bound |
| `kLcpBinary` | SA+LCP | Reuse known boundary prefixes |
| `kSaplingPwl` | Learned section | Predict, bracket, then verify a local range |
| `kChild` | LCP+CHILD | Explicit ESA traversal |
| `kAutoSelect` | Any | PWL when eligible; otherwise binary |

Auto selection never chooses CHILD. An unavailable explicit algorithm throws
`unsupported_backend`; it never silently changes semantics. PWL prediction is
only a hint: exponential bracketing, verified local search, and full binary
fallback preserve the exact range.

### FM lookup and batching

FM scalar search delegates backward search and position recovery to the fixed
SDSL CSA type selected by the index. `EqualRangeBatch()` and `CountBatch()`
preserve input order and scalar results while interleaving independent search
states. Batch width zero selects 16; explicit widths are 1 through 256. There
is no batch-locate API.

## Sampled standalone suffix arrays

For sampling rate `K>1`, sufkit stores only suffixes whose logical-text
position is divisible by K. Construction still forms the complete suffix
order first, so sampling reduces loaded and serialized size but not backend
peak construction memory.

`Count()` and `Locate()` recover complete-SA results by searching residue
anchors and verifying omitted prefixes. Patterns shorter than K use a direct
per-contig scan. `EqualRange()` is unsupported because the complete answer is
a union of residue-specific intervals rather than one public range.

The main public values are:

- `SuffixArrayBuildOptions::sampling_rate` and `--sa-sampling-rate K`;
- `SuffixArray::SamplingRate()`;
- `IndexInfo::sa_sampling_rate` and `IndexInfo::suffix_count`;
- sampling metadata introduced in format 1.3; current saves use 1.4 codecs,
  and legacy sampled 1.3 files remain readable.

ISA, LCP, CHILD, and PWL operate in the stored sampled-row domain. The learned
model budget is relative to the sampled SA payload. Sampling is a final-index
space/query-work trade-off, not direct sparse-SA construction or an
alternative to a compressed FM-index.

## Right-maximal exact matches

A reported tuple `(reference_start, query_start, length)` is exact and cannot
be extended jointly to the right. It may still extend to the left, so it is a
right-maximal MEM candidate rather than a MEM.

```cpp
sufkit::RightMaximalOptions options;
options.min_length = 20;
options.strands = sufkit::StrandMode::kBoth;

auto result = index.FindRightMaximalMatches(query, options);
index.ForEachRightMaximalMatch(query, options, callback);
```

The CLI equivalent is:

```bash
sufkit right-maximal \
  --index reference.sufidx \
  --query queries.fa.gz \
  --min-length 20 \
  --strand both
```

Query lowercase bases are normalized; every non-ACGT character is a hard
break. Matches cannot cross query breaks, reference N, contig separators, or
the sentinel. Reference positions are zero-based and contig-local. Query
positions always refer to the original forward query. Forward and reverse
matches remain distinct, including for palindromes.

`min_length=0` is invalid. Empty, all-break, or too-short queries return no
matches. A sampled index additionally requires `min_length>=K` and must return
the same normalized match set as the complete layout.

### Search modes

| Algorithm | Requirement | Role |
|---|---|---|
| `kBaseline` | SA | Root lookup at each query position |
| `kLcp` | SA+LCP | Reuse adjacent-suffix prefix information |
| `kChild` | SA+LCP+CHILD | Explicit ESA traversal |
| `kSuffixLink` | SA+ISA+LCP | Reuse the interval after deleting one query character |
| `kFull` | SA+ISA+LCP+CHILD | Suffix-link reuse plus explicit CHILD navigation |

All modes must return the same normalized set. Automatic selection is
workload-specific: MEM uses LCP with MUMmer-style query-anchor skipping when
LCP is present, otherwise baseline; reference-MAM uses suffix-link when
ISA+LCP is present, then LCP, then baseline. It never automatically chooses
CHILD or full. `lookup_algorithm` independently controls root initialization
and suffix-link fallback. An explicit suffix-link MEM request remains a
skip-one ablation unless `skip_multiplier` is supplied.

### Streaming and bounded results

`ForEachRightMaximalMatch()` calls its callback synchronously and does not
promise enumeration order across algorithms. Callback exceptions propagate.

`FindRightMaximalMatches()` sorts by query position, sequence ID, reference
position, length, and strand. `max_matches=N` retains the first N sorted
matches while still computing accurate `total_matches`; N=0 is count-only.

Query statistics are optional caller-owned mutable outputs. Use one statistics
object per concurrent operation. Built and loaded indexes themselves remain
immutable and support concurrent const queries.

## Formal MEM and reference-MAM search

`MemMatch` guarantees exactness and both-sided maximality: the match cannot be
extended jointly by one equal canonical base on either the left or the right.
`MamMatch` adds reference uniqueness across all indexed contigs. It follows
MUMmer4 `-mumreference` semantics, so the same reference-unique string may be
reported at multiple query positions. Strict query-and-reference-unique MUM
search is not implemented.

```cpp
sufkit::MemOptions mem_options;
mem_options.min_length = 20;
mem_options.strands = sufkit::StrandMode::kBoth;
auto mems = index.FindMems(query, mem_options);

sufkit::MamOptions mam_options;
mam_options.min_length = 20;
auto mams = index.FindMams(query, mam_options);
```

```bash
sufkit mem --index reference.sufidx --query queries.fa.gz \
  --min-length 20 --strand both
sufkit mam --index reference.sufidx --query queries.fa.gz \
  --min-length 20 --strand forward
```

MEM supports complete and sampled standalone SAs. For a sampled index, the
minimum length must be at least K. `MemOptions::skip_multiplier` controls the
MUMmer-style query-anchor spacing; omitted selects a deterministic default.
The implementation runs all K residue classes and recovers the true start by
at most `skip*K` bases. A fully recovered window is suppressed because the
previous anchor owns that MEM.

Reference-MAM requires a complete SA (`K=1`). It checks that the matched
string's complete-SA interval contains exactly one reference occurrence.
Neither operation is available on an FM index.

The algorithm modes and lookup modes have the same stored-data requirements
as right-maximal search. Auto selection is suffix-link, then LCP, then
baseline; CHILD/full remain explicit. All choices must produce the same
sorted tuple set.

`ForEachMem()` and `ForEachMam()` are synchronous streaming APIs.
`FindMems()` and `FindMams()` sort by query position, sequence ID, reference
position, length, and strand. Forward and reverse-complement matches remain
orientation-distinct, including palindromic queries. Bounded vector calls
still compute the exact complete `total_matches`.

## Boundaries

Every public coordinate is validated against its contig before emission.
Positions on N, separators, or the sentinel are never exposed. A damaged
payload is rejected rather than converted into an invalid result.

The legacy right-maximal API remains documented under its weaker contract and
must not be used when a caller needs a formal left-maximality guarantee. Use
the `Mem*` API for that guarantee and `Mam*` when reference uniqueness is also
required.
