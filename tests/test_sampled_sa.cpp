// SPDX-License-Identifier: MIT

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
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

template <class Function>
void ExpectError(sufkit::ErrorCode code, Function&& function) {
  try {
    function();
    CHECK(false);
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == code);
  }
}

sufkit::GenomeReference Reference() {
  return sufkit::GenomeReference::FromRecords(
      {{"alpha", "", "ACGTACGTGATTACACCCCGGGGTTTTACGTACGTNNACGTGATTACA"},
       {"beta", "", "TTTTACGTACGTAAAACCCCGATTACAGATTACATGCATGCAACGT"},
       {"repeat", "", "ACGTACGTACGTACGTACGTGATTACAGATTACAGATTACATTTT"}});
}

sufkit::SuffixArrayBuildOptions BuildOptions(sufkit::SaBackend backend,
                                             std::uint32_t sampling_rate,
                                             bool learned = false) {
  sufkit::SuffixArrayBuildOptions options;
  options.backend = backend;
  options.coordinate_width = sufkit::CoordinateWidth::kBits32;
  options.threads = backend == sufkit::SaBackend::kCaps ? 2 : 1;
  options.sampling_rate = sampling_rate;
  options.acceleration = sufkit::SaAcceleration::kFull;
  options.learned_index.enabled = learned;
  options.learned_index.k = 4;
  options.learned_index.bucket_bits = 4;
  return options;
}

bool SameQueryResult(const sufkit::QueryResult& left,
                     const sufkit::QueryResult& right) {
  if (left.total_hits != right.total_hits ||
      left.truncated != right.truncated ||
      left.hits.size() != right.hits.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.hits.size(); ++index) {
    const auto& a = left.hits[index];
    const auto& b = right.hits[index];
    if (std::tie(a.sequence_id, a.position, a.length, a.strand) !=
        std::tie(b.sequence_id, b.position, b.length, b.strand)) {
      return false;
    }
  }
  return true;
}

bool SameRightMaximalResult(const sufkit::RightMaximalResult& left,
                            const sufkit::RightMaximalResult& right) {
  if (left.total_matches != right.total_matches ||
      left.matches.size() != right.matches.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.matches.size(); ++index) {
    const auto& a = left.matches[index];
    const auto& b = right.matches[index];
    if (std::tie(a.sequence_id, a.reference_position, a.query_position,
                 a.length,
                 a.strand) != std::tie(b.sequence_id, b.reference_position,
                                       b.query_position, b.length, b.strand)) {
      return false;
    }
  }
  return true;
}

void CheckExact(const sufkit::SuffixArray& full,
                const sufkit::SuffixArray& sampled) {
  const std::vector<std::string> patterns{
      "A",        "AC",       "ACG",      "ACGT",     "GATTACA",
      "TTTTACGT", "TGCATGCA", "CCCCGGGG", "AAAAAAAA", "ACGTGATTACAC"};
  for (const auto& pattern : patterns) {
    for (const auto strands :
         {sufkit::StrandMode::kForward, sufkit::StrandMode::kReverseComplement,
          sufkit::StrandMode::kBoth}) {
      CHECK(full.Count(pattern, strands) == sampled.Count(pattern, strands));
      sufkit::LocateOptions options;
      options.strands = strands;
      CHECK(SameQueryResult(full.Locate(pattern, options),
                            sampled.Locate(pattern, options)));
      for (const std::uint64_t limit : {0ULL, 1ULL, 1000ULL}) {
        options.max_hits = limit;
        CHECK(SameQueryResult(full.Locate(pattern, options),
                              sampled.Locate(pattern, options)));
      }
    }
  }
}

void CheckRightMaximalMatches(const sufkit::SuffixArray& full,
                              const sufkit::SuffixArray& sampled,
                              std::uint64_t min_length) {
  const std::vector<std::string> queries{
      "GGACGTACGTGATTACATTTT", "NNACGTGATTACANNTGCATGCA", "GATTACAGATTACAGG",
      "ACGTACGTACGTACGT"};
  for (const auto algorithm : {sufkit::RightMaximalSearchAlgorithm::kBaseline,
                               sufkit::RightMaximalSearchAlgorithm::kLcp,
                               sufkit::RightMaximalSearchAlgorithm::kChild,
                               sufkit::RightMaximalSearchAlgorithm::kSuffixLink,
                               sufkit::RightMaximalSearchAlgorithm::kFull}) {
    sufkit::RightMaximalOptions options;
    options.min_length = min_length;
    options.strands = sufkit::StrandMode::kBoth;
    options.algorithm = algorithm;
    for (const auto& query : queries) {
      CHECK(SameRightMaximalResult(
          full.FindRightMaximalMatches(query, options),
          sampled.FindRightMaximalMatches(query, options)));
    }
  }
}

void RunBackend(sufkit::SaBackend backend,
                const std::filesystem::path& directory) {
  const auto ref = Reference();
  auto full = sufkit::SuffixArray::Build(ref, BuildOptions(backend, 1));
  for (const std::uint32_t rate : {2U, 4U, 8U}) {
    auto sampled =
        sufkit::SuffixArray::Build(ref, BuildOptions(backend, rate, true));
    const auto expected = (sampled.GetInfo().text_symbols + rate - 1) / rate;
    CHECK(sampled.SamplingRate() == rate);
    CHECK(sampled.GetInfo().sa_sampling_rate == rate);
    CHECK(sampled.GetInfo().suffix_count == expected);
    CHECK(sampled.GetInfo().format_version == "1.4");
    for (std::uint64_t row = 0; row < expected; ++row) {
      CHECK(sampled.SuffixAt(row) % rate == 0);
    }
    ExpectError(sufkit::ErrorCode::kUnsupportedBackend,
                [&] { (void)sampled.EqualRange("ACGT"); });
    CheckExact(full, sampled);
    CheckRightMaximalMatches(full, sampled, std::max<std::uint64_t>(8, rate));

    const auto path =
        directory /
        (std::string(backend == sufkit::SaBackend::kCaps ? "caps" : "div") +
         "-sample-" + std::to_string(rate) + ".sufidx");
    sampled.Save(path);
    const auto inspected = sufkit::InspectIndex(path);
    CHECK(inspected.sa_sampling_rate == rate);
    CHECK(inspected.suffix_count == expected);
    auto loaded = sufkit::SuffixArray::Load(path);
    CHECK(loaded.SamplingRate() == rate);
    CheckExact(full, loaded);
    CheckRightMaximalMatches(full, loaded, std::max<std::uint64_t>(8, rate));
  }

  sufkit::SuffixArrayBuildOptions invalid = BuildOptions(backend, 1);
  invalid.sampling_rate = 0;
  ExpectError(sufkit::ErrorCode::kInvalidInput,
              [&] { (void)sufkit::SuffixArray::Build(ref, invalid); });
  auto sampled4 = sufkit::SuffixArray::Build(ref, BuildOptions(backend, 4));
  sufkit::RightMaximalOptions too_short;
  too_short.min_length = 3;
  ExpectError(sufkit::ErrorCode::kInvalidInput, [&] {
    (void)sampled4.FindRightMaximalMatches("ACGTACGT", too_short);
  });
}

void CheckDirectLcpConstruction(const std::filesystem::path& directory) {
  const auto ref = Reference();
  auto full_options =
      BuildOptions(sufkit::SaBackend::kDivsufsort, 1);
  auto full = sufkit::SuffixArray::Build(ref, full_options);
  const std::vector<std::string> queries{
      "GGACGTACGTGATTACATTTT", "NNACGTGATTACANNTGCATGCA",
      "GATTACAGATTACAGG", "ACGTACGTACGTACGT"};

  for (const std::uint32_t rate : {1U, 2U, 4U, 8U}) {
    auto options = BuildOptions(sufkit::SaBackend::kDivsufsort, rate);
    options.acceleration = sufkit::SaAcceleration::kLcpSuffixLink;
    sufkit::SuffixArrayBuildStatistics statistics;
    options.statistics = &statistics;
    auto direct = sufkit::SuffixArray::Build(ref, options);
    constexpr auto kExpectedLcpEncoding = sufkit::SaLcpEncoding::kRaw;
    CHECK(direct.GetInfo().lcp_encoding == kExpectedLcpEncoding);
    CHECK(direct.GetInfo().isa_bytes != 0);
    CHECK(direct.GetInfo().child_bytes == 0);
    CHECK(statistics.isa_seconds > 0.0);
    CHECK(statistics.lcp_seconds > 0.0);
    CheckExact(full, direct);

    for (const auto& query : queries) {
      sufkit::RightMaximalOptions query_options;
      query_options.min_length = std::max<std::uint64_t>(8, rate);
      query_options.strands = sufkit::StrandMode::kBoth;
      query_options.algorithm =
          sufkit::RightMaximalSearchAlgorithm::kBaseline;
      const auto expected =
          full.FindRightMaximalMatches(query, query_options);
      for (const auto algorithm :
           {sufkit::RightMaximalSearchAlgorithm::kBaseline,
            sufkit::RightMaximalSearchAlgorithm::kLcp,
            sufkit::RightMaximalSearchAlgorithm::kSuffixLink}) {
        query_options.algorithm = algorithm;
        CHECK(SameRightMaximalResult(
            expected, direct.FindRightMaximalMatches(query, query_options)));
      }
    }

    const auto path = directory /
                      ("div-direct-sample-" + std::to_string(rate) +
                       ".sufidx");
    direct.Save(path);
    const auto inspected = sufkit::InspectIndex(path);
    CHECK(inspected.lcp_encoding == kExpectedLcpEncoding);
    CHECK(inspected.sa_acceleration ==
          sufkit::SaAcceleration::kLcpSuffixLink);
    auto loaded = sufkit::SuffixArray::Load(path);
    CHECK(loaded.GetInfo().lcp_encoding == kExpectedLcpEncoding);
    CheckExact(full, loaded);
  }
}

void RandomizedDifferential() {
  std::mt19937_64 generator(20260823);
  const auto base = [&] { return "ACGT"[generator() & 3U]; };
  std::vector<sufkit::SequenceRecord> records;
  for (int record = 0; record < 3; ++record) {
    std::string sequence;
    for (int position = 0; position < 320; ++position) {
      sequence.push_back(position % 79 == 0 ? 'N' : base());
    }
    records.push_back(
        {"random-" + std::to_string(record), "", std::move(sequence)});
  }
  const auto ref = sufkit::GenomeReference::FromRecords(records);
  auto full = sufkit::SuffixArray::Build(
      ref, BuildOptions(sufkit::SaBackend::kDivsufsort, 1));
  for (const std::uint32_t rate : {2U, 3U, 5U}) {
    auto sampled = sufkit::SuffixArray::Build(
        ref, BuildOptions(sufkit::SaBackend::kDivsufsort, rate));
    for (int trial = 0; trial < 200; ++trial) {
      const auto length = static_cast<std::size_t>(1 + generator() % 15);
      std::string pattern;
      for (std::size_t index = 0; index < length; ++index) {
        pattern.push_back(base());
      }
      CHECK(full.Count(pattern, sufkit::StrandMode::kBoth) ==
            sampled.Count(pattern, sufkit::StrandMode::kBoth));
      sufkit::LocateOptions locate;
      locate.strands = sufkit::StrandMode::kBoth;
      CHECK(SameQueryResult(full.Locate(pattern, locate),
                            sampled.Locate(pattern, locate)));
    }
    for (int trial = 0; trial < 30; ++trial) {
      std::string query;
      for (int position = 0; position < 96; ++position) {
        query.push_back(position % 37 == 0 ? 'N' : base());
      }
      sufkit::RightMaximalOptions options;
      options.min_length = std::max<std::uint64_t>(rate, 8);
      options.strands = sufkit::StrandMode::kBoth;
      options.algorithm = sufkit::RightMaximalSearchAlgorithm::kFull;
      CHECK(SameRightMaximalResult(
          full.FindRightMaximalMatches(query, options),
          sampled.FindRightMaximalMatches(query, options)));
    }
  }
}

}  // namespace

int main() {
  const auto directory =
      std::filesystem::path("/tmp") /
      ("sufkit-sampled-sa-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  RunBackend(sufkit::SaBackend::kDivsufsort, directory);
  const auto backends = sufkit::AvailableSaBackends();
  const auto caps =
      std::find_if(backends.begin(), backends.end(),
                   [](const auto& value) { return value.name == "caps"; });
  if (caps != backends.end() && caps->available) {
    RunBackend(sufkit::SaBackend::kCaps, directory);
  }
  CheckDirectLcpConstruction(directory);
  RandomizedDifferential();
  std::filesystem::remove_all(directory);
  if (failures != 0) {
    std::cerr << failures << " sampled SA assertion(s) failed\n";
    return 1;
  }
  std::cout << "sampled SA tests passed\n";
  return 0;
}
