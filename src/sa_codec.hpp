// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <iosfwd>

#include "coordinate_storage.hpp"
#include "lcp_storage.hpp"

namespace sufkit::detail {

// Version 1.4 section codecs. These identifiers are persisted and therefore
// must never be renumbered.
enum class SaCoordinateCodec : std::uint32_t {
  kNative32 = 0,
  kNative64 = 1,
  kSplit40Low32High8 = 2,
  kSplit48Low32High16 = 3,
};

enum class SaLcpCodec : std::uint32_t {
  kRaw = 0,
  kByteCoded = 1,
};

struct SaCoordinateCodecHeader {
  SaCoordinateCodec codec = SaCoordinateCodec::kNative32;
  CoordinateStorageWidth width = CoordinateStorageWidth::kBits32;
  std::uint64_t symbol_count = 0;
  std::uint64_t element_count = 0;
  std::uint64_t low_plane_bytes = 0;
  std::uint64_t high_plane_bytes = 0;
};

struct SaLcpCodecHeader {
  SaLcpCodec codec = SaLcpCodec::kRaw;
  std::uint8_t coordinate_width = 0;
  std::uint32_t sampling_rate = 0;
  std::uint64_t text_symbols = 0;
  std::uint64_t row_count = 0;
  std::uint64_t primary_bytes = 0;
  std::uint64_t anchor_count = 0;
  std::uint64_t anchor_position_bytes = 0;
  std::uint64_t anchor_value_bytes = 0;
};

SaCoordinateCodec CoordinateCodecFor(
    CoordinateStorageWidth width, ErrorCode error_code);

// Headers are exposed so inspect can report storage without decoding an
// entire section. A successful call leaves the stream at the first payload
// byte. Unknown codec versions/identifiers are version mismatches; malformed
// lengths and metadata are corrupt indexes.
SaCoordinateCodecHeader ReadCoordinateCodecHeader(std::istream& input);
SaLcpCodecHeader ReadLcpCodecHeader(std::istream& input);

void WriteCoordinateSectionV14(std::ostream& output,
                               const CoordinateStorage& storage,
                               std::uint64_t symbol_count);
CoordinateStorage ReadCoordinateSectionV14(
    std::istream& input, std::uint64_t expected_element_count,
    std::uint64_t expected_symbol_count, const char* label);

void WriteLcpSectionV14(std::ostream& output, const LcpStorage& storage,
                        std::uint64_t text_symbols);
LcpStorage ReadLcpSectionV14(
    std::istream& input, std::uint64_t expected_row_count,
    std::uint32_t expected_sampling_rate,
    std::uint64_t expected_text_symbols,
    const IntegerArrayView* suffix_array = nullptr);

}  // namespace sufkit::detail
