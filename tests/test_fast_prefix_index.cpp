// SPDX-License-Identifier: MIT

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "coordinate_storage.hpp"
#include "fast_prefix_index.hpp"
#include "reference_data.hpp"

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

std::uint8_t EncodeBase(char base) {
  switch (base) {
    case 'A':
      return sufkit::detail::kA;
    case 'C':
      return sufkit::detail::kC;
    case 'G':
      return sufkit::detail::kG;
    case 'T':
      return sufkit::detail::kT;
    default:
      return sufkit::detail::kN;
  }
}

struct TestReference {
  std::vector<std::uint8_t> text;
  std::vector<sufkit::Position> starts;
  std::vector<sufkit::Position> lengths;
  std::vector<std::uint64_t> sa;
  std::vector<std::uint64_t> isa;
};

TestReference MakeReference(const std::vector<std::string>& contigs) {
  TestReference result;
  for (const auto& contig : contigs) {
    result.starts.push_back(result.text.size());
    result.lengths.push_back(contig.size());
    for (const auto base : contig) {
      result.text.push_back(EncodeBase(base));
    }
    result.text.push_back(sufkit::detail::kSeparator);
  }
  result.text.push_back(sufkit::detail::kSentinel);

  result.sa.resize(result.text.size());
  for (std::size_t index = 0; index < result.sa.size(); ++index) {
    result.sa[index] = index;
  }
  std::sort(result.sa.begin(), result.sa.end(), [&](std::uint64_t left,
                                                    std::uint64_t right) {
    while (left < result.text.size() && right < result.text.size() &&
           result.text[static_cast<std::size_t>(left)] ==
               result.text[static_cast<std::size_t>(right)]) {
      ++left;
      ++right;
    }
    if (left == result.text.size() || right == result.text.size()) {
      return left == result.text.size() && right != result.text.size();
    }
    return result.text[static_cast<std::size_t>(left)] <
           result.text[static_cast<std::size_t>(right)];
  });
  result.isa.resize(result.sa.size());
  for (std::size_t row = 0; row < result.sa.size(); ++row) {
    result.isa[static_cast<std::size_t>(result.sa[row])] = row;
  }
  return result;
}

sufkit::detail::CoordinateStorage MakeCoordinates(
    const std::vector<std::uint64_t>& values, bool source64,
    std::uint64_t domain = 0) {
  if (domain == 0) {
    domain = values.size();
  }
  if (source64) {
    return sufkit::detail::CoordinateStorage::FromUInt64(
        std::vector<std::uint64_t>(values),
        sufkit::CoordinateStorageWidth::kBits64, domain,
        sufkit::ErrorCode::kBuildFailure, "test coordinate");
  }
  std::vector<std::uint32_t> narrow(values.begin(), values.end());
  return sufkit::detail::CoordinateStorage::FromUInt32(
      std::move(narrow), sufkit::CoordinateStorageWidth::kBits32,
      domain, sufkit::ErrorCode::kBuildFailure, "test coordinate");
}

sufkit::detail::FastPrefixIndexOptions FixedK(std::uint32_t k) {
  sufkit::detail::FastPrefixIndexOptions options;
  options.min_k = k;
  options.max_k = k;
  options.memory_budget_basis_points = 10000;
  options.min_suffix_count = 0;
  return options;
}

sufkit::detail::FastPrefixIndex BuildIndex(const TestReference& reference,
                                            std::uint32_t k,
                                            bool source64 = false) {
  auto sa = MakeCoordinates(reference.sa, source64);
  auto isa = MakeCoordinates(reference.isa, source64);
  return sufkit::detail::FastPrefixIndex::Build(
      reference.text, sa, isa, reference.starts, reference.lengths,
      std::numeric_limits<std::uint64_t>::max(), FixedK(k));
}

std::vector<std::uint8_t> DecodeKey(std::uint64_t key, std::uint32_t k) {
  std::vector<std::uint8_t> pattern(k);
  for (std::uint32_t offset = 0; offset < k; ++offset) {
    const auto index = k - offset - 1U;
    pattern[index] = static_cast<std::uint8_t>(
        sufkit::detail::kA + (key & 3U));
    key >>= 2U;
  }
  return pattern;
}

sufkit::detail::FastPrefixInterval ExpectedInterval(
    const TestReference& reference,
    const std::vector<std::uint8_t>& pattern) {
  bool found = false;
  sufkit::detail::FastPrefixInterval result;
  for (std::size_t row = 0; row < reference.sa.size(); ++row) {
    const auto suffix = reference.sa[row];
    bool equal = pattern.size() <= reference.text.size() - suffix;
    for (std::size_t index = 0; equal && index < pattern.size(); ++index) {
      equal = reference.text[static_cast<std::size_t>(suffix) + index] ==
              pattern[index];
    }
    if (!equal) {
      continue;
    }
    if (!found) {
      result.begin = row;
      found = true;
    }
    result.end = row + 1U;
  }
  return result;
}

void CheckEveryKey(const TestReference& reference, std::uint32_t k,
                   bool source64) {
  const auto index = BuildIndex(reference, k, source64);
  CHECK(!index.Empty());
  CHECK(index.K() == k);
  CHECK(index.RowWidth() == 32);
  CHECK(index.DirectoryEntries() == (std::uint64_t{1} << (2U * k)));
  CHECK(index.ResidentBytes() == index.DirectoryEntries() * 8U);

  const auto key_count = std::uint64_t{1} << (2U * k);
  for (std::uint64_t key = 0; key < key_count; ++key) {
    const auto pattern = DecodeKey(key, k);
    const auto actual = index.Lookup(pattern);
    const auto expected = ExpectedInterval(reference, pattern);
    CHECK(actual.has_value());
    if (actual) {
      CHECK(actual->begin == expected.begin);
      CHECK(actual->end == expected.end);
    }
  }
}

void TestSelectionAndWidths() {
  using sufkit::detail::FastPrefixIndex;
  using sufkit::detail::FastPrefixIndexOptions;

  CHECK(FastPrefixIndex::RowWidthForSuffixCount(
            std::numeric_limits<std::uint32_t>::max()) == 32);
  CHECK(FastPrefixIndex::RowWidthForSuffixCount(
            std::uint64_t{1} +
            std::numeric_limits<std::uint32_t>::max()) == 64);
  CHECK(FastPrefixIndex::ResidentBytesForK(10, 1U << 20U) ==
        (8U << 20U));
  CHECK(FastPrefixIndex::ResidentBytesForK(
            10, std::uint64_t{1} +
                    std::numeric_limits<std::uint32_t>::max()) ==
        (16U << 20U));
  CHECK(FastPrefixIndex::ResidentBytesForK(0, 1) == 0);

  FastPrefixIndexOptions options;
  options.min_suffix_count = 0;
  CHECK(FastPrefixIndex::SelectK(1U << 20U, 32U << 20U, options) == 10);
  CHECK(FastPrefixIndex::SelectK(1U << 20U, 8U << 20U, options) == 9);
  CHECK(FastPrefixIndex::SelectK(1U << 20U, 1U << 20U, options) == 0);

  options.min_suffix_count = 1U << 16U;
  CHECK(FastPrefixIndex::SelectK((1U << 16U) - 1U, 1U << 30U,
                                 options) == 0);
  options.memory_budget_basis_points = 0;
  CHECK(FastPrefixIndex::SelectK(1U << 20U, 1U << 30U, options) == 0);

  options = {};
  options.min_k = 0;
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)FastPrefixIndex::SelectK(1, 1, options);
  });
  options.min_k = 8;
  options.max_k = 11;
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)FastPrefixIndex::SelectK(1, 1, options);
  });
  options.max_k = 10;
  options.memory_budget_basis_points = 10001;
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)FastPrefixIndex::SelectK(1, 1, options);
  });
}

void TestRepeatedAndEmptyKeys() {
  const auto reference = MakeReference({"AAAAAA"});
  const auto index = BuildIndex(reference, 2);

  const std::vector<std::uint8_t> aa{sufkit::detail::kA,
                                     sufkit::detail::kA};
  const auto aa_interval = index.Lookup(aa);
  CHECK(aa_interval.has_value());
  CHECK(aa_interval && aa_interval->end - aa_interval->begin == 5);

  const std::vector<std::uint8_t> cc{sufkit::detail::kC,
                                     sufkit::detail::kC};
  const auto cc_interval = index.Lookup(cc);
  CHECK(cc_interval.has_value());
  CHECK(cc_interval && cc_interval->Empty());

  const std::vector<std::uint8_t> short_pattern{sufkit::detail::kA};
  CHECK(!index.Lookup(short_pattern).has_value());
  const std::vector<std::uint8_t> invalid{sufkit::detail::kA,
                                          sufkit::detail::kN};
  CHECK(!index.Lookup(invalid).has_value());
}

void TestHardBoundaries() {
  const auto reference = MakeReference({"AC", "GT", "AANAA"});
  CheckEveryKey(reference, 2, false);
  CheckEveryKey(reference, 3, true);

  const auto index = BuildIndex(reference, 3);
  // These byte triples exist only if a directory incorrectly crosses a
  // separator or an N hard boundary.
  const std::vector<std::uint8_t> acg{sufkit::detail::kA,
                                      sufkit::detail::kC,
                                      sufkit::detail::kG};
  const std::vector<std::uint8_t> aaa{sufkit::detail::kA,
                                      sufkit::detail::kA,
                                      sufkit::detail::kA};
  CHECK(index.Lookup(acg)->Empty());
  CHECK(index.Lookup(aaa)->Empty());
  CHECK(index.IndexedKmers() == 0);
}

void TestRandomReferences() {
  std::mt19937_64 random(20260830);
  constexpr std::array<char, 5> alphabet{'A', 'C', 'G', 'T', 'N'};
  for (std::uint32_t trial = 0; trial < 32; ++trial) {
    std::vector<std::string> contigs;
    const auto count = 1U + static_cast<std::uint32_t>(random() % 4U);
    for (std::uint32_t sequence = 0; sequence < count; ++sequence) {
      std::string value(4U + static_cast<std::size_t>(random() % 20U), 'A');
      for (auto& base : value) {
        base = alphabet[static_cast<std::size_t>(random() % alphabet.size())];
      }
      contigs.push_back(std::move(value));
    }
    const auto reference = MakeReference(contigs);
    for (std::uint32_t k = 1; k <= 3; ++k) {
      CheckEveryKey(reference, k, false);
      CheckEveryKey(reference, k, true);
    }
  }
}

void TestValidation() {
  const auto reference = MakeReference({"ACGT"});
  auto sa = MakeCoordinates(reference.sa, false);
  auto isa = MakeCoordinates(reference.isa, false);

  auto bad_starts = reference.starts;
  bad_starts[0] = 1;
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)sufkit::detail::FastPrefixIndex::Build(
        reference.text, sa, isa, bad_starts, reference.lengths,
        std::numeric_limits<std::uint64_t>::max(), FixedK(2));
  });

  auto wrong_isa_values = reference.isa;
  std::swap(wrong_isa_values[0], wrong_isa_values[1]);
  auto wrong_isa = MakeCoordinates(wrong_isa_values, false);
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)sufkit::detail::FastPrefixIndex::Build(
        reference.text, sa, wrong_isa, reference.starts, reference.lengths,
        std::numeric_limits<std::uint64_t>::max(), FixedK(2));
  });

  auto short_isa_values = reference.isa;
  short_isa_values.pop_back();
  auto short_isa =
      MakeCoordinates(short_isa_values, false, reference.isa.size());
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)sufkit::detail::FastPrefixIndex::Build(
        reference.text, sa, short_isa, reference.starts, reference.lengths,
        std::numeric_limits<std::uint64_t>::max(), FixedK(2));
  });
}

void TestParallelDirectory() {
  std::mt19937_64 random(918273);
  std::string sequence((2U << 20U) + 137U, 'A');
  constexpr char bases[] = {'A','C','G','T'};
  for (auto& base : sequence) base = bases[random() & 3U];
  sequence.replace(sequence.size() / 2 - 3, 17, "NNNNNNNNNNNNNNNNN");
  const auto reference = MakeReference({sequence, "ACGTNNACGTACGT", "AC"});
  for (const bool wide : {false, true}) {
    const auto sa = MakeCoordinates(reference.sa, wide);
    const auto isa = MakeCoordinates(reference.isa, wide);
    auto options = FixedK(4);
    const auto serial = sufkit::detail::FastPrefixIndex::Build(
        reference.text, sa, isa, reference.starts, reference.lengths,
        std::numeric_limits<std::uint64_t>::max(), options);
    options.threads = 4;
    const auto parallel = sufkit::detail::FastPrefixIndex::Build(
        reference.text, sa, isa, reference.starts, reference.lengths,
        std::numeric_limits<std::uint64_t>::max(), options);
    CHECK(serial.IndexedKmers() == parallel.IndexedKmers());
    CHECK(serial.NonemptyEntries() == parallel.NonemptyEntries());
    CHECK(serial.ResidentBytes() == parallel.ResidentBytes());
    for (std::uint64_t key = 0; key < 256; ++key) {
      const auto query = DecodeKey(key, 4);
      const auto a = serial.Lookup(query), b = parallel.Lookup(query);
      CHECK(a.has_value() && b.has_value());
      CHECK(a->begin == b->begin && a->end == b->end);
    }
  }
}

}  // namespace

int main() {
  TestParallelDirectory();
  TestSelectionAndWidths();
  TestRepeatedAndEmptyKeys();
  TestHardBoundaries();
  TestRandomReferences();
  TestValidation();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "fast prefix-index tests passed\n";
  return 0;
}
