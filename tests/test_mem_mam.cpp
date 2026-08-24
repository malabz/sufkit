// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
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
                   records, query.substr(static_cast<std::size_t>(query_position),
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
    CHECK(Tuples(observed) == NaiveMams(records, query, 7));
    CHECK(observed.total_matches == 2);
  }

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
      }
    }
  }
}

void TestWidthsAndConstructors() {
  const std::vector<sufkit::SequenceRecord> records{
      {"a", "", "ACGTACGTGATTACACCCCGGGGTTTTACGTACGT"},
      {"b", "", "TTTTACGTACGTAAAACCCCGATTACAGATTACA"}};
  const std::string query = "GGACGTACGTGATTACATTTT";
  const auto expected = NaiveMems(records, query, 8);
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
      CHECK(Tuples(index.FindMems(query, options)) == expected);
      if (rate == 1) {
        sufkit::MamOptions mam;
        mam.min_length = 4;
        mam.algorithm = sufkit::MemSearchAlgorithm::kFull;
        CHECK(Tuples(index.FindMams(query, mam)) ==
              NaiveMams(records, query, 4));
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
  TestSampledAndSkip();
  TestRandomDifferential();
  TestWidthsAndConstructors();
  TestContractsAndPersistence();
  if (failures != 0) {
    std::cerr << failures << " MEM/MAM assertion(s) failed\n";
    return 1;
  }
  std::cout << "MEM/MAM tests passed\n";
  return 0;
}
