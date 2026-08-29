// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <tuple>
#include <vector>

#include "coordinate_storage.hpp"

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

using sufkit::detail::CoordinateStorage;
using sufkit::detail::CoordinateStorageWidth;
using sufkit::detail::DivSufsort64Buffer;

void TestSelection() {
  using sufkit::detail::ResolveCoordinateStorageWidth;
  using sufkit::detail::SelectCoordinateStorageWidth;
  constexpr std::uint64_t kTwo32 = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kTwo40 = std::uint64_t{1} << 40U;
  constexpr std::uint64_t kTwo48 = std::uint64_t{1} << 48U;

  CHECK(SelectCoordinateStorageWidth(1) ==
        CoordinateStorageWidth::kBits32);
  CHECK(SelectCoordinateStorageWidth(kTwo32 - 1) ==
        CoordinateStorageWidth::kBits32);
  CHECK(SelectCoordinateStorageWidth(kTwo32) ==
        CoordinateStorageWidth::kBits32);
  CHECK(SelectCoordinateStorageWidth(kTwo32 + 1) ==
        CoordinateStorageWidth::kBits40);
  CHECK(SelectCoordinateStorageWidth(kTwo32 + 2) ==
        CoordinateStorageWidth::kBits40);
  CHECK(SelectCoordinateStorageWidth(kTwo40) ==
        CoordinateStorageWidth::kBits40);
  CHECK(SelectCoordinateStorageWidth(kTwo40 + 1) ==
        CoordinateStorageWidth::kBits48);
  CHECK(SelectCoordinateStorageWidth(kTwo40 + 2) ==
        CoordinateStorageWidth::kBits48);
  CHECK(SelectCoordinateStorageWidth(kTwo48) ==
        CoordinateStorageWidth::kBits48);
  CHECK(SelectCoordinateStorageWidth(kTwo48 + 1) ==
        CoordinateStorageWidth::kBits64);
  CHECK(SelectCoordinateStorageWidth(
            std::numeric_limits<std::uint64_t>::max()) ==
        CoordinateStorageWidth::kBits64);

  CHECK(ResolveCoordinateStorageWidth(CoordinateStorageWidth::kBits40,
                                      kTwo32) ==
        CoordinateStorageWidth::kBits40);
  ExpectError(sufkit::ErrorCode::kInvalidInput, [] {
    (void)sufkit::detail::SelectCoordinateStorageWidth(0);
  });
  ExpectError(sufkit::ErrorCode::kInvalidInput, [=] {
    (void)ResolveCoordinateStorageWidth(CoordinateStorageWidth::kBits32,
                                        kTwo32 + 1);
  });
  ExpectError(sufkit::ErrorCode::kInvalidInput, [=] {
    (void)ResolveCoordinateStorageWidth(CoordinateStorageWidth::kBits40,
                                        kTwo40 + 1);
  });
  ExpectError(sufkit::ErrorCode::kInvalidInput, [=] {
    (void)ResolveCoordinateStorageWidth(CoordinateStorageWidth::kBits48,
                                        kTwo48 + 1);
  });
  ExpectError(sufkit::ErrorCode::kInvalidInput, [] {
    (void)sufkit::detail::MaxCoordinateForWidth(
        static_cast<CoordinateStorageWidth>(17));
  });
}

void TestByteCounts() {
  using sufkit::detail::CoordinateStorageByteCount;
  CHECK(CoordinateStorageByteCount(7, CoordinateStorageWidth::kBits32) == 28);
  CHECK(CoordinateStorageByteCount(7, CoordinateStorageWidth::kBits40) == 35);
  CHECK(CoordinateStorageByteCount(7, CoordinateStorageWidth::kBits48) == 42);
  CHECK(CoordinateStorageByteCount(7, CoordinateStorageWidth::kBits64) == 56);
  ExpectError(sufkit::ErrorCode::kInvalidInput, [] {
    (void)sufkit::detail::CoordinateStorageByteCount(
        1, CoordinateStorageWidth::kAutoSelect);
  });
  ExpectError(sufkit::ErrorCode::kInvalidInput, [] {
    (void)sufkit::detail::CoordinateStorageByteCount(
        std::numeric_limits<std::uint64_t>::max(),
        CoordinateStorageWidth::kBits64);
  });
}

std::vector<std::uint64_t> BoundaryValues() {
  return {0,
          1,
          std::numeric_limits<std::uint32_t>::max(),
          std::uint64_t{1} << 32U,
          (std::uint64_t{1} << 40U) - 1U,
          std::uint64_t{1} << 40U,
          (std::uint64_t{1} << 48U) - 1U};
}

void CheckStorage(const CoordinateStorage& storage,
                  CoordinateStorageWidth expected_width,
                  const std::vector<std::uint64_t>& expected) {
  CHECK(storage.Width() == expected_width);
  CHECK(storage.Size() == expected.size());
  CHECK(storage.Empty() == expected.empty());
  CHECK(storage.Bytes() ==
        sufkit::detail::CoordinateStorageByteCount(expected.size(),
                                                   expected_width));
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(storage.At(index) == expected[index]);
  }

  std::vector<std::uint64_t> decoded(expected.size(), 0);
  storage.DecodeSpan(0, expected.size(), decoded.data());
  CHECK(decoded == expected);
  if (expected.size() >= 4) {
    std::fill(decoded.begin(), decoded.end(), 0);
    storage.DecodeSpan(1, 3, decoded.data());
    CHECK(std::equal(decoded.begin(), decoded.begin() + 3,
                     expected.begin() + 1));
  }
  storage.DecodeSpan(storage.Size(), 0, nullptr);
}

void TestPackAndDecode() {
  constexpr std::uint64_t kTwo32 = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kTwo40 = std::uint64_t{1} << 40U;
  constexpr std::uint64_t kTwo48 = std::uint64_t{1} << 48U;
  const auto all = BoundaryValues();

  CheckStorage(CoordinateStorage::FromUInt64(
                   {0, 1, std::numeric_limits<std::uint32_t>::max()},
                   CoordinateStorageWidth::kBits32, kTwo32),
               CoordinateStorageWidth::kBits32,
               {0, 1, std::numeric_limits<std::uint32_t>::max()});
  CheckStorage(CoordinateStorage::FromUInt64(
                   {0, kTwo32 - 1, kTwo32, kTwo40 - 1},
                   CoordinateStorageWidth::kBits40, kTwo40),
               CoordinateStorageWidth::kBits40,
               {0, kTwo32 - 1, kTwo32, kTwo40 - 1});
  CheckStorage(CoordinateStorage::FromUInt64(
                   std::vector<std::uint64_t>(all),
                   CoordinateStorageWidth::kBits48, kTwo48),
               CoordinateStorageWidth::kBits48, all);
  CheckStorage(CoordinateStorage::FromUInt64(
                   std::vector<std::uint64_t>(all),
                   CoordinateStorageWidth::kBits64, kTwo48),
               CoordinateStorageWidth::kBits64, all);

  std::vector<std::uint32_t> values32 = {
      0, 17, std::numeric_limits<std::uint32_t>::max()};
  CheckStorage(CoordinateStorage::FromUInt32(
                   std::move(values32), CoordinateStorageWidth::kBits32,
                   kTwo32),
               CoordinateStorageWidth::kBits32,
               {0, 17, std::numeric_limits<std::uint32_t>::max()});
  CheckStorage(CoordinateStorage::FromUInt32(
                   {0, 17, std::numeric_limits<std::uint32_t>::max()},
                   CoordinateStorageWidth::kBits40, kTwo40),
               CoordinateStorageWidth::kBits40,
               {0, 17, std::numeric_limits<std::uint32_t>::max()});
  CheckStorage(CoordinateStorage::FromUInt32(
                   {0, 17, std::numeric_limits<std::uint32_t>::max()},
                   CoordinateStorageWidth::kBits48, kTwo48),
               CoordinateStorageWidth::kBits48,
               {0, 17, std::numeric_limits<std::uint32_t>::max()});
  CheckStorage(CoordinateStorage::FromUInt32(
                   {0, 17, std::numeric_limits<std::uint32_t>::max()},
                   CoordinateStorageWidth::kBits64, kTwo48),
               CoordinateStorageWidth::kBits64,
               {0, 17, std::numeric_limits<std::uint32_t>::max()});
}

void TestSignedPackAndDecode() {
  constexpr std::uint64_t kTwo32 = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kTwo40 = std::uint64_t{1} << 40U;
  constexpr std::uint64_t kTwo48 = std::uint64_t{1} << 48U;

  const std::vector<std::uint64_t> odd32 = {
      0, 1, 17, kTwo32 - 2, kTwo32 - 1};
  CheckStorage(CoordinateStorage::FromInt64(
                   {0, 1, 17, static_cast<std::int64_t>(kTwo32 - 2),
                    static_cast<std::int64_t>(kTwo32 - 1)},
                   CoordinateStorageWidth::kBits32, kTwo32),
               CoordinateStorageWidth::kBits32, odd32);

  const std::vector<std::uint64_t> even40 = {
      0, 1, kTwo32 - 1, kTwo32, kTwo40 - 2, kTwo40 - 1};
  CheckStorage(CoordinateStorage::FromInt64(
                   {0, 1, static_cast<std::int64_t>(kTwo32 - 1),
                    static_cast<std::int64_t>(kTwo32),
                    static_cast<std::int64_t>(kTwo40 - 2),
                    static_cast<std::int64_t>(kTwo40 - 1)},
                   CoordinateStorageWidth::kBits40, kTwo40),
               CoordinateStorageWidth::kBits40, even40);

  const std::vector<std::uint64_t> odd48 = {
      0,          1,          kTwo32 - 1, kTwo32,
      kTwo40 - 1, kTwo40,     kTwo48 - 1};
  CheckStorage(CoordinateStorage::FromInt64(
                   {0, 1, static_cast<std::int64_t>(kTwo32 - 1),
                    static_cast<std::int64_t>(kTwo32),
                    static_cast<std::int64_t>(kTwo40 - 1),
                    static_cast<std::int64_t>(kTwo40),
                    static_cast<std::int64_t>(kTwo48 - 1)},
                   CoordinateStorageWidth::kBits48, kTwo48),
               CoordinateStorageWidth::kBits48, odd48);

  const auto maximum_signed =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  CheckStorage(CoordinateStorage::FromInt64(
                   {0, 1, std::numeric_limits<std::int64_t>::max()},
                   CoordinateStorageWidth::kBits64, maximum_signed + 1U),
               CoordinateStorageWidth::kBits64,
               {0, 1, maximum_signed});
}

void TestFailures() {
  constexpr std::uint64_t kTwo32 = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kTwo40 = std::uint64_t{1} << 40U;
  constexpr std::uint64_t kTwo48 = std::uint64_t{1} << 48U;
  ExpectError(sufkit::ErrorCode::kBuildFailure, [=] {
    (void)CoordinateStorage::FromUInt64(
        {kTwo32}, CoordinateStorageWidth::kBits32, kTwo32 + 1);
  });
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [] {
    (void)CoordinateStorage::FromUInt64(
        {0, 4}, CoordinateStorageWidth::kBits32, 4,
        sufkit::ErrorCode::kCorruptIndex, "loaded SA");
  });
  ExpectError(sufkit::ErrorCode::kBuildFailure, [] {
    (void)CoordinateStorage::FromInt64(
        {0, -1, 2}, CoordinateStorageWidth::kBits32, 3);
  });
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [] {
    (void)CoordinateStorage::FromInt64(
        {0, 4}, CoordinateStorageWidth::kBits32, 4,
        sufkit::ErrorCode::kCorruptIndex, "loaded signed SA");
  });
  ExpectError(sufkit::ErrorCode::kBuildFailure, [=] {
    (void)CoordinateStorage::FromInt64(
        {static_cast<std::int64_t>(kTwo32)},
        CoordinateStorageWidth::kBits32, kTwo32 + 1);
  });
  ExpectError(sufkit::ErrorCode::kBuildFailure, [=] {
    (void)CoordinateStorage::FromInt64(
        {static_cast<std::int64_t>(kTwo40)},
        CoordinateStorageWidth::kBits40, kTwo40 + 1);
  });
  ExpectError(sufkit::ErrorCode::kBuildFailure, [=] {
    (void)CoordinateStorage::FromInt64(
        {static_cast<std::int64_t>(kTwo48)},
        CoordinateStorageWidth::kBits48, kTwo48 + 1);
  });

  auto storage = CoordinateStorage::FromUInt64(
      {0, 1, 2}, CoordinateStorageWidth::kBits32, 3);
  ExpectError(sufkit::ErrorCode::kInvalidInput,
              [&] { (void)storage.At(3); });
  ExpectError(sufkit::ErrorCode::kInvalidInput,
              [&] { storage.DecodeSpan(2, 2, nullptr); });
  ExpectError(sufkit::ErrorCode::kInvalidInput,
              [&] { storage.DecodeSpan(0, 1, nullptr); });
}

void TestLargeSpanWidthEquivalence() {
  constexpr std::uint64_t kSymbolCount = std::uint64_t{1} << 20U;
  std::vector<std::uint64_t> expected;
  expected.reserve(1031);
  for (std::uint64_t index = 0; index < 1031; ++index) {
    expected.push_back((index * 65537U + 19U) % kSymbolCount);
  }

  for (const auto width : {CoordinateStorageWidth::kBits32,
                           CoordinateStorageWidth::kBits40,
                           CoordinateStorageWidth::kBits48,
                           CoordinateStorageWidth::kBits64}) {
    const auto storage = CoordinateStorage::FromUInt64(
        std::vector<std::uint64_t>(expected), width, kSymbolCount);
    for (const std::size_t begin : {std::size_t{0}, std::size_t{1},
                                    std::size_t{255}, std::size_t{256},
                                    std::size_t{511}, std::size_t{512},
                                    std::size_t{1023}}) {
      const auto count = std::min<std::size_t>(513, expected.size() - begin);
      std::vector<std::uint64_t> decoded(count);
      storage.DecodeSpan(begin, count, decoded.data());
      CHECK(std::equal(decoded.begin(), decoded.end(),
                       expected.begin() + static_cast<std::ptrdiff_t>(begin)));
    }
  }
}

void TestDivSufsort64InPlaceStorageConversion() {
  constexpr std::size_t kCount = 4097;
  constexpr std::size_t kConstructedCount = kCount + 257;

  for (const auto width : {CoordinateStorageWidth::kBits32,
                           CoordinateStorageWidth::kBits40,
                           CoordinateStorageWidth::kBits48,
                           CoordinateStorageWidth::kBits64}) {
    std::uint64_t base = 0;
    std::uint64_t symbol_count = std::uint64_t{1} << 20U;
    if (width == CoordinateStorageWidth::kBits40) {
      base = std::uint64_t{1} << 32U;
      symbol_count = std::uint64_t{1} << 40U;
    } else if (width == CoordinateStorageWidth::kBits48) {
      base = std::uint64_t{1} << 40U;
      symbol_count = std::uint64_t{1} << 48U;
    } else if (width == CoordinateStorageWidth::kBits64) {
      base = std::uint64_t{1} << 48U;
      symbol_count = std::uint64_t{1} << 49U;
    }

    DivSufsort64Buffer source;
    source.resize(kConstructedCount);
    std::vector<std::uint64_t> expected(kCount);
    for (std::size_t index = 0; index < kCount; ++index) {
      expected[index] = base + index;
      source[index] = static_cast<std::int64_t>(expected[index]);
    }
    source.resize(kCount);
    const auto storage = CoordinateStorage::FromDivSufsort64(
        std::move(source), width, symbol_count);
    CheckStorage(storage, width, expected);
  }

  DivSufsort64Buffer negative;
  negative.resize(3);
  negative[0] = 0;
  negative[1] = -1;
  negative[2] = 2;
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)CoordinateStorage::FromDivSufsort64(
        std::move(negative), CoordinateStorageWidth::kBits32, 3);
  });

  DivSufsort64Buffer too_wide;
  too_wide.resize(1);
  too_wide[0] = static_cast<std::int64_t>(std::uint64_t{1} << 32U);
  ExpectError(sufkit::ErrorCode::kBuildFailure, [&] {
    (void)CoordinateStorage::FromDivSufsort64(
        std::move(too_wide), CoordinateStorageWidth::kBits32,
        (std::uint64_t{1} << 32U) + 1U);
  });
}

void TestConcurrentReads() {
  constexpr std::uint64_t kSymbolCount = std::uint64_t{1} << 48U;
  std::vector<std::uint64_t> expected;
  expected.reserve(4096);
  for (std::uint64_t index = 0; index < 4096; ++index) {
    expected.push_back((index << 32U) | (index * 2654435761U));
  }
  const auto storage = CoordinateStorage::FromUInt64(
      std::vector<std::uint64_t>(expected), CoordinateStorageWidth::kBits48,
      kSymbolCount);

  std::atomic<bool> valid{true};
  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < 8; ++thread_index) {
    threads.emplace_back([&] {
      std::vector<std::uint64_t> decoded(127);
      for (std::size_t begin = 0; begin + decoded.size() <= expected.size();
           begin += 31) {
        storage.DecodeSpan(begin, decoded.size(), decoded.data());
        const auto expected_begin =
            expected.begin() + static_cast<std::ptrdiff_t>(begin);
        if (!std::equal(decoded.begin(), decoded.end(), expected_begin)) {
          valid.store(false, std::memory_order_relaxed);
          return;
        }
        if (storage.At(begin) != expected[begin]) {
          valid.store(false, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(valid.load(std::memory_order_relaxed));
}

}  // namespace

int main() {
  TestSelection();
  TestByteCounts();
  TestPackAndDecode();
  TestSignedPackAndDecode();
  TestFailures();
  TestLargeSpanWidthEquivalence();
  TestDivSufsort64InPlaceStorageConversion();
  TestConcurrentReads();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "coordinate storage tests passed\n";
  return 0;
}
