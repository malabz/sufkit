// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::detail {

// Owns an aligned allocation used by the divsufsort64 construction buffer.
// On POSIX systems, consumed pages may be discarded without releasing their
// virtual storage or ending the source array's lifetime.
class MappedAllocation {
 public:
  MappedAllocation() = default;
  MappedAllocation(MappedAllocation&& other) noexcept;
  MappedAllocation& operator=(MappedAllocation&& other) noexcept;
  MappedAllocation(const MappedAllocation&) = delete;
  MappedAllocation& operator=(const MappedAllocation&) = delete;
  ~MappedAllocation();

  static MappedAllocation Allocate(std::size_t bytes);

  void* Data() const noexcept { return data_; }
  std::size_t Bytes() const noexcept { return bytes_; }
  bool Empty() const noexcept { return data_ == nullptr; }

  // Discards complete pages wholly contained in [0, consumed_bytes). This is
  // an RSS hint only: the mapping and all C++ object lifetimes remain intact.
  void DiscardConsumedPrefix(std::size_t consumed_bytes) noexcept;

  // Discards complete pages after the logical result retained by suffix
  // sampling. The mapping and the original array lifetime remain intact.
  void DiscardUnusedSuffix(std::size_t retained_bytes) noexcept;

 private:
  enum class Kind : std::uint8_t { kNone, kPageMapping, kHeap };

  MappedAllocation(void* data, std::size_t bytes, Kind kind) noexcept
      : data_(data), bytes_(bytes), kind_(kind) {}
  void Reset() noexcept;

  void* data_ = nullptr;
  std::size_t bytes_ = 0;
  std::size_t discarded_prefix_bytes_ = 0;
  Kind kind_ = Kind::kNone;
};

// libdivsufsort64 writes signed 64-bit rows. This private vector-like buffer
// starts those objects explicitly and can later transfer the allocation to a
// separate target vector. resize() may shrink the logical result after suffix
// sampling, while constructed_size() tracks the allocation's live array.
class DivSufsort64Buffer {
 public:
  using value_type = std::int64_t;
  using iterator = value_type*;
  using const_iterator = const value_type*;

  DivSufsort64Buffer() = default;
  DivSufsort64Buffer(DivSufsort64Buffer&& other) noexcept;
  DivSufsort64Buffer& operator=(DivSufsort64Buffer&& other) noexcept;
  DivSufsort64Buffer(const DivSufsort64Buffer&) = delete;
  DivSufsort64Buffer& operator=(const DivSufsort64Buffer&) = delete;
  ~DivSufsort64Buffer();

  void resize(std::size_t size);
  void shrink_to_fit() noexcept {}
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  value_type* data() noexcept {
    return static_cast<value_type*>(allocation_.Data());
  }
  const value_type* data() const noexcept {
    return static_cast<const value_type*>(allocation_.Data());
  }
  value_type& operator[](std::size_t index) noexcept { return data()[index]; }
  const value_type& operator[](std::size_t index) const noexcept {
    return data()[index];
  }
  iterator begin() noexcept { return data(); }
  const_iterator begin() const noexcept { return data(); }
  iterator end() noexcept { return data() + size_; }
  const_iterator end() const noexcept { return data() + size_; }

 private:
  friend class CoordinateStorage;

  void DestroyConstructed() noexcept;

  MappedAllocation allocation_;
  std::size_t size_ = 0;
  std::size_t constructed_size_ = 0;
};

// The construction backend width and the persisted coordinate width are
// deliberately independent. A 64-bit constructor may safely down-pack its
// non-negative output when the complete logical text fits a narrower width.
using CoordinateStorageWidth = ::sufkit::CoordinateStorageWidth;

inline constexpr std::uint64_t kMaxCoordinate40 =
    (std::uint64_t{1} << 40U) - 1U;
inline constexpr std::uint64_t kMaxCoordinate48 =
    (std::uint64_t{1} << 48U) - 1U;

// Selects from the largest valid position, symbol_count - 1. In particular,
// UINT32_MAX + 1 symbols still fit 32-bit unsigned coordinates.
CoordinateStorageWidth SelectCoordinateStorageWidth(
    std::uint64_t symbol_count,
    ErrorCode error_code = ErrorCode::kInvalidInput);

// Resolves an explicit or automatic width and rejects a width that cannot
// represent every position in the logical text.
CoordinateStorageWidth ResolveCoordinateStorageWidth(
    CoordinateStorageWidth requested, std::uint64_t symbol_count,
    ErrorCode error_code = ErrorCode::kInvalidInput);

std::uint64_t MaxCoordinateForWidth(CoordinateStorageWidth width,
                                    ErrorCode error_code =
                                        ErrorCode::kInvalidInput);

// Returns the exact element payload size, excluding vector bookkeeping and
// allocator rounding. Overflow is reported before a byte count is returned.
std::uint64_t CoordinateStorageByteCount(
    std::uint64_t element_count, CoordinateStorageWidth width,
    ErrorCode error_code = ErrorCode::kInvalidInput);

// Packed coordinates use structure-of-arrays storage. This avoids padding a
// low/high pair to eight bytes while retaining contiguous low and high planes.
struct Coordinate40Storage {
  std::vector<std::uint32_t> low;
  std::vector<std::uint8_t> high;

  std::size_t Size() const noexcept { return low.size(); }
  std::size_t size() const noexcept { return Size(); }
  bool empty() const noexcept { return low.empty(); }
  std::uint64_t AtUnchecked(std::size_t index) const noexcept {
    return static_cast<std::uint64_t>(low[index]) |
           (static_cast<std::uint64_t>(high[index]) << 32U);
  }
  std::uint64_t operator[](std::size_t index) const noexcept {
    return AtUnchecked(index);
  }
};

struct Coordinate48Storage {
  std::vector<std::uint32_t> low;
  std::vector<std::uint16_t> high;

  std::size_t Size() const noexcept { return low.size(); }
  std::size_t size() const noexcept { return Size(); }
  bool empty() const noexcept { return low.empty(); }
  std::uint64_t AtUnchecked(std::size_t index) const noexcept {
    return static_cast<std::uint64_t>(low[index]) |
           (static_cast<std::uint64_t>(high[index]) << 32U);
  }
  std::uint64_t operator[](std::size_t index) const noexcept {
    return AtUnchecked(index);
  }
};

class CoordinateStorage {
 public:
  using Coordinate32 = std::vector<std::uint32_t>;
  using Coordinate64 = std::vector<std::uint64_t>;
  using Storage =
      std::variant<Coordinate32, Coordinate40Storage, Coordinate48Storage,
                   Coordinate64>;

  CoordinateStorage() : storage_(Coordinate32{}) {}
  CoordinateStorage(CoordinateStorage&&) noexcept = default;
  CoordinateStorage& operator=(CoordinateStorage&&) noexcept = default;
  CoordinateStorage(const CoordinateStorage&) = delete;
  CoordinateStorage& operator=(const CoordinateStorage&) = delete;

  // Every input coordinate is checked against symbol_count and the resolved
  // width before ownership is moved into the returned storage.
  static CoordinateStorage FromUInt32(
      std::vector<std::uint32_t>&& values, CoordinateStorageWidth requested,
      std::uint64_t symbol_count,
      ErrorCode error_code = ErrorCode::kBuildFailure,
      const char* label = "coordinate");

  static CoordinateStorage FromUInt64(
      std::vector<std::uint64_t>&& values, CoordinateStorageWidth requested,
      std::uint64_t symbol_count,
      ErrorCode error_code = ErrorCode::kBuildFailure,
      const char* label = "coordinate");

  // libdivsufsort64 returns signed coordinates even though a valid suffix
  // position is always non-negative. Validate that contract once and pack
  // directly into the requested representation, avoiding an intermediate
  // reference-sized uint64_t copy.
  static CoordinateStorage FromInt64(
      std::vector<std::int64_t>&& values, CoordinateStorageWidth requested,
      std::uint64_t symbol_count,
      ErrorCode error_code = ErrorCode::kBuildFailure,
      const char* label = "coordinate");

  // Consumes the page-backed divsufsort64 result into a separate standard
  // vector. POSIX builds discard consumed source pages as the target grows,
  // reducing RSS without overlapping objects of different types.
  static CoordinateStorage FromDivSufsort64(
      DivSufsort64Buffer&& values, CoordinateStorageWidth requested,
      std::uint64_t symbol_count,
      ErrorCode error_code = ErrorCode::kBuildFailure,
      const char* label = "coordinate");

  // These factories are used by the versioned section decoder. They preserve
  // the split planes produced by the stream instead of materializing a
  // temporary uint64_t array during load.
  static CoordinateStorage FromPacked40(
      Coordinate40Storage&& values, std::uint64_t symbol_count,
      ErrorCode error_code = ErrorCode::kCorruptIndex,
      const char* label = "coordinate");
  static CoordinateStorage FromPacked48(
      Coordinate48Storage&& values, std::uint64_t symbol_count,
      ErrorCode error_code = ErrorCode::kCorruptIndex,
      const char* label = "coordinate");

  CoordinateStorageWidth Width() const noexcept;
  std::uint64_t Size() const noexcept;
  bool Empty() const noexcept { return Size() == 0; }
  std::uint64_t Bytes() const noexcept;

  // At() and DecodeSpan() validate public boundaries. Typed query kernels can
  // lift variant dispatch out of hot loops through Visit() and then use direct
  // vector access or the packed AtUnchecked() methods.
  std::uint64_t At(std::uint64_t index) const;
  void DecodeSpan(std::uint64_t begin, std::uint64_t count,
                  std::uint64_t* output) const;

  template <class Visitor>
  decltype(auto) Visit(Visitor&& visitor) const {
    return std::visit(std::forward<Visitor>(visitor), storage_);
  }

 private:
  explicit CoordinateStorage(Storage&& storage)
      : storage_(std::move(storage)) {}

  Storage storage_;
};

}  // namespace sufkit::detail
