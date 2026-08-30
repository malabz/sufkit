// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

#include <sufkit/types.hpp>

#include "coordinate_storage.hpp"

namespace sufkit::detail {

// Configuration for the private exact k-mer directory used by Fast indexes.
// The defaults cap the directory at 25% of the already-resident core arrays
// and avoid paying a fixed directory cost for small references.
struct FastPrefixIndexOptions {
  std::uint32_t min_k = 8;
  std::uint32_t max_k = 10;
  std::uint32_t memory_budget_basis_points = 2500;
  std::uint64_t min_suffix_count = 1U << 16U;
};

struct FastPrefixInterval {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;

  bool Empty() const noexcept { return begin == end; }
};

// An immutable, exact canonical-DNA k-mer to suffix-array interval directory.
//
// Unlike a learned model, this table is not a hint: a successful Lookup()
// returns the complete [begin, end) interval for the first k query symbols.
// It is derived after build/load, is never serialized, and is safe to share
// between concurrent readers. Missing, short, or non-canonical query prefixes
// return nullopt so the caller can use its normal exact fallback.
class FastPrefixIndex {
 public:
  FastPrefixIndex() = default;

  FastPrefixIndex(FastPrefixIndex&&) noexcept = default;
  FastPrefixIndex& operator=(FastPrefixIndex&&) noexcept = default;
  FastPrefixIndex(const FastPrefixIndex&) = delete;
  FastPrefixIndex& operator=(const FastPrefixIndex&) = delete;

  static FastPrefixIndex Build(
      const std::vector<std::uint8_t>& text,
      const CoordinateStorage& suffix_array,
      const CoordinateStorage& inverse_suffix_array,
      const std::vector<Position>& contig_starts,
      const std::vector<Position>& contig_lengths,
      std::uint64_t resident_core_bytes,
      const FastPrefixIndexOptions& options = {});

  // Returns zero when the fixed directory should be disabled for this index.
  static std::uint32_t SelectK(
      std::uint64_t suffix_count, std::uint64_t resident_core_bytes,
      const FastPrefixIndexOptions& options = {});

  // Interval endpoints include suffix_count, so exactly 2^32 rows require
  // 64-bit directory entries even though their maximum row is UINT32_MAX.
  static std::uint8_t RowWidthForSuffixCount(
      std::uint64_t suffix_count) noexcept;

  static std::uint64_t ResidentBytesForK(
      std::uint32_t k, std::uint64_t suffix_count) noexcept;

  std::optional<FastPrefixInterval> Lookup(const std::uint8_t* pattern,
                                           std::size_t pattern_size) const;

  std::optional<FastPrefixInterval> Lookup(
      const std::vector<std::uint8_t>& pattern) const {
    return Lookup(pattern.data(), pattern.size());
  }

  bool Empty() const noexcept {
    return std::holds_alternative<std::monostate>(tables_);
  }
  std::uint32_t K() const noexcept { return k_; }
  std::uint8_t RowWidth() const noexcept { return row_width_; }
  std::uint64_t DirectoryEntries() const noexcept;
  std::uint64_t ResidentBytes() const noexcept;
  std::uint64_t IndexedKmers() const noexcept { return indexed_kmers_; }
  std::uint64_t NonemptyEntries() const noexcept {
    return nonempty_entries_;
  }

 private:
  // A 32-bit row interval is packed into one 64-bit word. Querying the
  // directory therefore needs one dependent memory load instead of loading
  // begin/end from two distant planes. The 64-bit row case keeps two planes
  // because both endpoints need the full coordinate width.
  struct Tables32 {
    std::vector<std::uint64_t> intervals;
  };

  template <class Row>
  struct WideTables {
    std::vector<Row> begin;
    std::vector<Row> end;
  };

  using Tables64 = WideTables<std::uint64_t>;
  using Storage = std::variant<std::monostate, Tables32, Tables64>;

  std::uint32_t k_ = 0;
  std::uint8_t row_width_ = 0;
  std::uint64_t suffix_count_ = 0;
  std::uint64_t indexed_kmers_ = 0;
  std::uint64_t nonempty_entries_ = 0;
  Storage tables_;
};

}  // namespace sufkit::detail
