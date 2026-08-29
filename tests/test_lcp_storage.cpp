// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "lcp_storage.hpp"

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

struct SampledAllA {
  std::uint64_t text_symbols = 0;
  std::uint32_t sampling_rate = 1;
  std::vector<std::uint64_t> sa;
  std::vector<std::uint64_t> isa;
  std::vector<std::uint64_t> lcp;
};

struct SampledTextOracle {
  std::vector<std::uint8_t> text;
  std::uint32_t sampling_rate = 1;
  std::vector<std::uint64_t> sa;
  std::vector<std::uint64_t> isa;
  std::vector<std::uint64_t> lcp;
};

SampledTextOracle MakeSampledTextOracle(std::vector<std::uint8_t> text,
                                        std::uint32_t sampling_rate) {
  CHECK(sampling_rate != 0);
  SampledTextOracle result;
  result.text = std::move(text);
  result.sampling_rate = sampling_rate;
  const auto count = result.text.empty()
                         ? 0
                         : 1 + (result.text.size() - 1) / sampling_rate;
  result.sa.resize(count);
  for (std::size_t sample = 0; sample < count; ++sample) {
    result.sa[sample] = sample * sampling_rate;
  }
  std::sort(result.sa.begin(), result.sa.end(), [&](std::uint64_t left,
                                                    std::uint64_t right) {
    auto left_index = static_cast<std::size_t>(left);
    auto right_index = static_cast<std::size_t>(right);
    while (left_index < result.text.size() &&
           right_index < result.text.size() &&
           result.text[left_index] == result.text[right_index]) {
      ++left_index;
      ++right_index;
    }
    if (left_index == result.text.size() ||
        right_index == result.text.size()) {
      return left_index == result.text.size() &&
             right_index != result.text.size();
    }
    return result.text[left_index] < result.text[right_index];
  });

  result.isa.resize(count);
  result.lcp.resize(count);
  for (std::size_t row = 0; row < count; ++row) {
    result.isa[static_cast<std::size_t>(
        result.sa[row] / sampling_rate)] = row;
    if (row == 0) {
      continue;
    }
    auto left = static_cast<std::size_t>(result.sa[row - 1]);
    auto right = static_cast<std::size_t>(result.sa[row]);
    std::uint64_t common = 0;
    while (left < result.text.size() && right < result.text.size() &&
           result.text[left] == result.text[right]) {
      ++left;
      ++right;
      ++common;
    }
    result.lcp[row] = common;
  }
  return result;
}

void CheckDirectAgainstRaw(const SampledTextOracle& input,
                           std::uint8_t coordinate_width) {
  using sufkit::ErrorCode;
  using sufkit::detail::IntegerArrayView;
  using sufkit::detail::LcpEncoding;
  using sufkit::detail::LcpStorage;

  auto direct = LcpStorage::BuildByteCodedDirect(
      input.text.data(), input.text.size(), IntegerArrayView(input.sa),
      IntegerArrayView(input.isa), input.sampling_rate, coordinate_width,
      ErrorCode::kBuildFailure);
  auto encoded_raw = LcpStorage::BuildByteCoded(
      IntegerArrayView(input.lcp), IntegerArrayView(input.isa),
      input.sampling_rate, coordinate_width, input.text.size(),
      ErrorCode::kBuildFailure);
  auto raw = LcpStorage::FromRaw64(input.lcp, input.sampling_rate,
                                   ErrorCode::kBuildFailure);

  CHECK(direct.Encoding() ==
        (coordinate_width == 32 ? LcpEncoding::kByteCoded32
                                : LcpEncoding::kByteCoded64));
  CHECK(direct.BytePrimary() != nullptr);
  CHECK(encoded_raw.BytePrimary() != nullptr);
  CHECK(*direct.BytePrimary() == *encoded_raw.BytePrimary());
  CHECK(direct.AnchorCount() == encoded_raw.AnchorCount());
  CHECK(direct.SerializedDataBytes() == encoded_raw.SerializedDataBytes());
  CHECK(direct.GuideBytes() == encoded_raw.GuideBytes());
  if (coordinate_width == 32) {
    CHECK(*direct.AnchorPositions32() ==
          *encoded_raw.AnchorPositions32());
    CHECK(*direct.AnchorValues32() == *encoded_raw.AnchorValues32());
  } else {
    CHECK(*direct.AnchorPositions64() ==
          *encoded_raw.AnchorPositions64());
    CHECK(*direct.AnchorValues64() == *encoded_raw.AnchorValues64());
  }

  direct.Validate(IntegerArrayView(input.sa), input.text.size(),
                  ErrorCode::kCorruptIndex);
  raw.Validate(IntegerArrayView(input.sa), input.text.size(),
               ErrorCode::kCorruptIndex);
  for (std::size_t row = 0; row < input.sa.size(); ++row) {
    const auto suffix = input.sa[row];
    const auto expected = raw.Exact(row, suffix);
    CHECK(direct.Exact(row, suffix) == expected);
    const std::uint64_t targets[] = {
        0, 1, 254, 255, 256, expected, expected + 1};
    for (const auto target : targets) {
      CHECK(direct.AtLeast(row, suffix, target) ==
            raw.AtLeast(row, suffix, target));
    }
  }
}

SampledAllA MakeSampledAllA(std::uint64_t text_symbols,
                            std::uint32_t sampling_rate) {
  CHECK(text_symbols != 0);
  CHECK(sampling_rate != 0);
  CHECK((text_symbols - 1) % sampling_rate == 0);
  SampledAllA result;
  result.text_symbols = text_symbols;
  result.sampling_rate = sampling_rate;
  const auto count = 1 + (text_symbols - 1) / sampling_rate;
  result.sa.resize(static_cast<std::size_t>(count));
  result.isa.resize(static_cast<std::size_t>(count));
  result.lcp.resize(static_cast<std::size_t>(count));
  for (std::uint64_t row = 0; row < count; ++row) {
    const auto sample = count - 1 - row;
    result.sa[static_cast<std::size_t>(row)] = sample * sampling_rate;
    result.isa[static_cast<std::size_t>(sample)] = row;
    result.lcp[static_cast<std::size_t>(row)] =
        row <= 1 ? 0 : (row - 1) * sampling_rate;
  }
  return result;
}

void CheckEveryRow(const sufkit::detail::LcpStorage& storage,
                   const SampledAllA& input) {
  CHECK(storage.Size() == input.lcp.size());
  for (std::size_t row = 0; row < input.lcp.size(); ++row) {
    const auto expected = input.lcp[row];
    const auto suffix = input.sa[row];
    CHECK(storage.Exact(row, suffix) == expected);
    CHECK(storage.AtLeast(row, suffix, 0));
    CHECK(storage.AtLeast(row, suffix, expected));
    if (expected != std::numeric_limits<std::uint64_t>::max()) {
      CHECK(!storage.AtLeast(row, suffix, expected + 1));
    }
    CHECK(storage.AtLeast(row, suffix, 255) == (expected >= 255));
    CHECK(storage.AtLeast(row, suffix, 256) == (expected >= 256));
  }
}

void TestRawStorage() {
  const auto input = MakeSampledAllA(601, 1);
  std::vector<std::uint32_t> raw32(input.lcp.begin(), input.lcp.end());
  auto storage32 = sufkit::detail::LcpStorage::FromRaw32(
      std::move(raw32), 1, sufkit::ErrorCode::kBuildFailure);
  CHECK(storage32.Encoding() == sufkit::detail::LcpEncoding::kRaw32);
  CHECK(storage32.CoordinateWidth() == 32);
  CHECK(storage32.Raw32Values() != nullptr);
  CHECK(storage32.Raw64Values() == nullptr);
  CHECK(storage32.ResidentBytes() == input.lcp.size() * 4);
  CHECK(storage32.SerializedDataBytes() == input.lcp.size() * 4);
  storage32.Validate(sufkit::detail::IntegerArrayView(input.sa),
                     input.text_symbols, sufkit::ErrorCode::kCorruptIndex);
  CheckEveryRow(storage32, input);

  auto storage64 = sufkit::detail::LcpStorage::FromRaw64(
      input.lcp, 1, sufkit::ErrorCode::kBuildFailure);
  CHECK(storage64.Encoding() == sufkit::detail::LcpEncoding::kRaw64);
  CHECK(storage64.CoordinateWidth() == 64);
  CHECK(storage64.Raw32Values() == nullptr);
  CHECK(storage64.Raw64Values() != nullptr);
  CHECK(storage64.ResidentBytes() == input.lcp.size() * 8);
  storage64.Validate(sufkit::detail::IntegerArrayView(input.sa),
                     input.text_symbols, sufkit::ErrorCode::kCorruptIndex);
  CheckEveryRow(storage64, input);
}

void TestByteCodedK1AndGuide() {
  const auto input = MakeSampledAllA(10001, 1);
  auto storage = sufkit::detail::LcpStorage::BuildByteCoded(
      sufkit::detail::IntegerArrayView(input.lcp),
      sufkit::detail::IntegerArrayView(input.isa), input.sampling_rate, 32,
      input.text_symbols, sufkit::ErrorCode::kBuildFailure);
  CHECK(storage.Encoding() == sufkit::detail::LcpEncoding::kByteCoded32);
  CHECK(storage.CoordinateWidth() == 32);
  CHECK(storage.AnchorCount() == 1);
  CHECK(storage.BytePrimary() != nullptr);
  CHECK(storage.AnchorPositions32() != nullptr);
  CHECK(storage.AnchorValues32() != nullptr);
  CHECK(storage.AnchorPositions64() == nullptr);
  CHECK(storage.AnchorValues64() == nullptr);
  CHECK(storage.AnchorPositions32()->front() == 0);
  CHECK(storage.AnchorValues32()->front() == 9999);
  CHECK(storage.GuideBytes() == 4 * sizeof(std::uint32_t));
  CHECK(storage.SerializedDataBytes() == input.lcp.size() + 8);
  CHECK(storage.ResidentBytes() ==
        storage.SerializedDataBytes() + storage.GuideBytes());
  storage.Validate(sufkit::detail::IntegerArrayView(input.sa),
                   input.text_symbols, sufkit::ErrorCode::kCorruptIndex);
  CheckEveryRow(storage, input);

  auto round_trip = sufkit::detail::LcpStorage::FromByteCoded32(
      *storage.BytePrimary(), *storage.AnchorPositions32(),
      *storage.AnchorValues32(), input.sampling_rate, input.text_symbols,
      sufkit::ErrorCode::kCorruptIndex);
  round_trip.Validate(sufkit::detail::IntegerArrayView(input.sa),
                      input.text_symbols,
                      sufkit::ErrorCode::kCorruptIndex);
  CheckEveryRow(round_trip, input);
}

void TestByteCodedGeneralizedK() {
  const auto input = MakeSampledAllA(1201, 4);
  auto storage32 = sufkit::detail::LcpStorage::BuildByteCoded(
      sufkit::detail::IntegerArrayView(input.lcp),
      sufkit::detail::IntegerArrayView(input.isa), input.sampling_rate, 32,
      input.text_symbols, sufkit::ErrorCode::kBuildFailure);
  CHECK(storage32.SamplingRate() == 4);
  CHECK(storage32.AnchorCount() == 1);
  CHECK(storage32.AnchorPositions32()->front() == 0);
  CHECK(storage32.AnchorValues32()->front() == 1196);
  storage32.Validate(sufkit::detail::IntegerArrayView(input.sa),
                     input.text_symbols, sufkit::ErrorCode::kCorruptIndex);
  CheckEveryRow(storage32, input);

  auto storage64 = sufkit::detail::LcpStorage::BuildByteCoded(
      sufkit::detail::IntegerArrayView(input.lcp),
      sufkit::detail::IntegerArrayView(input.isa), input.sampling_rate, 64,
      input.text_symbols, sufkit::ErrorCode::kBuildFailure);
  CHECK(storage64.Encoding() ==
        sufkit::detail::LcpEncoding::kByteCoded64);
  CHECK(storage64.AnchorCount() == 1);
  CHECK(storage64.AnchorPositions64()->front() == 0);
  CHECK(storage64.AnchorValues64()->front() == 1196);
  storage64.Validate(sufkit::detail::IntegerArrayView(input.sa),
                     input.text_symbols, sufkit::ErrorCode::kCorruptIndex);
  CheckEveryRow(storage64, input);
}

void TestSeparateOverflowRuns() {
  std::vector<std::uint32_t> lcp{0, 300, 299, 10, 400, 399};
  std::vector<std::uint32_t> isa{0, 1, 2, 3, 4, 5};
  auto storage = sufkit::detail::LcpStorage::BuildByteCoded(
      sufkit::detail::IntegerArrayView(lcp),
      sufkit::detail::IntegerArrayView(isa), 1, 32, 6,
      sufkit::ErrorCode::kBuildFailure);
  CHECK(storage.AnchorCount() == 2);
  CHECK((*storage.AnchorPositions32())[0] == 1);
  CHECK((*storage.AnchorPositions32())[1] == 4);
  CHECK(storage.Exact(1, 1) == 300);
  CHECK(storage.Exact(2, 2) == 299);
  CHECK(storage.Exact(4, 4) == 400);
  CHECK(storage.Exact(5, 5) == 399);
}

void TestGuideBlockBoundaries() {
  constexpr std::size_t kSize = 9000;
  std::vector<std::uint32_t> lcp(kSize);
  std::vector<std::uint32_t> isa(kSize);
  for (std::size_t index = 0; index < kSize; ++index) {
    isa[index] = static_cast<std::uint32_t>(index);
  }
  lcp[4095] = 300;
  lcp[4096] = 400;
  lcp[4097] = 399;
  lcp[8193] = 500;
  lcp[8194] = 499;
  auto storage = sufkit::detail::LcpStorage::BuildByteCoded(
      sufkit::detail::IntegerArrayView(lcp),
      sufkit::detail::IntegerArrayView(isa), 1, 32, kSize,
      sufkit::ErrorCode::kBuildFailure);
  CHECK(storage.AnchorCount() == 3);
  CHECK((*storage.AnchorPositions32())[0] == 4095);
  CHECK((*storage.AnchorPositions32())[1] == 4096);
  CHECK((*storage.AnchorPositions32())[2] == 8193);
  CHECK(storage.Exact(4095, 4095) == 300);
  CHECK(storage.Exact(4096, 4096) == 400);
  CHECK(storage.Exact(4097, 4097) == 399);
  CHECK(storage.Exact(8193, 8193) == 500);
  CHECK(storage.Exact(8194, 8194) == 499);
}

void TestDirectGeneralizedKasai() {
  std::vector<std::uint8_t> repeated(1025, 'A');
  std::vector<std::uint8_t> random(1537);
  std::uint32_t state = 0xC0FFEEU;
  for (auto& value : random) {
    state = state * 1664525U + 1013904223U;
    value = static_cast<std::uint8_t>(state >> 24U);
  }

  const std::uint32_t sampling_rates[] = {1, 2, 4, 8};
  for (const auto sampling_rate : sampling_rates) {
    const auto repeated_oracle =
        MakeSampledTextOracle(repeated, sampling_rate);
    CheckDirectAgainstRaw(repeated_oracle, 32);
    CheckDirectAgainstRaw(repeated_oracle, 64);

    const auto random_oracle = MakeSampledTextOracle(random, sampling_rate);
    CheckDirectAgainstRaw(random_oracle, 32);
    CheckDirectAgainstRaw(random_oracle, 64);
  }

  const std::vector<std::uint64_t> empty;
  auto empty_storage = sufkit::detail::LcpStorage::BuildByteCodedDirect(
      nullptr, 0, sufkit::detail::IntegerArrayView(empty),
      sufkit::detail::IntegerArrayView(empty), 8, 32,
      sufkit::ErrorCode::kBuildFailure);
  CHECK(empty_storage.Empty());
  empty_storage.Validate(sufkit::detail::IntegerArrayView(empty), 0,
                         sufkit::ErrorCode::kCorruptIndex);
}

void TestInvalidBuildInputs() {
  using sufkit::ErrorCode;
  using sufkit::detail::IntegerArrayView;
  using sufkit::detail::LcpStorage;

  ExpectError(ErrorCode::kBuildFailure, [] {
    (void)LcpStorage::FromRaw32({1}, 1, ErrorCode::kBuildFailure);
  });
  ExpectError(ErrorCode::kBuildFailure, [] {
    (void)LcpStorage::FromRaw64({0}, 0, ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> lcp{0, 1, 2};
  const std::vector<std::uint32_t> short_isa{0, 1};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCoded(IntegerArrayView(lcp),
                                     IntegerArrayView(short_isa), 1, 32, 3,
                                     ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> duplicate_isa{0, 1, 1};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCoded(IntegerArrayView(lcp),
                                     IntegerArrayView(duplicate_isa), 1, 32,
                                     3, ErrorCode::kBuildFailure);
  });
  const std::vector<std::int32_t> negative_isa{0, 1, -1};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCoded(IntegerArrayView(lcp),
                                     IntegerArrayView(negative_isa), 1, 32, 3,
                                     ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> isa{0, 1, 2};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCoded(IntegerArrayView(lcp),
                                     IntegerArrayView(isa), 1, 40, 3,
                                     ErrorCode::kBuildFailure);
  });
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCoded(IntegerArrayView(lcp),
                                     IntegerArrayView(isa), 1, 32, 4,
                                     ErrorCode::kBuildFailure);
  });

  const std::vector<std::uint8_t> text{3, 2, 1};
  const std::vector<std::uint32_t> sa{2, 1, 0};
  const std::vector<std::uint32_t> direct_isa{2, 1, 0};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        nullptr, text.size(), IntegerArrayView(sa),
        IntegerArrayView(direct_isa), 1, 32, ErrorCode::kBuildFailure);
  });
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        text.data(), text.size(), IntegerArrayView(sa),
        IntegerArrayView(direct_isa), 0, 32, ErrorCode::kBuildFailure);
  });
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        text.data(), text.size(), IntegerArrayView(sa),
        IntegerArrayView(direct_isa), 1, 40, ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> short_sa{2, 1};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        text.data(), text.size(), IntegerArrayView(short_sa),
        IntegerArrayView(direct_isa), 1, 32, ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> out_of_range_isa{2, 1, 3};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        text.data(), text.size(), IntegerArrayView(sa),
        IntegerArrayView(out_of_range_isa), 1, 32,
        ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> inconsistent_isa{2, 0, 1};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        text.data(), text.size(), IntegerArrayView(sa),
        IntegerArrayView(inconsistent_isa), 1, 32,
        ErrorCode::kBuildFailure);
  });
  const std::vector<std::uint32_t> invalid_sa{2, 99, 0};
  ExpectError(ErrorCode::kBuildFailure, [&] {
    (void)LcpStorage::BuildByteCodedDirect(
        text.data(), text.size(), IntegerArrayView(invalid_sa),
        IntegerArrayView(direct_isa), 1, 32, ErrorCode::kBuildFailure);
  });
}

void TestInvalidSerializedInputs() {
  using sufkit::ErrorCode;
  using sufkit::detail::IntegerArrayView;
  using sufkit::detail::LcpStorage;

  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0, 1}, {}, {}, 1, 3,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0}, {0}, {}, 1, 1,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0, 255}, {0, 1}, {300, 300}, 1, 2,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0, 255}, {1}, {254}, 1, 2,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0, 255, 255}, {1, 1}, {300, 299}, 1, 3,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0, 255, 255}, {1, 2}, {300, 299}, 1, 3,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32({0, 255}, {1}, {300}, 2, 3,
                                      ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [] {
    (void)LcpStorage::FromByteCoded32(
        {}, {}, {}, 1,
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max()) +
            2,
        ErrorCode::kCorruptIndex);
  });

  auto missing_anchor = LcpStorage::FromByteCoded32(
      {0, 255}, {}, {}, 1, 2, ErrorCode::kCorruptIndex);
  const std::vector<std::uint32_t> sa{1, 0};
  ExpectError(ErrorCode::kCorruptIndex, [&] {
    missing_anchor.Validate(IntegerArrayView(sa), 2,
                            ErrorCode::kCorruptIndex);
  });

  auto impossible_lcp = LcpStorage::FromByteCoded32(
      {0, 255}, {0}, {300}, 1, 2, ErrorCode::kCorruptIndex);
  ExpectError(ErrorCode::kCorruptIndex, [&] {
    impossible_lcp.Validate(IntegerArrayView(sa), 2,
                            ErrorCode::kCorruptIndex);
  });
  ExpectError(ErrorCode::kCorruptIndex, [&] {
    impossible_lcp.Validate(IntegerArrayView(sa), 3,
                            ErrorCode::kCorruptIndex);
  });
}

void TestQueryErrors() {
  auto empty = sufkit::detail::LcpStorage();
  ExpectError(sufkit::ErrorCode::kInvalidInput,
              [&] { (void)empty.Exact(0, 0); });
  auto raw = sufkit::detail::LcpStorage::FromRaw32(
      {0}, 1, sufkit::ErrorCode::kBuildFailure);
  ExpectError(sufkit::ErrorCode::kInvalidInput,
              [&] { (void)raw.Exact(1, 0); });
  const std::vector<std::uint32_t> sa{0};
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    raw.Validate(sufkit::detail::IntegerArrayView(sa), 0,
                 sufkit::ErrorCode::kCorruptIndex);
  });
}

}  // namespace

int main() {
  TestRawStorage();
  TestByteCodedK1AndGuide();
  TestByteCodedGeneralizedK();
  TestSeparateOverflowRuns();
  TestGuideBlockBoundaries();
  TestDirectGeneralizedKasai();
  TestInvalidBuildInputs();
  TestInvalidSerializedInputs();
  TestQueryErrors();
  if (failures != 0) {
    std::cerr << failures << " LCP storage test(s) failed\n";
    return 1;
  }
  std::cout << "all LCP storage tests passed\n";
  return 0;
}
