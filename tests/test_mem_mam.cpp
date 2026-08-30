// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <sufkit/sufkit.hpp>

namespace {

int failures = 0;

#define CHECK(condition)                               \
  do {                                                 \
    if (!(condition)) {                                \
      std::cerr << __FILE__ << ':' << __LINE__         \
                << ": CHECK failed: " #condition "\n"; \
      ++failures;                                      \
    }                                                  \
  } while (false)

template <class Callback>
void CheckError(sufkit::ErrorCode expected, Callback callback) {
  try {
    callback();
    CHECK(false);
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == expected);
  }
}

using MatchTuple =
    std::tuple<std::uint64_t, sufkit::SequenceId, std::uint64_t,
               std::uint64_t, sufkit::Strand>;

std::string Normalize(std::string sequence) {
  for (char& base : sequence) {
    base = static_cast<char>(
        std::toupper(static_cast<unsigned char>(base)));
    if (base != 'A' && base != 'C' && base != 'G' && base != 'T') {
      base = 'N';
    }
  }
  return sequence;
}

std::vector<MatchTuple> NaiveMems(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& raw_query, std::uint64_t min_length) {
  const auto query = Normalize(raw_query);
  std::vector<MatchTuple> result;
  for (std::size_t sequence_id = 0; sequence_id < records.size();
       ++sequence_id) {
    const auto reference = Normalize(records[sequence_id].sequence);
    for (std::size_t query_position = 0; query_position < query.size();
         ++query_position) {
      if (query[query_position] == 'N') {
        continue;
      }
      for (std::size_t reference_position = 0;
           reference_position < reference.size(); ++reference_position) {
        if (reference[reference_position] == 'N') {
          continue;
        }
        std::size_t length = 0;
        while (query_position + length < query.size() &&
               reference_position + length < reference.size() &&
               query[query_position + length] != 'N' &&
               reference[reference_position + length] != 'N' &&
               query[query_position + length] ==
                   reference[reference_position + length]) {
          ++length;
        }
        const bool left_extendable =
            query_position > 0 && reference_position > 0 &&
            query[query_position - 1] != 'N' &&
            reference[reference_position - 1] != 'N' &&
            query[query_position - 1] == reference[reference_position - 1];
        if (length >= min_length && !left_extendable) {
          result.emplace_back(query_position,
                              static_cast<sufkit::SequenceId>(sequence_id),
                              reference_position, length,
                              sufkit::Strand::kForward);
        }
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::uint64_t ReferenceOccurrences(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& pattern) {
  std::uint64_t count = 0;
  for (const auto& record : records) {
    const auto reference = Normalize(record.sequence);
    for (std::size_t position = 0;
         position + pattern.size() <= reference.size(); ++position) {
      if (reference.compare(position, pattern.size(), pattern) == 0) {
        ++count;
      }
    }
  }
  return count;
}

std::vector<MatchTuple> NaiveMams(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& raw_query, std::uint64_t min_length) {
  const auto query = Normalize(raw_query);
  auto result = NaiveMems(records, raw_query, min_length);
  result.erase(
      std::remove_if(result.begin(), result.end(), [&](const auto& match) {
        const auto query_position = std::get<0>(match);
        const auto length = std::get<3>(match);
        return ReferenceOccurrences(
                   records,
                   query.substr(static_cast<std::size_t>(query_position),
                                static_cast<std::size_t>(length))) !=
               1;
      }),
      result.end());
  return result;
}

std::vector<MatchTuple> Tuples(const sufkit::MemResult& result) {
  std::vector<MatchTuple> values;
  for (const auto& match : result.matches) {
    values.emplace_back(match.query_position, match.sequence_id,
                        match.reference_position, match.length, match.strand);
  }
  return values;
}

std::vector<MatchTuple> Tuples(const sufkit::MamResult& result) {
  std::vector<MatchTuple> values;
  for (const auto& match : result.matches) {
    values.emplace_back(match.query_position, match.sequence_id,
                        match.reference_position, match.length, match.strand);
  }
  return values;
}

sufkit::SuffixArray Build(
    const std::vector<sufkit::SequenceRecord>& records,
    std::uint32_t sampling_rate = 1) {
  sufkit::SuffixArrayBuildOptions options;
  options.sampling_rate = sampling_rate;
  options.acceleration = sufkit::SaAcceleration::kFull;
  return sufkit::SuffixArray::Build(
      sufkit::GenomeReference::FromRecords(records), options);
}

void TestKnownMems() {
  const std::vector<sufkit::SequenceRecord> records{
      {"long", "", "TTACGTACGTGGG"},
      {"short", "", "CCACGTACGACCC"},
      {"break", "", "AAAANCCCC"}};
  const std::string query = "AAACGTACGTTTT";
  const auto expected = NaiveMems(records, query, 4);
  auto index = Build(records);
  for (const auto algorithm :
       {sufkit::MemSearchAlgorithm::kBaseline,
        sufkit::MemSearchAlgorithm::kLcp,
        sufkit::MemSearchAlgorithm::kChild,
        sufkit::MemSearchAlgorithm::kSuffixLink,
        sufkit::MemSearchAlgorithm::kFull,
        sufkit::MemSearchAlgorithm::kAutoSelect}) {
    sufkit::MemOptions options;
    options.min_length = 4;
    options.algorithm = algorithm;
    options.skip_multiplier = 1;
    CHECK(Tuples(index.FindMems(query, options)) == expected);
  }

  sufkit::MemOptions hard_break;
  hard_break.min_length = 3;
  CHECK(Tuples(index.FindMems("AAAANCCCC", hard_break)) ==
        NaiveMems(records, "AAAANCCCC", 3));

  std::uint64_t streamed = 0;
  index.ForEachMem(query, sufkit::MemOptions{},
                   [&](const sufkit::MemMatch&) { ++streamed; });
  CHECK(streamed == index.FindMems(query).total_matches);
  const auto limited = index.FindMems(query, sufkit::MemOptions{}, 1);
  CHECK(limited.matches.size() <= 1);
  CHECK(limited.truncated == (limited.total_matches > 1));
  const auto count_only = index.FindMems(query, sufkit::MemOptions{}, 0);
  CHECK(count_only.matches.empty());
  CHECK(count_only.truncated == (count_only.total_matches != 0));
}

void TestMams() {
  const std::vector<sufkit::SequenceRecord> records{
      {"unique", "", "TTGATTACAGG"}, {"other", "", "CCACGTCC"}};
  const std::string query = "AAGATTACACCGATTACA";
  auto index = Build(records);
  sufkit::MamOptions options;
  options.min_length = 7;
  for (const auto algorithm :
       {sufkit::MemSearchAlgorithm::kBaseline,
        sufkit::MemSearchAlgorithm::kLcp,
        sufkit::MemSearchAlgorithm::kChild,
        sufkit::MemSearchAlgorithm::kSuffixLink,
        sufkit::MemSearchAlgorithm::kFull,
        sufkit::MemSearchAlgorithm::kAutoSelect}) {
    options.algorithm = algorithm;
    const auto observed = index.FindMams(query, options);
    const auto observed_tuples = Tuples(observed);
    CHECK(observed_tuples == NaiveMams(records, query, 7));
    CHECK(observed.total_matches == 2);
    const auto limited = index.FindMams(query, options, 1);
    CHECK(limited.total_matches == observed.total_matches);
    CHECK(limited.matches.size() == 1);
    CHECK(limited.truncated);
    CHECK(Tuples(limited) == std::vector<MatchTuple>(
                                 observed_tuples.begin(),
                                 observed_tuples.begin() + 1));
    const auto count_only = index.FindMams(query, options, 0);
    CHECK(count_only.total_matches == observed.total_matches);
    CHECK(count_only.matches.empty());
    CHECK(count_only.truncated);
    const auto exact_limit = index.FindMams(query, options, 2);
    CHECK(exact_limit.total_matches == observed.total_matches);
    CHECK(Tuples(exact_limit) == observed_tuples);
    CHECK(!exact_limit.truncated);
  }

  sufkit::MamOptions no_hit_options;
  no_hit_options.min_length = 7;
  const auto no_hit_count_only = index.FindMams("NNNN", no_hit_options, 0);
  CHECK(no_hit_count_only.total_matches == 0);
  CHECK(no_hit_count_only.matches.empty());
  CHECK(!no_hit_count_only.truncated);

  std::vector<MatchTuple> streamed;
  index.ForEachMam(query, options, [&](const sufkit::MamMatch& match) {
    streamed.emplace_back(match.query_position, match.sequence_id,
                          match.reference_position, match.length,
                          match.strand);
  });
  std::sort(streamed.begin(), streamed.end());
  CHECK(streamed == NaiveMams(records, query, 7));

  bool propagated = false;
  try {
    index.ForEachMam(query, options, [](const sufkit::MamMatch&) {
      throw std::runtime_error("MAM callback sentinel");
    });
  } catch (const std::runtime_error& error) {
    propagated = std::string(error.what()) == "MAM callback sentinel";
  }
  CHECK(propagated);

  auto repeated = records;
  repeated.push_back({"duplicate", "", "GGGATTACAT"});
  auto repeated_index = Build(repeated);
  CHECK(Tuples(repeated_index.FindMams(query, options)) ==
        NaiveMams(repeated, query, 7));

  auto sampled = Build(records, 2);
  try {
    (void)sampled.FindMams(query, options);
    CHECK(false);
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == sufkit::ErrorCode::kUnsupportedBackend);
  }
}

void TestMamUniquenessAfterRightExtension() {
  // The minimum-length prefix at query position 3 occurs in both contigs, but
  // only the first occurrence extends through GGG. MAM uniqueness must use the
  // complete candidate length, not the initial search interval size.
  const std::vector<sufkit::SequenceRecord> records{
      {"long", "", "TTTACGTAAAAGGG"},
      {"short", "", "CCCACGTAAAATTT"}};
  const std::string query = "GGGACGTAAAAGGGCCC";
  const auto expected = NaiveMams(records, query, 8);

  auto build_options = sufkit::LowMemorySuffixArrayBuildOptions();
  auto index = sufkit::SuffixArray::Build(
      sufkit::GenomeReference::FromRecords(records), build_options);
  sufkit::MamOptions options;
  options.min_length = 8;
  options.algorithm = sufkit::MemSearchAlgorithm::kLcp;
  const auto observed = Tuples(index.FindMams(query, options));
  CHECK(observed == expected);

  const MatchTuple extended_unique{3, 0, 3, 11,
                                   sufkit::Strand::kForward};
  const MatchTuple repeated_prefix{3, 1, 3, 8,
                                   sufkit::Strand::kForward};
  CHECK(std::find(observed.begin(), observed.end(), extended_unique) !=
        observed.end());
  CHECK(std::find(observed.begin(), observed.end(), repeated_prefix) ==
        observed.end());

  // SA-only legacy/compact indexes have no adjacent-LCP shortcut and must
  // retain the exact binary-range fallback.
  sufkit::SuffixArrayBuildOptions sa_only_options;
  sa_only_options.acceleration = sufkit::SaAcceleration::kNone;
  auto sa_only = sufkit::SuffixArray::Build(
      sufkit::GenomeReference::FromRecords(records), sa_only_options);
  options.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
  CHECK(Tuples(sa_only.FindMams(query, options)) == expected);
}

void TestSampledAndSkip() {
  const std::vector<sufkit::SequenceRecord> records{
      {"a", "", "ACGTACGTGATTACACCCCGGGGTTTTACGTACGT"},
      {"b", "", "TTTTACGTACGTAAAACCCCGATTACAGATTACA"},
      {"c", "", "ACGTACGTACGTACGTACGTGATTACAGATTACA"}};
  const std::vector<std::string> queries{
      "GGACGTACGTGATTACATTTT", "NNACGTGATTACANNGATTACA",
      "ACGTACGTACGTACGT"};
  for (const auto rate : {1U, 2U, 4U, 8U}) {
    auto index = Build(records, rate);
    for (const auto& query : queries) {
      const auto expected = NaiveMems(records, query, 8);
      for (const auto skip : {1U, 2U}) {
        if (static_cast<std::uint64_t>(skip) * rate > 8) {
          continue;
        }
        sufkit::MemOptions options;
        options.min_length = 8;
        options.algorithm = sufkit::MemSearchAlgorithm::kFull;
        options.skip_multiplier = skip;
        CHECK(Tuples(index.FindMems(query, options)) == expected);
        std::vector<MatchTuple> streamed;
        index.ForEachMem(query, options, [&](const sufkit::MemMatch& match) {
          streamed.emplace_back(match.query_position, match.sequence_id,
                                match.reference_position, match.length,
                                match.strand);
        });
        std::sort(streamed.begin(), streamed.end());
        CHECK(streamed == expected);
      }
    }
  }

  const std::vector<sufkit::SequenceRecord> long_records{
      {"long", "",
       "TTACGTCAGTACGATCGTACCTGACTGATCGTAGCTAGGGTAAACGTCAGTACGATC"}};
  const std::string long_query =
      "NNGGACGTCAGTACGATCGTACCTGACTGATCGTAGCTANN";
  const auto long_expected = NaiveMems(long_records, long_query, 32);
  CHECK(!long_expected.empty());
  for (const auto rate : {4U, 8U}) {
    auto index = Build(long_records, rate);
    sufkit::MemOptions automatic;
    automatic.min_length = 32;
    CHECK(Tuples(index.FindMems(long_query, automatic)) == long_expected);
    for (const auto skip : {std::optional<std::uint32_t>{2},
                            std::optional<std::uint32_t>{}}) {
      sufkit::MemOptions options;
      options.min_length = 32;
      options.algorithm = sufkit::MemSearchAlgorithm::kFull;
      options.skip_multiplier = skip;
      CHECK(Tuples(index.FindMems(long_query, options)) == long_expected);
      std::vector<MatchTuple> streamed;
      index.ForEachMem(long_query, options,
                       [&](const sufkit::MemMatch& match) {
                         streamed.emplace_back(
                             match.query_position, match.sequence_id,
                             match.reference_position, match.length,
                             match.strand);
                       });
      std::sort(streamed.begin(), streamed.end());
      CHECK(streamed == long_expected);
    }
  }
}

void TestWidthsAndConstructors() {
  const std::vector<sufkit::SequenceRecord> records{
      {"a", "", "ACGTACGTGATTACACCCCGGGGTTTTACGTACGT"},
      {"b", "", "TTTTACGTACGTAAAACCCCGATTACAGATTACA"}};
  const std::string query = "GGACGTACGTGATTACATTTT";
  const auto expected = NaiveMems(records, query, 8);
  const auto expected_mams = NaiveMams(records, query, 8);
  const auto reference = sufkit::GenomeReference::FromRecords(records);
  for (const auto width : {sufkit::CoordinateWidth::kBits32,
                           sufkit::CoordinateWidth::kBits64}) {
    sufkit::SuffixArrayBuildOptions options;
    options.backend = sufkit::SaBackend::kDivsufsort;
    options.coordinate_width = width;
    options.acceleration = sufkit::SaAcceleration::kFull;
    auto index = sufkit::SuffixArray::Build(reference, options);
    sufkit::MemOptions search;
    search.min_length = 8;
    search.algorithm = sufkit::MemSearchAlgorithm::kFull;
    CHECK(Tuples(index.FindMems(query, search)) == expected);

    const auto backends = sufkit::AvailableSaBackends();
    const auto caps = std::find_if(
        backends.begin(), backends.end(), [&](const auto& descriptor) {
          return descriptor.name == "caps" && descriptor.available;
        });
    if (caps != backends.end()) {
      options.backend = sufkit::SaBackend::kCaps;
      options.threads = 2;
      auto caps_index = sufkit::SuffixArray::Build(reference, options);
      CHECK(Tuples(caps_index.FindMems(query, search)) == expected);
    }
  }

  const std::array<sufkit::CoordinateStorageWidth, 4> storage_widths{
      {sufkit::CoordinateStorageWidth::kBits32,
       sufkit::CoordinateStorageWidth::kBits40,
       sufkit::CoordinateStorageWidth::kBits48,
       sufkit::CoordinateStorageWidth::kBits64}};
  for (const auto storage_width : storage_widths) {
    auto options = sufkit::FastSuffixArrayBuildOptions();
    options.backend = sufkit::SaBackend::kDivsufsort;
    options.coordinate_width = sufkit::CoordinateWidth::kBits64;
    options.storage_width = storage_width;
    options.acceleration = sufkit::SaAcceleration::kFull;
    auto index = sufkit::SuffixArray::Build(reference, options);

    sufkit::MemOptions mem;
    mem.min_length = 8;
    mem.algorithm = sufkit::MemSearchAlgorithm::kFull;
    CHECK(Tuples(index.FindMems(query, mem)) == expected);

    sufkit::MamOptions mam;
    mam.min_length = 8;
    for (const auto algorithm : {sufkit::MemSearchAlgorithm::kFull,
                                 sufkit::MemSearchAlgorithm::kSuffixLink}) {
      mam.algorithm = algorithm;
      CHECK(Tuples(index.FindMams(query, mam)) == expected_mams);
    }
  }

  auto low_options = sufkit::LowMemorySuffixArrayBuildOptions();
  low_options.backend = sufkit::SaBackend::kDivsufsort;
  low_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  auto low = sufkit::SuffixArray::Build(reference, low_options);
  CHECK(low.GetInfo().sa_resource_profile ==
        sufkit::SaResourceProfile::kLowMemory);
  CHECK(low.GetInfo().stored_coordinate_width == 32);
  CHECK(low.GetInfo().isa_bytes == 0);
  CHECK(low.GetInfo().child_bytes == 0);

  sufkit::MemOptions low_mem;
  low_mem.min_length = 8;
  CHECK(Tuples(low.FindMems(query, low_mem)) == expected);
  sufkit::MamOptions low_mam;
  low_mam.min_length = 8;
  CHECK(Tuples(low.FindMams(query, low_mam)) == expected_mams);

  const auto low_path = std::filesystem::current_path() /
                        "sufkit-low-memory-mem-mam.sufidx";
  std::error_code ignored;
  std::filesystem::remove(low_path, ignored);
  low.Save(low_path);
  auto loaded_low = sufkit::SuffixArray::Load(low_path);
  std::filesystem::remove(low_path, ignored);
  CHECK(loaded_low.GetInfo().sa_resource_profile ==
        sufkit::SaResourceProfile::kLowMemory);
  CHECK(Tuples(loaded_low.FindMems(query, low_mem)) == expected);
  CHECK(Tuples(loaded_low.FindMams(query, low_mam)) == expected_mams);
}

void TestProfileAutoPolicies() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTACGTCAGTACGATCGTACCTGACTGATCGTAGCTAGGGTAAA"},
      {"r1", "", "CCGATCGTAGCTAGTTTTACGTCAGTACGATCGTACAAAA"}};
  const std::string query =
      "NNACGTCAGTACGATCGTACCTGACTGATCGTAGCTANN";
  const auto reference = sufkit::GenomeReference::FromRecords(records);
  const auto expected_mems = NaiveMems(records, query, 20);
  const auto expected_mams = NaiveMams(records, query, 20);

  auto fast_options = sufkit::FastSuffixArrayBuildOptions();
  const auto fast = sufkit::SuffixArray::Build(reference, fast_options);
  CHECK(fast.GetInfo().sa_resource_profile ==
        sufkit::SaResourceProfile::kFast);
  CHECK(fast.GetInfo().isa_bytes != 0);
  CHECK(fast.GetInfo().lcp_encoding == sufkit::SaLcpEncoding::kRaw);

  sufkit::MemOptions mem_auto;
  mem_auto.min_length = 20;
  sufkit::MemOptions mem_lcp = mem_auto;
  mem_lcp.algorithm = sufkit::MemSearchAlgorithm::kLcp;
  sufkit::MemOptions mem_suffix_link = mem_auto;
  mem_suffix_link.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
  mem_suffix_link.skip_multiplier = 1;
  CHECK(Tuples(fast.FindMems(query, mem_auto)) == expected_mems);
  CHECK(Tuples(fast.FindMems(query, mem_lcp)) == expected_mems);
  CHECK(Tuples(fast.FindMems(query, mem_suffix_link)) == expected_mems);

  sufkit::MamOptions mam_auto;
  mam_auto.min_length = 20;
  sufkit::MamOptions mam_suffix_link = mam_auto;
  mam_suffix_link.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
  CHECK(Tuples(fast.FindMams(query, mam_auto)) == expected_mams);
  CHECK(Tuples(fast.FindMams(query, mam_suffix_link)) == expected_mams);

  auto low_options = sufkit::LowMemorySuffixArrayBuildOptions();
  const auto low = sufkit::SuffixArray::Build(reference, low_options);
  CHECK(low.GetInfo().sa_resource_profile ==
        sufkit::SaResourceProfile::kLowMemory);
  CHECK(low.GetInfo().isa_bytes == 0);
  CHECK(low.GetInfo().lcp_encoding == sufkit::SaLcpEncoding::kByteCoded);
  CHECK(Tuples(low.FindMems(query, mem_auto)) == expected_mems);
  CHECK(Tuples(low.FindMems(query, mem_lcp)) == expected_mems);
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)low.FindMems(query, mem_suffix_link);
  });

  sufkit::MamOptions mam_lcp = mam_auto;
  mam_lcp.algorithm = sufkit::MemSearchAlgorithm::kLcp;
  CHECK(Tuples(low.FindMams(query, mam_auto)) == expected_mams);
  CHECK(Tuples(low.FindMams(query, mam_lcp)) == expected_mams);
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)low.FindMams(query, mam_suffix_link);
  });
}

void TestPackedSpanMemMamEnumeration() {
  const std::vector<sufkit::SequenceRecord> records{
      {"repeat", "", std::string(320, 'A')}};
  const auto reference = sufkit::GenomeReference::FromRecords(records);
  const std::string query(32, 'A');

  sufkit::MemOptions mem_options;
  mem_options.min_length = 20;
  mem_options.algorithm = sufkit::MemSearchAlgorithm::kFull;
  mem_options.skip_multiplier = 1;
  sufkit::MamOptions mam_options;
  mam_options.min_length = 20;
  mam_options.algorithm = sufkit::MemSearchAlgorithm::kFull;

  auto native_options = sufkit::FastSuffixArrayBuildOptions();
  native_options.backend = sufkit::SaBackend::kDivsufsort;
  native_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  native_options.storage_width =
      sufkit::CoordinateStorageWidth::kBits32;
  native_options.acceleration = sufkit::SaAcceleration::kFull;
  const auto native = sufkit::SuffixArray::Build(reference, native_options);
  const auto expected_mem = native.FindMems(query, mem_options);
  const auto expected_mam = native.FindMams(query, mam_options);
  const auto expected_mem_tuples = Tuples(expected_mem);
  CHECK(expected_mem.total_matches > 256);
  CHECK(Tuples(expected_mem) == NaiveMems(records, query, 20));
  CHECK(Tuples(expected_mam) == NaiveMams(records, query, 20));

  auto low_options = sufkit::LowMemorySuffixArrayBuildOptions();
  low_options.backend = sufkit::SaBackend::kDivsufsort;
  low_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  const auto low = sufkit::SuffixArray::Build(reference, low_options);
  CHECK(low.GetInfo().lcp_encoding == sufkit::SaLcpEncoding::kByteCoded);
  CHECK(low.GetInfo().lcp_overflow_anchors != 0);
  auto low_mem_options = mem_options;
  low_mem_options.algorithm = sufkit::MemSearchAlgorithm::kLcp;
  const auto low_mem = low.FindMems(query, low_mem_options);
  CHECK(low_mem.total_matches == expected_mem.total_matches);
  CHECK(low_mem.truncated == expected_mem.truncated);
  CHECK(Tuples(low_mem) == expected_mem_tuples);

  const std::array<sufkit::CoordinateStorageWidth, 4> storage_widths{
      {sufkit::CoordinateStorageWidth::kBits32,
       sufkit::CoordinateStorageWidth::kBits40,
       sufkit::CoordinateStorageWidth::kBits48,
       sufkit::CoordinateStorageWidth::kBits64}};
  for (const auto storage_width : storage_widths) {
    auto options = sufkit::FastSuffixArrayBuildOptions();
    options.backend = sufkit::SaBackend::kDivsufsort;
    options.coordinate_width = sufkit::CoordinateWidth::kBits64;
    options.storage_width = storage_width;
    options.acceleration = sufkit::SaAcceleration::kFull;
    const auto candidate = sufkit::SuffixArray::Build(reference, options);

    const auto mem = candidate.FindMems(query, mem_options);
    CHECK(mem.total_matches == expected_mem.total_matches);
    CHECK(mem.truncated == expected_mem.truncated);
    CHECK(Tuples(mem) == Tuples(expected_mem));

    for (const auto algorithm : {sufkit::MemSearchAlgorithm::kFull,
                                 sufkit::MemSearchAlgorithm::kSuffixLink}) {
      auto candidate_mam_options = mam_options;
      candidate_mam_options.algorithm = algorithm;
      const auto mam = candidate.FindMams(query, candidate_mam_options);
      CHECK(mam.total_matches == expected_mam.total_matches);
      CHECK(mam.truncated == expected_mam.truncated);
      CHECK(Tuples(mam) == Tuples(expected_mam));
    }

    const auto limited = candidate.FindMems(query, mem_options, 257);
    CHECK(limited.total_matches == expected_mem.total_matches);
    CHECK(limited.truncated);
    CHECK(Tuples(limited) ==
          std::vector<MatchTuple>(expected_mem_tuples.begin(),
                                  expected_mem_tuples.begin() + 257));
  }
}

void TestRandomDifferential() {
  std::mt19937_64 generator(20260824);
  const auto random_base = [&] { return "ACGTN"[generator() % 5]; };
  for (int trial = 0; trial < 20; ++trial) {
    std::vector<sufkit::SequenceRecord> records;
    for (int id = 0; id < 2; ++id) {
      std::string sequence(36, 'A');
      std::generate(sequence.begin(), sequence.end(), random_base);
      records.push_back({"r" + std::to_string(id), "", std::move(sequence)});
    }
    std::string query(30, 'A');
    std::generate(query.begin(), query.end(), random_base);
    const auto expected = NaiveMems(records, query, 4);
    for (const auto rate : {1U, 2U}) {
      auto index = Build(records, rate);
      sufkit::MemOptions options;
      options.min_length = 4;
      options.algorithm = sufkit::MemSearchAlgorithm::kFull;
      options.skip_multiplier = 2;
      const auto observed = index.FindMems(query, options);
      CHECK(Tuples(observed) == expected);
      std::vector<MatchTuple> streamed;
      index.ForEachMem(query, options, [&](const sufkit::MemMatch& match) {
        streamed.emplace_back(match.query_position, match.sequence_id,
                              match.reference_position, match.length,
                              match.strand);
      });
      std::sort(streamed.begin(), streamed.end());
      // The bounded collector relies on the anchor/residue ownership rule:
      // the streaming kernel must emit every directional tuple exactly once.
      CHECK(streamed == expected);
      const auto count_only = index.FindMems(query, options, 0);
      CHECK(count_only.total_matches == expected.size());
      CHECK(count_only.matches.empty());
      const auto limited = index.FindMems(query, options, 1);
      CHECK(limited.total_matches == expected.size());
      CHECK(limited.matches.size() ==
            std::min<std::size_t>(1, expected.size()));
      CHECK(Tuples(limited) == std::vector<MatchTuple>(
                                   expected.begin(),
                                   expected.begin() + limited.matches.size()));
      if (rate == 1) {
        sufkit::MamOptions mam;
        mam.min_length = 4;
        mam.algorithm = sufkit::MemSearchAlgorithm::kFull;
        const auto expected_mams = NaiveMams(records, query, 4);
        CHECK(Tuples(index.FindMams(query, mam)) == expected_mams);
        std::vector<MatchTuple> streamed_mams;
        index.ForEachMam(query, mam, [&](const sufkit::MamMatch& match) {
          streamed_mams.emplace_back(
              match.query_position, match.sequence_id,
              match.reference_position, match.length, match.strand);
        });
        std::sort(streamed_mams.begin(), streamed_mams.end());
        CHECK(streamed_mams == expected_mams);
        const auto mam_count_only = index.FindMams(query, mam, 0);
        CHECK(mam_count_only.total_matches == expected_mams.size());
        CHECK(mam_count_only.matches.empty());
      }
    }
  }
}

void TestContractsAndPersistence() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTGATTACAGGACGTACGT"},
      {"r1", "", "CCCCAAAATTTT"}};
  const std::string query = "aaGATTACAcc";
  auto index = Build(records);

  sufkit::MemOptions options;
  options.min_length = 7;
  CHECK(Tuples(index.FindMems(query, options)) ==
        NaiveMems(records, query, 7));

  const auto invalid_strand = static_cast<sufkit::StrandMode>(255);
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    auto invalid = options;
    invalid.strands = invalid_strand;
    (void)index.FindMems(query, invalid);
  });
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    sufkit::MamOptions invalid;
    invalid.min_length = 7;
    invalid.strands = invalid_strand;
    (void)index.FindMams(query, invalid);
  });

  bool propagated = false;
  try {
    index.ForEachMem(query, options, [](const sufkit::MemMatch&) {
      throw std::runtime_error("callback sentinel");
    });
  } catch (const std::runtime_error& error) {
    propagated = std::string(error.what()) == "callback sentinel";
  }
  CHECK(propagated);

  options.min_length = 0;
  try {
    (void)index.FindMems(query, options);
    CHECK(false);
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == sufkit::ErrorCode::kInvalidInput);
  }

  const auto path = std::filesystem::current_path() /
                    "sufkit-mem-mam-roundtrip.sufidx";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  index.Save(path);
  auto loaded = sufkit::SuffixArray::Load(path);
  std::filesystem::remove(path, ignored);

  sufkit::MemOptions concurrent_options;
  concurrent_options.min_length = 7;
  const auto expected = Tuples(loaded.FindMems(query, concurrent_options));
  std::atomic<bool> consistent{true};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < 50; ++iteration) {
        if (Tuples(loaded.FindMems(query, concurrent_options)) != expected) {
          consistent = false;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  CHECK(consistent.load());
}

}  // namespace

int main() {
  TestKnownMems();
  TestMams();
  TestMamUniquenessAfterRightExtension();
  TestSampledAndSkip();
  TestRandomDifferential();
  TestWidthsAndConstructors();
  TestProfileAutoPolicies();
  TestPackedSpanMemMamEnumeration();
  TestContractsAndPersistence();
  if (failures != 0) {
    std::cerr << failures << " MEM/MAM assertion(s) failed\n";
    return 1;
  }
  std::cout << "MEM/MAM tests passed\n";
  return 0;
}
