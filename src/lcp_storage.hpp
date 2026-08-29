// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

#include "coordinate_storage.hpp"

#include <sufkit/types.hpp>

namespace sufkit::detail {

// A small type-erased view used only while constructing or validating an LCP
// representation. Query kernels pass an already decoded suffix position to
// LcpStorage and therefore do not pay this dispatch cost.
class IntegerArrayView {
 public:
  IntegerArrayView() = default;
  IntegerArrayView(const std::uint32_t* data, std::size_t size) noexcept;
  IntegerArrayView(const std::uint64_t* data, std::size_t size) noexcept;
  IntegerArrayView(const std::int32_t* data, std::size_t size) noexcept;
  IntegerArrayView(const std::int64_t* data, std::size_t size) noexcept;
  explicit IntegerArrayView(const Coordinate40Storage& values) noexcept;
  explicit IntegerArrayView(const Coordinate48Storage& values) noexcept;

  template <class Integer, class Allocator>
  explicit IntegerArrayView(
      const std::vector<Integer, Allocator>& values) noexcept
      : IntegerArrayView(values.data(), values.size()) {}

  std::size_t Size() const noexcept { return size_; }
  bool Empty() const noexcept { return size_ == 0; }
  std::uint64_t operator[](std::size_t index) const noexcept;

 private:
  enum class Kind : std::uint8_t {
    kUnsigned32,
    kUnsigned64,
    kSigned32,
    kSigned64,
    kSplit40,
    kSplit48,
  };

  const void* data_ = nullptr;
  const void* high_ = nullptr;
  std::size_t size_ = 0;
  Kind kind_ = Kind::kUnsigned32;
};

enum class LcpEncoding : std::uint8_t {
  kEmpty = 0,
  kRaw32 = 1,
  kRaw64 = 2,
  kByteCoded32 = 3,
  kByteCoded64 = 4,
};

template <class Coordinate>
struct ByteCodedLcpData {
  std::vector<std::uint8_t> primary;
  std::vector<Coordinate> anchor_positions;
  std::vector<Coordinate> anchor_values;

  // Each entry identifies the first anchor at or after a 4096-symbol block
  // boundary. The guide is derived data and is intentionally not serialized.
  std::vector<Coordinate> guide;
};

// Private immutable LCP storage. Byte coding stores common values directly in
// one byte and represents long generalized-PLCP runs with text-ordered SoA
// anchors. A run may decrease by sampling_rate symbols per sampled position.
class LcpStorage {
 public:
  static constexpr std::uint64_t kByteLimit = 255;
  static constexpr std::uint64_t kGuideBlockSize = 4096;

  using Raw32 = std::vector<std::uint32_t>;
  using Raw64 = std::vector<std::uint64_t>;
  using Byte32 = ByteCodedLcpData<std::uint32_t>;
  using Byte64 = ByteCodedLcpData<std::uint64_t>;

  LcpStorage() = default;

  static LcpStorage FromRaw32(Raw32 values, std::uint32_t sampling_rate,
                              ErrorCode error_code);
  static LcpStorage FromRaw64(Raw64 values, std::uint32_t sampling_rate,
                              ErrorCode error_code);

  // Builds byte coding from LCP in SA-row order and ISA in sampled-text order.
  // isa[sample] is the row for text position sample * sampling_rate.
  static LcpStorage BuildByteCoded(const IntegerArrayView& raw_lcp,
                                   const IntegerArrayView& isa,
                                   std::uint32_t sampling_rate,
                                   std::uint8_t coordinate_width,
                                   std::uint64_t text_symbols,
                                   ErrorCode error_code);

  // Computes generalized-Kasai LCP values in sampled-text order and writes
  // byte coding directly, avoiding a row-sized raw-LCP temporary. suffix_array
  // is in lexicographic row order; isa[sample] is the row for text position
  // sample * sampling_rate.
  static LcpStorage BuildByteCodedDirect(
      const std::uint8_t* text, std::size_t text_size,
      const IntegerArrayView& suffix_array, const IntegerArrayView& isa,
      std::uint32_t sampling_rate, std::uint8_t coordinate_width,
      ErrorCode error_code);

  // Reconstructs a serialized byte-coded representation. Full semantic
  // validation against the suffix array is performed by Validate().
  static LcpStorage FromByteCoded32(
      std::vector<std::uint8_t> primary,
      std::vector<std::uint32_t> anchor_positions,
      std::vector<std::uint32_t> anchor_values,
      std::uint32_t sampling_rate, std::uint64_t text_symbols,
      ErrorCode error_code);
  static LcpStorage FromByteCoded64(
      std::vector<std::uint8_t> primary,
      std::vector<std::uint64_t> anchor_positions,
      std::vector<std::uint64_t> anchor_values,
      std::uint32_t sampling_rate, std::uint64_t text_symbols,
      ErrorCode error_code);

  LcpEncoding Encoding() const noexcept;
  bool Empty() const noexcept;
  std::uint64_t Size() const noexcept;
  std::uint32_t SamplingRate() const noexcept { return sampling_rate_; }
  std::uint8_t CoordinateWidth() const noexcept;

  // suffix_position is ignored by raw storage. For byte coding it identifies
  // the generalized-PLCP run containing this row.
  std::uint64_t Exact(std::uint64_t row,
                      std::uint64_t suffix_position) const;
  bool AtLeast(std::uint64_t row, std::uint64_t suffix_position,
               std::uint64_t target) const;

  // Validates row count, suffix bounds, every decoded LCP value, all overflow
  // anchors, and the generalized sampled-SA range-compaction invariants.
  void Validate(const IntegerArrayView& suffix_array,
                std::uint64_t text_symbols, ErrorCode error_code) const;

  std::uint64_t ResidentBytes() const noexcept;
  std::uint64_t SerializedDataBytes() const noexcept;
  std::uint64_t GuideBytes() const noexcept;
  std::uint64_t AnchorCount() const noexcept;

  // Serialization accessors return null for a different encoding. The guide
  // never appears here because it is reconstructed after loading.
  const Raw32* Raw32Values() const noexcept;
  const Raw64* Raw64Values() const noexcept;
  const std::vector<std::uint8_t>* BytePrimary() const noexcept;
  const std::vector<std::uint32_t>* AnchorPositions32() const noexcept;
  const std::vector<std::uint32_t>* AnchorValues32() const noexcept;
  const std::vector<std::uint64_t>* AnchorPositions64() const noexcept;
  const std::vector<std::uint64_t>* AnchorValues64() const noexcept;

 private:
  using Storage = std::variant<std::monostate, Raw32, Raw64, Byte32, Byte64>;

  explicit LcpStorage(Storage storage, std::uint32_t sampling_rate,
                      std::uint64_t text_symbols) noexcept;

  Storage storage_;
  std::uint32_t sampling_rate_ = 1;
  std::uint64_t text_symbols_ = 0;
};

}  // namespace sufkit::detail
