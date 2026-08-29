// SPDX-License-Identifier: MIT

#include "lcp_storage.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <type_traits>

#include "sequence_compare.hpp"

namespace sufkit::detail {
namespace {

[[noreturn]] void ThrowLcp(ErrorCode code, const std::string& message) {
  throw Error(code, "LCP storage: " + message);
}

std::size_t CheckedSize(std::uint64_t value, ErrorCode code,
                        const char* label) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    ThrowLcp(code, std::string(label) + " exceeds addressable memory");
  }
  return static_cast<std::size_t>(value);
}

std::uint64_t CheckedBytes(std::uint64_t count, std::uint64_t width) noexcept {
  if (width != 0 && count > std::numeric_limits<std::uint64_t>::max() / width) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return count * width;
}

std::uint64_t SaturatingAdd(std::uint64_t left,
                            std::uint64_t right) noexcept {
  return left > std::numeric_limits<std::uint64_t>::max() - right
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

template <class Coordinate>
std::uint64_t CoordinateMaximum() noexcept {
  return static_cast<std::uint64_t>(
      std::numeric_limits<Coordinate>::max());
}

template <class Coordinate>
bool TextFitsCoordinate(std::uint64_t text_symbols) noexcept {
  return text_symbols == 0 ||
         text_symbols - 1 <= CoordinateMaximum<Coordinate>();
}

template <class Coordinate>
void BuildGuide(ByteCodedLcpData<Coordinate>& data,
                std::uint64_t text_symbols, ErrorCode error_code) {
  data.guide.clear();
  if (text_symbols == 0) {
    return;
  }
  const std::uint64_t block_count =
      1 + (text_symbols - 1) / LcpStorage::kGuideBlockSize;
  if (block_count == std::numeric_limits<std::uint64_t>::max()) {
    ThrowLcp(error_code, "guide size overflows");
  }
  const auto guide_size = CheckedSize(block_count + 1, error_code,
                                      "guide entry count");
  if (guide_size > data.guide.max_size()) {
    ThrowLcp(error_code, "guide is too large");
  }
  if (data.anchor_positions.size() > CoordinateMaximum<Coordinate>()) {
    ThrowLcp(error_code, "guide index exceeds its coordinate width");
  }
  try {
    data.guide.resize(guide_size);
  } catch (const std::bad_alloc&) {
    ThrowLcp(error_code, "cannot allocate guide");
  }

  std::size_t anchor = 0;
  for (std::uint64_t block = 0; block <= block_count; ++block) {
    const auto boundary =
        block > std::numeric_limits<std::uint64_t>::max() /
                    LcpStorage::kGuideBlockSize
            ? std::numeric_limits<std::uint64_t>::max()
            : block * LcpStorage::kGuideBlockSize;
    while (anchor < data.anchor_positions.size() &&
           static_cast<std::uint64_t>(data.anchor_positions[anchor]) <
               boundary) {
      ++anchor;
    }
    data.guide[static_cast<std::size_t>(block)] =
        static_cast<Coordinate>(anchor);
  }
}

template <class Coordinate>
void ValidateByteStructure(const ByteCodedLcpData<Coordinate>& data,
                           std::uint32_t sampling_rate,
                           std::uint64_t text_symbols, ErrorCode error_code) {
  if (sampling_rate == 0) {
    ThrowLcp(error_code, "sampling rate must be positive");
  }
  if (data.anchor_positions.size() != data.anchor_values.size()) {
    ThrowLcp(error_code, "overflow anchor planes have different lengths");
  }
  const auto maximum_anchor_count =
      data.primary.empty() ? 0 : data.primary.size() - 1;
  if (data.anchor_positions.size() > maximum_anchor_count) {
    ThrowLcp(error_code, "too many overflow anchors for the LCP rows");
  }
  if (!TextFitsCoordinate<Coordinate>(text_symbols)) {
    ThrowLcp(error_code, "text does not fit the overflow coordinate width");
  }
  const auto expected_rows =
      text_symbols == 0 ? 0 : 1 + (text_symbols - 1) / sampling_rate;
  if (data.primary.size() != expected_rows) {
    ThrowLcp(error_code, "LCP row count disagrees with sampling metadata");
  }
  if (!data.primary.empty() && data.primary.front() != 0) {
    ThrowLcp(error_code, "LCP[0] must be zero");
  }
  for (std::size_t index = 0; index < data.anchor_positions.size(); ++index) {
    const auto position =
        static_cast<std::uint64_t>(data.anchor_positions[index]);
    const auto value = static_cast<std::uint64_t>(data.anchor_values[index]);
    if (position >= text_symbols) {
      ThrowLcp(error_code, "overflow anchor position is out of range");
    }
    if (position % sampling_rate != 0) {
      ThrowLcp(error_code, "overflow anchor is not sampling-rate aligned");
    }
    if (value < LcpStorage::kByteLimit) {
      ThrowLcp(error_code, "overflow anchor value is smaller than 255");
    }
    if (index == 0) {
      continue;
    }
    const auto previous_position =
        static_cast<std::uint64_t>(data.anchor_positions[index - 1]);
    const auto previous_value =
        static_cast<std::uint64_t>(data.anchor_values[index - 1]);
    if (position <= previous_position) {
      ThrowLcp(error_code, "overflow anchor positions are not increasing");
    }
    const auto delta = position - previous_position;
    if (delta == sampling_rate && previous_value >= delta &&
        previous_value - delta == value) {
      ThrowLcp(error_code, "overflow anchor redundantly continues a run");
    }
  }
}

template <class Coordinate>
std::size_t FindAnchor(const ByteCodedLcpData<Coordinate>& data,
                       std::uint64_t suffix_position, ErrorCode error_code) {
  if (data.anchor_positions.empty()) {
    ThrowLcp(error_code, "byte overflow has no anchor");
  }
  const auto block = suffix_position / LcpStorage::kGuideBlockSize;
  if (block + 1 >= data.guide.size()) {
    ThrowLcp(error_code, "suffix position exceeds the overflow guide");
  }
  std::size_t begin = static_cast<std::size_t>(
      data.guide[static_cast<std::size_t>(block)]);
  const std::size_t end = static_cast<std::size_t>(
      data.guide[static_cast<std::size_t>(block + 1)]);
  const auto first = data.anchor_positions.begin() +
                     static_cast<std::ptrdiff_t>(begin);
  const auto last = data.anchor_positions.begin() +
                    static_cast<std::ptrdiff_t>(end);
  const auto found = std::upper_bound(
      first, last, suffix_position,
      [](std::uint64_t position, Coordinate anchor) {
        return position < static_cast<std::uint64_t>(anchor);
      });
  if (found != first) {
    return static_cast<std::size_t>(found - data.anchor_positions.begin() - 1);
  }
  if (begin == 0) {
    ThrowLcp(error_code, "byte overflow precedes its first anchor");
  }
  return begin - 1;
}

template <class Coordinate>
std::uint64_t DecodeByte(const ByteCodedLcpData<Coordinate>& data,
                         std::uint64_t row,
                         std::uint64_t suffix_position,
                         std::uint32_t sampling_rate, ErrorCode error_code) {
  if (row >= data.primary.size()) {
    ThrowLcp(error_code, "row is out of range");
  }
  const auto primary = data.primary[static_cast<std::size_t>(row)];
  if (primary < LcpStorage::kByteLimit) {
    return primary;
  }
  if (suffix_position % sampling_rate != 0) {
    ThrowLcp(error_code, "suffix position is not sampling-rate aligned");
  }
  const auto anchor = FindAnchor(data, suffix_position, error_code);
  const auto anchor_position =
      static_cast<std::uint64_t>(data.anchor_positions[anchor]);
  const auto anchor_value =
      static_cast<std::uint64_t>(data.anchor_values[anchor]);
  if (anchor_position > suffix_position) {
    ThrowLcp(error_code, "overflow anchor follows its suffix position");
  }
  const auto delta = suffix_position - anchor_position;
  if (delta % sampling_rate != 0 || anchor_value < delta ||
      anchor_value - delta < LcpStorage::kByteLimit) {
    ThrowLcp(error_code, "overflow anchor does not cover the suffix position");
  }
  return anchor_value - delta;
}

template <class Coordinate>
ByteCodedLcpData<Coordinate> BuildByteData(
    const IntegerArrayView& raw_lcp, const IntegerArrayView& isa,
    std::uint32_t sampling_rate, std::uint64_t text_symbols,
    ErrorCode error_code) {
  ByteCodedLcpData<Coordinate> data;
  if (raw_lcp.Size() != isa.Size()) {
    ThrowLcp(error_code, "LCP and ISA lengths differ");
  }
  if (sampling_rate == 0) {
    ThrowLcp(error_code, "sampling rate must be positive");
  }
  if (raw_lcp.Size() != 0 && text_symbols == 0) {
    ThrowLcp(error_code, "non-empty LCP has an empty logical text");
  }
  const auto expected_rows =
      text_symbols == 0 ? 0 : 1 + (text_symbols - 1) / sampling_rate;
  if (raw_lcp.Size() != expected_rows) {
    ThrowLcp(error_code, "LCP row count disagrees with sampling metadata");
  }
  if (!TextFitsCoordinate<Coordinate>(text_symbols)) {
    ThrowLcp(error_code, "text does not fit the overflow coordinate width");
  }
  std::vector<bool> assigned;
  try {
    data.primary.resize(raw_lcp.Size());
    data.anchor_positions.reserve(raw_lcp.Size() / 256 + 1);
    data.anchor_values.reserve(raw_lcp.Size() / 256 + 1);
    assigned.resize(raw_lcp.Size());
  } catch (const std::bad_alloc&) {
    ThrowLcp(error_code, "cannot allocate byte-coded LCP");
  }

  bool previous_was_overflow = false;
  std::uint64_t previous_position = 0;
  std::uint64_t previous_value = 0;
  for (std::uint64_t sample = 0; sample < raw_lcp.Size(); ++sample) {
    if (sample > std::numeric_limits<std::uint64_t>::max() / sampling_rate) {
      ThrowLcp(error_code, "sample position overflows");
    }
    const auto position = sample * sampling_rate;
    if (position >= text_symbols) {
      ThrowLcp(error_code, "sample position is outside the logical text");
    }
    const auto row = isa[static_cast<std::size_t>(sample)];
    if (row >= raw_lcp.Size()) {
      ThrowLcp(error_code, "ISA row is out of range");
    }
    if (assigned[static_cast<std::size_t>(row)]) {
      ThrowLcp(error_code, "ISA is not a permutation");
    }
    assigned[static_cast<std::size_t>(row)] = true;
    const auto value = raw_lcp[static_cast<std::size_t>(row)];
    if (value > CoordinateMaximum<Coordinate>()) {
      ThrowLcp(error_code, "LCP value exceeds the overflow coordinate width");
    }
    data.primary[static_cast<std::size_t>(row)] = static_cast<std::uint8_t>(
        std::min<std::uint64_t>(value, LcpStorage::kByteLimit));
    if (value < LcpStorage::kByteLimit) {
      previous_was_overflow = false;
      continue;
    }
    const bool continues =
        previous_was_overflow &&
        position - previous_position == sampling_rate &&
        previous_value >= sampling_rate &&
        previous_value - sampling_rate == value;
    if (!continues) {
      try {
        data.anchor_positions.push_back(static_cast<Coordinate>(position));
        data.anchor_values.push_back(static_cast<Coordinate>(value));
      } catch (const std::bad_alloc&) {
        ThrowLcp(error_code, "cannot allocate overflow anchors");
      }
    }
    previous_was_overflow = true;
    previous_position = position;
    previous_value = value;
  }
  if (!data.primary.empty() && data.primary.front() != 0) {
    ThrowLcp(error_code, "LCP[0] must be zero");
  }
  ValidateByteStructure(data, sampling_rate, text_symbols, error_code);
  BuildGuide(data, text_symbols, error_code);
  return data;
}

template <class Coordinate>
ByteCodedLcpData<Coordinate> BuildByteDataDirect(
    const std::uint8_t* text, std::size_t text_size,
    const IntegerArrayView& suffix_array, const IntegerArrayView& isa,
    std::uint32_t sampling_rate, ErrorCode error_code) {
  ByteCodedLcpData<Coordinate> data;
  if (sampling_rate == 0) {
    ThrowLcp(error_code, "sampling rate must be positive");
  }
  if (text_size != 0 && text == nullptr) {
    ThrowLcp(error_code, "non-empty text has a null data pointer");
  }
  const auto text_symbols = static_cast<std::uint64_t>(text_size);
  const auto expected_rows =
      text_symbols == 0 ? 0 : 1 + (text_symbols - 1) / sampling_rate;
  if (suffix_array.Size() != expected_rows || isa.Size() != expected_rows) {
    ThrowLcp(error_code,
             "suffix-array or ISA length disagrees with sampling metadata");
  }
  if (!TextFitsCoordinate<Coordinate>(text_symbols)) {
    ThrowLcp(error_code, "text does not fit the overflow coordinate width");
  }
  if (expected_rows > data.primary.max_size()) {
    ThrowLcp(error_code, "byte-coded LCP is too large");
  }
  try {
    data.primary.resize(static_cast<std::size_t>(expected_rows));
    data.anchor_positions.reserve(
        static_cast<std::size_t>(expected_rows / 256 + 1));
    data.anchor_values.reserve(
        static_cast<std::size_t>(expected_rows / 256 + 1));
  } catch (const std::bad_alloc&) {
    ThrowLcp(error_code, "cannot allocate byte-coded LCP");
  }

  std::uint64_t common = 0;
  bool previous_was_overflow = false;
  std::uint64_t previous_position = 0;
  std::uint64_t previous_value = 0;
  for (std::uint64_t sample = 0; sample < expected_rows; ++sample) {
    if (sample > std::numeric_limits<std::uint64_t>::max() /
                     sampling_rate) {
      ThrowLcp(error_code, "sample position overflows");
    }
    const auto suffix = sample * sampling_rate;
    if (suffix >= text_symbols) {
      ThrowLcp(error_code, "sample position is outside the logical text");
    }
    const auto row = isa[static_cast<std::size_t>(sample)];
    if (row >= expected_rows) {
      ThrowLcp(error_code, "ISA row is out of range");
    }
    if (suffix_array[static_cast<std::size_t>(row)] != suffix) {
      ThrowLcp(error_code, "ISA is not the inverse of the suffix array");
    }

    std::uint64_t value = 0;
    if (row == 0) {
      // A lexicographically first suffix has no predecessor. For a valid
      // sampled suffix array the generalized-Kasai carry is already zero;
      // clearing it also prevents malformed inputs from propagating a hint.
      common = 0;
    } else {
      const auto previous_suffix =
          suffix_array[static_cast<std::size_t>(row - 1)];
      if (previous_suffix >= text_symbols ||
          previous_suffix % sampling_rate != 0) {
        ThrowLcp(error_code, "suffix-array position is invalid");
      }
      if (common > text_symbols - suffix ||
          common > text_symbols - previous_suffix) {
        ThrowLcp(error_code, "generalized-Kasai carry exceeds suffix bounds");
      }
      const auto remaining = static_cast<std::size_t>(std::min(
          text_symbols - suffix - common,
          text_symbols - previous_suffix - common));
      common += LongestCommonPrefixBytes(
          text + static_cast<std::size_t>(suffix + common),
          text + static_cast<std::size_t>(previous_suffix + common),
          remaining);
      value = common;
    }

    if (value > CoordinateMaximum<Coordinate>()) {
      ThrowLcp(error_code, "LCP value exceeds the overflow coordinate width");
    }
    data.primary[static_cast<std::size_t>(row)] = static_cast<std::uint8_t>(
        std::min<std::uint64_t>(value, LcpStorage::kByteLimit));
    if (value < LcpStorage::kByteLimit) {
      previous_was_overflow = false;
    } else {
      const bool continues =
          previous_was_overflow &&
          suffix - previous_position == sampling_rate &&
          previous_value >= sampling_rate &&
          previous_value - sampling_rate == value;
      if (!continues) {
        try {
          data.anchor_positions.push_back(
              static_cast<Coordinate>(suffix));
          data.anchor_values.push_back(static_cast<Coordinate>(value));
        } catch (const std::bad_alloc&) {
          ThrowLcp(error_code, "cannot allocate overflow anchors");
        }
      }
      previous_was_overflow = true;
      previous_position = suffix;
      previous_value = value;
    }

    common = common > sampling_rate ? common - sampling_rate : 0;
  }

  ValidateByteStructure(data, sampling_rate, text_symbols, error_code);
  BuildGuide(data, text_symbols, error_code);
  return data;
}

template <class Coordinate>
std::uint64_t ByteResidentBytes(
    const ByteCodedLcpData<Coordinate>& data) noexcept {
  auto bytes = static_cast<std::uint64_t>(data.primary.size());
  bytes = SaturatingAdd(bytes, CheckedBytes(data.anchor_positions.size(),
                                            sizeof(Coordinate)));
  bytes = SaturatingAdd(bytes, CheckedBytes(data.anchor_values.size(),
                                            sizeof(Coordinate)));
  return SaturatingAdd(
      bytes, CheckedBytes(data.guide.size(), sizeof(Coordinate)));
}

template <class Coordinate>
std::uint64_t ByteSerializedBytes(
    const ByteCodedLcpData<Coordinate>& data) noexcept {
  auto bytes = static_cast<std::uint64_t>(data.primary.size());
  bytes = SaturatingAdd(bytes, CheckedBytes(data.anchor_positions.size(),
                                            sizeof(Coordinate)));
  return SaturatingAdd(bytes, CheckedBytes(data.anchor_values.size(),
                                           sizeof(Coordinate)));
}

}  // namespace

IntegerArrayView::IntegerArrayView(const std::uint32_t* data,
                                   std::size_t size) noexcept
    : data_(data), size_(size), kind_(Kind::kUnsigned32) {}

IntegerArrayView::IntegerArrayView(const std::uint64_t* data,
                                   std::size_t size) noexcept
    : data_(data), size_(size), kind_(Kind::kUnsigned64) {}

IntegerArrayView::IntegerArrayView(const std::int32_t* data,
                                   std::size_t size) noexcept
    : data_(data), size_(size), kind_(Kind::kSigned32) {}

IntegerArrayView::IntegerArrayView(const std::int64_t* data,
                                   std::size_t size) noexcept
    : data_(data), size_(size), kind_(Kind::kSigned64) {}

IntegerArrayView::IntegerArrayView(
    const Coordinate40Storage& values) noexcept
    : data_(values.low.data()),
      high_(values.high.data()),
      size_(values.low.size()),
      kind_(Kind::kSplit40) {}

IntegerArrayView::IntegerArrayView(
    const Coordinate48Storage& values) noexcept
    : data_(values.low.data()),
      high_(values.high.data()),
      size_(values.low.size()),
      kind_(Kind::kSplit48) {}

std::uint64_t IntegerArrayView::operator[](std::size_t index) const noexcept {
  switch (kind_) {
    case Kind::kUnsigned32:
      return static_cast<const std::uint32_t*>(data_)[index];
    case Kind::kUnsigned64:
      return static_cast<const std::uint64_t*>(data_)[index];
    case Kind::kSigned32:
      return static_cast<std::uint64_t>(
          static_cast<const std::int32_t*>(data_)[index]);
    case Kind::kSigned64:
      return static_cast<std::uint64_t>(
          static_cast<const std::int64_t*>(data_)[index]);
    case Kind::kSplit40:
      return static_cast<const std::uint32_t*>(data_)[index] |
             (static_cast<std::uint64_t>(
                  static_cast<const std::uint8_t*>(high_)[index])
              << 32U);
    case Kind::kSplit48:
      return static_cast<const std::uint32_t*>(data_)[index] |
             (static_cast<std::uint64_t>(
                  static_cast<const std::uint16_t*>(high_)[index])
              << 32U);
  }
  return 0;
}

LcpStorage::LcpStorage(Storage storage, std::uint32_t sampling_rate,
                       std::uint64_t text_symbols) noexcept
    : storage_(std::move(storage)),
      sampling_rate_(sampling_rate),
      text_symbols_(text_symbols) {}

LcpStorage LcpStorage::FromRaw32(Raw32 values, std::uint32_t sampling_rate,
                                 ErrorCode error_code) {
  if (sampling_rate == 0) {
    ThrowLcp(error_code, "sampling rate must be positive");
  }
  if (!values.empty() && values.front() != 0) {
    ThrowLcp(error_code, "LCP[0] must be zero");
  }
  return LcpStorage(Storage(std::move(values)), sampling_rate, 0);
}

LcpStorage LcpStorage::FromRaw64(Raw64 values, std::uint32_t sampling_rate,
                                 ErrorCode error_code) {
  if (sampling_rate == 0) {
    ThrowLcp(error_code, "sampling rate must be positive");
  }
  if (!values.empty() && values.front() != 0) {
    ThrowLcp(error_code, "LCP[0] must be zero");
  }
  return LcpStorage(Storage(std::move(values)), sampling_rate, 0);
}

LcpStorage LcpStorage::BuildByteCoded(const IntegerArrayView& raw_lcp,
                                      const IntegerArrayView& isa,
                                      std::uint32_t sampling_rate,
                                      std::uint8_t coordinate_width,
                                      std::uint64_t text_symbols,
                                      ErrorCode error_code) {
  if (coordinate_width == 32) {
    auto data = BuildByteData<std::uint32_t>(
        raw_lcp, isa, sampling_rate, text_symbols, error_code);
    return LcpStorage(Storage(std::move(data)), sampling_rate, text_symbols);
  }
  if (coordinate_width == 64) {
    auto data = BuildByteData<std::uint64_t>(
        raw_lcp, isa, sampling_rate, text_symbols, error_code);
    return LcpStorage(Storage(std::move(data)), sampling_rate, text_symbols);
  }
  ThrowLcp(error_code, "byte-coded coordinates must be 32 or 64 bits");
}

LcpStorage LcpStorage::BuildByteCodedDirect(
    const std::uint8_t* text, std::size_t text_size,
    const IntegerArrayView& suffix_array, const IntegerArrayView& isa,
    std::uint32_t sampling_rate, std::uint8_t coordinate_width,
    ErrorCode error_code) {
  if (coordinate_width == 32) {
    auto data = BuildByteDataDirect<std::uint32_t>(
        text, text_size, suffix_array, isa, sampling_rate, error_code);
    return LcpStorage(Storage(std::move(data)), sampling_rate, text_size);
  }
  if (coordinate_width == 64) {
    auto data = BuildByteDataDirect<std::uint64_t>(
        text, text_size, suffix_array, isa, sampling_rate, error_code);
    return LcpStorage(Storage(std::move(data)), sampling_rate, text_size);
  }
  ThrowLcp(error_code, "byte-coded coordinates must be 32 or 64 bits");
}

LcpStorage LcpStorage::FromByteCoded32(
    std::vector<std::uint8_t> primary,
    std::vector<std::uint32_t> anchor_positions,
    std::vector<std::uint32_t> anchor_values,
    std::uint32_t sampling_rate, std::uint64_t text_symbols,
    ErrorCode error_code) {
  Byte32 data{std::move(primary), std::move(anchor_positions),
              std::move(anchor_values), {}};
  ValidateByteStructure(data, sampling_rate, text_symbols, error_code);
  BuildGuide(data, text_symbols, error_code);
  return LcpStorage(Storage(std::move(data)), sampling_rate, text_symbols);
}

LcpStorage LcpStorage::FromByteCoded64(
    std::vector<std::uint8_t> primary,
    std::vector<std::uint64_t> anchor_positions,
    std::vector<std::uint64_t> anchor_values,
    std::uint32_t sampling_rate, std::uint64_t text_symbols,
    ErrorCode error_code) {
  Byte64 data{std::move(primary), std::move(anchor_positions),
              std::move(anchor_values), {}};
  ValidateByteStructure(data, sampling_rate, text_symbols, error_code);
  BuildGuide(data, text_symbols, error_code);
  return LcpStorage(Storage(std::move(data)), sampling_rate, text_symbols);
}

LcpEncoding LcpStorage::Encoding() const noexcept {
  switch (storage_.index()) {
    case 0:
      return LcpEncoding::kEmpty;
    case 1:
      return LcpEncoding::kRaw32;
    case 2:
      return LcpEncoding::kRaw64;
    case 3:
      return LcpEncoding::kByteCoded32;
    case 4:
      return LcpEncoding::kByteCoded64;
  }
  return LcpEncoding::kEmpty;
}

bool LcpStorage::Empty() const noexcept { return Size() == 0; }

std::uint64_t LcpStorage::Size() const noexcept {
  return std::visit(
      [](const auto& values) -> std::uint64_t {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return 0;
        } else if constexpr (std::is_same_v<Value, Raw32> ||
                             std::is_same_v<Value, Raw64>) {
          return values.size();
        } else {
          return values.primary.size();
        }
      },
      storage_);
}

std::uint8_t LcpStorage::CoordinateWidth() const noexcept {
  switch (Encoding()) {
    case LcpEncoding::kRaw32:
    case LcpEncoding::kByteCoded32:
      return 32;
    case LcpEncoding::kRaw64:
    case LcpEncoding::kByteCoded64:
      return 64;
    case LcpEncoding::kEmpty:
      return 0;
  }
  return 0;
}

std::uint64_t LcpStorage::Exact(std::uint64_t row,
                                std::uint64_t suffix_position) const {
  return std::visit(
      [&](const auto& values) -> std::uint64_t {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          ThrowLcp(ErrorCode::kInvalidInput, "LCP storage is empty");
        } else if constexpr (std::is_same_v<Value, Raw32> ||
                             std::is_same_v<Value, Raw64>) {
          if (row >= values.size()) {
            ThrowLcp(ErrorCode::kInvalidInput, "row is out of range");
          }
          return values[static_cast<std::size_t>(row)];
        } else {
          return DecodeByte(values, row, suffix_position, sampling_rate_,
                            ErrorCode::kCorruptIndex);
        }
      },
      storage_);
}

bool LcpStorage::AtLeast(std::uint64_t row, std::uint64_t suffix_position,
                         std::uint64_t target) const {
  return std::visit(
      [&](const auto& values) -> bool {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          ThrowLcp(ErrorCode::kInvalidInput, "LCP storage is empty");
        } else if constexpr (std::is_same_v<Value, Raw32> ||
                             std::is_same_v<Value, Raw64>) {
          if (row >= values.size()) {
            ThrowLcp(ErrorCode::kInvalidInput, "row is out of range");
          }
          return values[static_cast<std::size_t>(row)] >= target;
        } else {
          if (row >= values.primary.size()) {
            ThrowLcp(ErrorCode::kInvalidInput, "row is out of range");
          }
          const auto primary = values.primary[static_cast<std::size_t>(row)];
          if (primary < kByteLimit) {
            return primary >= target;
          }
          if (target <= kByteLimit) {
            return true;
          }
          return DecodeByte(values, row, suffix_position, sampling_rate_,
                            ErrorCode::kCorruptIndex) >= target;
        }
      },
      storage_);
}

void LcpStorage::Validate(const IntegerArrayView& suffix_array,
                          std::uint64_t text_symbols,
                          ErrorCode error_code) const {
  if (sampling_rate_ == 0) {
    ThrowLcp(error_code, "sampling rate must be positive");
  }
  if (Size() != suffix_array.Size()) {
    ThrowLcp(error_code, "LCP and suffix-array lengths differ");
  }
  if ((Encoding() == LcpEncoding::kByteCoded32 ||
       Encoding() == LcpEncoding::kByteCoded64) &&
      text_symbols_ != text_symbols) {
    ThrowLcp(error_code, "logical text size differs from byte-code metadata");
  }
  if (Size() == 0) {
    if (text_symbols != 0) {
      ThrowLcp(error_code, "empty LCP has a non-empty logical text");
    }
    return;
  }
  if (text_symbols == 0) {
    ThrowLcp(error_code, "non-empty LCP has an empty logical text");
  }
  const auto expected = 1 + (text_symbols - 1) / sampling_rate_;
  if (Size() != expected) {
    ThrowLcp(error_code, "LCP row count disagrees with sampling metadata");
  }
  if (Exact(0, suffix_array[0]) != 0) {
    ThrowLcp(error_code, "LCP[0] must be zero");
  }

  std::vector<bool> anchor_starts;
  if (AnchorCount() > std::numeric_limits<std::size_t>::max()) {
    ThrowLcp(error_code, "overflow anchor count is too large");
  }
  try {
    anchor_starts.resize(static_cast<std::size_t>(AnchorCount()));
  } catch (const std::bad_alloc&) {
    ThrowLcp(error_code, "cannot validate overflow anchors");
  }
  for (std::uint64_t row = 0; row < Size(); ++row) {
    const auto suffix = suffix_array[static_cast<std::size_t>(row)];
    if (suffix >= text_symbols || suffix % sampling_rate_ != 0) {
      ThrowLcp(error_code, "suffix-array position is invalid");
    }
    const auto value = Exact(row, suffix);
    if (row != 0) {
      const auto previous = suffix_array[static_cast<std::size_t>(row - 1)];
      if (previous >= text_symbols) {
        ThrowLcp(error_code, "suffix-array position is invalid");
      }
      const auto bound = std::min(text_symbols - suffix,
                                  text_symbols - previous);
      if (value > bound) {
        ThrowLcp(error_code, "LCP value exceeds adjacent suffix bounds");
      }
    }
    std::visit(
        [&](const auto& values) {
          using Value = std::decay_t<decltype(values)>;
          if constexpr (std::is_same_v<Value, Byte32> ||
                        std::is_same_v<Value, Byte64>) {
            if (values.primary[static_cast<std::size_t>(row)] == kByteLimit) {
              const auto anchor = FindAnchor(values, suffix, error_code);
              if (static_cast<std::uint64_t>(
                      values.anchor_positions[anchor]) == suffix) {
                anchor_starts[anchor] = true;
              }
            }
          }
        },
        storage_);
  }
  if (std::find(anchor_starts.begin(), anchor_starts.end(), false) !=
      anchor_starts.end()) {
    ThrowLcp(error_code, "overflow anchor has no matching suffix row");
  }
}

std::uint64_t LcpStorage::ResidentBytes() const noexcept {
  return std::visit(
      [](const auto& values) -> std::uint64_t {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return 0;
        } else if constexpr (std::is_same_v<Value, Raw32> ||
                             std::is_same_v<Value, Raw64>) {
          return CheckedBytes(
              values.size(), sizeof(typename Value::value_type));
        } else {
          return ByteResidentBytes(values);
        }
      },
      storage_);
}

std::uint64_t LcpStorage::SerializedDataBytes() const noexcept {
  return std::visit(
      [](const auto& values) -> std::uint64_t {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, std::monostate>) {
          return 0;
        } else if constexpr (std::is_same_v<Value, Raw32> ||
                             std::is_same_v<Value, Raw64>) {
          return CheckedBytes(
              values.size(), sizeof(typename Value::value_type));
        } else {
          return ByteSerializedBytes(values);
        }
      },
      storage_);
}

std::uint64_t LcpStorage::GuideBytes() const noexcept {
  return std::visit(
      [](const auto& values) -> std::uint64_t {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, Byte32> ||
                      std::is_same_v<Value, Byte64>) {
          return CheckedBytes(
              values.guide.size(),
              sizeof(typename std::decay_t<
                     decltype(values.anchor_positions)>::value_type));
        }
        return 0;
      },
      storage_);
}

std::uint64_t LcpStorage::AnchorCount() const noexcept {
  return std::visit(
      [](const auto& values) -> std::uint64_t {
        using Value = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Value, Byte32> ||
                      std::is_same_v<Value, Byte64>) {
          return values.anchor_positions.size();
        }
        return 0;
      },
      storage_);
}

const LcpStorage::Raw32* LcpStorage::Raw32Values() const noexcept {
  return std::get_if<Raw32>(&storage_);
}

const LcpStorage::Raw64* LcpStorage::Raw64Values() const noexcept {
  return std::get_if<Raw64>(&storage_);
}

const std::vector<std::uint8_t>* LcpStorage::BytePrimary() const noexcept {
  if (const auto* data = std::get_if<Byte32>(&storage_)) {
    return &data->primary;
  }
  if (const auto* data = std::get_if<Byte64>(&storage_)) {
    return &data->primary;
  }
  return nullptr;
}

const std::vector<std::uint32_t>* LcpStorage::AnchorPositions32()
    const noexcept {
  const auto* data = std::get_if<Byte32>(&storage_);
  return data == nullptr ? nullptr : &data->anchor_positions;
}

const std::vector<std::uint32_t>* LcpStorage::AnchorValues32() const noexcept {
  const auto* data = std::get_if<Byte32>(&storage_);
  return data == nullptr ? nullptr : &data->anchor_values;
}

const std::vector<std::uint64_t>* LcpStorage::AnchorPositions64()
    const noexcept {
  const auto* data = std::get_if<Byte64>(&storage_);
  return data == nullptr ? nullptr : &data->anchor_positions;
}

const std::vector<std::uint64_t>* LcpStorage::AnchorValues64() const noexcept {
  const auto* data = std::get_if<Byte64>(&storage_);
  return data == nullptr ? nullptr : &data->anchor_values;
}

}  // namespace sufkit::detail
