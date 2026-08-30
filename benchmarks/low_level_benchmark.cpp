// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(SUFKIT_X86_64_SSE42_BASELINE)
#include <nmmintrin.h>
#endif

#include "sequence_compare.hpp"

namespace {

using Clock = std::chrono::steady_clock;

#if defined(__GNUC__) || defined(__clang__)
#define SUFKIT_NOINLINE __attribute__((noinline))
#else
#define SUFKIT_NOINLINE
#endif

struct CompareResult {
  int ordering = 0;
  std::size_t lcp = 0;
  std::size_t comparisons = 0;
};

struct Options {
  bool verify_only = false;
  std::string profile = "smoke";
  std::filesystem::path output;
  std::uint32_t repetitions = 7;
};

struct InputCase {
  std::size_t length = 0;
  std::optional<std::size_t> mismatch;
  std::uint8_t lhs_offset = 0;
  std::uint8_t rhs_offset = 0;
  std::vector<std::uint8_t> lhs_storage;
  std::vector<std::uint8_t> rhs_storage;

  const std::uint8_t* lhs() const {
    return lhs_storage.data() + lhs_offset;
  }

  const std::uint8_t* rhs() const {
    return rhs_storage.data() + rhs_offset;
  }
};

struct CaseGroup {
  std::size_t length = 0;
  std::optional<std::size_t> mismatch;
  std::vector<InputCase> cases;
};

// Independent scalar oracle for the production suffix-versus-pattern
// contract. Consuming the whole pattern is equality even when text remains.
SUFKIT_NOINLINE CompareResult ComparePatternScalar(
    const std::uint8_t* text, std::size_t text_size,
    const std::uint8_t* pattern, std::size_t pattern_size,
    std::size_t known_lcp = 0) {
  std::size_t index = std::min(known_lcp, pattern_size);
  while (index < pattern_size) {
    if (index >= text_size) {
      return {-1, index, index - known_lcp};
    }
    if (text[index] != pattern[index]) {
      return {text[index] < pattern[index] ? -1 : 1, index,
              index - known_lcp + 1};
    }
    ++index;
  }
  return {0, index, index - known_lcp};
}

// This wrapper is deliberately the only timed SIMD path. The production
// inline kernel is instantiated here instead of duplicated in the harness.
SUFKIT_NOINLINE CompareResult ComparePatternProduction(
    const std::uint8_t* text, std::size_t text_size,
    const std::uint8_t* pattern, std::size_t pattern_size,
    std::size_t known_lcp = 0) {
  const auto result = sufkit::detail::ComparePatternBytes(
      text, text_size, pattern, pattern_size, known_lcp);
  return {result.order, result.lcp, result.comparisons};
}

SUFKIT_NOINLINE std::size_t LongestCommonPrefixScalar(
    const std::uint8_t* left, const std::uint8_t* right,
    std::size_t length) {
  std::size_t index = 0;
  while (index < length && left[index] == right[index]) {
    ++index;
  }
  return index;
}

SUFKIT_NOINLINE std::size_t LongestCommonPrefixProduction(
    const std::uint8_t* left, const std::uint8_t* right,
    std::size_t length) {
  return sufkit::detail::LongestCommonPrefixBytesLong(left, right, length);
}

SUFKIT_NOINLINE std::uint64_t PopcountKernel(std::uint64_t value) {
#if defined(SUFKIT_X86_64_SSE42_BASELINE)
  return static_cast<std::uint64_t>(_mm_popcnt_u64(value));
#else
  std::uint64_t count = 0;
  while (value != 0) {
    value &= value - 1;
    ++count;
  }
  return count;
#endif
}

bool RuntimeSupportsSse42() {
#if defined(SUFKIT_X86_64_SSE42_BASELINE) && \
    (defined(__GNUC__) || defined(__clang__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2") != 0 &&
         __builtin_cpu_supports("popcnt") != 0;
#else
  return false;
#endif
}

std::uint64_t ParseUnsigned(const std::string& value, const char* name) {
  std::size_t consumed = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(value, &consumed);
  } catch (...) {
    throw std::runtime_error(std::string("invalid ") + name);
  }
  if (consumed != value.size()) {
    throw std::runtime_error(std::string("invalid ") + name);
  }
  return static_cast<std::uint64_t>(parsed);
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help") {
      std::cout
          << "sufkit_low_level_bench [--profile smoke|quick] "
             "--output FILE\n"
          << "  [--repetitions N] [--verify-only]\n";
      std::exit(0);
    }
    if (name == "--verify-only") {
      options.verify_only = true;
      continue;
    }
    if (index + 1 >= argc) {
      throw std::runtime_error("missing value for " + name);
    }
    const std::string value = argv[++index];
    if (name == "--profile") {
      options.profile = value;
    } else if (name == "--output") {
      options.output = value;
    } else if (name == "--repetitions") {
      const auto parsed = ParseUnsigned(value, "repetition count");
      if (parsed == 0 ||
          parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("repetition count is out of range");
      }
      options.repetitions = static_cast<std::uint32_t>(parsed);
    } else {
      throw std::runtime_error("unknown option: " + name);
    }
  }
  if (options.profile != "smoke" && options.profile != "quick") {
    throw std::runtime_error("profile must be smoke or quick");
  }
  if (!options.verify_only && options.output.empty()) {
    throw std::runtime_error("--output is required unless --verify-only is used");
  }
  return options;
}

std::vector<std::size_t> LengthsForProfile(const std::string& profile) {
  if (profile == "smoke") {
    return {0,  1,  7,  8,  9,  15, 16, 17, 31, 32, 33,
            47, 48, 49, 63, 64, 65, 79, 80, 81, 95, 96, 97,
            100, 127, 128, 129, 500};
  }
  std::vector<std::size_t> lengths;
  lengths.reserve(165);
  for (std::size_t length = 0; length <= 160; ++length) {
    lengths.push_back(length);
  }
  lengths.insert(lengths.end(), {100, 200, 500, 4096});
  return lengths;
}

std::vector<std::optional<std::size_t>> MismatchesForLength(
    std::size_t length) {
  std::vector<std::optional<std::size_t>> mismatches;
  const std::array<std::size_t, 14> candidates{
      0,  1,  3,  7,  8,  15, 16,
      31, 32, 47, 48, 63, 64, length - 1};
  for (const auto candidate : candidates) {
    if (candidate >= length) {
      continue;
    }
    if (std::find(mismatches.begin(), mismatches.end(), candidate) ==
        mismatches.end()) {
      mismatches.emplace_back(candidate);
    }
  }
  mismatches.emplace_back(std::nullopt);
  return mismatches;
}

InputCase MakeInputCase(std::size_t length,
                        std::optional<std::size_t> mismatch,
                        std::uint8_t lhs_offset,
                        std::uint8_t rhs_offset) {
  InputCase result;
  result.length = length;
  result.mismatch = mismatch;
  result.lhs_offset = lhs_offset;
  result.rhs_offset = rhs_offset;
  // Do not add tail padding: ASan must be able to catch a vector load past the
  // caller-provided logical extent.
  result.lhs_storage.assign(
      std::max<std::size_t>(1, length + lhs_offset), 0xeeU);
  result.rhs_storage.assign(
      std::max<std::size_t>(1, length + rhs_offset), 0xddU);
  for (std::size_t index = 0; index < length; ++index) {
    const auto base = static_cast<std::uint8_t>(2 +
        ((index * 5 + length * 3 + lhs_offset) & 3U));
    result.lhs_storage[lhs_offset + index] = base;
    result.rhs_storage[rhs_offset + index] = base;
  }
  if (mismatch.has_value()) {
    auto& rhs_base = result.rhs_storage[rhs_offset + *mismatch];
    rhs_base = rhs_base == 5 ? 2 : static_cast<std::uint8_t>(rhs_base + 1);
  }
  return result;
}

std::vector<CaseGroup> MakeCaseGroups(const std::string& profile) {
  std::vector<CaseGroup> groups;
  for (const auto length : LengthsForProfile(profile)) {
    for (const auto mismatch : MismatchesForLength(length)) {
      CaseGroup group;
      group.length = length;
      group.mismatch = mismatch;
      group.cases.reserve(16);
      for (std::uint8_t offset = 0; offset < 16; ++offset) {
        const auto rhs_offset = static_cast<std::uint8_t>(
            (static_cast<unsigned int>(offset) * 7U + 3U) & 15U);
        group.cases.push_back(
            MakeInputCase(length, mismatch, offset, rhs_offset));
      }
      groups.push_back(std::move(group));
    }
  }
  return groups;
}

void VerifyCase(const InputCase& input) {
  const auto verify = [&](const std::uint8_t* text, std::size_t text_size,
                          const std::uint8_t* pattern,
                          std::size_t pattern_size, const char* direction) {
    const auto expected =
        ComparePatternScalar(text, text_size, pattern, pattern_size);
    const auto actual =
        ComparePatternProduction(text, text_size, pattern, pattern_size);
    if (expected.ordering != actual.ordering || expected.lcp != actual.lcp ||
        expected.comparisons != actual.comparisons) {
      throw std::runtime_error(
          std::string(direction) + " production/oracle mismatch at length " +
          std::to_string(input.length));
    }

    // All candidates are known-valid prefixes. This checks the LCP-aware SA
    // path as well as the zero-prefix path used by ordinary binary search.
    const std::array<std::size_t, 7> candidates{
        0, 1, 7, 8, 15, 16, expected.lcp};
    std::vector<std::size_t> checked;
    checked.reserve(candidates.size());
    for (const auto known_lcp : candidates) {
      if (known_lcp > expected.lcp ||
          std::find(checked.begin(), checked.end(), known_lcp) !=
              checked.end()) {
        continue;
      }
      checked.push_back(known_lcp);
      const auto expected_known = ComparePatternScalar(
          text, text_size, pattern, pattern_size, known_lcp);
      const auto actual_known = ComparePatternProduction(
          text, text_size, pattern, pattern_size, known_lcp);
      if (expected_known.ordering != actual_known.ordering ||
          expected_known.lcp != actual_known.lcp ||
          expected_known.comparisons != actual_known.comparisons) {
        throw std::runtime_error(
            std::string(direction) +
            " known-LCP production/oracle mismatch at length " +
            std::to_string(input.length));
      }
    }

    const auto shared_size = std::min(text_size, pattern_size);
    const auto expected_lce =
        LongestCommonPrefixScalar(text, pattern, shared_size);
    const auto actual_lce = sufkit::detail::LongestCommonPrefixBytes(
        text, pattern, shared_size);
    if (expected_lce != actual_lce) {
      throw std::runtime_error(
          std::string(direction) + " production LCE mismatch at length " +
          std::to_string(input.length));
    }
    return expected;
  };

  const auto forward = verify(input.lhs(), input.length, input.rhs(),
                              input.length, "forward");
  const auto reverse = verify(input.rhs(), input.length, input.lhs(),
                              input.length, "reverse");
  if (reverse.ordering != -forward.ordering || reverse.lcp != forward.lcp) {
    throw std::runtime_error("reverse comparison mismatch at length " +
                             std::to_string(input.length));
  }

  if (input.length > 0) {
    static_cast<void>(verify(input.lhs(), input.length - 1, input.rhs(),
                             input.length, "short-text"));
    static_cast<void>(verify(input.lhs(), input.length, input.rhs(),
                             input.length - 1, "short-pattern"));
  }
}

void VerifyLceBoundaryMatrix() {
  if (sufkit::detail::LongestCommonPrefixBytes(nullptr, nullptr, 0) != 0) {
    throw std::runtime_error("zero-length LCE mismatch");
  }

  // Exhaust every mismatch position around and beyond both the 16-byte SIMD
  // boundary and the 32-byte unrolled boundary. Each allocation ends exactly
  // at the logical input boundary, so ASan also verifies that the kernel never
  // issues a full vector load for a short tail.
  for (std::size_t length = 0; length <= 160; ++length) {
    for (std::size_t left_offset = 0; left_offset < 16; ++left_offset) {
      const auto right_offset = (left_offset * 7 + 3) & 15U;
      const auto left_allocation =
          std::max<std::size_t>(1, left_offset + length);
      const auto right_allocation =
          std::max<std::size_t>(1, right_offset + length);
      auto left_storage =
          std::make_unique<std::uint8_t[]>(left_allocation);
      auto right_storage =
          std::make_unique<std::uint8_t[]>(right_allocation);
      auto* const left = left_storage.get() + left_offset;
      auto* const right = right_storage.get() + right_offset;
      for (std::size_t index = 0; index < length; ++index) {
        const auto value = static_cast<std::uint8_t>(
            2U + ((index * 13U + length * 5U + left_offset) & 3U));
        left[index] = value;
        right[index] = value;
      }

      const auto verify = [&](std::size_t expected) {
        const auto oracle = LongestCommonPrefixScalar(left, right, length);
        const auto actual = sufkit::detail::LongestCommonPrefixBytesLong(
            left, right, length);
        if (oracle != expected || actual != oracle) {
          throw std::runtime_error(
              "production/oracle LCE boundary mismatch at length " +
              std::to_string(length) + ", expected LCP " +
              std::to_string(expected));
        }
      };

      verify(length);
      for (std::size_t mismatch = 0; mismatch < length; ++mismatch) {
        right[mismatch] ^= 0x5aU;
        verify(mismatch);
        right[mismatch] ^= 0x5aU;
      }
    }
  }
}

void VerifyAll(const std::vector<CaseGroup>& groups) {
  VerifyLceBoundaryMatrix();
  for (const auto& group : groups) {
    for (const auto& input : group.cases) {
      VerifyCase(input);
    }
  }
}

void Mix(std::uint64_t& hash, std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  hash *= 1099511628211ULL;
}

template <CompareResult (*Kernel)(const std::uint8_t*, std::size_t,
                                  const std::uint8_t*, std::size_t,
                                  std::size_t)>
std::pair<std::uint64_t, std::uint64_t> RunGroup(
    const CaseGroup& group, std::size_t rounds) {
  std::uint64_t checksum = 0xcbf29ce484222325ULL;
  std::uint64_t examined_bytes = 0;
  for (std::size_t round = 0; round < rounds; ++round) {
    for (const auto& input : group.cases) {
      const auto result = Kernel(
          input.lhs(), input.length, input.rhs(), input.length, 0);
      Mix(checksum, static_cast<std::uint64_t>(result.ordering + 1));
      Mix(checksum, static_cast<std::uint64_t>(result.lcp));
      Mix(checksum, static_cast<std::uint64_t>(result.comparisons));
      examined_bytes += static_cast<std::uint64_t>(result.comparisons);
    }
  }
  return {checksum, examined_bytes};
}

template <std::size_t (*Kernel)(const std::uint8_t*, const std::uint8_t*,
                                std::size_t)>
std::pair<std::uint64_t, std::uint64_t> RunLceGroup(
    const CaseGroup& group, std::size_t rounds) {
  std::uint64_t checksum = 0xcbf29ce484222325ULL;
  std::uint64_t examined_bytes = 0;
  for (std::size_t round = 0; round < rounds; ++round) {
    for (const auto& input : group.cases) {
      const auto lcp = Kernel(input.lhs(), input.rhs(), input.length);
      Mix(checksum, static_cast<std::uint64_t>(lcp));
      examined_bytes += static_cast<std::uint64_t>(
          lcp + (lcp < input.length ? 1U : 0U));
    }
  }
  return {checksum, examined_bytes};
}

std::string MismatchLabel(const std::optional<std::size_t>& mismatch) {
  return mismatch.has_value() ? std::to_string(*mismatch) : "equal";
}

template <CompareResult (*Kernel)(const std::uint8_t*, std::size_t,
                                  const std::uint8_t*, std::size_t,
                                  std::size_t)>
void MeasureGroup(const char* kernel_name, const Options& options,
                  const CaseGroup& group, std::uint32_t repetition,
                  std::size_t rounds, std::ofstream& output) {
  const auto begin = Clock::now();
  const auto [checksum, examined_bytes] = RunGroup<Kernel>(group, rounds);
  const double seconds =
      std::chrono::duration<double>(Clock::now() - begin).count();
  const auto iterations = rounds * group.cases.size();
  const double nanoseconds_per_call =
      seconds * 1.0e9 / static_cast<double>(iterations);
  const double bytes_per_second =
      seconds == 0.0 ? 0.0
                     : static_cast<double>(examined_bytes) / seconds;
  output << options.profile << '\t' << kernel_name << '\t' << group.length
         << '\t' << MismatchLabel(group.mismatch) << '\t' << repetition
         << '\t' << iterations << '\t' << std::setprecision(17) << seconds
         << '\t' << nanoseconds_per_call << '\t' << bytes_per_second << '\t'
         << checksum << '\t'
#if defined(SUFKIT_X86_64_SSE42_BASELINE)
         << 1
#else
         << 0
#endif
         << '\t' << (RuntimeSupportsSse42() ? 1 : 0) << '\t'
         << PopcountKernel(checksum) << '\n';
}

template <std::size_t (*Kernel)(const std::uint8_t*, const std::uint8_t*,
                                std::size_t)>
void MeasureLceGroup(const char* kernel_name, const Options& options,
                     const CaseGroup& group, std::uint32_t repetition,
                     std::size_t rounds, std::ofstream& output) {
  const auto begin = Clock::now();
  const auto [checksum, examined_bytes] = RunLceGroup<Kernel>(group, rounds);
  const double seconds =
      std::chrono::duration<double>(Clock::now() - begin).count();
  const auto iterations = rounds * group.cases.size();
  const double nanoseconds_per_call =
      seconds * 1.0e9 / static_cast<double>(iterations);
  const double bytes_per_second =
      seconds == 0.0 ? 0.0
                     : static_cast<double>(examined_bytes) / seconds;
  output << options.profile << '\t' << kernel_name << '\t' << group.length
         << '\t' << MismatchLabel(group.mismatch) << '\t' << repetition
         << '\t' << iterations << '\t' << std::setprecision(17) << seconds
         << '\t' << nanoseconds_per_call << '\t' << bytes_per_second << '\t'
         << checksum << '\t'
#if defined(SUFKIT_X86_64_SSE42_BASELINE)
         << 1
#else
         << 0
#endif
         << '\t' << (RuntimeSupportsSse42() ? 1 : 0) << '\t'
         << PopcountKernel(checksum) << '\n';
}

void WriteMeasurements(const Options& options,
                       const std::vector<CaseGroup>& groups) {
  if (!options.output.parent_path().empty()) {
    std::filesystem::create_directories(options.output.parent_path());
  }
  std::ofstream output(options.output, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create output file");
  }
  output << "profile\tkernel\tlength\tmismatch_position\trepetition\t"
            "iterations\tseconds\tnanoseconds_per_call\t"
            "logical_bytes_per_second\tchecksum\tsse42_compiled\t"
            "sse42_runtime\tpopcount_checksum\n";

  // One warm-up pass is deliberately outside the measured repetitions.
  for (const auto& group : groups) {
    static_cast<void>(RunGroup<ComparePatternScalar>(group, 1));
    static_cast<void>(RunGroup<ComparePatternProduction>(group, 1));
    static_cast<void>(RunLceGroup<LongestCommonPrefixScalar>(group, 1));
    static_cast<void>(RunLceGroup<LongestCommonPrefixProduction>(group, 1));
  }
  const std::size_t rounds = options.profile == "smoke" ? 32 : 2048;
  for (std::uint32_t repetition = 0;
       repetition < options.repetitions; ++repetition) {
    for (const auto& group : groups) {
      if ((repetition & 1U) == 0) {
        MeasureGroup<ComparePatternScalar>("scalar", options, group,
                                           repetition, rounds, output);
        MeasureGroup<ComparePatternProduction>(
            "production_sse", options, group, repetition, rounds, output);
        MeasureLceGroup<LongestCommonPrefixScalar>(
            "lce_scalar", options, group, repetition, rounds, output);
        MeasureLceGroup<LongestCommonPrefixProduction>(
            "lce_sse", options, group, repetition, rounds, output);
      } else {
        MeasureLceGroup<LongestCommonPrefixProduction>(
            "lce_sse", options, group, repetition, rounds, output);
        MeasureLceGroup<LongestCommonPrefixScalar>(
            "lce_scalar", options, group, repetition, rounds, output);
        MeasureGroup<ComparePatternProduction>(
            "production_sse", options, group, repetition, rounds, output);
        MeasureGroup<ComparePatternScalar>("scalar", options, group,
                                           repetition, rounds, output);
      }
    }
  }
  if (!output) {
    throw std::runtime_error("failed to write output file");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    if (!RuntimeSupportsSse42()) {
      throw std::runtime_error(
          "the low-level benchmark requires SSE4.2 and POPCNT at runtime");
    }
    const auto groups = MakeCaseGroups(options.profile);
    VerifyAll(groups);
    if (options.verify_only) {
      std::cout << "verified " << groups.size()
                << " scalar/production-SSE case groups\n";
      return 0;
    }
    WriteMeasurements(options, groups);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "sufkit_low_level_bench: " << error.what() << '\n';
    return 2;
  }
}
