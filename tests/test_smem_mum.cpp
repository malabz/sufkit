// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
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

using SmemTuple =
    std::tuple<std::uint64_t, sufkit::SequenceId, std::uint64_t,
               std::uint64_t, std::uint64_t, sufkit::Strand>;
using MumTuple =
    std::tuple<std::uint64_t, sufkit::SequenceId, std::uint64_t,
               std::uint64_t, sufkit::Strand>;

struct ReferenceOccurrence {
  sufkit::SequenceId sequence_id = 0;
  std::uint64_t position = 0;

  auto Tie() const { return std::tie(sequence_id, position); }
  bool operator<(const ReferenceOccurrence& other) const {
    return Tie() < other.Tie();
  }
  bool operator==(const ReferenceOccurrence& other) const {
    return Tie() == other.Tie();
  }
};

struct SmemOracleResult {
  std::uint64_t total_smems = 0;
  std::vector<SmemTuple> matches;
};

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

char Complement(char base) {
  switch (base) {
    case 'A':
      return 'T';
    case 'C':
      return 'G';
    case 'G':
      return 'C';
    case 'T':
      return 'A';
    default:
      return 'N';
  }
}

std::string ReverseComplement(const std::string& raw_query) {
  const auto query = Normalize(raw_query);
  std::string result(query.size(), 'N');
  for (std::size_t index = 0; index < query.size(); ++index) {
    result[query.size() - index - 1] = Complement(query[index]);
  }
  return result;
}

std::vector<std::string> NormalizeRecords(
    const std::vector<sufkit::SequenceRecord>& records) {
  std::vector<std::string> normalized;
  normalized.reserve(records.size());
  for (const auto& record : records) {
    normalized.push_back(Normalize(record.sequence));
  }
  return normalized;
}

std::vector<ReferenceOccurrence> FindReferenceOccurrences(
    const std::vector<std::string>& references, const std::string& pattern) {
  std::vector<ReferenceOccurrence> result;
  for (std::size_t sequence_id = 0; sequence_id < references.size();
       ++sequence_id) {
    const auto& reference = references[sequence_id];
    for (std::size_t position = 0;
         position + pattern.size() <= reference.size(); ++position) {
      if (reference.compare(position, pattern.size(), pattern) == 0) {
        result.push_back(
            {static_cast<sufkit::SequenceId>(sequence_id), position});
      }
    }
  }
  return result;
}

std::uint64_t QueryOccurrences(const std::string& query,
                               const std::string& pattern) {
  std::uint64_t result = 0;
  for (std::size_t position = 0;
       position + pattern.size() <= query.size(); ++position) {
    if (query.compare(position, pattern.size(), pattern) == 0) {
      ++result;
    }
  }
  return result;
}

struct QualifyingInterval {
  std::size_t begin = 0;
  std::size_t end = 0;
  std::vector<ReferenceOccurrence> occurrences;
};

SmemOracleResult NaiveDirectionalSmems(
    const std::vector<std::string>& references,
    const std::string& oriented_query, std::size_t original_query_length,
    std::uint64_t min_length, std::uint64_t min_occurrences,
    sufkit::Strand strand) {
  std::vector<QualifyingInterval> qualifying;
  for (std::size_t begin = 0; begin < oriented_query.size(); ++begin) {
    if (oriented_query[begin] == 'N') {
      continue;
    }
    for (std::size_t end = begin + 1; end <= oriented_query.size(); ++end) {
      if (oriented_query[end - 1] == 'N') {
        break;
      }
      const auto length = end - begin;
      if (length < min_length) {
        continue;
      }
      auto occurrences = FindReferenceOccurrences(
          references, oriented_query.substr(begin, length));
      if (occurrences.size() >= min_occurrences) {
        qualifying.push_back({begin, end, std::move(occurrences)});
      }
    }
  }

  SmemOracleResult result;
  for (std::size_t index = 0; index < qualifying.size(); ++index) {
    const auto& candidate = qualifying[index];
    bool contained = false;
    for (std::size_t other_index = 0; other_index < qualifying.size();
         ++other_index) {
      if (index == other_index) {
        continue;
      }
      const auto& other = qualifying[other_index];
      if (other.begin <= candidate.begin && other.end >= candidate.end &&
          (other.begin < candidate.begin || other.end > candidate.end)) {
        contained = true;
        break;
      }
    }
    if (contained) {
      continue;
    }

    ++result.total_smems;
    const auto length = candidate.end - candidate.begin;
    const auto query_position =
        strand == sufkit::Strand::kReverseComplement
            ? original_query_length - (candidate.begin + length)
            : candidate.begin;
    for (const auto& occurrence : candidate.occurrences) {
      result.matches.emplace_back(
          query_position, occurrence.sequence_id, occurrence.position, length,
          candidate.occurrences.size(), strand);
    }
  }
  std::sort(result.matches.begin(), result.matches.end());
  result.matches.erase(
      std::unique(result.matches.begin(), result.matches.end()),
      result.matches.end());
  return result;
}

SmemOracleResult NaiveSmems(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& raw_query, std::uint64_t min_length,
    std::uint64_t min_occurrences, sufkit::StrandMode strands) {
  const auto references = NormalizeRecords(records);
  const auto forward = Normalize(raw_query);
  SmemOracleResult result;
  const auto append = [&](SmemOracleResult directional) {
    result.total_smems += directional.total_smems;
    result.matches.insert(result.matches.end(), directional.matches.begin(),
                          directional.matches.end());
  };
  if (strands == sufkit::StrandMode::kForward ||
      strands == sufkit::StrandMode::kBoth) {
    append(NaiveDirectionalSmems(references, forward, forward.size(),
                                 min_length, min_occurrences,
                                 sufkit::Strand::kForward));
  }
  if (strands == sufkit::StrandMode::kReverseComplement ||
      strands == sufkit::StrandMode::kBoth) {
    append(NaiveDirectionalSmems(
        references, ReverseComplement(raw_query), forward.size(), min_length,
        min_occurrences, sufkit::Strand::kReverseComplement));
  }
  std::sort(result.matches.begin(), result.matches.end());
  result.matches.erase(
      std::unique(result.matches.begin(), result.matches.end()),
      result.matches.end());
  return result;
}

std::vector<MumTuple> NaiveDirectionalMums(
    const std::vector<std::string>& references,
    const std::string& oriented_query, std::size_t original_query_length,
    std::uint64_t min_length, sufkit::Strand strand) {
  std::vector<MumTuple> result;
  for (std::size_t sequence_id = 0; sequence_id < references.size();
       ++sequence_id) {
    const auto& reference = references[sequence_id];
    for (std::size_t query_position = 0;
         query_position < oriented_query.size(); ++query_position) {
      if (oriented_query[query_position] == 'N') {
        continue;
      }
      for (std::size_t reference_position = 0;
           reference_position < reference.size(); ++reference_position) {
        if (reference[reference_position] == 'N') {
          continue;
        }
        std::size_t length = 0;
        while (query_position + length < oriented_query.size() &&
               reference_position + length < reference.size() &&
               oriented_query[query_position + length] != 'N' &&
               reference[reference_position + length] != 'N' &&
               oriented_query[query_position + length] ==
                   reference[reference_position + length]) {
          ++length;
        }
        const bool left_extendable =
            query_position > 0 && reference_position > 0 &&
            oriented_query[query_position - 1] != 'N' &&
            reference[reference_position - 1] != 'N' &&
            oriented_query[query_position - 1] ==
                reference[reference_position - 1];
        if (length < min_length || left_extendable) {
          continue;
        }
        const auto pattern = oriented_query.substr(query_position, length);
        if (FindReferenceOccurrences(references, pattern).size() != 1 ||
            QueryOccurrences(oriented_query, pattern) != 1) {
          continue;
        }
        const auto mapped_query_position =
            strand == sufkit::Strand::kReverseComplement
                ? original_query_length - (query_position + length)
                : query_position;
        result.emplace_back(
            mapped_query_position,
            static_cast<sufkit::SequenceId>(sequence_id), reference_position,
            length, strand);
      }
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<MumTuple> NaiveMums(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& raw_query, std::uint64_t min_length,
    sufkit::StrandMode strands) {
  const auto references = NormalizeRecords(records);
  const auto forward = Normalize(raw_query);
  std::vector<MumTuple> result;
  const auto append = [&](std::vector<MumTuple> directional) {
    result.insert(result.end(), directional.begin(), directional.end());
  };
  if (strands == sufkit::StrandMode::kForward ||
      strands == sufkit::StrandMode::kBoth) {
    append(NaiveDirectionalMums(references, forward, forward.size(),
                                min_length, sufkit::Strand::kForward));
  }
  if (strands == sufkit::StrandMode::kReverseComplement ||
      strands == sufkit::StrandMode::kBoth) {
    append(NaiveDirectionalMums(
        references, ReverseComplement(raw_query), forward.size(), min_length,
        sufkit::Strand::kReverseComplement));
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<SmemTuple> Tuples(const sufkit::SmemResult& result) {
  std::vector<SmemTuple> values;
  values.reserve(result.matches.size());
  for (const auto& match : result.matches) {
    values.emplace_back(match.query_position, match.sequence_id,
                        match.reference_position, match.length,
                        match.reference_occurrences, match.strand);
  }
  return values;
}

std::vector<MumTuple> Tuples(const sufkit::MumResult& result) {
  std::vector<MumTuple> values;
  values.reserve(result.matches.size());
  for (const auto& match : result.matches) {
    values.emplace_back(match.query_position, match.sequence_id,
                        match.reference_position, match.length, match.strand);
  }
  return values;
}

sufkit::SuffixArray Build(
    const std::vector<sufkit::SequenceRecord>& records,
    bool low_memory = false, std::uint32_t sampling_rate = 1) {
  auto options = low_memory ? sufkit::LowMemorySuffixArrayBuildOptions()
                            : sufkit::FastSuffixArrayBuildOptions();
  options.sampling_rate = sampling_rate;
  if (!low_memory) {
    options.acceleration = sufkit::SaAcceleration::kFull;
  }
  return sufkit::SuffixArray::Build(
      sufkit::GenomeReference::FromRecords(records), options);
}

void CheckSmemResult(const sufkit::SmemResult& observed,
                     const SmemOracleResult& expected) {
  CHECK(observed.total_smems == expected.total_smems);
  CHECK(observed.total_matches == expected.matches.size());
  CHECK(!observed.truncated);
  CHECK(Tuples(observed) == expected.matches);
}

void TestGeneralizedSmemAndAlgorithms() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "ACGTAACGTCGGGTTACGT"},
      {"r1", "", "CCACGTGGGNACGTAAA"}};
  const std::string query = "GGACGTAGGNTTACGTCC";
  const auto index = Build(records);

  for (const auto min_occurrences : {1ULL, 2ULL, 3ULL}) {
    const auto expected =
        NaiveSmems(records, query, 4, min_occurrences,
                   sufkit::StrandMode::kForward);
    for (const auto algorithm :
         {sufkit::MemSearchAlgorithm::kBaseline,
          sufkit::MemSearchAlgorithm::kLcp,
          sufkit::MemSearchAlgorithm::kChild,
          sufkit::MemSearchAlgorithm::kSuffixLink,
          sufkit::MemSearchAlgorithm::kFull,
          sufkit::MemSearchAlgorithm::kAutoSelect}) {
      sufkit::SmemOptions options;
      options.min_length = 4;
      options.min_occurrences = min_occurrences;
      options.algorithm = algorithm;
      CheckSmemResult(index.FindSmems(query, options), expected);
    }
  }

  // At c=2, ACGT is an SMEM even though one reference occurrence extends
  // through the following query A. This distinguishes generalized SMEM from
  // filtering ordinary per-coordinate MEM output.
  const std::vector<sufkit::SequenceRecord> threshold_records{
      {"threshold", "", "ACGTAACGTC"}};
  const auto generalized =
      NaiveSmems(threshold_records, "GGACGTAGG", 4, 2,
                 sufkit::StrandMode::kForward);
  CHECK(std::any_of(generalized.matches.begin(), generalized.matches.end(),
                    [](const auto& value) {
                      return std::get<0>(value) == 2 &&
                             std::get<3>(value) == 4 &&
                             std::get<4>(value) >= 2;
                    }));
}

void TestSmemLcpTraversal() {
  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  std::string shared;
  shared.reserve(272);
  for (std::size_t index = 0; index < 272; ++index) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    shared.push_back("ACGT"[(state >> 32U) & 3U]);
  }
  auto diverged = shared;
  diverged[260] = diverged[260] == 'A' ? 'C' : 'A';

  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "T" + shared + "G"},
      {"r1", "", "C" + shared + "A"},
      {"r2", "", "G" + diverged + "T"}};
  const std::string query = "N" + shared + "N";
  const auto expected =
      NaiveSmems(records, query, 256, 2, sufkit::StrandMode::kForward);
  CHECK(expected.total_smems != 0);
  auto mismatching = shared;
  mismatching[258] = mismatching[258] == 'A' ? 'C' : 'A';
  const std::string mismatch_query = "N" + mismatching + "N";
  const auto mismatch_expected = NaiveSmems(
      records, mismatch_query, 256, 2, sufkit::StrandMode::kForward);
  CHECK(mismatch_expected.total_smems != 0);

  auto low_build = sufkit::LowMemorySuffixArrayBuildOptions();
  const auto reference = sufkit::GenomeReference::FromRecords(records);
  const auto low = sufkit::SuffixArray::Build(reference, low_build);
  CHECK(low.GetInfo().lcp_encoding != sufkit::SaLcpEncoding::kNone);

  sufkit::SmemOptions options;
  options.min_length = 256;
  options.min_occurrences = 2;
  options.algorithm = sufkit::MemSearchAlgorithm::kLcp;
  CheckSmemResult(low.FindSmems(query, options), expected);
  CheckSmemResult(low.FindSmems(mismatch_query, options), mismatch_expected);

  const auto path = std::filesystem::current_path() /
                    "sufkit-smem-lcp-roundtrip.sufidx";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  low.Save(path);
  const auto loaded = sufkit::SuffixArray::Load(path);
  std::filesystem::remove(path, ignored);
  CHECK(loaded.GetInfo().lcp_encoding != sufkit::SaLcpEncoding::kNone);
  CheckSmemResult(loaded.FindSmems(query, options), expected);
  CheckSmemResult(loaded.FindSmems(mismatch_query, options),
                  mismatch_expected);
  options.algorithm = sufkit::MemSearchAlgorithm::kAutoSelect;
  CheckSmemResult(loaded.FindSmems(query, options), expected);

  auto sa_only_build = sufkit::FastSuffixArrayBuildOptions();
  sa_only_build.acceleration = sufkit::SaAcceleration::kNone;
  const auto sa_only = sufkit::SuffixArray::Build(reference, sa_only_build);
  options.algorithm = sufkit::MemSearchAlgorithm::kLcp;
  CheckError(sufkit::ErrorCode::kUnsupportedBackend,
             [&] { (void)sa_only.FindSmems(query, options); });
  options.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
  CheckSmemResult(sa_only.FindSmems(query, options), expected);
}

void TestSmemContractsAndProfiles() {
  const std::vector<sufkit::SequenceRecord> records{
      {"a", "", "TTACGTACGTGGGNACGTACGT"},
      {"b", "", "CCACGTACGTAAATTT"}};
  const std::string query = "aaACGTACGTnnACGT";
  for (const auto strands :
       {sufkit::StrandMode::kForward,
        sufkit::StrandMode::kReverseComplement,
        sufkit::StrandMode::kBoth}) {
    const auto expected = NaiveSmems(records, query, 4, 1, strands);
    for (const bool low_memory : {false, true}) {
      const auto index = Build(records, low_memory);
      sufkit::SmemOptions options;
      options.min_length = 4;
      options.min_occurrences = 1;
      options.strands = strands;
      CheckSmemResult(index.FindSmems(query, options), expected);
    }
  }

  const auto index = Build(records);
  sufkit::SmemOptions options;
  options.min_length = 4;
  options.min_occurrences = 1;
  const auto complete = index.FindSmems(query, options);
  const auto complete_tuples = Tuples(complete);
  const auto limited = index.FindSmems(query, options, 1);
  CHECK(limited.total_smems == complete.total_smems);
  CHECK(limited.total_matches == complete.total_matches);
  CHECK(limited.matches.size() ==
        std::min<std::uint64_t>(1, complete.total_matches));
  CHECK(limited.truncated == (complete.total_matches > 1));
  CHECK(Tuples(limited) ==
        std::vector<SmemTuple>(
            complete_tuples.begin(),
            complete_tuples.begin() + limited.matches.size()));
  const auto count_only = index.FindSmems(query, options, 0);
  CHECK(count_only.total_smems == complete.total_smems);
  CHECK(count_only.total_matches == complete.total_matches);
  CHECK(count_only.matches.empty());
  CHECK(count_only.truncated == (complete.total_matches != 0));

  std::vector<SmemTuple> streamed;
  index.ForEachSmem(query, options, [&](const sufkit::SmemMatch& match) {
    streamed.emplace_back(match.query_position, match.sequence_id,
                          match.reference_position, match.length,
                          match.reference_occurrences, match.strand);
  });
  std::sort(streamed.begin(), streamed.end());
  CHECK(streamed == complete_tuples);

  bool propagated = false;
  try {
    index.ForEachSmem(query, options, [](const sufkit::SmemMatch&) {
      throw std::runtime_error("SMEM callback sentinel");
    });
  } catch (const std::runtime_error& error) {
    propagated = std::string(error.what()) == "SMEM callback sentinel";
  }
  CHECK(propagated);

  auto invalid = options;
  invalid.min_length = 0;
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)index.FindSmems(query, invalid); });
  invalid = options;
  invalid.min_occurrences = 0;
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)index.FindSmems(query, invalid); });
  invalid = options;
  invalid.strands = static_cast<sufkit::StrandMode>(255);
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)index.FindSmems(query, invalid); });

  auto too_long = options;
  too_long.min_length = std::numeric_limits<std::uint64_t>::max();
  CHECK(index.FindSmems("NACGT", too_long).total_matches == 0);

  auto impossible = options;
  impossible.min_occurrences = 1000000;
  const auto empty = index.FindSmems(query, impossible);
  CHECK(empty.total_smems == 0);
  CHECK(empty.total_matches == 0);
  CHECK(empty.matches.empty());
  CHECK(!empty.truncated);

  const auto sampled = Build(records, false, 2);
  CheckError(sufkit::ErrorCode::kUnsupportedBackend,
             [&] { (void)sampled.FindSmems(query, options); });

  const auto low = Build(records, true);
  auto unavailable_lookup = options;
  unavailable_lookup.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
  unavailable_lookup.lookup_algorithm =
      sufkit::SaSearchAlgorithm::kSaplingPwl;
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)low.FindSmems("", unavailable_lookup);
  });
  unavailable_lookup.lookup_algorithm = sufkit::SaSearchAlgorithm::kChild;
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    low.ForEachSmem("A", unavailable_lookup,
                    [](const sufkit::SmemMatch&) {});
  });
  unavailable_lookup.lookup_algorithm =
      sufkit::SaSearchAlgorithm::kSaplingPwl;
  unavailable_lookup.min_occurrences =
      std::numeric_limits<std::uint64_t>::max();
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)low.FindSmems(query, unavailable_lookup);
  });
}

void TestStrictMumAndAlgorithms() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTGATTACAGGCCCTACGAAT"},
      {"r1", "", "AACCGGTTAACCC"}};
  const std::vector<std::string> queries{
      "AAGATTACATT",                   // Unique in query and reference.
      "GATTACANNNGATTACA",             // MAM but not MUM.
      "TTTACGAAGG",                    // Another unique match.
      "NNGATTACANN"};
  const auto index = Build(records);
  for (const auto& query : queries) {
    const auto expected =
        NaiveMums(records, query, 4, sufkit::StrandMode::kForward);
    for (const auto algorithm :
         {sufkit::MemSearchAlgorithm::kBaseline,
          sufkit::MemSearchAlgorithm::kLcp,
          sufkit::MemSearchAlgorithm::kChild,
          sufkit::MemSearchAlgorithm::kSuffixLink,
          sufkit::MemSearchAlgorithm::kFull,
          sufkit::MemSearchAlgorithm::kAutoSelect}) {
      sufkit::MumOptions options;
      options.min_length = 4;
      options.algorithm = algorithm;
      const auto observed = index.FindMums(query, options);
      CHECK(observed.total_matches == expected.size());
      CHECK(Tuples(observed) == expected);
    }
  }

  sufkit::MumOptions options;
  options.min_length = 7;
  CHECK(index.FindMums("GATTACA", options).total_matches != 0);
  CHECK(index.FindMums("GATTACANNNGATTACA", options).total_matches == 0);

  auto duplicated_reference = records;
  duplicated_reference.push_back({"duplicate", "", "CCGATTACATT"});
  const auto duplicated_index = Build(duplicated_reference);
  CHECK(duplicated_index.FindMums("GATTACA", options).total_matches == 0);

  const std::vector<sufkit::SequenceRecord> overlapping_records{
      {"overlap", "", "CCAAAAAGG"}};
  const auto overlapping = Build(overlapping_records);
  options.min_length = 5;
  // Both overlapping query occurrences map to the one reference interval, so
  // the reference-MAM is not unique in the query and cannot be a strict MUM.
  CHECK(overlapping.FindMums("AAAAAA", options).total_matches == 0);

  const std::vector<sufkit::SequenceRecord> containment_records{
      {"containment", "", "TTGATTACAGG"}};
  const std::string containment_query =
      "AAGATTACAGGCCGATTACATT";
  const auto containment = Build(containment_records);
  options.min_length = 7;
  const auto containment_expected =
      NaiveMums(containment_records, containment_query, 7,
                sufkit::StrandMode::kForward);
  CHECK(Tuples(containment.FindMums(containment_query, options)) ==
        containment_expected);
  CHECK(!containment_expected.empty());
}

void TestMumContractsAndProfiles() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTGATTACAGGCCCTACGAAT"},
      {"r1", "", "AACCGGTTAACCC"}};
  const std::string query = "aaGATTACAttNNTACGAA";
  for (const auto strands :
       {sufkit::StrandMode::kForward,
        sufkit::StrandMode::kReverseComplement,
        sufkit::StrandMode::kBoth}) {
    const auto expected = NaiveMums(records, query, 4, strands);
    for (const bool low_memory : {false, true}) {
      const auto index = Build(records, low_memory);
      sufkit::MumOptions options;
      options.min_length = 4;
      options.strands = strands;
      const auto observed = index.FindMums(query, options);
      CHECK(observed.total_matches == expected.size());
      CHECK(Tuples(observed) == expected);
    }
  }

  const auto index = Build(records);
  sufkit::MumOptions options;
  options.min_length = 4;
  const auto complete = index.FindMums(query, options);
  const auto complete_tuples = Tuples(complete);
  const auto limited = index.FindMums(query, options, 1);
  CHECK(limited.total_matches == complete.total_matches);
  CHECK(limited.matches.size() ==
        std::min<std::uint64_t>(1, complete.total_matches));
  CHECK(limited.truncated == (complete.total_matches > 1));
  CHECK(Tuples(limited) ==
        std::vector<MumTuple>(
            complete_tuples.begin(),
            complete_tuples.begin() + limited.matches.size()));
  const auto count_only = index.FindMums(query, options, 0);
  CHECK(count_only.total_matches == complete.total_matches);
  CHECK(count_only.matches.empty());
  CHECK(count_only.truncated == (complete.total_matches != 0));

  std::vector<MumTuple> streamed;
  index.ForEachMum(query, options, [&](const sufkit::MumMatch& match) {
    streamed.emplace_back(match.query_position, match.sequence_id,
                          match.reference_position, match.length,
                          match.strand);
  });
  std::sort(streamed.begin(), streamed.end());
  CHECK(streamed == complete_tuples);

  bool propagated = false;
  try {
    index.ForEachMum(query, options, [](const sufkit::MumMatch&) {
      throw std::runtime_error("MUM callback sentinel");
    });
  } catch (const std::runtime_error& error) {
    propagated = std::string(error.what()) == "MUM callback sentinel";
  }
  CHECK(propagated);

  auto invalid = options;
  invalid.min_length = 0;
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)index.FindMums(query, invalid); });
  invalid = options;
  invalid.strands = static_cast<sufkit::StrandMode>(255);
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)index.FindMums(query, invalid); });

  auto too_long = options;
  too_long.min_length = std::numeric_limits<std::uint64_t>::max();
  CHECK(index.FindMums("NACGT", too_long).total_matches == 0);

  const auto sampled = Build(records, false, 2);
  CheckError(sufkit::ErrorCode::kUnsupportedBackend,
             [&] { (void)sampled.FindMums(query, options); });

  const auto low = Build(records, true);
  auto suffix_link_smem = sufkit::SmemOptions{};
  suffix_link_smem.min_length = 4;
  suffix_link_smem.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
  CheckError(sufkit::ErrorCode::kUnsupportedBackend,
             [&] { (void)low.FindSmems(query, suffix_link_smem); });
  auto suffix_link_mum = options;
  suffix_link_mum.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
  CheckError(sufkit::ErrorCode::kUnsupportedBackend,
             [&] { (void)low.FindMums(query, suffix_link_mum); });

  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    index.ForEachSmem(query, sufkit::SmemOptions{}, {});
  });
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    index.ForEachMum(query, sufkit::MumOptions{}, {});
  });
}

void TestWidthsBackendsAndLookups() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTACGTACGTGGGCCGATTACAT"},
      {"r1", "", "CCACGTACGTAAATTT"}};
  const std::string query = "GGACGTACGTCCNNGATTACATT";
  const auto expected_smems =
      NaiveSmems(records, query, 4, 1, sufkit::StrandMode::kForward);
  const auto expected_mums =
      NaiveMums(records, query, 4, sufkit::StrandMode::kForward);
  const auto reference = sufkit::GenomeReference::FromRecords(records);

  for (const auto width : {sufkit::CoordinateWidth::kBits32,
                           sufkit::CoordinateWidth::kBits64}) {
    auto build = sufkit::FastSuffixArrayBuildOptions();
    build.coordinate_width = width;
    build.acceleration = sufkit::SaAcceleration::kFull;
    const auto index = sufkit::SuffixArray::Build(reference, build);
    sufkit::SmemOptions smem;
    smem.min_length = 4;
    CheckSmemResult(index.FindSmems(query, smem), expected_smems);
    sufkit::MumOptions mum;
    mum.min_length = 4;
    const auto observed_mums = index.FindMums(query, mum);
    CHECK(observed_mums.total_matches == expected_mums.size());
    CHECK(Tuples(observed_mums) == expected_mums);
  }

  const std::array<sufkit::CoordinateStorageWidth, 4> storage_widths{
      {sufkit::CoordinateStorageWidth::kBits32,
       sufkit::CoordinateStorageWidth::kBits40,
       sufkit::CoordinateStorageWidth::kBits48,
       sufkit::CoordinateStorageWidth::kBits64}};
  for (const auto storage_width : storage_widths) {
    auto build = sufkit::FastSuffixArrayBuildOptions();
    build.backend = sufkit::SaBackend::kDivsufsort;
    build.coordinate_width = sufkit::CoordinateWidth::kBits64;
    build.storage_width = storage_width;
    build.acceleration = sufkit::SaAcceleration::kFull;
    const auto index = sufkit::SuffixArray::Build(reference, build);

    for (const auto algorithm :
         {sufkit::MemSearchAlgorithm::kSuffixLink,
          sufkit::MemSearchAlgorithm::kFull}) {
      sufkit::SmemOptions smem;
      smem.min_length = 4;
      smem.algorithm = algorithm;
      CheckSmemResult(index.FindSmems(query, smem), expected_smems);

      sufkit::MumOptions mum;
      mum.min_length = 4;
      mum.algorithm = algorithm;
      const auto observed_mums = index.FindMums(query, mum);
      CHECK(observed_mums.total_matches == expected_mums.size());
      CHECK(Tuples(observed_mums) == expected_mums);
    }

    const auto width_value = static_cast<unsigned>(storage_width);
    const auto path = std::filesystem::current_path() /
                      ("sufkit-smem-mum-width-" +
                       std::to_string(width_value) + ".sufidx");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    index.Save(path);
    const auto loaded = sufkit::SuffixArray::Load(path);
    std::filesystem::remove(path, ignored);
    CHECK(loaded.GetInfo().stored_coordinate_width == width_value);

    sufkit::SmemOptions smem;
    smem.min_length = 4;
    smem.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
    CheckSmemResult(loaded.FindSmems(query, smem), expected_smems);
    sufkit::MumOptions mum;
    mum.min_length = 4;
    mum.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
    const auto observed_mums = loaded.FindMums(query, mum);
    CHECK(observed_mums.total_matches == expected_mums.size());
    CHECK(Tuples(observed_mums) == expected_mums);
  }

  auto learned_build = sufkit::FastSuffixArrayBuildOptions();
  learned_build.acceleration = sufkit::SaAcceleration::kFull;
  learned_build.learned_index.enabled = true;
  learned_build.learned_index.k = 4;
  learned_build.learned_index.bucket_bits = 3;
  const auto learned = sufkit::SuffixArray::Build(reference, learned_build);
  for (const auto lookup : {sufkit::SaSearchAlgorithm::kBinary,
                            sufkit::SaSearchAlgorithm::kLcpBinary,
                            sufkit::SaSearchAlgorithm::kSaplingPwl,
                            sufkit::SaSearchAlgorithm::kChild,
                            sufkit::SaSearchAlgorithm::kAutoSelect}) {
    sufkit::SmemOptions smem;
    smem.min_length = 4;
    smem.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
    smem.lookup_algorithm = lookup;
    CheckSmemResult(learned.FindSmems(query, smem), expected_smems);
    sufkit::MumOptions mum;
    mum.min_length = 4;
    mum.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
    mum.lookup_algorithm = lookup;
    const auto observed_mums = learned.FindMums(query, mum);
    CHECK(observed_mums.total_matches == expected_mums.size());
    CHECK(Tuples(observed_mums) == expected_mums);
  }

  const auto backends = sufkit::AvailableSaBackends();
  const auto caps = std::find_if(
      backends.begin(), backends.end(), [](const auto& descriptor) {
        return descriptor.name == "caps" && descriptor.available;
      });
  if (caps != backends.end()) {
    auto build = sufkit::FastSuffixArrayBuildOptions();
    build.backend = sufkit::SaBackend::kCaps;
    build.coordinate_width = sufkit::CoordinateWidth::kBits32;
    build.threads = 2;
    build.acceleration = sufkit::SaAcceleration::kFull;
    const auto index = sufkit::SuffixArray::Build(reference, build);
    sufkit::SmemOptions smem;
    smem.min_length = 4;
    CheckSmemResult(index.FindSmems(query, smem), expected_smems);
    sufkit::MumOptions mum;
    mum.min_length = 4;
    CHECK(Tuples(index.FindMums(query, mum)) == expected_mums);
  }
}

void TestSuffixLinkBudgetFallsBackToRoot() {
  // Deleting the leading C expands the A^32 prefix beyond the production
  // suffix-link scan budget. SMEM and strict MUM must discard that partial
  // interval and recover the next query start through an exact root lookup.
  constexpr std::size_t kProbeBudget = 4096;
  constexpr std::size_t kMinLength = 32;
  constexpr std::size_t kRepeatBases =
      kProbeBudget + kMinLength + 16;
  const std::vector<sufkit::SequenceRecord> records{
      {"ref", "", "C" + std::string(kRepeatBases, 'A') + "G"}};
  const std::string query = "C" + std::string(kMinLength, 'A') + "G";
  const auto index = Build(records);
  const auto expected_smems =
      NaiveSmems(records, query, kMinLength, 1,
                 sufkit::StrandMode::kForward);
  const auto expected_mums =
      NaiveMums(records, query, kMinLength,
                sufkit::StrandMode::kForward);

  for (const auto algorithm :
       {sufkit::MemSearchAlgorithm::kBaseline,
        sufkit::MemSearchAlgorithm::kLcp,
        sufkit::MemSearchAlgorithm::kSuffixLink,
        sufkit::MemSearchAlgorithm::kFull,
        sufkit::MemSearchAlgorithm::kAutoSelect}) {
    sufkit::SmemOptions smem;
    smem.min_length = kMinLength;
    smem.algorithm = algorithm;
    CheckSmemResult(index.FindSmems(query, smem), expected_smems);

    sufkit::MumOptions mum;
    mum.min_length = kMinLength;
    mum.algorithm = algorithm;
    const auto observed_mums = index.FindMums(query, mum);
    CHECK(observed_mums.total_matches == expected_mums.size());
    CHECK(Tuples(observed_mums) == expected_mums);
  }
}

void TestRandomDifferential() {
  std::mt19937_64 generator(20260830);
  const auto random_base = [&] { return "ACGTN"[generator() % 5]; };
  for (int trial = 0; trial < 20; ++trial) {
    std::vector<sufkit::SequenceRecord> records;
    for (int sequence_id = 0; sequence_id < 2; ++sequence_id) {
      std::string sequence(20, 'A');
      std::generate(sequence.begin(), sequence.end(), random_base);
      records.push_back(
          {"r" + std::to_string(sequence_id), "", std::move(sequence)});
    }
    std::string query(16, 'A');
    std::generate(query.begin(), query.end(), random_base);
    const auto index = Build(records);
    for (const auto occurrences : {1ULL, 2ULL}) {
      sufkit::SmemOptions options;
      options.min_length = 3;
      options.min_occurrences = occurrences;
      const auto expected =
          NaiveSmems(records, query, 3, occurrences,
                     sufkit::StrandMode::kForward);
      CheckSmemResult(index.FindSmems(query, options), expected);
    }
    sufkit::MumOptions options;
    options.min_length = 3;
    const auto expected_mums =
        NaiveMums(records, query, 3, sufkit::StrandMode::kForward);
    const auto observed = index.FindMums(query, options);
    CHECK(observed.total_matches == expected_mums.size());
    CHECK(Tuples(observed) == expected_mums);
  }
}

void TestPersistenceAndConcurrency() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTGATTACAGGACGTACGTACGT"},
      {"r1", "", "CCACGTACGTAAAATTTT"}};
  const std::string query = "aaGATTACAccNNACGTACGT";
  auto index = Build(records);
  const auto path = std::filesystem::current_path() /
                    "sufkit-smem-mum-roundtrip.sufidx";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  index.Save(path);
  const auto loaded = sufkit::SuffixArray::Load(path);
  std::filesystem::remove(path, ignored);

  sufkit::SmemOptions smem_options;
  smem_options.min_length = 4;
  smem_options.min_occurrences = 1;
  sufkit::MumOptions mum_options;
  mum_options.min_length = 4;
  const auto expected_smems = Tuples(index.FindSmems(query, smem_options));
  const auto expected_mums = Tuples(index.FindMums(query, mum_options));
  CHECK(Tuples(loaded.FindSmems(query, smem_options)) == expected_smems);
  CHECK(Tuples(loaded.FindMums(query, mum_options)) == expected_mums);

  std::atomic<bool> consistent{true};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < 20; ++iteration) {
        if (Tuples(loaded.FindSmems(query, smem_options)) != expected_smems ||
            Tuples(loaded.FindMums(query, mum_options)) != expected_mums) {
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

void TestLegacySaFixtures() {
  const std::vector<sufkit::SequenceRecord> records{
      {"chr1", "first", "ACGTNACGT"},
      {"chr2", "second", "TTTACGTAAA"},
      {"chr3", "third", "AAA"}};
  const std::string query = "GGACGTAAANACGTCC";
  const auto expected_smems =
      NaiveSmems(records, query, 3, 1, sufkit::StrandMode::kForward);
  const auto expected_mums =
      NaiveMums(records, query, 3, sufkit::StrandMode::kForward);
  const auto fixture_directory =
      std::filesystem::path(__FILE__).parent_path() / "data";

  struct LegacyFixture {
    const char* filename;
    std::uint32_t sampling_rate;
  };
  const std::array<LegacyFixture, 4> fixtures{{
      {"legacy-sa-v1.0.fixture", 1},
      {"legacy-sa-v1.1.fixture", 1},
      {"legacy-sa-v1.2.fixture", 1},
      {"legacy-sa-v1.3.fixture", 2},
  }};

  for (const auto& fixture : fixtures) {
    const auto path = fixture_directory / fixture.filename;
    CHECK(std::filesystem::is_regular_file(path));
    const auto loaded = sufkit::SuffixArray::Load(path);
    CHECK(loaded.SamplingRate() == fixture.sampling_rate);

    sufkit::SmemOptions smem;
    smem.min_length = 3;
    smem.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
    smem.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
    sufkit::MumOptions mum;
    mum.min_length = 3;
    mum.algorithm = sufkit::MemSearchAlgorithm::kBaseline;
    mum.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;

    if (fixture.sampling_rate == 1) {
      CheckSmemResult(loaded.FindSmems(query, smem), expected_smems);
      const auto observed_mums = loaded.FindMums(query, mum);
      CHECK(observed_mums.total_matches == expected_mums.size());
      CHECK(Tuples(observed_mums) == expected_mums);
    } else {
      CheckError(sufkit::ErrorCode::kUnsupportedBackend,
                 [&] { (void)loaded.FindSmems(query, smem); });
      CheckError(sufkit::ErrorCode::kUnsupportedBackend,
                 [&] { (void)loaded.FindMums(query, mum); });
    }
  }
}

}  // namespace

int main() {
  TestGeneralizedSmemAndAlgorithms();
  TestSmemLcpTraversal();
  TestSmemContractsAndProfiles();
  TestStrictMumAndAlgorithms();
  TestMumContractsAndProfiles();
  TestWidthsBackendsAndLookups();
  TestSuffixLinkBudgetFallsBackToRoot();
  TestRandomDifferential();
  TestPersistenceAndConcurrency();
  TestLegacySaFixtures();
  if (failures != 0) {
    std::cerr << failures << " SMEM/MUM assertion(s) failed\n";
    return 1;
  }
  std::cout << "SMEM/MUM tests passed\n";
  return 0;
}
