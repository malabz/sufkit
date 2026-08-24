# C++ API naming migration for 0.2.0

Version 0.2.0 standardizes public C++ functions on `PascalCase` and
enumerators on `kPascalCase`. This is an intentional source-level breaking
change from 0.1.x. No deprecated forwarding functions,
enumerator aliases, or compatibility macros are provided.

The following interfaces are unchanged:

- public headers such as `<sufkit/sufkit.hpp>`;
- the `sufkit::sufkit` CMake target;
- public type names, parameter lists, return types, option/result field names,
  and enum underlying integer values;
- CLI commands, options, output columns, and exit-code meanings; and
- `.sufidx` layout, backend IDs, section IDs, and serialized selector strings.

## Functions and methods

| 0.1.x name | 0.2.0 name |
|---|---|
| `GenomeReference::from_fasta()` | `GenomeReference::FromFasta()` |
| `GenomeReference::from_records()` | `GenomeReference::FromRecords()` |
| `GenomeReference::sequence_count()` | `GenomeReference::SequenceCount()` |
| `GenomeReference::total_bases()` | `GenomeReference::TotalBases()` |
| `GenomeReference::ambiguous_bases()` | `GenomeReference::AmbiguousBases()` |
| `GenomeReference::fingerprint()` | `GenomeReference::Fingerprint()` |
| `GenomeReference::sequence_info()` | `GenomeReference::GetSequenceInfo()` |
| `SuffixArray::build()` | `SuffixArray::Build()` |
| `SuffixArray::load()` | `SuffixArray::Load()` |
| `SuffixArray::save()` | `SuffixArray::Save()` |
| `SuffixArray::equal_range()` | `SuffixArray::EqualRange()` |
| `SuffixArray::count()` | `SuffixArray::Count()` |
| `SuffixArray::locate()` | `SuffixArray::Locate()` |
| `SuffixArray::for_each_right_maximal_match()` | `SuffixArray::ForEachRightMaximalMatch()` |
| `SuffixArray::find_right_maximal_matches()` | `SuffixArray::FindRightMaximalMatches()` |
| `SuffixArray::acceleration()` | `SuffixArray::Acceleration()` |
| `SuffixArray::lookup_acceleration()` | `SuffixArray::LookupAcceleration()` |
| `SuffixArray::sampling_rate()` | `SuffixArray::SamplingRate()` |
| `SuffixArray::suffix_at()` | `SuffixArray::SuffixAt()` |
| `SuffixArray::sequence_info()` | `SuffixArray::GetSequenceInfo()` |
| `SuffixArray::info()` | `SuffixArray::GetInfo()` |
| `FmIndex::build()` | `FmIndex::Build()` |
| `FmIndex::load()` | `FmIndex::Load()` |
| `FmIndex::save()` | `FmIndex::Save()` |
| `FmIndex::equal_range()` | `FmIndex::EqualRange()` |
| `FmIndex::equal_range_batch()` | `FmIndex::EqualRangeBatch()` |
| `FmIndex::count()` | `FmIndex::Count()` |
| `FmIndex::count_batch()` | `FmIndex::CountBatch()` |
| `FmIndex::locate()` | `FmIndex::Locate()` |
| `FmIndex::sequence_info()` | `FmIndex::GetSequenceInfo()` |
| `FmIndex::info()` | `FmIndex::GetInfo()` |
| `SuffixRange::size()` | `SuffixRange::Size()` |
| `SuffixRange::empty()` | `SuffixRange::Empty()` |
| `Error::code()` | `Error::Code()` |
| `inspect_index()` | `InspectIndex()` |
| `available_sa_backends()` | `AvailableSaBackends()` |
| `available_fm_backends()` | `AvailableFmBackends()` |
| `to_string()` | `ToString()` |

Overloads retain the same parameter lists and behavior. Structure fields such
as `SuffixArrayBuildOptions::sampling_rate`, `IndexInfo::suffix_count`,
`LocateOptions::max_hits`, and `QueryResult::total_hits` remain `snake_case`.

## Enumerators

Apply the same mechanical rule to every public enum value:

```text
lower_snake_case -> kPascalCase
```

Complete mappings are:

| Enum | 0.1.x values | 0.2.0 values |
|---|---|---|
| `IndexKind` | `suffix_array`, `fm_index` | `kSuffixArray`, `kFmIndex` |
| `SaBackend` | `auto_select`, `divsufsort`, `caps` | `kAutoSelect`, `kDivsufsort`, `kCaps` |
| `CoordinateWidth` | `auto_select`, `bits32`, `bits64` | `kAutoSelect`, `kBits32`, `kBits64` |
| `SaAcceleration` | `none`, `lcp`, `lcp_child`, `lcp_suffix_link`, `full` | `kNone`, `kLcp`, `kLcpChild`, `kLcpSuffixLink`, `kFull` |
| `SaLookupAcceleration` | `binary`, `sapling_pwl` | `kBinary`, `kSaplingPwl` |
| `SaSearchAlgorithm` | `auto_select`, `binary`, `lcp_binary`, `sapling_pwl`, `child` | `kAutoSelect`, `kBinary`, `kLcpBinary`, `kSaplingPwl`, `kChild` |
| `RightMaximalSearchAlgorithm` | `auto_select`, `baseline`, `lcp`, `child`, `suffix_link`, `full` | `kAutoSelect`, `kBaseline`, `kLcp`, `kChild`, `kSuffixLink`, `kFull` |
| `FmBackend` | `sdsl_csa_wt_huff`, `sdsl_csa_wt_balanced`, `sdsl_csa_sada`, `sdsl_csa_wt_epr` | `kSdslCsaWtHuff`, `kSdslCsaWtBalanced`, `kSdslCsaSada`, `kSdslCsaWtEpr` |
| `StrandMode` | `forward`, `reverse_complement`, `both` | `kForward`, `kReverseComplement`, `kBoth` |
| `Strand` | `forward`, `reverse_complement`, `both` | `kForward`, `kReverseComplement`, `kBoth` |
| `ErrorCode` | `invalid_input`, `io_error`, `unsupported_backend`, `corrupt_index`, `version_mismatch`, `build_failure` | `kInvalidInput`, `kIoError`, `kUnsupportedBackend`, `kCorruptIndex`, `kVersionMismatch`, `kBuildFailure` |

The underlying numeric values are unchanged. `ToString()` also continues to
return the existing lowercase selector, strand, and error strings.

## Minimal migration

Before:

```cpp
auto reference = sufkit::GenomeReference::from_fasta("reference.fa.gz");
auto index = sufkit::SuffixArray::build(reference);
sufkit::LocateOptions options;
options.strands = sufkit::StrandMode::both;
auto hits = index.locate("ACGT", options);
```

After:

```cpp
auto reference = sufkit::GenomeReference::FromFasta("reference.fa.gz");
auto index = sufkit::SuffixArray::Build(reference);
sufkit::LocateOptions options;
options.strands = sufkit::StrandMode::kBoth;
auto hits = index.Locate("ACGT", options);
```

Search the consuming source tree for `sufkit::` and apply this table before
upgrading. A compile failure is preferred to silently retaining two public
naming schemes with different long-term support.
