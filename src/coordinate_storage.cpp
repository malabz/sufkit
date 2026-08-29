// SPDX-License-Identifier: MIT

#include "coordinate_storage.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace sufkit::detail {
namespace {

std::size_t CheckedAllocationBytes(std::size_t count, std::size_t width,
                                   const char* label) {
  if (count > std::numeric_limits<std::size_t>::max() / width) {
    throw std::length_error(std::string(label) + " allocation is too large");
  }
  return count * width;
}

std::size_t PageSize() noexcept {
#if defined(__unix__) || defined(__APPLE__)
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size > 0) {
    return static_cast<std::size_t>(page_size);
  }
#endif
  return 4096;
}

std::size_t RoundToPage(std::size_t bytes) {
  const std::size_t page_size = PageSize();
  if (bytes > std::numeric_limits<std::size_t>::max() - (page_size - 1U)) {
    throw std::length_error("mapped coordinate allocation is too large");
  }
  return ((bytes + page_size - 1U) / page_size) * page_size;
}

std::string Label(const char* label) {
  return label == nullptr ? "coordinate" : label;
}

[[noreturn]] void Throw(ErrorCode error_code, const std::string& message) {
  throw Error(error_code, message);
}

void ValidateSymbolCount(std::uint64_t symbol_count, ErrorCode error_code) {
  if (symbol_count == 0) {
    Throw(error_code, "coordinate storage requires a non-empty logical text");
  }
}

template <class Value>
void ValidateValues(const std::vector<Value>& values,
                    std::uint64_t symbol_count,
                    CoordinateStorageWidth width, ErrorCode error_code,
                    const char* label) {
  const std::uint64_t maximum = MaxCoordinateForWidth(width, error_code);
  for (const Value value : values) {
    const std::uint64_t coordinate = static_cast<std::uint64_t>(value);
    if (coordinate >= symbol_count) {
      Throw(error_code, Label(label) + " value is outside the logical text");
    }
    if (coordinate > maximum) {
      Throw(error_code,
            Label(label) + " value exceeds the selected storage width");
    }
  }
}

template <class Values>
void ValidateInt64Values(const Values& values, std::uint64_t symbol_count,
                         CoordinateStorageWidth width,
                         ErrorCode error_code, const char* label) {
  const std::uint64_t maximum = MaxCoordinateForWidth(width, error_code);
  for (const std::int64_t value : values) {
    if (value < 0) {
      Throw(error_code, Label(label) + " value is negative");
    }
    const auto coordinate = static_cast<std::uint64_t>(value);
    if (coordinate >= symbol_count) {
      Throw(error_code, Label(label) + " value is outside the logical text");
    }
    if (coordinate > maximum) {
      Throw(error_code,
            Label(label) + " value exceeds the selected storage width");
    }
  }
}

template <class Packed>
void ValidatePackedValues(const Packed& values,
                          std::uint64_t symbol_count,
                          CoordinateStorageWidth width,
                          ErrorCode error_code, const char* label) {
  if (values.low.size() != values.high.size()) {
    Throw(error_code, Label(label) + " coordinate planes differ in length");
  }
  const std::uint64_t maximum = MaxCoordinateForWidth(width, error_code);
  for (std::size_t index = 0; index < values.low.size(); ++index) {
    const std::uint64_t coordinate = values.AtUnchecked(index);
    if (coordinate >= symbol_count) {
      Throw(error_code, Label(label) + " value is outside the logical text");
    }
    if (coordinate > maximum) {
      Throw(error_code,
            Label(label) + " value exceeds the selected storage width");
    }
  }
}

template <class Function>
auto ConvertWithAllocationErrors(Function&& function, ErrorCode error_code,
                                 const char* label) -> decltype(function()) {
  try {
    return function();
  } catch (const std::bad_alloc&) {
    Throw(error_code, "cannot allocate " + Label(label) + " storage");
  } catch (const std::length_error&) {
    Throw(error_code, Label(label) + " storage is too large");
  }
}

template <class Value>
Coordinate40Storage Pack40(const std::vector<Value>& values) {
  Coordinate40Storage packed;
  packed.low.resize(values.size());
  packed.high.resize(values.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    const std::uint64_t value = static_cast<std::uint64_t>(values[index]);
    packed.low[index] = static_cast<std::uint32_t>(value);
    packed.high[index] = static_cast<std::uint8_t>(value >> 32U);
  }
  return packed;
}

template <class Value>
Coordinate48Storage Pack48(const std::vector<Value>& values) {
  Coordinate48Storage packed;
  packed.low.resize(values.size());
  packed.high.resize(values.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    const std::uint64_t value = static_cast<std::uint64_t>(values[index]);
    packed.low[index] = static_cast<std::uint32_t>(value);
    packed.high[index] = static_cast<std::uint16_t>(value >> 32U);
  }
  return packed;
}

template <class Value>
std::vector<std::uint32_t> Pack32(const std::vector<Value>& values) {
  std::vector<std::uint32_t> packed(values.size());
  std::transform(values.begin(), values.end(), packed.begin(), [](Value value) {
    return static_cast<std::uint32_t>(value);
  });
  return packed;
}

template <class Value>
std::vector<std::uint64_t> Pack64(const std::vector<Value>& values) {
  std::vector<std::uint64_t> packed(values.size());
  std::transform(values.begin(), values.end(), packed.begin(), [](Value value) {
    return static_cast<std::uint64_t>(value);
  });
  return packed;
}

template <class Values>
std::size_t StorageSize(const Values& values) noexcept {
  if constexpr (std::is_same_v<Values, Coordinate40Storage> ||
                std::is_same_v<Values, Coordinate48Storage>) {
    return values.Size();
  } else {
    return values.size();
  }
}

template <class Values>
std::uint64_t StorageAt(const Values& values, std::size_t index) noexcept {
  if constexpr (std::is_same_v<Values, Coordinate40Storage> ||
                std::is_same_v<Values, Coordinate48Storage>) {
    return values.AtUnchecked(index);
  } else {
    return static_cast<std::uint64_t>(values[index]);
  }
}

}  // namespace

MappedAllocation::MappedAllocation(MappedAllocation&& other) noexcept
    : data_(other.data_),
      bytes_(other.bytes_),
      discarded_prefix_bytes_(other.discarded_prefix_bytes_),
      kind_(other.kind_) {
  other.data_ = nullptr;
  other.bytes_ = 0;
  other.discarded_prefix_bytes_ = 0;
  other.kind_ = Kind::kNone;
}

MappedAllocation& MappedAllocation::operator=(
    MappedAllocation&& other) noexcept {
  if (this != &other) {
    Reset();
    data_ = other.data_;
    bytes_ = other.bytes_;
    discarded_prefix_bytes_ = other.discarded_prefix_bytes_;
    kind_ = other.kind_;
    other.data_ = nullptr;
    other.bytes_ = 0;
    other.discarded_prefix_bytes_ = 0;
    other.kind_ = Kind::kNone;
  }
  return *this;
}

MappedAllocation::~MappedAllocation() { Reset(); }

MappedAllocation MappedAllocation::Allocate(std::size_t bytes) {
  if (bytes == 0) {
    return {};
  }
#if defined(__unix__) || defined(__APPLE__)
  const std::size_t mapped_bytes = RoundToPage(bytes);
  void* const data =
      ::mmap(nullptr, mapped_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (data == MAP_FAILED) {
    throw std::bad_alloc();
  }
  return MappedAllocation(data, mapped_bytes, Kind::kPageMapping);
#else
  void* const data =
      ::operator new(bytes,
                     std::align_val_t{alignof(std::max_align_t)});
  return MappedAllocation(data, bytes, Kind::kHeap);
#endif
}

void MappedAllocation::DiscardConsumedPrefix(
    std::size_t consumed_bytes) noexcept {
  if (data_ == nullptr || kind_ != Kind::kPageMapping) {
    return;
  }
  const std::size_t page_size = PageSize();
  const std::size_t bounded = std::min(consumed_bytes, bytes_);
  const std::size_t discard_end = (bounded / page_size) * page_size;
  if (discard_end <= discarded_prefix_bytes_) {
    return;
  }
#if defined(__unix__) || defined(__APPLE__)
  auto* const begin =
      static_cast<unsigned char*>(data_) + discarded_prefix_bytes_;
  const std::size_t length = discard_end - discarded_prefix_bytes_;
  if (::madvise(begin, length, MADV_DONTNEED) == 0) {
    discarded_prefix_bytes_ = discard_end;
  }
#endif
}

void MappedAllocation::DiscardUnusedSuffix(
    std::size_t retained_bytes) noexcept {
  if (data_ == nullptr || kind_ != Kind::kPageMapping) {
    return;
  }
  const std::size_t page_size = PageSize();
  const std::size_t bounded = std::min(retained_bytes, bytes_);
  if (bounded >
      std::numeric_limits<std::size_t>::max() - (page_size - 1U)) {
    return;
  }
  const std::size_t suffix_begin =
      ((bounded + page_size - 1U) / page_size) * page_size;
  if (suffix_begin >= bytes_) {
    return;
  }
#if defined(__unix__) || defined(__APPLE__)
  auto* const begin = static_cast<unsigned char*>(data_) + suffix_begin;
  (void)::madvise(begin, bytes_ - suffix_begin, MADV_DONTNEED);
#endif
}

void MappedAllocation::Reset() noexcept {
  if (data_ == nullptr) {
    return;
  }
  if (kind_ == Kind::kPageMapping) {
#if defined(__unix__) || defined(__APPLE__)
    (void)::munmap(data_, bytes_);
#endif
  } else if (kind_ == Kind::kHeap) {
    ::operator delete(data_, std::align_val_t{alignof(std::max_align_t)});
  }
  data_ = nullptr;
  bytes_ = 0;
  discarded_prefix_bytes_ = 0;
  kind_ = Kind::kNone;
}

DivSufsort64Buffer::DivSufsort64Buffer(
    DivSufsort64Buffer&& other) noexcept
    : allocation_(std::move(other.allocation_)),
      size_(other.size_),
      constructed_size_(other.constructed_size_) {
  other.size_ = 0;
  other.constructed_size_ = 0;
}

DivSufsort64Buffer& DivSufsort64Buffer::operator=(
    DivSufsort64Buffer&& other) noexcept {
  if (this != &other) {
    DestroyConstructed();
    allocation_ = std::move(other.allocation_);
    size_ = other.size_;
    constructed_size_ = other.constructed_size_;
    other.size_ = 0;
    other.constructed_size_ = 0;
  }
  return *this;
}

DivSufsort64Buffer::~DivSufsort64Buffer() { DestroyConstructed(); }

void DivSufsort64Buffer::resize(std::size_t size) {
  if (allocation_.Empty()) {
    const std::size_t bytes =
        CheckedAllocationBytes(size, sizeof(value_type), "divsufsort64");
    allocation_ = MappedAllocation::Allocate(bytes);
    // The standard non-allocating placement array form starts one real array
    // lifetime in the page-backed storage. divsufsort may therefore use normal
    // pointer arithmetic; this is not an array of separately placed objects.
    auto* const values =
        ::new (allocation_.Data()) value_type[size];
    if (static_cast<void*>(values) != allocation_.Data()) {
      throw std::logic_error("placement array changed the storage address");
    }
    size_ = size;
    constructed_size_ = size;
    return;
  }
  if (size > constructed_size_) {
    throw std::length_error("cannot grow an initialized divsufsort64 buffer");
  }
  size_ = size;
}

void DivSufsort64Buffer::DestroyConstructed() noexcept {
  // value_type is trivially destructible. Releasing the allocation ends the
  // live array without per-element pseudo-destructor calls, so the array
  // remains valid for ordinary pointer arithmetic until that point.
  size_ = 0;
  constructed_size_ = 0;
  allocation_ = {};
}

CoordinateStorageWidth SelectCoordinateStorageWidth(
    std::uint64_t symbol_count, ErrorCode error_code) {
  ValidateSymbolCount(symbol_count, error_code);
  const std::uint64_t maximum_position = symbol_count - 1U;
  if (maximum_position <= std::numeric_limits<std::uint32_t>::max()) {
    return CoordinateStorageWidth::kBits32;
  }
  if (maximum_position <= kMaxCoordinate40) {
    return CoordinateStorageWidth::kBits40;
  }
  if (maximum_position <= kMaxCoordinate48) {
    return CoordinateStorageWidth::kBits48;
  }
  return CoordinateStorageWidth::kBits64;
}

CoordinateStorageWidth ResolveCoordinateStorageWidth(
    CoordinateStorageWidth requested, std::uint64_t symbol_count,
    ErrorCode error_code) {
  ValidateSymbolCount(symbol_count, error_code);
  if (requested == CoordinateStorageWidth::kAutoSelect) {
    return SelectCoordinateStorageWidth(symbol_count, error_code);
  }
  const std::uint64_t maximum =
      MaxCoordinateForWidth(requested, error_code);
  if (symbol_count - 1U > maximum) {
    Throw(error_code,
          "logical text exceeds the selected coordinate storage width");
  }
  return requested;
}

std::uint64_t MaxCoordinateForWidth(CoordinateStorageWidth width,
                                    ErrorCode error_code) {
  switch (width) {
    case CoordinateStorageWidth::kBits32:
      return std::numeric_limits<std::uint32_t>::max();
    case CoordinateStorageWidth::kBits40:
      return kMaxCoordinate40;
    case CoordinateStorageWidth::kBits48:
      return kMaxCoordinate48;
    case CoordinateStorageWidth::kBits64:
      return std::numeric_limits<std::uint64_t>::max();
    case CoordinateStorageWidth::kAutoSelect:
      break;
  }
  Throw(error_code, "invalid coordinate storage width");
}

std::uint64_t CoordinateStorageByteCount(std::uint64_t element_count,
                                         CoordinateStorageWidth width,
                                         ErrorCode error_code) {
  std::uint64_t bytes_per_element = 0;
  switch (width) {
    case CoordinateStorageWidth::kBits32:
      bytes_per_element = 4;
      break;
    case CoordinateStorageWidth::kBits40:
      bytes_per_element = 5;
      break;
    case CoordinateStorageWidth::kBits48:
      bytes_per_element = 6;
      break;
    case CoordinateStorageWidth::kBits64:
      bytes_per_element = 8;
      break;
    case CoordinateStorageWidth::kAutoSelect:
      Throw(error_code, "automatic coordinate width is not resolved");
  }
  if (element_count >
      std::numeric_limits<std::uint64_t>::max() / bytes_per_element) {
    Throw(error_code, "coordinate storage byte count overflows uint64");
  }
  return element_count * bytes_per_element;
}

CoordinateStorage CoordinateStorage::FromUInt32(
    std::vector<std::uint32_t>&& values, CoordinateStorageWidth requested,
    std::uint64_t symbol_count, ErrorCode error_code, const char* label) {
  const CoordinateStorageWidth width =
      ResolveCoordinateStorageWidth(requested, symbol_count, error_code);
  ValidateValues(values, symbol_count, width, error_code, label);

  if (width == CoordinateStorageWidth::kBits32) {
    return CoordinateStorage(Storage(Coordinate32(std::move(values))));
  }
  return ConvertWithAllocationErrors(
      [&]() -> CoordinateStorage {
        switch (width) {
          case CoordinateStorageWidth::kBits40:
            return CoordinateStorage(Storage(Pack40(values)));
          case CoordinateStorageWidth::kBits48:
            return CoordinateStorage(Storage(Pack48(values)));
          case CoordinateStorageWidth::kBits64:
            return CoordinateStorage(
                Storage(Coordinate64(Pack64(values))));
          case CoordinateStorageWidth::kAutoSelect:
          case CoordinateStorageWidth::kBits32:
            break;
        }
        Throw(error_code, "invalid coordinate storage width");
      },
      error_code, label);
}

CoordinateStorage CoordinateStorage::FromUInt64(
    std::vector<std::uint64_t>&& values, CoordinateStorageWidth requested,
    std::uint64_t symbol_count, ErrorCode error_code, const char* label) {
  const CoordinateStorageWidth width =
      ResolveCoordinateStorageWidth(requested, symbol_count, error_code);
  ValidateValues(values, symbol_count, width, error_code, label);

  if (width == CoordinateStorageWidth::kBits64) {
    return CoordinateStorage(Storage(Coordinate64(std::move(values))));
  }
  return ConvertWithAllocationErrors(
      [&]() -> CoordinateStorage {
        switch (width) {
          case CoordinateStorageWidth::kBits32:
            return CoordinateStorage(
                Storage(Coordinate32(Pack32(values))));
          case CoordinateStorageWidth::kBits40:
            return CoordinateStorage(Storage(Pack40(values)));
          case CoordinateStorageWidth::kBits48:
            return CoordinateStorage(Storage(Pack48(values)));
          case CoordinateStorageWidth::kAutoSelect:
          case CoordinateStorageWidth::kBits64:
            break;
        }
        Throw(error_code, "invalid coordinate storage width");
      },
      error_code, label);
}

CoordinateStorage CoordinateStorage::FromInt64(
    std::vector<std::int64_t>&& values, CoordinateStorageWidth requested,
    std::uint64_t symbol_count, ErrorCode error_code, const char* label) {
  const CoordinateStorageWidth width =
      ResolveCoordinateStorageWidth(requested, symbol_count, error_code);
  ValidateInt64Values(values, symbol_count, width, error_code, label);

  return ConvertWithAllocationErrors(
      [&]() -> CoordinateStorage {
        switch (width) {
          case CoordinateStorageWidth::kBits32:
            return CoordinateStorage(
                Storage(Coordinate32(Pack32(values))));
          case CoordinateStorageWidth::kBits40:
            return CoordinateStorage(Storage(Pack40(values)));
          case CoordinateStorageWidth::kBits48:
            return CoordinateStorage(Storage(Pack48(values)));
          case CoordinateStorageWidth::kBits64:
            return CoordinateStorage(
                Storage(Coordinate64(Pack64(values))));
          case CoordinateStorageWidth::kAutoSelect:
            break;
        }
        Throw(error_code, "invalid coordinate storage width");
      },
      error_code, label);
}

CoordinateStorage CoordinateStorage::FromDivSufsort64(
    DivSufsort64Buffer&& values, CoordinateStorageWidth requested,
    std::uint64_t symbol_count, ErrorCode error_code, const char* label) {
  const CoordinateStorageWidth width =
      ResolveCoordinateStorageWidth(requested, symbol_count, error_code);
  ValidateInt64Values(values, symbol_count, width, error_code, label);

  const std::size_t count = values.size_;
  values.allocation_.DiscardUnusedSuffix(
      count * sizeof(DivSufsort64Buffer::value_type));
  constexpr std::size_t kDiscardBlockElements = 32U * 1024U;
  const auto discard_consumed = [&](std::size_t processed) noexcept {
    if (processed == count ||
        processed % kDiscardBlockElements == 0) {
      values.allocation_.DiscardConsumedPrefix(
          processed * sizeof(DivSufsort64Buffer::value_type));
    }
  };

  return ConvertWithAllocationErrors(
      [&]() -> CoordinateStorage {
        if (width == CoordinateStorageWidth::kBits64) {
          Coordinate64 output;
          output.reserve(count);
          for (std::size_t index = 0; index < count; ++index) {
            output.push_back(static_cast<std::uint64_t>(values[index]));
            discard_consumed(index + 1U);
          }
          values.DestroyConstructed();
          return CoordinateStorage(Storage(std::move(output)));
        }

        if (width == CoordinateStorageWidth::kBits32) {
          Coordinate32 low;
          low.reserve(count);
          for (std::size_t index = 0; index < count; ++index) {
            low.push_back(static_cast<std::uint32_t>(values[index]));
            discard_consumed(index + 1U);
          }
          values.DestroyConstructed();
          return CoordinateStorage(Storage(std::move(low)));
        }

        if (width == CoordinateStorageWidth::kBits40) {
          Coordinate40Storage packed;
          packed.low.reserve(count);
          packed.high.reserve(count);
          for (std::size_t index = 0; index < count; ++index) {
            const auto coordinate =
                static_cast<std::uint64_t>(values[index]);
            packed.low.push_back(static_cast<std::uint32_t>(coordinate));
            packed.high.push_back(
                static_cast<std::uint8_t>(coordinate >> 32U));
            discard_consumed(index + 1U);
          }
          values.DestroyConstructed();
          return CoordinateStorage(Storage(std::move(packed)));
        }

        if (width == CoordinateStorageWidth::kBits48) {
          Coordinate48Storage packed;
          packed.low.reserve(count);
          packed.high.reserve(count);
          for (std::size_t index = 0; index < count; ++index) {
            const auto coordinate =
                static_cast<std::uint64_t>(values[index]);
            packed.low.push_back(static_cast<std::uint32_t>(coordinate));
            packed.high.push_back(
                static_cast<std::uint16_t>(coordinate >> 32U));
            discard_consumed(index + 1U);
          }
          values.DestroyConstructed();
          return CoordinateStorage(Storage(std::move(packed)));
        }
        Throw(error_code, "invalid coordinate storage width");
      },
      error_code, label);
}

CoordinateStorage CoordinateStorage::FromPacked40(
    Coordinate40Storage&& values, std::uint64_t symbol_count,
    ErrorCode error_code, const char* label) {
  ValidateSymbolCount(symbol_count, error_code);
  ValidatePackedValues(values, symbol_count,
                       CoordinateStorageWidth::kBits40, error_code, label);
  return CoordinateStorage(Storage(std::move(values)));
}

CoordinateStorage CoordinateStorage::FromPacked48(
    Coordinate48Storage&& values, std::uint64_t symbol_count,
    ErrorCode error_code, const char* label) {
  ValidateSymbolCount(symbol_count, error_code);
  ValidatePackedValues(values, symbol_count,
                       CoordinateStorageWidth::kBits48, error_code, label);
  return CoordinateStorage(Storage(std::move(values)));
}

CoordinateStorageWidth CoordinateStorage::Width() const noexcept {
  return std::visit(
      [](const auto& values) {
        using Values = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Values, Coordinate32>) {
          return CoordinateStorageWidth::kBits32;
        } else if constexpr (std::is_same_v<Values, Coordinate40Storage>) {
          return CoordinateStorageWidth::kBits40;
        } else if constexpr (std::is_same_v<Values, Coordinate48Storage>) {
          return CoordinateStorageWidth::kBits48;
        } else {
          return CoordinateStorageWidth::kBits64;
        }
      },
      storage_);
}

std::uint64_t CoordinateStorage::Size() const noexcept {
  return std::visit(
      [](const auto& values) {
        return static_cast<std::uint64_t>(StorageSize(values));
      },
      storage_);
}

std::uint64_t CoordinateStorage::Bytes() const noexcept {
  const std::uint64_t bytes_per_element =
      static_cast<std::uint8_t>(Width()) / 8U;
  return Size() * bytes_per_element;
}

std::uint64_t CoordinateStorage::At(std::uint64_t index) const {
  if (index >= Size()) {
    throw Error(ErrorCode::kInvalidInput,
                "coordinate storage index is out of range");
  }
  return std::visit(
      [index](const auto& values) {
        return StorageAt(values, static_cast<std::size_t>(index));
      },
      storage_);
}

void CoordinateStorage::DecodeSpan(std::uint64_t begin, std::uint64_t count,
                                   std::uint64_t* output) const {
  const std::uint64_t size = Size();
  if (begin > size || count > size - begin) {
    throw Error(ErrorCode::kInvalidInput,
                "coordinate decode span is out of range");
  }
  if (count != 0 && output == nullptr) {
    throw Error(ErrorCode::kInvalidInput,
                "coordinate decode output must not be null");
  }
  std::visit(
      [begin, count, output](const auto& values) {
        const std::size_t first = static_cast<std::size_t>(begin);
        const std::size_t length = static_cast<std::size_t>(count);
        for (std::size_t offset = 0; offset < length; ++offset) {
          output[offset] = StorageAt(values, first + offset);
        }
      },
      storage_);
}

}  // namespace sufkit::detail
