# C++ workflows

Include the aggregate public header unless a project deliberately minimizes
includes:

```cpp
#include <sufkit/sufkit.hpp>
```

Public index objects are move-only. Build or load them once, then share const
access between query threads. Builders are not documented as concurrent.

## Create a reference

From FASTA or FASTA.gz:

```cpp
auto reference = sufkit::GenomeReference::FromFasta("reference.fa.gz");
```

From memory:

```cpp
std::vector<sufkit::SequenceRecord> records{
    {"chr1", "primary contig", "ACGTNNACGT"},
    {"chr2", "", "TTTTCCCC"},
};
auto reference = sufkit::GenomeReference::FromRecords(std::move(records));
```

Names must be non-empty and unique. Every sequence must be non-empty. Input
A/C/G/T is upper-cased; all other reference symbols become N. Use
`GetSequenceInfo`, `TotalBases`, `AmbiguousBases`, and `Fingerprint` to record
the normalized reference contract.

## Build a suffix array

Default SA+ISA+LCP:

```cpp
auto index = sufkit::SuffixArray::Build(reference);
```

The default is the Fast resource preset and retains raw LCP. For a complete
low-memory index:

```cpp
auto options = sufkit::LowMemorySuffixArrayBuildOptions();
auto compact = sufkit::SuffixArray::Build(reference, options);
```

Low-memory keeps SA+byte-coded LCP, discards the temporary ISA after LCP
construction, and disables CHILD and PWL. It supports exact, right-maximal,
MEM, and complete-SA reference-MAM queries through LCP traversal rather than
suffix-link reuse. It currently requires `sampling_rate=1`.

Explicit CaPS build:

```cpp
sufkit::SuffixArrayBuildOptions options;
options.backend = sufkit::SaBackend::kCaps;
options.coordinate_width = sufkit::CoordinateWidth::kBits32;
options.threads = 16;
options.acceleration = sufkit::SaAcceleration::kLcpSuffixLink;

sufkit::SuffixArrayBuildStatistics timing;
options.statistics = &timing;
auto index = sufkit::SuffixArray::Build(reference, options);
```

`coordinate_width` selects the constructor integer width. Primary SA storage
and the preferred auxiliary width are an independent choice:

```cpp
auto options = sufkit::FastSuffixArrayBuildOptions();
options.coordinate_width = sufkit::CoordinateWidth::kBits64;
options.storage_width = sufkit::CoordinateStorageWidth::kBits32;
auto index = sufkit::SuffixArray::Build(reference, options);
```

This combination is legal when the complete logical text fits unsigned
32-bit positions, even if the chosen constructor uses 64-bit integers. Other
explicit storage choices are `kBits40`, `kBits48`, and `kBits64`. Auto Fast
uses native32 then native64; auto Low-memory selects the narrowest width.
Every down-pack is validated before the build-width representation is freed.

Low-memory describes the retained index, not the constructor peak. In
particular, the bundled CaPS implementation owns complete SA/LCP and temporary
work arrays during construction and exposes borrowed result pointers, so the
wrapper must copy the final SA before releasing the CaPS object.

The statistics pointer is caller-owned and written during the build. Do not
share one statistics object across concurrent calls. `threads` controls CaPS
construction and parallelizable auxiliary work; divsufsort itself remains
serial.

Build a text-position sampled SA independently of constructor choice:

```cpp
sufkit::SuffixArrayBuildOptions sampled_options;
sampled_options.sampling_rate = 4;
sampled_options.acceleration = sufkit::SaAcceleration::kLcpSuffixLink;
auto sampled = sufkit::SuffixArray::Build(reference, sampled_options);
```

`sampled.SamplingRate()` returns four and
`sampled.GetInfo().suffix_count` is the retained row count. Use `Count` or
`Locate`, not `EqualRange`, and keep right-maximal exact match `min_length` at
least four.

Enable the experimental PWL lookup independently:

```cpp
options.learned_index.enabled = true;
options.learned_index.k = 20;
options.learned_index.memory_overhead_basis_points = 100;  // 1% raw SA budget
```

## Build an FM-index

```cpp
sufkit::FmIndexBuildOptions options;
options.backend = sufkit::FmBackend::kSdslCsaWtHuff;
auto index = sufkit::FmIndex::Build(reference, options);
```

Balanced and EPR are fixed alternative SDSL CSA types. They do not accept SA
constructor, thread, or runtime sampling parameters.

## Save, inspect, and load

```cpp
index.Save("reference.sufidx");

auto metadata = sufkit::InspectIndex("reference.sufidx");
auto loaded = sufkit::FmIndex::Load("reference.sufidx");
```

Save refuses an existing target by default:

```cpp
sufkit::SaveOptions save;
save.overwrite = true;
index.Save("reference.sufidx", save);
```

`InspectIndex` validates the outer container and reports kind, backend,
versions, counts, fingerprint, size, construction/stored widths, resource
profile, LCP encoding, and resident payload breakdown without exposing the
private backend type.

## Exact search

```cpp
auto range = loaded.EqualRange("ACGT");
auto count = loaded.Count("ACGT", sufkit::StrandMode::kBoth);

sufkit::LocateOptions locate;
locate.strands = sufkit::StrandMode::kBoth;
locate.max_hits = 100;
auto result = loaded.Locate("ACGT", locate);
```

`result.total_hits` is complete. `result.hits` may be bounded and
`result.truncated` indicates omitted coordinates.

An SA can explicitly select an exact algorithm and collect statistics:

```cpp
sufkit::SaSearchStatistics stats;
auto range = sa.EqualRange(
    "ACGTACGTACGTACGTACGT",
    sufkit::SaSearchAlgorithm::kSaplingPwl,
    &stats);
```

Explicit PWL or CHILD requests fail with `unsupported_backend` if the loaded
index lacks the required data. `kAutoSelect` never selects CHILD.

## FM batched count

```cpp
std::vector<std::string_view> patterns{"ACGT", "GATTACA", "TTTT"};
sufkit::FmBatchOptions options;
options.strands = sufkit::StrandMode::kBoth;
options.batch_width = 16;
auto counts = loaded.CountBatch(patterns, options);
```

Output order equals input order. Width zero selects 16; explicit widths must
be 1–256. All patterns are validated before search, so one invalid pattern
rejects the whole call. Locate remains scalar.

## right-maximal exact match search

Vector API:

```cpp
sufkit::RightMaximalOptions options;
options.min_length = 20;
options.strands = sufkit::StrandMode::kBoth;
options.algorithm = sufkit::RightMaximalSearchAlgorithm::kAutoSelect;

auto result = sa.FindRightMaximalMatches(query, options, 1000);
```

Streaming API:

```cpp
sa.ForEachRightMaximalMatch(
    query, options,
    [](const sufkit::RightMaximalMatch& match) { consume(match); });
```

The callback executes synchronously in the caller thread and exceptions pass
through unchanged. Streaming order is not stable across algorithms. The
vector API returns a deterministic query-first order and can retain only the
first N while still counting the complete result.

## MEM, reference-MAM, SMEM, and MUM search

```cpp
sufkit::MemOptions mem_options;
mem_options.min_length = 20;
mem_options.strands = sufkit::StrandMode::kBoth;
mem_options.algorithm = sufkit::MemSearchAlgorithm::kAutoSelect;

auto mems = sa.FindMems(query, mem_options, 1000);
sa.ForEachMem(query, mem_options,
              [](const sufkit::MemMatch& match) { consume(match); });

sufkit::MamOptions mam_options;
mam_options.min_length = 20;
auto mams = sa.FindMams(query, mam_options);

sufkit::SmemOptions smem_options;
smem_options.min_length = 20;
smem_options.min_occurrences = 2;
auto smems = sa.FindSmems(query, smem_options);

sufkit::MumOptions mum_options;
mum_options.min_length = 20;
auto mums = sa.FindMums(query, mum_options);
```

MEM guarantees both left and right maximality. Reference-MAM additionally
requires one occurrence in the combined reference but permits repeated query
occurrences. SMEM reports maximal qualifying query intervals plus each
reference coordinate; MUM requires uniqueness in both the reference and the
current query record. Streaming callbacks are synchronous; vector results are
sorted and can retain a bounded prefix while reporting complete counts.

## Error handling

```cpp
try {
  auto index = sufkit::FmIndex::Load(path);
} catch (const sufkit::Error& error) {
  std::cerr << sufkit::ToString(error.Code()) << ": " << error.what() << '\n';
}
```

Do not parse message text. Use `Error::Code()` for stable categories and the
message for diagnostics.

## Concurrent queries

After build/load, const `Count`, `Locate`, `EqualRange`, and maximal-match
operations do not mutate the index. Multiple threads may call them on the same
object. Any
statistics object passed to a query is mutable caller-owned output and must be
thread-local or otherwise synchronized.
