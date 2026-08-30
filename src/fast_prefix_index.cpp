// SPDX-License-Identifier: MIT

#include "fast_prefix_index.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>

#include "reference_data.hpp"

namespace sufkit::detail {
namespace {

constexpr std::uint32_t kMaximumSupportedK = 10;
constexpr std::uint32_t kBasisPointDenominator = 10000;

bool IsCanonical(std::uint8_t symbol) noexcept {
  return symbol >= kA && symbol <= kT;
}

std::uint64_t BudgetBytes(std::uint64_t core_bytes,
                          std::uint32_t basis_points) noexcept {
  // Split the product around the division to avoid overflowing uint64_t.
  return (core_bytes / kBasisPointDenominator) * basis_points +
         ((core_bytes % kBasisPointDenominator) * basis_points) /
             kBasisPointDenominator;
}

std::uint64_t DirectoryBytes(std::uint32_t k,
                             std::uint8_t row_width) noexcept {
  const auto entries = std::uint64_t{1} << (2U * k);
  return entries * 2U * (row_width / 8U);
}

void ValidateOptions(const FastPrefixIndexOptions& options) {
  if (options.min_k == 0 || options.min_k > options.max_k ||
      options.max_k > kMaximumSupportedK) {
    throw Error(ErrorCode::kBuildFailure,
                "invalid Fast prefix-index k range");
  }
  if (options.memory_budget_basis_points > kBasisPointDenominator) {
    throw Error(ErrorCode::kBuildFailure,
                "Fast prefix-index memory budget exceeds 100 percent");
  }
}

void ValidateReferenceLayout(
    const std::vector<std::uint8_t>& text,
    const std::vector<Position>& contig_starts,
    const std::vector<Position>& contig_lengths) {
  if (text.empty() || text.back() != kSentinel) {
    throw Error(ErrorCode::kBuildFailure,
                "Fast prefix-index text must end in one sentinel");
  }
  if (contig_starts.empty() ||
      contig_starts.size() != contig_lengths.size()) {
    throw Error(ErrorCode::kBuildFailure,
                "Fast prefix-index contig metadata is inconsistent");
  }

  std::uint64_t expected_start = 0;
  for (std::size_t index = 0; index < contig_starts.size(); ++index) {
    const auto start = contig_starts[index];
    const auto length = contig_lengths[index];
    if (start != expected_start || start >= text.size() ||
        length >= text.size() - static_cast<std::size_t>(start)) {
      throw Error(ErrorCode::kBuildFailure,
                  "Fast prefix-index contig metadata is out of range");
    }
    const auto separator = start + length;
    if (text[static_cast<std::size_t>(separator)] != kSeparator) {
      throw Error(ErrorCode::kBuildFailure,
                  "Fast prefix-index contig lacks its separator");
    }
    if (separator == std::numeric_limits<std::uint64_t>::max()) {
      throw Error(ErrorCode::kBuildFailure,
                  "Fast prefix-index contig layout overflows");
    }
    expected_start = separator + 1U;
  }
  if (expected_start + 1U != text.size()) {
    throw Error(ErrorCode::kBuildFailure,
                "Fast prefix-index text contains unowned symbols");
  }
}

std::uint64_t PackInterval32(std::uint32_t begin,
                             std::uint32_t end) noexcept {
  return static_cast<std::uint64_t>(begin) |
         (static_cast<std::uint64_t>(end) << 32U);
}

FastPrefixInterval UnpackInterval32(std::uint64_t packed) noexcept {
  return {static_cast<std::uint32_t>(packed),
          static_cast<std::uint32_t>(packed >> 32U)};
}

template <class Tables>
auto RecordRow(Tables& tables, std::uint64_t key, std::uint64_t row,
               std::uint64_t suffix_count,
               std::uint64_t& nonempty_entries)
    -> decltype(tables.intervals, void()) {
  const auto index = static_cast<std::size_t>(key);
  auto interval = UnpackInterval32(tables.intervals[index]);
  if (interval.begin == suffix_count) {
    tables.intervals[index] = PackInterval32(
        static_cast<std::uint32_t>(row),
        static_cast<std::uint32_t>(row + 1U));
    ++nonempty_entries;
    return;
  }
  interval.begin = std::min(interval.begin, row);
  interval.end = std::max(interval.end, row + 1U);
  tables.intervals[index] = PackInterval32(
      static_cast<std::uint32_t>(interval.begin),
      static_cast<std::uint32_t>(interval.end));
}

template <class Tables>
auto RecordRow(Tables& tables, std::uint64_t key, std::uint64_t row,
               std::uint64_t suffix_count,
               std::uint64_t& nonempty_entries)
    -> decltype(tables.begin, tables.end, void()) {
  using Row = typename std::decay_t<decltype(tables.begin)>::value_type;
  const auto index = static_cast<std::size_t>(key);
  const auto empty = static_cast<Row>(suffix_count);
  if (tables.begin[index] == empty) {
    tables.begin[index] = static_cast<Row>(row);
    tables.end[index] = static_cast<Row>(row + 1U);
    ++nonempty_entries;
    return;
  }
  tables.begin[index] =
      std::min(tables.begin[index], static_cast<Row>(row));
  tables.end[index] =
      std::max(tables.end[index], static_cast<Row>(row + 1U));
}

template <class Values>
std::uint64_t CoordinateAtUnchecked(const Values& values,
                                    std::size_t index) noexcept {
  if constexpr (std::is_same_v<Values, Coordinate40Storage> ||
                std::is_same_v<Values, Coordinate48Storage>) {
    return values.AtUnchecked(index);
  } else {
    return static_cast<std::uint64_t>(values[index]);
  }
}

template <class Tables, class SaValues, class IsaValues>
void Populate(
    Tables& tables, const std::vector<std::uint8_t>& text,
    const SaValues& suffix_array, const IsaValues& inverse_suffix_array,
    const std::vector<Position>& contig_starts,
    const std::vector<Position>& contig_lengths, std::uint32_t k,
    std::uint64_t suffix_count, std::uint64_t& indexed_kmers,
    std::uint64_t& nonempty_entries) {
  const auto key_mask = (std::uint64_t{1} << (2U * k)) - 1U;
  for (std::size_t sequence_id = 0; sequence_id < contig_starts.size();
       ++sequence_id) {
    const auto begin = contig_starts[sequence_id];
    const auto length = contig_lengths[sequence_id];
    std::uint64_t key = 0;
    std::uint32_t valid = 0;
    for (std::uint64_t local = 0; local < length; ++local) {
      const auto symbol = text[static_cast<std::size_t>(begin + local)];
      if (!IsCanonical(symbol)) {
        key = 0;
        valid = 0;
        continue;
      }
      key = ((key << 2U) |
             static_cast<std::uint64_t>(symbol - kA)) &
            key_mask;
      if (valid < k) {
        ++valid;
      }
      if (valid < k) {
        continue;
      }

      const auto suffix = begin + local + 1U - k;
      const auto row = CoordinateAtUnchecked(
          inverse_suffix_array, static_cast<std::size_t>(suffix));
      if (row >= suffix_count ||
          CoordinateAtUnchecked(suffix_array,
                                static_cast<std::size_t>(row)) != suffix) {
        throw Error(ErrorCode::kBuildFailure,
                    "Fast prefix-index SA and ISA are not inverse");
      }
      RecordRow(tables, key, row, suffix_count, nonempty_entries);
      ++indexed_kmers;
    }
  }
}

std::optional<std::uint64_t> EncodeQueryKey(const std::uint8_t* pattern,
                                            std::size_t pattern_size,
                                            std::uint32_t k) noexcept {
  if (pattern == nullptr || pattern_size < k) {
    return std::nullopt;
  }
  std::uint64_t key = 0;
  for (std::uint32_t index = 0; index < k; ++index) {
    const auto symbol = pattern[index];
    if (!IsCanonical(symbol)) {
      return std::nullopt;
    }
    key = (key << 2U) | static_cast<std::uint64_t>(symbol - kA);
  }
  return key;
}

}  // namespace

std::uint32_t FastPrefixIndex::SelectK(
    std::uint64_t suffix_count, std::uint64_t resident_core_bytes,
    const FastPrefixIndexOptions& options) {
  ValidateOptions(options);
  if (suffix_count < options.min_suffix_count ||
      options.memory_budget_basis_points == 0) {
    return 0;
  }
  const auto row_width = RowWidthForSuffixCount(suffix_count);
  const auto budget =
      BudgetBytes(resident_core_bytes, options.memory_budget_basis_points);
  for (auto k = options.max_k;; --k) {
    if (DirectoryBytes(k, row_width) <= budget) {
      return k;
    }
    if (k == options.min_k) {
      break;
    }
  }
  return 0;
}

std::uint8_t FastPrefixIndex::RowWidthForSuffixCount(
    std::uint64_t suffix_count) noexcept {
  return suffix_count <= std::numeric_limits<std::uint32_t>::max() ? 32 : 64;
}

std::uint64_t FastPrefixIndex::ResidentBytesForK(
    std::uint32_t k, std::uint64_t suffix_count) noexcept {
  if (k == 0 || k > kMaximumSupportedK) {
    return 0;
  }
  return DirectoryBytes(k, RowWidthForSuffixCount(suffix_count));
}

FastPrefixIndex FastPrefixIndex::Build(
    const std::vector<std::uint8_t>& text,
    const CoordinateStorage& suffix_array,
    const CoordinateStorage& inverse_suffix_array,
    const std::vector<Position>& contig_starts,
    const std::vector<Position>& contig_lengths,
    std::uint64_t resident_core_bytes,
    const FastPrefixIndexOptions& options) {
  ValidateOptions(options);
  ValidateReferenceLayout(text, contig_starts, contig_lengths);
  const auto suffix_count = suffix_array.Size();
  if (suffix_count != text.size() ||
      inverse_suffix_array.Size() != suffix_count) {
    throw Error(ErrorCode::kBuildFailure,
                "Fast prefix-index requires a complete SA and ISA");
  }

  FastPrefixIndex result;
  result.k_ = SelectK(suffix_count, resident_core_bytes, options);
  if (result.k_ == 0) {
    return result;
  }
  result.row_width_ = RowWidthForSuffixCount(suffix_count);
  result.suffix_count_ = suffix_count;
  const auto entry_count =
      static_cast<std::size_t>(std::uint64_t{1} << (2U * result.k_));

  if (result.row_width_ == 32) {
    Tables32 tables;
    const auto empty = static_cast<std::uint32_t>(suffix_count);
    tables.intervals.assign(entry_count, PackInterval32(empty, empty));
    suffix_array.Visit([&](const auto& sa_values) {
      inverse_suffix_array.Visit([&](const auto& isa_values) {
        Populate(tables, text, sa_values, isa_values, contig_starts,
                 contig_lengths, result.k_, suffix_count,
                 result.indexed_kmers_, result.nonempty_entries_);
      });
    });
    result.tables_ = std::move(tables);
  } else {
    Tables64 tables;
    tables.begin.assign(entry_count, suffix_count);
    tables.end.assign(entry_count, suffix_count);
    suffix_array.Visit([&](const auto& sa_values) {
      inverse_suffix_array.Visit([&](const auto& isa_values) {
        Populate(tables, text, sa_values, isa_values, contig_starts,
                 contig_lengths, result.k_, suffix_count,
                 result.indexed_kmers_, result.nonempty_entries_);
      });
    });
    result.tables_ = std::move(tables);
  }
  return result;
}

std::optional<FastPrefixInterval> FastPrefixIndex::Lookup(
    const std::uint8_t* pattern, std::size_t pattern_size) const {
  if (Empty()) {
    return std::nullopt;
  }
  const auto key = EncodeQueryKey(pattern, pattern_size, k_);
  if (!key) {
    return std::nullopt;
  }
  return std::visit(
      [&](const auto& tables) -> std::optional<FastPrefixInterval> {
        using TablesType = std::decay_t<decltype(tables)>;
        if constexpr (std::is_same_v<TablesType, std::monostate>) {
          return std::nullopt;
        } else if constexpr (std::is_same_v<TablesType, Tables32>) {
          const auto interval = UnpackInterval32(
              tables.intervals[static_cast<std::size_t>(*key)]);
          if (interval.begin == suffix_count_) {
            return FastPrefixInterval{};
          }
          return interval;
        } else {
          const auto index = static_cast<std::size_t>(*key);
          const auto begin = static_cast<std::uint64_t>(tables.begin[index]);
          if (begin == suffix_count_) {
            return FastPrefixInterval{};
          }
          return FastPrefixInterval{
              begin, static_cast<std::uint64_t>(tables.end[index])};
        }
      },
      tables_);
}

std::uint64_t FastPrefixIndex::DirectoryEntries() const noexcept {
  return std::visit(
      [](const auto& tables) -> std::uint64_t {
        using TablesType = std::decay_t<decltype(tables)>;
        if constexpr (std::is_same_v<TablesType, std::monostate>) {
          return 0;
        } else if constexpr (std::is_same_v<TablesType, Tables32>) {
          return tables.intervals.size();
        } else {
          return tables.begin.size();
        }
      },
      tables_);
}

std::uint64_t FastPrefixIndex::ResidentBytes() const noexcept {
  return std::visit(
      [](const auto& tables) -> std::uint64_t {
        using TablesType = std::decay_t<decltype(tables)>;
        if constexpr (std::is_same_v<TablesType, std::monostate>) {
          return 0;
        } else if constexpr (std::is_same_v<TablesType, Tables32>) {
          return static_cast<std::uint64_t>(tables.intervals.size()) *
                 sizeof(std::uint64_t);
        } else {
          using Row =
              typename std::decay_t<decltype(tables.begin)>::value_type;
          return static_cast<std::uint64_t>(tables.begin.size() +
                                            tables.end.size()) *
                 sizeof(Row);
        }
      },
      tables_);
}

}  // namespace sufkit::detail
