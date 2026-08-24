// SPDX-License-Identifier: MIT

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "caps_backend.hpp"
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
void CheckError(sufkit::ErrorCode expected, Function&& function) {
  try {
    function();
    std::cerr << "expected sufkit::Error was not thrown\n";
    ++failures;
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == expected);
  }
}

sufkit::SuffixArrayBuildOptions Options(
    sufkit::SaBackend backend, sufkit::CoordinateWidth width,
    std::uint32_t threads,
    sufkit::SaAcceleration acceleration = sufkit::SaAcceleration::kNone) {
  sufkit::SuffixArrayBuildOptions result;
  result.backend = backend;
  result.coordinate_width = width;
  result.threads = threads;
  result.acceleration = acceleration;
  return result;
}

sufkit::GenomeReference MakeReference() {
  std::string first;
  std::string second;
  first.reserve(12288);
  second.reserve(8192);
  std::uint64_t state = 0x9e3779b97f4a7c15ULL;
  constexpr char kBases[] = {'A', 'C', 'G', 'T'};
  for (std::size_t index = 0; index < 12288; ++index) {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    first.push_back(index % 521 == 0
                        ? 'N'
                        : kBases[(state * 2685821657736338717ULL) & 3ULL]);
  }
  const std::string repeat = "ACGTACGTGATTACATTTTCCCCAAAAGGGG";
  while (second.size() < 8192) {
    second += repeat;
  }
  second.resize(8192);
  return sufkit::GenomeReference::FromRecords(
      {{"random", "deterministic", std::move(first)},
       {"repeat", "repeat-rich", std::move(second)}});
}

void CompareSuffixArrays(const sufkit::SuffixArray& left,
                         const sufkit::SuffixArray& right) {
  CHECK(left.GetInfo().text_symbols == right.GetInfo().text_symbols);
  const auto count =
      std::min(left.GetInfo().text_symbols, right.GetInfo().text_symbols);
  for (std::uint64_t row = 0; row < count; ++row) {
    if (left.SuffixAt(row) != right.SuffixAt(row)) {
      std::cerr << "suffix-array mismatch at row " << row << '\n';
      ++failures;
      break;
    }
  }
}

std::uint64_t SuffixChecksum(const sufkit::SuffixArray& index) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (std::uint64_t row = 0; row < index.GetInfo().text_symbols; ++row) {
    hash ^= index.SuffixAt(row);
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool SameRightMaximalResult(const sufkit::RightMaximalResult& left,
                            const sufkit::RightMaximalResult& right) {
  if (left.total_matches != right.total_matches ||
      left.truncated != right.truncated ||
      left.matches.size() != right.matches.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.matches.size(); ++index) {
    const auto& a = left.matches[index];
    const auto& b = right.matches[index];
    if (a.sequence_id != b.sequence_id ||
        a.reference_position != b.reference_position ||
        a.query_position != b.query_position || a.length != b.length ||
        a.strand != b.strand) {
      return false;
    }
  }
  return true;
}

void TestSelectionPolicy() {
  using sufkit::SaBackend;
  using sufkit::detail::ResolveSaBackend;
  constexpr auto kThreshold = sufkit::detail::kCapsAutoThresholdSymbols;
  CHECK(ResolveSaBackend(SaBackend::kAutoSelect, kThreshold - 1, 2, true) ==
        SaBackend::kDivsufsort);
  CHECK(ResolveSaBackend(SaBackend::kAutoSelect, kThreshold, 1, true) ==
        SaBackend::kDivsufsort);
  CHECK(ResolveSaBackend(SaBackend::kAutoSelect, kThreshold, 2, true) ==
        SaBackend::kCaps);
  CHECK(ResolveSaBackend(SaBackend::kAutoSelect, kThreshold, 2, false) ==
        SaBackend::kDivsufsort);
  CHECK(ResolveSaBackend(SaBackend::kCaps, 32, 1, false) == SaBackend::kCaps);
  CHECK(sufkit::detail::ResolveSaCoordinateWidth(
            SaBackend::kDivsufsort, sufkit::CoordinateWidth::kAutoSelect,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int32_t>::max())) ==
        sufkit::CoordinateWidth::kBits32);
  CHECK(
      sufkit::detail::ResolveSaCoordinateWidth(
          SaBackend::kDivsufsort, sufkit::CoordinateWidth::kAutoSelect,
          static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) +
              1) == sufkit::CoordinateWidth::kBits64);
  CHECK(sufkit::detail::ResolveSaCoordinateWidth(
            SaBackend::kCaps, sufkit::CoordinateWidth::kAutoSelect,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max())) ==
        sufkit::CoordinateWidth::kBits32);
  CHECK(sufkit::detail::ResolveSaCoordinateWidth(
            SaBackend::kCaps, sufkit::CoordinateWidth::kAutoSelect,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max()) +
                1) == sufkit::CoordinateWidth::kBits64);
  CHECK(sufkit::detail::CapsSubproblemCount(16, 1) == 1);
  CHECK(sufkit::detail::CapsSubproblemCount(1ULL << 30, 64) == 8192);
  CHECK(sufkit::detail::CapsSubproblemCount(1ULL << 30, 2) == 256);
}

void TestAvailability() {
  const auto backends = sufkit::AvailableSaBackends();
  const auto caps =
      std::find_if(backends.begin(), backends.end(),
                   [](const auto& backend) { return backend.name == "caps"; });
  CHECK(caps != backends.end());
  if (caps != backends.end()) {
    CHECK(caps->supports_threads);
    CHECK(caps->available == static_cast<bool>(SUFKIT_TEST_CAPS_ENABLED));
  }
}

#if SUFKIT_TEST_CAPS_ENABLED
void TestCapsBuilds(const std::filesystem::path& directory) {
  const auto reference = MakeReference();
  auto div32 = sufkit::SuffixArray::Build(
      reference, Options(sufkit::SaBackend::kDivsufsort,
                         sufkit::CoordinateWidth::kBits32, 1));
  auto caps32_1 = sufkit::SuffixArray::Build(
      reference,
      Options(sufkit::SaBackend::kCaps, sufkit::CoordinateWidth::kBits32, 1));
  auto caps32_2 = sufkit::SuffixArray::Build(
      reference,
      Options(sufkit::SaBackend::kCaps, sufkit::CoordinateWidth::kBits32, 2));
  auto caps64 = sufkit::SuffixArray::Build(
      reference,
      Options(sufkit::SaBackend::kCaps, sufkit::CoordinateWidth::kBits64, 4));

  CHECK(caps32_1.GetInfo().backend == "caps32");
  CHECK(caps32_1.GetInfo().coordinate_width == 32);
  CHECK(caps64.GetInfo().backend == "caps64");
  CHECK(caps64.GetInfo().coordinate_width == 64);
  CompareSuffixArrays(div32, caps32_1);
  CompareSuffixArrays(div32, caps32_2);
  CompareSuffixArrays(div32, caps64);
  CHECK(caps32_1.Count("ACGTACGT") == div32.Count("ACGTACGT"));
  CHECK(caps64.Locate("GATTACA").total_hits ==
        div32.Locate("GATTACA").total_hits);

  auto automatic = sufkit::SuffixArray::Build(
      reference, Options(sufkit::SaBackend::kAutoSelect,
                         sufkit::CoordinateWidth::kAutoSelect, 8));
  CHECK(automatic.GetInfo().backend == "divsufsort32");

  const auto path = directory / "caps32.sufidx";
  caps32_2.Save(path);
  const auto inspected = sufkit::InspectIndex(path);
  CHECK(inspected.backend == "caps32");
  CHECK(inspected.backend_signature.find("2597b373") != std::string::npos);
  auto loaded = sufkit::SuffixArray::Load(path);
  CHECK(loaded.GetInfo().backend == "caps32");
  CompareSuffixArrays(caps32_2, loaded);
  CHECK(loaded.Locate("ACGTACGT").total_hits ==
        caps32_2.Locate("ACGTACGT").total_hits);

  sufkit::SuffixArrayBuildStatistics build_statistics;
  auto learned_options =
      Options(sufkit::SaBackend::kCaps, sufkit::CoordinateWidth::kBits32, 2,
              sufkit::SaAcceleration::kFull);
  learned_options.learned_index.enabled = true;
  learned_options.learned_index.k = 4;
  learned_options.learned_index.bucket_bits = 4;
  learned_options.statistics = &build_statistics;
  auto learned_caps = sufkit::SuffixArray::Build(reference, learned_options);
  CHECK(learned_caps.GetInfo().backend == "caps32");
  CHECK(learned_caps.LookupAcceleration() ==
        sufkit::SaLookupAcceleration::kSaplingPwl);
  CHECK(build_statistics.sa_seconds >= 0.0);
  for (const std::string pattern :
       {"ACGT", "GATTACA", "CCCCAAAA", "TGCATGCA"}) {
    const auto binary =
        learned_caps.EqualRange(pattern, sufkit::SaSearchAlgorithm::kBinary);
    const auto learned = learned_caps.EqualRange(
        pattern, sufkit::SaSearchAlgorithm::kSaplingPwl);
    CHECK(binary.begin == learned.begin && binary.end == learned.end);
  }
  const auto learned_path = directory / "caps32-learned.sufidx";
  learned_caps.Save(learned_path);
  auto loaded_learned = sufkit::SuffixArray::Load(learned_path);
  CHECK(loaded_learned.GetInfo().backend == "caps32");
  CHECK(loaded_learned.LookupAcceleration() ==
        sufkit::SaLookupAcceleration::kSaplingPwl);
  const auto loaded_binary =
      loaded_learned.EqualRange("ACGT", sufkit::SaSearchAlgorithm::kBinary);
  const auto loaded_sapling =
      loaded_learned.EqualRange("ACGT", sufkit::SaSearchAlgorithm::kSaplingPwl);
  CHECK(loaded_binary.begin == loaded_sapling.begin &&
        loaded_binary.end == loaded_sapling.end);

  const std::vector<sufkit::SaAcceleration> accelerations{
      sufkit::SaAcceleration::kLcp, sufkit::SaAcceleration::kLcpChild,
      sufkit::SaAcceleration::kLcpSuffixLink, sufkit::SaAcceleration::kFull};
  for (const auto acceleration : accelerations) {
    auto div = sufkit::SuffixArray::Build(
        reference, Options(sufkit::SaBackend::kDivsufsort,
                           sufkit::CoordinateWidth::kBits32, 2, acceleration));
    auto caps = sufkit::SuffixArray::Build(
        reference, Options(sufkit::SaBackend::kCaps,
                           sufkit::CoordinateWidth::kBits32, 2, acceleration));
    CompareSuffixArrays(div, caps);
    CHECK(div.Count("ACGTACGT") == caps.Count("ACGTACGT"));
    sufkit::RightMaximalOptions right_maximal;
    right_maximal.min_length = 8;
    right_maximal.strands = sufkit::StrandMode::kBoth;
    CHECK(SameRightMaximalResult(
        div.FindRightMaximalMatches("TTACACGTACGTGATTACATTTT"),
        caps.FindRightMaximalMatches("TTACACGTACGTGATTACATTTT")));
  }

  const auto tiny =
      sufkit::GenomeReference::FromRecords({{"tiny", "", "ACGT"}});
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    (void)sufkit::SuffixArray::Build(
        tiny,
        Options(sufkit::SaBackend::kCaps, sufkit::CoordinateWidth::kBits32, 2));
  });
}

void TestConcurrentBuilds() {
  const auto reference = MakeReference();
  std::uint64_t first = 0;
  std::uint64_t second = 0;
  std::exception_ptr first_error;
  std::exception_ptr second_error;
  std::thread left([&] {
    try {
      auto index = sufkit::SuffixArray::Build(
          reference, Options(sufkit::SaBackend::kCaps,
                             sufkit::CoordinateWidth::kBits32, 2));
      first = SuffixChecksum(index);
    } catch (...) {
      first_error = std::current_exception();
    }
  });
  std::thread right([&] {
    try {
      auto index = sufkit::SuffixArray::Build(
          reference, Options(sufkit::SaBackend::kCaps,
                             sufkit::CoordinateWidth::kBits64, 3));
      second = SuffixChecksum(index);
    } catch (...) {
      second_error = std::current_exception();
    }
  });
  left.join();
  right.join();
  CHECK(first_error == nullptr);
  CHECK(second_error == nullptr);
  CHECK(first != 0);
  CHECK(first == second);
}
#else
void TestDisabledBuild() {
  const auto reference = MakeReference();
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)sufkit::SuffixArray::Build(
        reference,
        Options(sufkit::SaBackend::kCaps, sufkit::CoordinateWidth::kBits32, 2));
  });
}
#endif

}  // namespace

int main() {
  TestSelectionPolicy();
  TestAvailability();
  const auto directory =
      std::filesystem::path("/tmp") /
      ("sufkit-caps-tests-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
#if SUFKIT_TEST_CAPS_ENABLED
  TestCapsBuilds(directory);
  TestConcurrentBuilds();
#else
  TestDisabledBuild();
#endif
  std::filesystem::remove_all(directory);
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "CaPS backend tests passed\n";
  return 0;
}
