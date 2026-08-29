// SPDX-License-Identifier: MIT

#include "sa_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <istream>
#include <limits>
#include <new>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sufkit::detail {
namespace {

constexpr std::uint32_t kCoordinateMagic = 0x31434b53U;  // "SKC1"
constexpr std::uint32_t kLcpMagic = 0x314c4b53U;         // "SKL1"
constexpr std::uint32_t kCodecHeaderVersion = 1;
constexpr std::uint32_t kLcpGuideBlockShift = 12;
constexpr std::uint64_t kCoordinateHeaderBytes = 56;
constexpr std::uint64_t kLcpHeaderBytes = 80;
constexpr std::size_t kIoBlockBytes = 256U * 1024U;

[[noreturn]] void Throw(ErrorCode code, const std::string& message) {
  throw Error(code, "SA section codec: " + message);
}

std::string Label(const char* label) {
  return label == nullptr ? "coordinate" : label;
}

bool IsLittleEndian() noexcept {
  const std::uint16_t value = 1;
  return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

void WriteExact(std::ostream& output, const void* data, std::size_t bytes,
                const char* label) {
  output.write(static_cast<const char*>(data),
               static_cast<std::streamsize>(bytes));
  if (!output) {
    Throw(ErrorCode::kIoError, std::string("cannot write ") + label);
  }
}

void ReadExact(std::istream& input, void* data, std::size_t bytes,
               const char* label) {
  input.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
  if (!input || static_cast<std::size_t>(input.gcount()) != bytes) {
    Throw(ErrorCode::kCorruptIndex,
          std::string("truncated ") + label);
  }
}

void WriteU32Local(std::ostream& output, std::uint32_t value) {
  const std::array<std::uint8_t, 4> bytes = {
      static_cast<std::uint8_t>(value),
      static_cast<std::uint8_t>(value >> 8U),
      static_cast<std::uint8_t>(value >> 16U),
      static_cast<std::uint8_t>(value >> 24U)};
  WriteExact(output, bytes.data(), bytes.size(), "codec header");
}

void WriteU64Local(std::ostream& output, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
  WriteExact(output, bytes.data(), bytes.size(), "codec header");
}

std::uint32_t ReadU32Local(std::istream& input, const char* label) {
  std::array<std::uint8_t, 4> bytes{};
  ReadExact(input, bytes.data(), bytes.size(), label);
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) |
         (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t ReadU64Local(std::istream& input, const char* label) {
  std::array<std::uint8_t, 8> bytes{};
  ReadExact(input, bytes.data(), bytes.size(), label);
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t CheckedMultiply(
    std::uint64_t count, std::uint64_t width, const char* label,
    ErrorCode error_code = ErrorCode::kCorruptIndex) {
  if (width != 0 && count > std::numeric_limits<std::uint64_t>::max() / width) {
    Throw(error_code, std::string(label) + " byte count overflows");
  }
  return count * width;
}

std::uint64_t CheckedAdd(std::uint64_t left, std::uint64_t right,
                         const char* label) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    Throw(ErrorCode::kCorruptIndex,
          std::string(label) + " byte count overflows");
  }
  return left + right;
}

std::size_t CheckedSize(std::uint64_t size, const char* label) {
  if (size > std::numeric_limits<std::size_t>::max()) {
    Throw(ErrorCode::kCorruptIndex,
          std::string(label) + " exceeds addressable memory");
  }
  return static_cast<std::size_t>(size);
}

template <class Values>
void WriteIntegerPlane(std::ostream& output, const Values& values,
                       const char* label) {
  using Integer = typename Values::value_type;
  static_assert(std::is_unsigned_v<Integer>);
  if (values.empty()) {
    return;
  }
  constexpr std::size_t kElementBytes = sizeof(Integer);
  const std::size_t elements_per_block =
      std::max<std::size_t>(1, kIoBlockBytes / kElementBytes);
  if (IsLittleEndian()) {
    for (std::size_t begin = 0; begin < values.size();
         begin += elements_per_block) {
      const auto count = std::min(elements_per_block, values.size() - begin);
      WriteExact(output, values.data() + begin, count * kElementBytes, label);
    }
    return;
  }

  std::vector<std::uint8_t> bytes;
  try {
    bytes.resize(elements_per_block * kElementBytes);
  } catch (const std::bad_alloc&) {
    Throw(ErrorCode::kIoError, "cannot allocate endian-conversion buffer");
  }
  for (std::size_t begin = 0; begin < values.size();
       begin += elements_per_block) {
    const auto count = std::min(elements_per_block, values.size() - begin);
    for (std::size_t index = 0; index < count; ++index) {
      const auto value = values[begin + index];
      for (std::size_t byte = 0; byte < kElementBytes; ++byte) {
        bytes[index * kElementBytes + byte] =
            static_cast<std::uint8_t>(value >> (byte * 8U));
      }
    }
    WriteExact(output, bytes.data(), count * kElementBytes, label);
  }
}

template <class Integer>
std::vector<Integer> ReadIntegerPlane(std::istream& input,
                                      std::uint64_t count,
                                      const char* label) {
  static_assert(std::is_unsigned_v<Integer>);
  std::vector<Integer> values;
  const auto size = CheckedSize(count, label);
  if (size > values.max_size()) {
    Throw(ErrorCode::kCorruptIndex, std::string(label) + " is too large");
  }
  try {
    values.resize(size);
  } catch (const std::bad_alloc&) {
    Throw(ErrorCode::kCorruptIndex,
          std::string("cannot allocate ") + label);
  } catch (const std::length_error&) {
    Throw(ErrorCode::kCorruptIndex, std::string(label) + " is too large");
  }
  if (values.empty()) {
    return values;
  }

  constexpr std::size_t kElementBytes = sizeof(Integer);
  const std::size_t elements_per_block =
      std::max<std::size_t>(1, kIoBlockBytes / kElementBytes);
  if (IsLittleEndian()) {
    for (std::size_t begin = 0; begin < values.size();
         begin += elements_per_block) {
      const auto block_count =
          std::min(elements_per_block, values.size() - begin);
      ReadExact(input, values.data() + begin,
                block_count * kElementBytes, label);
    }
    return values;
  }

  std::vector<std::uint8_t> bytes;
  try {
    bytes.resize(elements_per_block * kElementBytes);
  } catch (const std::bad_alloc&) {
    Throw(ErrorCode::kCorruptIndex,
          "cannot allocate endian-conversion buffer");
  }
  for (std::size_t begin = 0; begin < values.size();
       begin += elements_per_block) {
    const auto block_count =
        std::min(elements_per_block, values.size() - begin);
    ReadExact(input, bytes.data(), block_count * kElementBytes, label);
    for (std::size_t index = 0; index < block_count; ++index) {
      std::uint64_t value = 0;
      for (std::size_t byte = 0; byte < kElementBytes; ++byte) {
        value |= static_cast<std::uint64_t>(
                     bytes[index * kElementBytes + byte])
                 << (byte * 8U);
      }
      values[begin + index] = static_cast<Integer>(value);
    }
  }
  return values;
}

std::vector<std::uint8_t> ReadBytePlane(std::istream& input,
                                        std::uint64_t count,
                                        const char* label) {
  std::vector<std::uint8_t> values;
  const auto size = CheckedSize(count, label);
  if (size > values.max_size()) {
    Throw(ErrorCode::kCorruptIndex, std::string(label) + " is too large");
  }
  try {
    values.resize(size);
  } catch (const std::bad_alloc&) {
    Throw(ErrorCode::kCorruptIndex,
          std::string("cannot allocate ") + label);
  } catch (const std::length_error&) {
    Throw(ErrorCode::kCorruptIndex, std::string(label) + " is too large");
  }
  for (std::size_t begin = 0; begin < values.size(); begin += kIoBlockBytes) {
    const auto bytes = std::min(kIoBlockBytes, values.size() - begin);
    ReadExact(input, values.data() + begin, bytes, label);
  }
  return values;
}

void WriteBytePlane(std::ostream& output,
                    const std::vector<std::uint8_t>& values,
                    const char* label) {
  for (std::size_t begin = 0; begin < values.size(); begin += kIoBlockBytes) {
    const auto bytes = std::min(kIoBlockBytes, values.size() - begin);
    WriteExact(output, values.data() + begin, bytes, label);
  }
}

void RequireEndOfSection(std::istream& input) {
  const auto next = input.peek();
  if (next != std::char_traits<char>::eof()) {
    Throw(ErrorCode::kCorruptIndex, "section contains trailing bytes");
  }
  if (input.bad()) {
    Throw(ErrorCode::kCorruptIndex, "cannot finish reading section");
  }
}

CoordinateStorageWidth WidthForCodec(SaCoordinateCodec codec) {
  switch (codec) {
    case SaCoordinateCodec::kNative32:
      return CoordinateStorageWidth::kBits32;
    case SaCoordinateCodec::kNative64:
      return CoordinateStorageWidth::kBits64;
    case SaCoordinateCodec::kSplit40Low32High8:
      return CoordinateStorageWidth::kBits40;
    case SaCoordinateCodec::kSplit48Low32High16:
      return CoordinateStorageWidth::kBits48;
  }
  Throw(ErrorCode::kVersionMismatch, "unknown coordinate codec");
}

void ValidateCoordinateHeader(const SaCoordinateCodecHeader& header) {
  if (header.symbol_count == 0) {
    Throw(ErrorCode::kCorruptIndex,
          "coordinate header has an empty logical text");
  }
  std::uint64_t expected_low = 0;
  std::uint64_t expected_high = 0;
  switch (header.codec) {
    case SaCoordinateCodec::kNative32:
      expected_low = CheckedMultiply(header.element_count, 4,
                                     "native32 coordinate");
      break;
    case SaCoordinateCodec::kNative64:
      expected_low = CheckedMultiply(header.element_count, 8,
                                     "native64 coordinate");
      break;
    case SaCoordinateCodec::kSplit40Low32High8:
      expected_low = CheckedMultiply(header.element_count, 4,
                                     "split40 low plane");
      expected_high = header.element_count;
      break;
    case SaCoordinateCodec::kSplit48Low32High16:
      expected_low = CheckedMultiply(header.element_count, 4,
                                     "split48 low plane");
      expected_high = CheckedMultiply(header.element_count, 2,
                                      "split48 high plane");
      break;
  }
  if (header.low_plane_bytes != expected_low ||
      header.high_plane_bytes != expected_high) {
    Throw(ErrorCode::kCorruptIndex,
          "coordinate plane lengths disagree with the codec");
  }
  (void)CheckedAdd(
      CheckedAdd(kCoordinateHeaderBytes, header.low_plane_bytes,
                 "coordinate section"),
      header.high_plane_bytes, "coordinate section");
  (void)ResolveCoordinateStorageWidth(header.width, header.symbol_count,
                                      ErrorCode::kCorruptIndex);
}

void ValidateLcpHeader(const SaLcpCodecHeader& header) {
  if (header.sampling_rate == 0) {
    Throw(ErrorCode::kCorruptIndex, "LCP sampling rate is zero");
  }
  if (header.coordinate_width != 32 && header.coordinate_width != 64) {
    Throw(ErrorCode::kCorruptIndex, "invalid LCP coordinate width");
  }
  if (header.text_symbols == 0) {
    Throw(ErrorCode::kCorruptIndex, "LCP header has an empty logical text");
  }
  if (header.codec == SaLcpCodec::kByteCoded &&
      header.coordinate_width == 32 &&
      header.text_symbols - 1U >
          std::numeric_limits<std::uint32_t>::max()) {
    Throw(ErrorCode::kCorruptIndex,
          "LCP logical text exceeds its coordinate width");
  }
  const auto expected_rows =
      1 + (header.text_symbols - 1) / header.sampling_rate;
  if (header.row_count != expected_rows) {
    Throw(ErrorCode::kCorruptIndex,
          "LCP row count disagrees with sampling metadata");
  }
  const auto coordinate_bytes = header.coordinate_width / 8U;
  if (header.codec == SaLcpCodec::kRaw) {
    if (header.primary_bytes !=
            CheckedMultiply(header.row_count, coordinate_bytes,
                            "raw LCP") ||
        header.anchor_count != 0 || header.anchor_position_bytes != 0 ||
        header.anchor_value_bytes != 0) {
      Throw(ErrorCode::kCorruptIndex,
            "raw LCP payload lengths are inconsistent");
    }
    (void)CheckedAdd(kLcpHeaderBytes, header.primary_bytes, "LCP section");
    return;
  }
  if (header.primary_bytes != header.row_count ||
      header.anchor_count >= header.row_count ||
      header.anchor_position_bytes !=
          CheckedMultiply(header.anchor_count, coordinate_bytes,
                          "LCP anchor position") ||
      header.anchor_value_bytes !=
          CheckedMultiply(header.anchor_count, coordinate_bytes,
                          "LCP anchor value")) {
    Throw(ErrorCode::kCorruptIndex,
          "byte-coded LCP payload lengths are inconsistent");
  }
  auto total = CheckedAdd(kLcpHeaderBytes, header.primary_bytes,
                          "LCP section");
  total = CheckedAdd(total, header.anchor_position_bytes, "LCP section");
  (void)CheckedAdd(total, header.anchor_value_bytes, "LCP section");
}

}  // namespace

SaCoordinateCodec CoordinateCodecFor(CoordinateStorageWidth width,
                                     ErrorCode error_code) {
  switch (width) {
    case CoordinateStorageWidth::kBits32:
      return SaCoordinateCodec::kNative32;
    case CoordinateStorageWidth::kBits40:
      return SaCoordinateCodec::kSplit40Low32High8;
    case CoordinateStorageWidth::kBits48:
      return SaCoordinateCodec::kSplit48Low32High16;
    case CoordinateStorageWidth::kBits64:
      return SaCoordinateCodec::kNative64;
    case CoordinateStorageWidth::kAutoSelect:
      break;
  }
  Throw(error_code, "coordinate storage width is not resolved");
}

SaCoordinateCodecHeader ReadCoordinateCodecHeader(std::istream& input) {
  if (ReadU32Local(input, "coordinate codec magic") != kCoordinateMagic) {
    Throw(ErrorCode::kCorruptIndex, "wrong coordinate codec magic");
  }
  if (ReadU32Local(input, "coordinate codec version") !=
      kCodecHeaderVersion) {
    Throw(ErrorCode::kVersionMismatch,
          "unsupported coordinate codec header version");
  }
  const auto raw_codec = ReadU32Local(input, "coordinate codec identifier");
  if (raw_codec >
      static_cast<std::uint32_t>(
          SaCoordinateCodec::kSplit48Low32High16)) {
    Throw(ErrorCode::kVersionMismatch, "unknown coordinate codec identifier");
  }
  if (ReadU32Local(input, "coordinate codec reserved field") != 0) {
    Throw(ErrorCode::kCorruptIndex,
          "coordinate codec reserved field is non-zero");
  }
  SaCoordinateCodecHeader header;
  header.codec = static_cast<SaCoordinateCodec>(raw_codec);
  header.width = WidthForCodec(header.codec);
  header.symbol_count = ReadU64Local(input, "coordinate symbol count");
  header.element_count = ReadU64Local(input, "coordinate element count");
  header.low_plane_bytes = ReadU64Local(input, "coordinate low-plane size");
  header.high_plane_bytes =
      ReadU64Local(input, "coordinate high-plane size");
  if (ReadU64Local(input, "coordinate codec reserved field") != 0) {
    Throw(ErrorCode::kCorruptIndex,
          "coordinate codec reserved field is non-zero");
  }
  ValidateCoordinateHeader(header);
  return header;
}

SaLcpCodecHeader ReadLcpCodecHeader(std::istream& input) {
  if (ReadU32Local(input, "LCP codec magic") != kLcpMagic) {
    Throw(ErrorCode::kCorruptIndex, "wrong LCP codec magic");
  }
  if (ReadU32Local(input, "LCP codec version") != kCodecHeaderVersion) {
    Throw(ErrorCode::kVersionMismatch,
          "unsupported LCP codec header version");
  }
  const auto raw_codec = ReadU32Local(input, "LCP codec identifier");
  if (raw_codec > static_cast<std::uint32_t>(SaLcpCodec::kByteCoded)) {
    Throw(ErrorCode::kVersionMismatch, "unknown LCP codec identifier");
  }
  SaLcpCodecHeader header;
  header.codec = static_cast<SaLcpCodec>(raw_codec);
  const auto width = ReadU32Local(input, "LCP coordinate width");
  if (width > std::numeric_limits<std::uint8_t>::max()) {
    Throw(ErrorCode::kCorruptIndex, "invalid LCP coordinate width");
  }
  header.coordinate_width = static_cast<std::uint8_t>(width);
  header.sampling_rate = ReadU32Local(input, "LCP sampling rate");
  if (ReadU32Local(input, "LCP guide block shift") !=
      kLcpGuideBlockShift) {
    Throw(ErrorCode::kCorruptIndex, "invalid LCP guide block shift");
  }
  header.text_symbols = ReadU64Local(input, "LCP text-symbol count");
  header.row_count = ReadU64Local(input, "LCP row count");
  header.primary_bytes = ReadU64Local(input, "LCP primary-plane size");
  header.anchor_count = ReadU64Local(input, "LCP anchor count");
  header.anchor_position_bytes =
      ReadU64Local(input, "LCP anchor-position size");
  header.anchor_value_bytes =
      ReadU64Local(input, "LCP anchor-value size");
  if (ReadU64Local(input, "LCP codec reserved field") != 0) {
    Throw(ErrorCode::kCorruptIndex,
          "LCP codec reserved field is non-zero");
  }
  ValidateLcpHeader(header);
  return header;
}

void WriteCoordinateSectionV14(std::ostream& output,
                               const CoordinateStorage& storage,
                               std::uint64_t symbol_count) {
  (void)ResolveCoordinateStorageWidth(storage.Width(), symbol_count,
                                      ErrorCode::kBuildFailure);
  storage.Visit([&](const auto& values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (static_cast<std::uint64_t>(values[index]) >= symbol_count) {
        Throw(ErrorCode::kBuildFailure,
              "coordinate value is outside the logical text");
      }
    }
  });
  const auto codec = CoordinateCodecFor(storage.Width(),
                                        ErrorCode::kBuildFailure);
  const auto count = storage.Size();
  const auto bytes = CoordinateStorageByteCount(
      count, storage.Width(), ErrorCode::kBuildFailure);
  const auto high_bytes =
      storage.Width() == CoordinateStorageWidth::kBits40
          ? count
          : storage.Width() == CoordinateStorageWidth::kBits48
                ? CheckedMultiply(count, 2, "split48 high plane",
                                  ErrorCode::kBuildFailure)
                : 0;
  const auto low_bytes = bytes - high_bytes;
  WriteU32Local(output, kCoordinateMagic);
  WriteU32Local(output, kCodecHeaderVersion);
  WriteU32Local(output, static_cast<std::uint32_t>(codec));
  WriteU32Local(output, 0);
  WriteU64Local(output, symbol_count);
  WriteU64Local(output, count);
  WriteU64Local(output, low_bytes);
  WriteU64Local(output, high_bytes);
  WriteU64Local(output, 0);

  storage.Visit([&](const auto& values) {
    using Values = std::decay_t<decltype(values)>;
    if constexpr (std::is_same_v<Values, Coordinate40Storage> ||
                  std::is_same_v<Values, Coordinate48Storage>) {
      WriteIntegerPlane(output, values.low, "coordinate low plane");
      WriteIntegerPlane(output, values.high, "coordinate high plane");
    } else {
      WriteIntegerPlane(output, values, "coordinate values");
    }
  });
}

CoordinateStorage ReadCoordinateSectionV14(
    std::istream& input, std::uint64_t expected_element_count,
    std::uint64_t expected_symbol_count, const char* label) {
  const auto header = ReadCoordinateCodecHeader(input);
  if (header.element_count != expected_element_count ||
      header.symbol_count != expected_symbol_count) {
    Throw(ErrorCode::kCorruptIndex,
          Label(label) + " metadata disagrees with the container");
  }

  CoordinateStorage result;
  switch (header.codec) {
    case SaCoordinateCodec::kNative32: {
      auto values = ReadIntegerPlane<std::uint32_t>(
          input, header.element_count, "native32 coordinate values");
      result = CoordinateStorage::FromUInt32(
          std::move(values), CoordinateStorageWidth::kBits32,
          header.symbol_count, ErrorCode::kCorruptIndex, label);
      break;
    }
    case SaCoordinateCodec::kNative64: {
      auto values = ReadIntegerPlane<std::uint64_t>(
          input, header.element_count, "native64 coordinate values");
      result = CoordinateStorage::FromUInt64(
          std::move(values), CoordinateStorageWidth::kBits64,
          header.symbol_count, ErrorCode::kCorruptIndex, label);
      break;
    }
    case SaCoordinateCodec::kSplit40Low32High8: {
      Coordinate40Storage values;
      values.low = ReadIntegerPlane<std::uint32_t>(
          input, header.element_count, "split40 low plane");
      values.high = ReadIntegerPlane<std::uint8_t>(
          input, header.element_count, "split40 high plane");
      result = CoordinateStorage::FromPacked40(
          std::move(values), header.symbol_count,
          ErrorCode::kCorruptIndex, label);
      break;
    }
    case SaCoordinateCodec::kSplit48Low32High16: {
      Coordinate48Storage values;
      values.low = ReadIntegerPlane<std::uint32_t>(
          input, header.element_count, "split48 low plane");
      values.high = ReadIntegerPlane<std::uint16_t>(
          input, header.element_count, "split48 high plane");
      result = CoordinateStorage::FromPacked48(
          std::move(values), header.symbol_count,
          ErrorCode::kCorruptIndex, label);
      break;
    }
  }
  RequireEndOfSection(input);
  return result;
}

void WriteLcpSectionV14(std::ostream& output, const LcpStorage& storage,
                        std::uint64_t text_symbols) {
  const auto encoding = storage.Encoding();
  const bool byte_coded =
      encoding == LcpEncoding::kByteCoded32 ||
      encoding == LcpEncoding::kByteCoded64;
  const bool raw = encoding == LcpEncoding::kRaw32 ||
                   encoding == LcpEncoding::kRaw64;
  if (!raw && !byte_coded) {
    Throw(ErrorCode::kBuildFailure, "cannot serialize an empty LCP");
  }
  if (text_symbols == 0 || storage.Size() !=
                               1 + (text_symbols - 1) /
                                       storage.SamplingRate()) {
    Throw(ErrorCode::kBuildFailure,
          "LCP metadata is inconsistent before serialization");
  }
  const auto codec =
      byte_coded ? SaLcpCodec::kByteCoded : SaLcpCodec::kRaw;
  const auto coordinate_bytes = storage.CoordinateWidth() / 8U;
  const auto primary_bytes =
      byte_coded
          ? storage.Size()
          : CheckedMultiply(storage.Size(), coordinate_bytes, "raw LCP",
                            ErrorCode::kBuildFailure);
  const auto anchor_count = byte_coded ? storage.AnchorCount() : 0;
  const auto anchor_bytes = CheckedMultiply(
      anchor_count, coordinate_bytes, "LCP overflow anchors",
      ErrorCode::kBuildFailure);

  WriteU32Local(output, kLcpMagic);
  WriteU32Local(output, kCodecHeaderVersion);
  WriteU32Local(output, static_cast<std::uint32_t>(codec));
  WriteU32Local(output, storage.CoordinateWidth());
  WriteU32Local(output, storage.SamplingRate());
  WriteU32Local(output, kLcpGuideBlockShift);
  WriteU64Local(output, text_symbols);
  WriteU64Local(output, storage.Size());
  WriteU64Local(output, primary_bytes);
  WriteU64Local(output, anchor_count);
  WriteU64Local(output, anchor_bytes);
  WriteU64Local(output, anchor_bytes);
  WriteU64Local(output, 0);

  if (const auto* raw32 = storage.Raw32Values()) {
    WriteIntegerPlane(output, *raw32, "raw32 LCP values");
  } else if (const auto* raw64 = storage.Raw64Values()) {
    WriteIntegerPlane(output, *raw64, "raw64 LCP values");
  } else {
    WriteBytePlane(output, *storage.BytePrimary(), "byte-coded LCP primary");
    if (storage.CoordinateWidth() == 32) {
      WriteIntegerPlane(output, *storage.AnchorPositions32(),
                        "LCP anchor positions");
      WriteIntegerPlane(output, *storage.AnchorValues32(),
                        "LCP anchor values");
    } else {
      WriteIntegerPlane(output, *storage.AnchorPositions64(),
                        "LCP anchor positions");
      WriteIntegerPlane(output, *storage.AnchorValues64(),
                        "LCP anchor values");
    }
  }
}

LcpStorage ReadLcpSectionV14(
    std::istream& input, std::uint64_t expected_row_count,
    std::uint32_t expected_sampling_rate,
    std::uint64_t expected_text_symbols,
    const IntegerArrayView* suffix_array) {
  const auto header = ReadLcpCodecHeader(input);
  if (header.row_count != expected_row_count ||
      header.sampling_rate != expected_sampling_rate ||
      header.text_symbols != expected_text_symbols) {
    Throw(ErrorCode::kCorruptIndex,
          "LCP metadata disagrees with the container");
  }

  LcpStorage storage;
  if (header.codec == SaLcpCodec::kRaw) {
    if (header.coordinate_width == 32) {
      storage = LcpStorage::FromRaw32(
          ReadIntegerPlane<std::uint32_t>(input, header.row_count,
                                          "raw32 LCP values"),
          header.sampling_rate, ErrorCode::kCorruptIndex);
    } else {
      storage = LcpStorage::FromRaw64(
          ReadIntegerPlane<std::uint64_t>(input, header.row_count,
                                          "raw64 LCP values"),
          header.sampling_rate, ErrorCode::kCorruptIndex);
    }
  } else {
    auto primary = ReadBytePlane(input, header.row_count,
                                 "byte-coded LCP primary");
    if (header.coordinate_width == 32) {
      auto positions = ReadIntegerPlane<std::uint32_t>(
          input, header.anchor_count, "LCP anchor positions");
      auto values = ReadIntegerPlane<std::uint32_t>(
          input, header.anchor_count, "LCP anchor values");
      storage = LcpStorage::FromByteCoded32(
          std::move(primary), std::move(positions), std::move(values),
          header.sampling_rate, header.text_symbols,
          ErrorCode::kCorruptIndex);
    } else {
      auto positions = ReadIntegerPlane<std::uint64_t>(
          input, header.anchor_count, "LCP anchor positions");
      auto values = ReadIntegerPlane<std::uint64_t>(
          input, header.anchor_count, "LCP anchor values");
      storage = LcpStorage::FromByteCoded64(
          std::move(primary), std::move(positions), std::move(values),
          header.sampling_rate, header.text_symbols,
          ErrorCode::kCorruptIndex);
    }
  }
  RequireEndOfSection(input);
  if (suffix_array != nullptr) {
    storage.Validate(*suffix_array, expected_text_symbols,
                     ErrorCode::kCorruptIndex);
  }
  return storage;
}

}  // namespace sufkit::detail
