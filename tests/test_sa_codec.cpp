// SPDX-License-Identifier: MIT

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "sa_codec.hpp"
#include "serialization.hpp"

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

template <class Function>
void ExpectErrorContaining(sufkit::ErrorCode code, std::string_view needle,
                           Function&& function) {
  try {
    function();
    CHECK(false);
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == code);
    CHECK(std::string_view(error.what()).find(needle) !=
          std::string_view::npos);
  }
}

void PutU32(std::string& bytes, std::size_t offset, std::uint32_t value) {
  CHECK(offset + 4 <= bytes.size());
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[offset + index] =
        static_cast<char>(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void PutU64(std::string& bytes, std::size_t offset, std::uint64_t value) {
  CHECK(offset + 8 <= bytes.size());
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[offset + index] =
        static_cast<char>(static_cast<std::uint8_t>(value >> (index * 8U)));
  }
}

void AppendU32(std::string& bytes, std::uint32_t value) {
  const auto offset = bytes.size();
  bytes.resize(offset + 4);
  PutU32(bytes, offset, value);
}

void AppendU16(std::string& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<char>(static_cast<std::uint8_t>(value)));
  bytes.push_back(
      static_cast<char>(static_cast<std::uint8_t>(value >> 8U)));
}

void AppendU64(std::string& bytes, std::uint64_t value) {
  const auto offset = bytes.size();
  bytes.resize(offset + 8);
  PutU64(bytes, offset, value);
}

// These builders intentionally duplicate the on-disk layout with literals.
// They must not call the production writers: the resulting golden bytes test
// the reader contract independently of the writer implementation.
std::string HandEncodeCoordinateHeader(std::uint32_t codec,
                                       std::uint64_t symbol_count,
                                       std::uint64_t element_count,
                                       std::uint64_t low_plane_bytes,
                                       std::uint64_t high_plane_bytes) {
  std::string bytes;
  AppendU32(bytes, 0x31434b53U);  // "SKC1"
  AppendU32(bytes, 1);            // codec header version
  AppendU32(bytes, codec);
  AppendU32(bytes, 0);  // reserved
  AppendU64(bytes, symbol_count);
  AppendU64(bytes, element_count);
  AppendU64(bytes, low_plane_bytes);
  AppendU64(bytes, high_plane_bytes);
  AppendU64(bytes, 0);  // reserved
  CHECK(bytes.size() == 56);
  return bytes;
}

std::string HandEncodeLcpHeader(std::uint32_t codec,
                                std::uint32_t coordinate_width,
                                std::uint32_t sampling_rate,
                                std::uint64_t text_symbols,
                                std::uint64_t row_count,
                                std::uint64_t primary_bytes,
                                std::uint64_t anchor_count,
                                std::uint64_t anchor_position_bytes,
                                std::uint64_t anchor_value_bytes) {
  std::string bytes;
  AppendU32(bytes, 0x314c4b53U);  // "SKL1"
  AppendU32(bytes, 1);            // codec header version
  AppendU32(bytes, codec);
  AppendU32(bytes, coordinate_width);
  AppendU32(bytes, sampling_rate);
  AppendU32(bytes, 12);  // fixed 4096-symbol guide block shift
  AppendU64(bytes, text_symbols);
  AppendU64(bytes, row_count);
  AppendU64(bytes, primary_bytes);
  AppendU64(bytes, anchor_count);
  AppendU64(bytes, anchor_position_bytes);
  AppendU64(bytes, anchor_value_bytes);
  AppendU64(bytes, 0);  // reserved
  CHECK(bytes.size() == 80);
  return bytes;
}

void CheckHandEncodedCoordinates(
    const std::string& bytes, sufkit::CoordinateStorageWidth width,
    std::uint64_t symbol_count,
    const std::vector<std::uint64_t>& expected) {
  std::istringstream input(bytes, std::ios::in | std::ios::binary);
  const auto decoded = sufkit::detail::ReadCoordinateSectionV14(
      input, expected.size(), symbol_count, "hand-encoded coordinates");
  CHECK(decoded.Width() == width);
  CHECK(decoded.Size() == expected.size());
  std::vector<std::uint64_t> span(expected.size());
  decoded.DecodeSpan(0, span.size(), span.data());
  CHECK(span == expected);
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(decoded.At(index) == expected[index]);
  }
}

void TestHandEncodedCoordinateSections() {
  constexpr std::uint64_t kTwo32 = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kTwo40 = std::uint64_t{1} << 40U;
  constexpr std::uint64_t kTwo48 = std::uint64_t{1} << 48U;

  auto native32 = HandEncodeCoordinateHeader(0, kTwo32, 3, 12, 0);
  AppendU32(native32, 0x00000000U);
  AppendU32(native32, 0x78563412U);
  AppendU32(native32, 0xffffffffU);
  CheckHandEncodedCoordinates(
      native32, sufkit::CoordinateStorageWidth::kBits32, kTwo32,
      {0, 0x78563412U, kTwo32 - 1});

  auto native64 =
      HandEncodeCoordinateHeader(1, kTwo48 + 0x10000U, 3, 24, 0);
  AppendU64(native64, 0);
  AppendU64(native64, kTwo40 + 0x12345678U);
  AppendU64(native64, kTwo48 + 0x4321U);
  CheckHandEncodedCoordinates(
      native64, sufkit::CoordinateStorageWidth::kBits64,
      kTwo48 + 0x10000U,
      {0, kTwo40 + 0x12345678U, kTwo48 + 0x4321U});

  auto split40 = HandEncodeCoordinateHeader(2, kTwo40, 4, 16, 4);
  AppendU32(split40, 0);
  AppendU32(split40, 0xffffffffU);
  AppendU32(split40, 0);
  AppendU32(split40, 0x89abcdefU);
  split40.push_back(static_cast<char>(0x00));
  split40.push_back(static_cast<char>(0x00));
  split40.push_back(static_cast<char>(0x01));
  split40.push_back(static_cast<char>(0xff));
  CheckHandEncodedCoordinates(
      split40, sufkit::CoordinateStorageWidth::kBits40, kTwo40,
      {0, kTwo32 - 1, kTwo32, 0xff89abcdefULL});

  auto split48 = HandEncodeCoordinateHeader(3, kTwo48, 4, 16, 8);
  AppendU32(split48, 0);
  AppendU32(split48, 0xffffffffU);
  AppendU32(split48, 0);
  AppendU32(split48, 0x01234567U);
  AppendU16(split48, 0x0000U);
  AppendU16(split48, 0x0000U);
  AppendU16(split48, 0x0001U);
  AppendU16(split48, 0xabcdU);
  CheckHandEncodedCoordinates(
      split48, sufkit::CoordinateStorageWidth::kBits48, kTwo48,
      {0, kTwo32 - 1, kTwo32, 0xabcd01234567ULL});

  auto unknown = HandEncodeCoordinateHeader(4, 1, 1, 4, 0);
  AppendU32(unknown, 0);
  ExpectError(sufkit::ErrorCode::kVersionMismatch, [&] {
    std::istringstream input(unknown, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 1, 1,
                                                    "hand-encoded error");
  });

  auto wrong_planes = HandEncodeCoordinateHeader(2, kTwo40, 1, 4, 0);
  AppendU32(wrong_planes, 0);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(wrong_planes,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 1, kTwo40,
                                                    "hand-encoded error");
  });

  auto out_of_domain = HandEncodeCoordinateHeader(2, 3, 1, 4, 1);
  AppendU32(out_of_domain, 0);
  out_of_domain.push_back(static_cast<char>(1));
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(out_of_domain,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 1, 3,
                                                    "hand-encoded error");
  });
}

std::string WriteCoordinates(const sufkit::detail::CoordinateStorage& storage,
                             std::uint64_t symbol_count) {
  std::ostringstream output(std::ios::out | std::ios::binary);
  sufkit::detail::WriteCoordinateSectionV14(output, storage, symbol_count);
  return output.str();
}

void CheckCoordinateRoundTrip(
    std::vector<std::uint64_t> values,
    sufkit::CoordinateStorageWidth width, std::uint64_t symbol_count) {
  const auto expected = values;
  auto storage = sufkit::detail::CoordinateStorage::FromUInt64(
      std::move(values), width, symbol_count);
  const auto bytes = WriteCoordinates(storage, symbol_count);

  std::istringstream header_input(bytes,
                                  std::ios::in | std::ios::binary);
  const auto header =
      sufkit::detail::ReadCoordinateCodecHeader(header_input);
  CHECK(header.width == width);
  CHECK(header.element_count == expected.size());
  CHECK(header.symbol_count == symbol_count);

  std::istringstream input(bytes, std::ios::in | std::ios::binary);
  const auto decoded = sufkit::detail::ReadCoordinateSectionV14(
      input, expected.size(), symbol_count, "test coordinates");
  CHECK(decoded.Width() == width);
  CHECK(decoded.Size() == expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(decoded.At(index) == expected[index]);
  }
  CHECK(WriteCoordinates(decoded, symbol_count) == bytes);
}

void TestCoordinateRoundTrips() {
  constexpr std::uint64_t kTwo32 = std::uint64_t{1} << 32U;
  constexpr std::uint64_t kTwo40 = std::uint64_t{1} << 40U;
  constexpr std::uint64_t kTwo48 = std::uint64_t{1} << 48U;
  CheckCoordinateRoundTrip(
      {0, 17, std::numeric_limits<std::uint32_t>::max()},
      sufkit::CoordinateStorageWidth::kBits32, kTwo32);
  CheckCoordinateRoundTrip(
      {0, kTwo32 - 1, kTwo32, kTwo40 - 1},
      sufkit::CoordinateStorageWidth::kBits40, kTwo40);
  CheckCoordinateRoundTrip(
      {0, kTwo32, kTwo40, kTwo48 - 1},
      sufkit::CoordinateStorageWidth::kBits48, kTwo48);
  CheckCoordinateRoundTrip(
      {0, kTwo32, kTwo48, kTwo48 + 17},
      sufkit::CoordinateStorageWidth::kBits64, kTwo48 + 18);
}

void TestCoordinateCorruption() {
  auto storage = sufkit::detail::CoordinateStorage::FromUInt64(
      {0, 1, 2}, sufkit::CoordinateStorageWidth::kBits40, 3);
  const auto valid = WriteCoordinates(storage, 3);

  auto unknown = valid;
  PutU32(unknown, 8, 99);
  ExpectError(sufkit::ErrorCode::kVersionMismatch, [&] {
    std::istringstream input(unknown, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 3, 3, "test");
  });

  auto wrong_length = valid;
  PutU64(wrong_length, 40, 4);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(wrong_length,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 3, 3, "test");
  });

  auto out_of_range = valid;
  constexpr std::size_t kCoordinateHeaderBytes = 56;
  out_of_range[kCoordinateHeaderBytes + 3 * sizeof(std::uint32_t)] = 1;
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(out_of_range,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 3, 3, "test");
  });

  auto trailing = valid;
  trailing.push_back('\0');
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(trailing, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 3, 3, "test");
  });

  auto truncated = valid.substr(0, valid.size() - 1);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(truncated, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateSectionV14(input, 3, 3, "test");
  });

  const auto native64 = WriteCoordinates(
      sufkit::detail::CoordinateStorage::FromUInt64(
          {0}, sufkit::CoordinateStorageWidth::kBits64, 1),
      1);
  auto total_size_overflow = native64;
  const auto huge_count = std::numeric_limits<std::uint64_t>::max() / 8U;
  PutU64(total_size_overflow, 24, huge_count);
  PutU64(total_size_overflow, 32, huge_count * 8U);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(total_size_overflow,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadCoordinateCodecHeader(input);
  });
}

void TestConstructionWidthMetadataBounds() {
  sufkit::detail::ParsedContainer container;
  container.spec.kind = sufkit::IndexKind::kSuffixArray;
  container.spec.format_minor = 4;
  container.spec.coordinate_width = 32;
  container.sections = {
      {sufkit::detail::SectionType::kMetadata},
      {sufkit::detail::SectionType::kText},
      {sufkit::detail::SectionType::kSuffixArray},
  };

  container.spec.backend = sufkit::detail::StoredBackend::kDivsufsort32;
  container.spec.text_symbols =
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) +
      1U;
  ExpectErrorContaining(sufkit::ErrorCode::kCorruptIndex,
                        "construction width", [&] {
                          (void)sufkit::detail::IndexInfoFromContainer(
                              container);
                        });

  container.spec.backend = sufkit::detail::StoredBackend::kCaps32;
  container.spec.text_symbols =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) +
      1U;
  ExpectErrorContaining(sufkit::ErrorCode::kCorruptIndex,
                        "construction width", [&] {
                          (void)sufkit::detail::IndexInfoFromContainer(
                              container);
                        });
}

struct AllAData {
  std::uint64_t text_symbols = 0;
  std::vector<std::uint64_t> sa;
  std::vector<std::uint64_t> isa;
  std::vector<std::uint64_t> lcp;
};

AllAData MakeAllA(std::uint64_t text_symbols) {
  AllAData data;
  data.text_symbols = text_symbols;
  data.sa.resize(static_cast<std::size_t>(text_symbols));
  data.isa.resize(static_cast<std::size_t>(text_symbols));
  data.lcp.resize(static_cast<std::size_t>(text_symbols));
  for (std::uint64_t row = 0; row < text_symbols; ++row) {
    const auto position = text_symbols - 1 - row;
    data.sa[static_cast<std::size_t>(row)] = position;
    data.isa[static_cast<std::size_t>(position)] = row;
    data.lcp[static_cast<std::size_t>(row)] = row <= 1 ? 0 : row - 1;
  }
  return data;
}

void TestHandEncodedLcpSections() {
  {
    constexpr std::uint64_t kTextSymbols = 4;
    const std::vector<std::uint64_t> suffix_array = {0, 1, 2, 3};
    const std::vector<std::uint64_t> expected = {0, 3, 2, 1};
    auto bytes = HandEncodeLcpHeader(0, 32, 1, kTextSymbols,
                                     expected.size(), 16, 0, 0, 0);
    for (const auto value : expected) {
      AppendU32(bytes, static_cast<std::uint32_t>(value));
    }
    const sufkit::detail::IntegerArrayView sa(suffix_array);
    std::istringstream input(bytes, std::ios::in | std::ios::binary);
    const auto decoded = sufkit::detail::ReadLcpSectionV14(
        input, expected.size(), 1, kTextSymbols, &sa);
    CHECK(decoded.Encoding() == sufkit::detail::LcpEncoding::kRaw32);
    for (std::size_t row = 0; row < expected.size(); ++row) {
      CHECK(decoded.Exact(row, suffix_array[row]) == expected[row]);
    }
  }

  {
    constexpr std::uint64_t kSamplingRate =
        std::numeric_limits<std::uint32_t>::max();
    constexpr std::uint64_t kTextSymbols = 3 * kSamplingRate + 1;
    const std::vector<std::uint64_t> suffix_array = {
        0, kSamplingRate, 2 * kSamplingRate, 3 * kSamplingRate};
    const std::vector<std::uint64_t> expected = {
        0, 1, std::uint64_t{1} << 32U, 1};
    auto bytes = HandEncodeLcpHeader(0, 64, kSamplingRate, kTextSymbols,
                                     expected.size(), 32, 0, 0, 0);
    for (const auto value : expected) {
      AppendU64(bytes, value);
    }
    const sufkit::detail::IntegerArrayView sa(suffix_array);
    std::istringstream input(bytes, std::ios::in | std::ios::binary);
    const auto decoded = sufkit::detail::ReadLcpSectionV14(
        input, expected.size(), kSamplingRate, kTextSymbols, &sa);
    CHECK(decoded.Encoding() == sufkit::detail::LcpEncoding::kRaw64);
    for (std::size_t row = 0; row < expected.size(); ++row) {
      CHECK(decoded.Exact(row, suffix_array[row]) == expected[row]);
    }
  }

  const auto check_byte_coded = [](std::uint32_t coordinate_width) {
    constexpr std::uint64_t kTextSymbols = 300;
    constexpr std::uint64_t kAnchorPosition = 5;
    constexpr std::uint64_t kAnchorValue = 260;
    std::vector<std::uint64_t> suffix_array(kTextSymbols);
    for (std::size_t row = 0; row < suffix_array.size(); ++row) {
      suffix_array[row] = row;
    }

    std::string primary(kTextSymbols, '\0');
    primary[5] = static_cast<char>(255);
    primary[6] = static_cast<char>(255);
    const auto coordinate_bytes = coordinate_width / 8U;
    auto bytes = HandEncodeLcpHeader(
        1, coordinate_width, 1, kTextSymbols, kTextSymbols,
        primary.size(), 1, coordinate_bytes, coordinate_bytes);
    bytes.append(primary);
    if (coordinate_width == 32) {
      AppendU32(bytes, static_cast<std::uint32_t>(kAnchorPosition));
      AppendU32(bytes, static_cast<std::uint32_t>(kAnchorValue));
    } else {
      AppendU64(bytes, kAnchorPosition);
      AppendU64(bytes, kAnchorValue);
    }

    const sufkit::detail::IntegerArrayView sa(suffix_array);
    std::istringstream input(bytes, std::ios::in | std::ios::binary);
    const auto decoded = sufkit::detail::ReadLcpSectionV14(
        input, kTextSymbols, 1, kTextSymbols, &sa);
    CHECK(decoded.Encoding() ==
          (coordinate_width == 32
               ? sufkit::detail::LcpEncoding::kByteCoded32
               : sufkit::detail::LcpEncoding::kByteCoded64));
    CHECK(decoded.Exact(5, 5) == 260);
    CHECK(decoded.Exact(6, 6) == 259);
    CHECK(decoded.Exact(7, 7) == 0);
  };
  check_byte_coded(32);
  check_byte_coded(64);

  auto unknown = HandEncodeLcpHeader(2, 32, 1, 1, 1, 4, 0, 0, 0);
  AppendU32(unknown, 0);
  ExpectError(sufkit::ErrorCode::kVersionMismatch, [&] {
    std::istringstream input(unknown, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(input, 1, 1, 1);
  });

  auto wrong_planes = HandEncodeLcpHeader(1, 32, 1, 4, 4, 4, 1, 0, 4);
  wrong_planes.append(4, '\0');
  AppendU32(wrong_planes, 255);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(wrong_planes,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(input, 4, 1, 4);
  });

  std::string primary(300, '\0');
  primary[5] = static_cast<char>(255);
  auto out_of_bounds =
      HandEncodeLcpHeader(1, 64, 1, 300, 300, 300, 1, 8, 8);
  out_of_bounds.append(primary);
  AppendU64(out_of_bounds, 5);
  AppendU64(out_of_bounds, (std::uint64_t{1} << 32U) + 260);
  std::vector<std::uint64_t> suffix_array(300);
  for (std::size_t row = 0; row < suffix_array.size(); ++row) {
    suffix_array[row] = row;
  }
  const sufkit::detail::IntegerArrayView sa(suffix_array);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(out_of_bounds,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(input, 300, 1, 300, &sa);
  });
}

std::string WriteLcp(const sufkit::detail::LcpStorage& storage,
                     std::uint64_t text_symbols) {
  std::ostringstream output(std::ios::out | std::ios::binary);
  sufkit::detail::WriteLcpSectionV14(output, storage, text_symbols);
  return output.str();
}

void CheckLcpRoundTrip(const sufkit::detail::LcpStorage& storage,
                       const AllAData& data) {
  const auto bytes = WriteLcp(storage, data.text_symbols);
  std::istringstream header_input(bytes,
                                  std::ios::in | std::ios::binary);
  const auto header = sufkit::detail::ReadLcpCodecHeader(header_input);
  CHECK(header.row_count == data.lcp.size());
  CHECK(header.text_symbols == data.text_symbols);

  const sufkit::detail::IntegerArrayView sa(data.sa);
  std::istringstream input(bytes, std::ios::in | std::ios::binary);
  const auto decoded = sufkit::detail::ReadLcpSectionV14(
      input, data.lcp.size(), 1, data.text_symbols, &sa);
  CHECK(decoded.Encoding() == storage.Encoding());
  for (std::size_t row = 0; row < data.lcp.size(); ++row) {
    CHECK(decoded.Exact(row, data.sa[row]) == data.lcp[row]);
  }
  CHECK(WriteLcp(decoded, data.text_symbols) == bytes);
}

void TestLcpRoundTrips() {
  const auto data = MakeAllA(601);
  std::vector<std::uint32_t> raw32(data.lcp.begin(), data.lcp.end());
  CheckLcpRoundTrip(sufkit::detail::LcpStorage::FromRaw32(
                        std::move(raw32), 1,
                        sufkit::ErrorCode::kBuildFailure),
                    data);
  CheckLcpRoundTrip(sufkit::detail::LcpStorage::FromRaw64(
                        data.lcp, 1, sufkit::ErrorCode::kBuildFailure),
                    data);
  CheckLcpRoundTrip(sufkit::detail::LcpStorage::BuildByteCoded(
                        sufkit::detail::IntegerArrayView(data.lcp),
                        sufkit::detail::IntegerArrayView(data.isa), 1, 32,
                        data.text_symbols,
                        sufkit::ErrorCode::kBuildFailure),
                    data);
  CheckLcpRoundTrip(sufkit::detail::LcpStorage::BuildByteCoded(
                        sufkit::detail::IntegerArrayView(data.lcp),
                        sufkit::detail::IntegerArrayView(data.isa), 1, 64,
                        data.text_symbols,
                        sufkit::ErrorCode::kBuildFailure),
                    data);

  constexpr std::uint64_t kLargeTextSymbols =
      (std::uint64_t{1} << 32U) + 1U;
  constexpr std::uint32_t kLargeSamplingRate =
      std::numeric_limits<std::uint32_t>::max();
  auto sparse_raw = sufkit::detail::LcpStorage::FromRaw32(
      {0, 0}, kLargeSamplingRate, sufkit::ErrorCode::kBuildFailure);
  const auto sparse_bytes = WriteLcp(sparse_raw, kLargeTextSymbols);
  std::istringstream sparse_input(sparse_bytes,
                                  std::ios::in | std::ios::binary);
  const auto sparse_decoded = sufkit::detail::ReadLcpSectionV14(
      sparse_input, 2, kLargeSamplingRate, kLargeTextSymbols);
  CHECK(sparse_decoded.Encoding() ==
        sufkit::detail::LcpEncoding::kRaw32);
  CHECK(sparse_decoded.Exact(0, 0) == 0);
  CHECK(sparse_decoded.Exact(1, kLargeSamplingRate) == 0);
}

void TestLcpCorruption() {
  const auto data = MakeAllA(601);
  const auto storage = sufkit::detail::LcpStorage::BuildByteCoded(
      sufkit::detail::IntegerArrayView(data.lcp),
      sufkit::detail::IntegerArrayView(data.isa), 1, 32,
      data.text_symbols, sufkit::ErrorCode::kBuildFailure);
  const auto valid = WriteLcp(storage, data.text_symbols);

  auto unknown = valid;
  PutU32(unknown, 8, 99);
  ExpectError(sufkit::ErrorCode::kVersionMismatch, [&] {
    std::istringstream input(unknown, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(
        input, data.lcp.size(), 1, data.text_symbols);
  });

  auto wrong_primary_size = valid;
  PutU64(wrong_primary_size, 40, data.lcp.size() + 1);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(wrong_primary_size,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(
        input, data.lcp.size(), 1, data.text_symbols);
  });

  auto too_many_anchors = valid;
  const auto invalid_anchor_count = data.lcp.size();
  PutU64(too_many_anchors, 48, invalid_anchor_count);
  PutU64(too_many_anchors, 56,
         invalid_anchor_count * sizeof(std::uint32_t));
  PutU64(too_many_anchors, 64,
         invalid_anchor_count * sizeof(std::uint32_t));
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(too_many_anchors,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(
        input, data.lcp.size(), 1, data.text_symbols);
  });

  auto narrow_coordinate_domain = valid;
  PutU32(narrow_coordinate_domain, 16,
         std::numeric_limits<std::uint32_t>::max());
  PutU64(narrow_coordinate_domain, 24,
         (std::uint64_t{1} << 32U) + 1U);
  PutU64(narrow_coordinate_domain, 32, 2);
  PutU64(narrow_coordinate_domain, 40, 2);
  PutU64(narrow_coordinate_domain, 48, 0);
  PutU64(narrow_coordinate_domain, 56, 0);
  PutU64(narrow_coordinate_domain, 64, 0);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(narrow_coordinate_domain,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpCodecHeader(input);
  });

  auto trailing = valid;
  trailing.push_back('\0');
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(trailing, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(
        input, data.lcp.size(), 1, data.text_symbols);
  });

  auto truncated = valid.substr(0, valid.size() - 1);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(truncated, std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpSectionV14(
        input, data.lcp.size(), 1, data.text_symbols);
  });

  const auto raw64 = WriteLcp(
      sufkit::detail::LcpStorage::FromRaw64(
          {0}, 1, sufkit::ErrorCode::kBuildFailure),
      1);
  auto total_size_overflow = raw64;
  const auto huge_rows = std::numeric_limits<std::uint64_t>::max() / 8U;
  PutU64(total_size_overflow, 24, huge_rows);
  PutU64(total_size_overflow, 32, huge_rows);
  PutU64(total_size_overflow, 40, huge_rows * 8U);
  ExpectError(sufkit::ErrorCode::kCorruptIndex, [&] {
    std::istringstream input(total_size_overflow,
                             std::ios::in | std::ios::binary);
    (void)sufkit::detail::ReadLcpCodecHeader(input);
  });
}

void TestMetadataAllocationAndSentinelBounds() {
  const auto path = std::filesystem::current_path() /
                    "sufkit-sa-codec-metadata-test.bin";
  const auto check = [&](const std::string& bytes,
                         sufkit::detail::ContainerSpec spec) {
    {
      std::ofstream output(path, std::ios::binary | std::ios::trunc);
      output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      CHECK(static_cast<bool>(output));
    }
    sufkit::detail::ParsedContainer container;
    container.path = path;
    container.spec = spec;
    container.sections.push_back(
        {sufkit::detail::SectionType::kMetadata, 1, 0, bytes.size(), 0});
    ExpectError(sufkit::ErrorCode::kCorruptIndex,
                [&] { (void)sufkit::detail::ReadMetadata(container); });
  };

  sufkit::detail::ContainerSpec spec;
  spec.sequence_count = 1;
  spec.total_bases = 1;
  spec.text_symbols = 3;

  std::string oversized_name;
  AppendU32(oversized_name, 1);  // sequence count
  AppendU32(oversized_name, 0);  // sequence id
  AppendU64(oversized_name, 0);  // global offset
  AppendU64(oversized_name, 1);  // length
  AppendU64(oversized_name, 0);  // ambiguous bases
  AppendU32(oversized_name, 1U << 30U);
  AppendU32(oversized_name, 0);  // keeps the section above its fixed minimum
  check(oversized_name, spec);

  std::string sentinel_overflow;
  AppendU32(sentinel_overflow, 1);
  AppendU32(sentinel_overflow, 0);
  AppendU64(sentinel_overflow, 0);
  AppendU64(sentinel_overflow,
            std::numeric_limits<std::uint64_t>::max() - 1U);
  AppendU64(sentinel_overflow, 0);
  AppendU32(sentinel_overflow, 1);
  sentinel_overflow.push_back('x');
  AppendU32(sentinel_overflow, 0);
  spec.total_bases = std::numeric_limits<std::uint64_t>::max() - 1U;
  spec.text_symbols = 0;
  check(sentinel_overflow, spec);

  // A prior contig may consume the last representable coordinate. The next
  // contig must be rejected before subtracting one from a zero remainder.
  std::string intermediate_offset_overflow;
  AppendU32(intermediate_offset_overflow, 2);
  AppendU32(intermediate_offset_overflow, 0);
  AppendU64(intermediate_offset_overflow, 0);
  AppendU64(intermediate_offset_overflow,
            std::numeric_limits<std::uint64_t>::max() - 1U);
  AppendU64(intermediate_offset_overflow, 0);
  AppendU32(intermediate_offset_overflow, 1);
  intermediate_offset_overflow.push_back('a');
  AppendU32(intermediate_offset_overflow, 0);
  AppendU32(intermediate_offset_overflow, 1);
  AppendU64(intermediate_offset_overflow,
            std::numeric_limits<std::uint64_t>::max());
  AppendU64(intermediate_offset_overflow, 1);
  AppendU64(intermediate_offset_overflow, 0);
  AppendU32(intermediate_offset_overflow, 1);
  intermediate_offset_overflow.push_back('b');
  AppendU32(intermediate_offset_overflow, 0);
  spec.sequence_count = 2;
  spec.total_bases = std::numeric_limits<std::uint64_t>::max();
  spec.text_symbols = 2;
  check(intermediate_offset_overflow, spec);

  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

}  // namespace

int main() {
  TestHandEncodedCoordinateSections();
  TestCoordinateRoundTrips();
  TestCoordinateCorruption();
  TestConstructionWidthMetadataBounds();
  TestHandEncodedLcpSections();
  TestLcpRoundTrips();
  TestLcpCorruption();
  TestMetadataAllocationAndSentinelBounds();
  if (failures != 0) {
    std::cerr << failures << " test(s) failed\n";
    return 1;
  }
  std::cout << "sa codec tests passed\n";
  return 0;
}
