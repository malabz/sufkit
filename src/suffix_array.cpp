// SPDX-License-Identifier: MIT

#include "sufkit/suffix_array.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <thread>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "caps_backend.hpp"
#include "divsufsort_backend.hpp"
#include "genome_reference_internal.hpp"
#include "query.hpp"
#include "reference_data.hpp"
#include "sequence_compare.hpp"
#include "serialization.hpp"
#include "suffix_link_diagnostics.hpp"
#include <sufkit/version.hpp>

namespace sufkit {
namespace {

using Sa32 = std::vector<std::int32_t>;
using Sa64 = std::vector<std::int64_t>;
using CapsSa32 = std::vector<std::uint32_t>;
using CapsSa64 = std::vector<std::uint64_t>;
using SaStorage = std::variant<Sa32, Sa64, CapsSa32, CapsSa64>;

using Coordinate32 = std::vector<std::uint32_t>;
using Coordinate64 = std::vector<std::uint64_t>;
using CoordinateStorage = std::variant<Coordinate32, Coordinate64>;
using BuildClock = std::chrono::steady_clock;

struct CoordinateView {
  const void* data = nullptr;
  std::size_t size = 0;
  bool is_64_bit = false;

  std::uint64_t operator[](std::size_t index) const noexcept {
    return is_64_bit
               ? static_cast<const std::uint64_t*>(data)[index]
               : static_cast<const std::uint32_t*>(data)[index];
  }
};

CoordinateView ViewCoordinates(const CoordinateStorage& storage) noexcept {
  return std::visit(
      [](const auto& values) {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        return CoordinateView{values.data(), values.size(),
                              sizeof(Value) == sizeof(std::uint64_t)};
      },
      storage);
}

std::uint64_t CoordinateSize(const CoordinateStorage& storage) noexcept {
  return std::visit(
      [](const auto& values) {
        return static_cast<std::uint64_t>(values.size());
      },
      storage);
}

bool CoordinatesEmpty(const CoordinateStorage& storage) noexcept {
  return std::visit([](const auto& values) { return values.empty(); }, storage);
}

std::uint64_t CoordinateBytes(const CoordinateStorage& storage) noexcept {
  return std::visit(
      [](const auto& values) {
        return static_cast<std::uint64_t>(values.size()) *
               sizeof(typename std::decay_t<decltype(values)>::value_type);
      },
      storage);
}

CoordinateStorage CompactCoordinates(std::vector<std::uint64_t>&& values,
                                     std::uint8_t width,
                                     ErrorCode error_code,
                                     const char* label) {
  if (width == 64) {
    return Coordinate64(std::move(values));
  }
  if (width != 32) {
    throw Error(error_code, std::string("invalid ") + label + " width");
  }
  Coordinate32 compact;
  try {
    compact.resize(values.size());
  } catch (const std::bad_alloc&) {
    throw Error(error_code, std::string("cannot allocate ") + label);
  }
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (values[index] > std::numeric_limits<std::uint32_t>::max()) {
      throw Error(error_code, std::string(label) + " value exceeds 32 bits");
    }
    compact[index] = static_cast<std::uint32_t>(values[index]);
  }
  return CoordinateStorage(std::move(compact));
}

constexpr std::array<std::uint8_t, 256> MakeRightMaximalEncodingTable() {
  std::array<std::uint8_t, 256> table{};
  for (auto& value : table) {
    value = detail::kSentinel;
  }
  table[static_cast<unsigned char>('A')] = detail::kA;
  table[static_cast<unsigned char>('a')] = detail::kA;
  table[static_cast<unsigned char>('C')] = detail::kC;
  table[static_cast<unsigned char>('c')] = detail::kC;
  table[static_cast<unsigned char>('G')] = detail::kG;
  table[static_cast<unsigned char>('g')] = detail::kG;
  table[static_cast<unsigned char>('T')] = detail::kT;
  table[static_cast<unsigned char>('t')] = detail::kT;
  return table;
}

constexpr std::array<std::uint8_t, 7> kRightMaximalComplement = {
    detail::kSentinel, detail::kSentinel, detail::kT, detail::kG,
    detail::kC,        detail::kA,        detail::kSentinel};
constexpr auto kRightMaximalEncoding = MakeRightMaximalEncodingTable();

// A non-owning encoded pattern keeps per-position right-maximal and sampled
// lookups from allocating prefix vectors. Views never escape a synchronous
// query call, so their backing query remains alive for every comparison.
struct EncodedView {
  const std::uint8_t* data = nullptr;
  std::size_t length = 0;

  EncodedView() = default;
  EncodedView(const std::vector<std::uint8_t>& values)
      : data(values.data()), length(values.size()) {}
  EncodedView(const std::uint8_t* values, std::size_t size)
      : data(values), length(size) {}

  const std::uint8_t* begin() const noexcept { return data; }
  const std::uint8_t* end() const noexcept { return data + length; }
  std::size_t size() const noexcept { return length; }
  bool empty() const noexcept { return length == 0; }
  std::uint8_t operator[](std::size_t index) const noexcept {
    return data[index];
  }
  std::uint8_t back() const noexcept { return data[length - 1]; }
};

// Exact locate keeps the globally ordered coordinate until after retention.
// Reference concatenation preserves contig order, so sorting this compact
// representation is equivalent to sorting public (sequence, local) matches.
struct GlobalMatch {
  Position global_position = 0;
  Strand strand = Strand::kForward;
};

bool GlobalMatchLess(const GlobalMatch& left, const GlobalMatch& right) {
  return std::tie(left.global_position, left.strand) <
         std::tie(right.global_position, right.strand);
}

void RetainGlobalMatch(std::vector<GlobalMatch>& matches, GlobalMatch match,
                       const LocateOptions& options, bool& heap_active) {
  if (!options.max_hits) {
    matches.push_back(match);
    return;
  }
  const auto limit = *options.max_hits;
  if (limit == 0) {
    return;
  }
  if (!heap_active && matches.size() < limit) {
    matches.push_back(match);
    return;
  }
  if (!heap_active) {
    std::make_heap(matches.begin(), matches.end(), GlobalMatchLess);
    heap_active = true;
  }
  if (GlobalMatchLess(match, matches.front())) {
    std::pop_heap(matches.begin(), matches.end(), GlobalMatchLess);
    matches.back() = match;
    std::push_heap(matches.begin(), matches.end(), GlobalMatchLess);
  }
}

QueryResult FinalizeGlobalMatches(std::vector<GlobalMatch> globals,
                                  const detail::ReferenceData& reference,
                                  std::uint64_t pattern_length,
                                  std::uint64_t total_hits) {
  std::sort(globals.begin(), globals.end(), GlobalMatchLess);

  QueryResult result;
  result.total_hits = total_hits;
  if (globals.empty()) {
    result.truncated = total_hits != 0;
    return result;
  }

  const auto& starts = reference.contig_starts;
  const auto& lengths = reference.contig_lengths;
  if (starts.empty() || starts.size() != lengths.size()) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array reference coordinates are invalid");
  }

  const auto first = std::upper_bound(starts.begin(), starts.end(),
                                      globals.front().global_position);
  if (first == starts.begin()) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array hit is outside a reference contig");
  }
  std::size_t contig = static_cast<std::size_t>(first - starts.begin() - 1);
  result.hits.reserve(globals.size());
  for (const auto& global : globals) {
    while (contig + 1 < starts.size() &&
           starts[contig + 1] <= global.global_position) {
      ++contig;
    }
    if (global.global_position < starts[contig]) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array hits are not globally ordered");
    }
    const Position local = global.global_position - starts[contig];
    if (local > lengths[contig] ||
        pattern_length > lengths[contig] - local) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array hit is outside a reference contig");
    }
    result.hits.push_back(
        {static_cast<SequenceId>(contig), local, pattern_length,
         global.strand});
  }

  // Both-strand duplicates are adjacent after global-coordinate sorting.
  // Merge in place so delayed mapping does not allocate a second result copy.
  std::size_t output = 0;
  for (auto& match : result.hits) {
    if (output != 0 &&
        result.hits[output - 1].sequence_id == match.sequence_id &&
        result.hits[output - 1].position == match.position &&
        result.hits[output - 1].length == match.length) {
      if (result.hits[output - 1].strand != match.strand) {
        result.hits[output - 1].strand = Strand::kBoth;
      }
      continue;
    }
    const auto input = static_cast<std::size_t>(&match - result.hits.data());
    if (output != input) {
      result.hits[output] = std::move(match);
    }
    ++output;
  }
  result.hits.resize(output);
  result.truncated = result.hits.size() < result.total_hits;
  return result;
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
using WideUnsigned = unsigned __int128;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

double BuildElapsed(BuildClock::time_point begin) {
  return std::chrono::duration<double>(BuildClock::now() - begin).count();
}

detail::ReferenceData MetadataCopy(const detail::ReferenceData& source) {
  detail::ReferenceData result;
  result.sequences = source.sequences;
  result.contig_starts = source.contig_starts;
  result.contig_lengths = source.contig_lengths;
  result.total_bases = source.total_bases;
  result.ambiguous_bases = source.ambiguous_bases;
  result.fingerprint = source.fingerprint;
  return result;
}

int CompareSuffixPattern(const std::vector<std::uint8_t>& text,
                         std::uint64_t suffix, EncodedView pattern) {
  if (suffix >= text.size()) {
    return -1;
  }
  const auto offset = static_cast<std::size_t>(suffix);
  return detail::ComparePatternBytes(text.data() + offset,
                                     text.size() - offset, pattern.data,
                                     pattern.size())
      .order;
}

struct PatternComparison {
  int order = 0;
  std::uint64_t lcp = 0;
};

PatternComparison CompareSuffixPatternLcp(
    const std::vector<std::uint8_t>& text, std::uint64_t suffix,
    EncodedView pattern, std::uint64_t known_lcp,
    SaSearchStatistics* statistics) {
  if (statistics) {
    ++statistics->suffix_comparisons;
  }
  if (suffix >= text.size()) {
    return {-1, 0};
  }
  const auto offset = static_cast<std::size_t>(suffix);
  const auto comparison = detail::ComparePatternBytes(
      text.data() + offset, text.size() - offset, pattern.data, pattern.size(),
      static_cast<std::size_t>(known_lcp));
  if (statistics) {
    statistics->character_comparisons += comparison.comparisons;
  }
  return {comparison.order, comparison.lcp};
}

template <class SaVector>
SuffixRange RangeFor(const std::vector<std::uint8_t>& text,
                     const SaVector& suffix_array, EncodedView pattern,
                     SaSearchStatistics* statistics = nullptr) {
  const auto lower = std::lower_bound(
      suffix_array.begin(), suffix_array.end(), 0, [&](const auto suffix, int) {
        if (!statistics) {
          return CompareSuffixPattern(text, static_cast<std::uint64_t>(suffix),
                                      pattern) < 0;
        }
        return CompareSuffixPatternLcp(text, static_cast<std::uint64_t>(suffix),
                                       pattern, 0, statistics)
                   .order < 0;
      });
  const auto upper = std::upper_bound(
      lower, suffix_array.end(), 0, [&](int, const auto suffix) {
        if (!statistics) {
          return CompareSuffixPattern(text, static_cast<std::uint64_t>(suffix),
                                      pattern) > 0;
        }
        return CompareSuffixPatternLcp(text, static_cast<std::uint64_t>(suffix),
                                       pattern, 0, statistics)
                   .order > 0;
      });
  return {
      static_cast<std::uint64_t>(std::distance(suffix_array.begin(), lower)),
      static_cast<std::uint64_t>(std::distance(suffix_array.begin(), upper))};
}

template <class SaVector>
std::uint64_t LcpBoundaryFor(const std::vector<std::uint8_t>& text,
                             const SaVector& suffix_array,
                             EncodedView pattern,
                             std::uint64_t begin, std::uint64_t end, bool upper,
                             SaSearchStatistics* statistics) {
  std::uint64_t left_lcp = 0;
  std::uint64_t right_lcp = 0;
  while (begin < end) {
    const auto middle = begin + (end - begin) / 2;
    const auto comparison = CompareSuffixPatternLcp(
        text,
        static_cast<std::uint64_t>(
            suffix_array[static_cast<std::size_t>(middle)]),
        pattern, std::min(left_lcp, right_lcp), statistics);
    const bool before =
        comparison.order < 0 || (upper && comparison.order == 0);
    if (before) {
      begin = middle + 1;
      left_lcp = comparison.lcp;
    } else {
      end = middle;
      right_lcp = comparison.lcp;
    }
  }
  return begin;
}

template <class SaVector>
SuffixRange LcpRangeFor(const std::vector<std::uint8_t>& text,
                        const SaVector& suffix_array, EncodedView pattern,
                        SuffixRange search_range,
                        SaSearchStatistics* statistics) {
  const auto lower =
      LcpBoundaryFor(text, suffix_array, pattern, search_range.begin,
                     search_range.end, false, statistics);
  const auto upper = LcpBoundaryFor(text, suffix_array, pattern, lower,
                                    search_range.end, true, statistics);
  return {lower, upper};
}

template <class SaVector>
std::uint64_t GallopingBoundaryFor(const std::vector<std::uint8_t>& text,
                                   const SaVector& suffix_array,
                                   EncodedView pattern,
                                   SuffixRange search_range,
                                   std::uint64_t prediction, bool upper,
                                   SaSearchStatistics* statistics) {
  // A learned prediction is only a hint. Exponential bracketing first proves
  // an ordered window, then the ordinary LCP-aware boundary search finishes
  // inside it; a poor prediction therefore changes cost, never correctness.
  if (search_range.Empty()) {
    return search_range.begin;
  }
  prediction =
      std::max(search_range.begin, std::min(prediction, search_range.end - 1));
  const auto before = [&](std::uint64_t row) {
    if (statistics) {
      ++statistics->gallop_probes;
    }
    const auto comparison = CompareSuffixPatternLcp(
        text,
        static_cast<std::uint64_t>(suffix_array[static_cast<std::size_t>(row)]),
        pattern, 0, statistics);
    return comparison.order < 0 || (upper && comparison.order == 0);
  };

  std::uint64_t begin = search_range.begin;
  std::uint64_t end = search_range.end;
  if (before(prediction)) {
    begin = prediction + 1;
    if (begin == search_range.end) {
      return search_range.end;
    }
    std::uint64_t step = 1;
    while (true) {
      const auto room = search_range.end - 1 - prediction;
      const auto probe =
          step >= room ? search_range.end - 1 : prediction + step;
      if (!before(probe)) {
        end = probe + 1;
        break;
      }
      begin = probe + 1;
      if (probe == search_range.end - 1) {
        return search_range.end;
      }
      step = step > std::numeric_limits<std::uint64_t>::max() / 2
                 ? std::numeric_limits<std::uint64_t>::max()
                 : step * 2;
    }
  } else {
    end = prediction + 1;
    if (prediction != search_range.begin) {
      std::uint64_t step = 1;
      while (true) {
        const auto distance = prediction - search_range.begin;
        const auto probe =
            step >= distance ? search_range.begin : prediction - step;
        if (before(probe)) {
          begin = probe + 1;
          break;
        }
        end = probe + 1;
        if (probe == search_range.begin) {
          begin = search_range.begin;
          break;
        }
        step = step > std::numeric_limits<std::uint64_t>::max() / 2
                   ? std::numeric_limits<std::uint64_t>::max()
                   : step * 2;
      }
    }
  }
  if (statistics) {
    statistics->local_window_rows += end - begin;
    statistics->local_window_rows_max =
        std::max(statistics->local_window_rows_max, end - begin);
  }
  return LcpBoundaryFor(text, suffix_array, pattern, begin, end, upper,
                        statistics);
}

std::uint64_t SaValue(const SaStorage& storage, std::uint64_t row) {
  return std::visit(
      [row](const auto& suffix_array) -> std::uint64_t {
        if (row >= suffix_array.size()) {
          throw Error(ErrorCode::kInvalidInput,
                      "suffix-array row is out of range");
        }
        return static_cast<std::uint64_t>(
            suffix_array[static_cast<std::size_t>(row)]);
      },
      storage);
}

std::uint64_t SaSize(const SaStorage& storage) noexcept {
  return std::visit(
      [](const auto& values) {
        return static_cast<std::uint64_t>(values.size());
      },
      storage);
}

std::uint64_t SampledSuffixCount(std::uint64_t text_size,
                                 std::uint32_t sampling_rate) noexcept {
  return text_size == 0 ? 0 : 1 + (text_size - 1) / sampling_rate;
}

template <class SaVector>
void SampleSaInPlace(SaVector& values, std::uint32_t sampling_rate) {
  if (sampling_rate == 1) {
    return;
  }
  std::size_t output = 0;
  for (const auto value : values) {
    if (static_cast<std::uint64_t>(value) % sampling_rate == 0) {
      values[output++] = value;
    }
  }
  values.resize(output);
  values.shrink_to_fit();
}

template <class SaVector, class LcpVector>
void SampleSaLcpInPlace(SaVector& values, LcpVector& lcp,
                        std::uint32_t sampling_rate) {
  if (sampling_rate == 1) {
    return;
  }
  std::size_t output = 0;
  // Adjacent retained suffixes may have removed rows between them. Their LCP
  // is the minimum complete-SA LCP over that removed interval.
  std::uint64_t interval_min = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t row = 0; row < values.size(); ++row) {
    if (row != 0) {
      interval_min =
          std::min(interval_min, static_cast<std::uint64_t>(lcp[row]));
    }
    if (static_cast<std::uint64_t>(values[row]) % sampling_rate != 0) {
      continue;
    }
    values[output] = values[row];
    lcp[output] = static_cast<typename LcpVector::value_type>(
        output == 0 ? 0 : interval_min);
    ++output;
    interval_min = std::numeric_limits<std::uint64_t>::max();
  }
  values.resize(output);
  values.shrink_to_fit();
  lcp.resize(output);
  lcp.shrink_to_fit();
}

void SampleSa(SaStorage& storage, std::uint32_t sampling_rate) {
  std::visit([&](auto& values) { SampleSaInPlace(values, sampling_rate); },
             storage);
}

void SampleSaLcp(SaStorage& storage, CoordinateStorage& lcp,
                 std::uint32_t sampling_rate) {
  std::visit(
      [&](auto& values, auto& lcp_values) {
        SampleSaLcpInPlace(values, lcp_values, sampling_rate);
      },
      storage, lcp);
}

template <class Coordinate, class SaVector>
std::vector<Coordinate> BuildIsaFor(const SaVector& sa,
                                    std::uint64_t text_size,
                                    std::uint32_t sampling_rate,
                                    std::uint32_t requested_threads) {
  // ISA is the exact inverse of the stored SA. For a sampled index its dense
  // domain is position / sampling_rate, not every logical-text position.
  const auto count = static_cast<std::uint64_t>(sa.size());
  std::vector<Coordinate> isa(static_cast<std::size_t>(count));
  const auto thread_count = std::min<std::uint64_t>(requested_threads, count);
  if (thread_count <= 1 || count < 1U << 20) {
    for (std::uint64_t row = 0; row < count; ++row) {
      const auto suffix = static_cast<std::uint64_t>(
          sa[static_cast<std::size_t>(row)]);
      if (suffix >= text_size || suffix % sampling_rate != 0) {
        throw Error(ErrorCode::kBuildFailure,
                    "sampled suffix array contains an invalid position");
      }
      isa[static_cast<std::size_t>(suffix / sampling_rate)] =
          static_cast<Coordinate>(row);
    }
    return isa;
  }
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(thread_count));
  for (std::uint64_t worker = 0; worker < thread_count; ++worker) {
    const auto begin = count * worker / thread_count;
    const auto end = count * (worker + 1) / thread_count;
    workers.emplace_back([&, begin, end] {
      for (auto row = begin; row < end; ++row) {
        const auto suffix = static_cast<std::uint64_t>(
            sa[static_cast<std::size_t>(row)]);
        isa[static_cast<std::size_t>(suffix / sampling_rate)] =
            static_cast<Coordinate>(row);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  return isa;
}

CoordinateStorage BuildIsa(const SaStorage& sa, std::uint64_t text_size,
                           std::uint32_t sampling_rate,
                           std::uint32_t requested_threads,
                           std::uint8_t coordinate_width) {
  return std::visit(
      [&](const auto& values) -> CoordinateStorage {
        if (coordinate_width == 32) {
          return BuildIsaFor<std::uint32_t>(values, text_size, sampling_rate,
                                            requested_threads);
        }
        if (coordinate_width == 64) {
          return BuildIsaFor<std::uint64_t>(values, text_size, sampling_rate,
                                            requested_threads);
        }
        throw Error(ErrorCode::kBuildFailure,
                    "invalid inverse suffix-array width");
      },
      sa);
}

template <class LcpCoordinate, class SaVector, class IsaVector>
std::vector<LcpCoordinate> BuildLcpFor(
    const std::vector<std::uint8_t>& text, const SaVector& sa,
    const IsaVector& isa, std::uint32_t sampling_rate) {
  std::vector<LcpCoordinate> lcp(sa.size(), 0);
  std::uint64_t common = 0;
  // Generalized Kasai visits sampled text positions K apart. Removing those K
  // leading symbols preserves at least max(common-K, 0) matched symbols.
  for (std::uint64_t sample = 0; sample < isa.size(); ++sample) {
    const auto suffix = sample * sampling_rate;
    const auto row =
        static_cast<std::uint64_t>(isa[static_cast<std::size_t>(sample)]);
    if (row == 0) {
      continue;
    }
    const auto previous = static_cast<std::uint64_t>(
        sa[static_cast<std::size_t>(row - 1)]);
    if (suffix + common < text.size() && previous + common < text.size()) {
      const auto remaining = static_cast<std::size_t>(std::min(
          text.size() - static_cast<std::size_t>(suffix + common),
          text.size() - static_cast<std::size_t>(previous + common)));
      common += detail::LongestCommonPrefixBytes(
          text.data() + static_cast<std::size_t>(suffix + common),
          text.data() + static_cast<std::size_t>(previous + common),
          remaining);
    }
    lcp[static_cast<std::size_t>(row)] =
        static_cast<LcpCoordinate>(common);
    common = common > sampling_rate ? common - sampling_rate : 0;
  }
  return lcp;
}

CoordinateStorage BuildLcp(const std::vector<std::uint8_t>& text,
                           const SaStorage& sa,
                           const CoordinateStorage& isa,
                           std::uint32_t sampling_rate,
                           std::uint8_t lcp_width) {
  return std::visit(
      [&](const auto& sa_values, const auto& isa_values) -> CoordinateStorage {
        if (lcp_width == 32) {
          return BuildLcpFor<std::uint32_t>(text, sa_values, isa_values,
                                            sampling_rate);
        }
        if (lcp_width == 64) {
          return BuildLcpFor<std::uint64_t>(text, sa_values, isa_values,
                                            sampling_rate);
        }
        throw Error(ErrorCode::kBuildFailure, "invalid LCP width");
      },
      sa, isa);
}

struct LearnedSaIndex {
  static constexpr std::uint32_t kModelId = 1;

  std::uint32_t k = 0;
  std::uint32_t bucket_bits = 0;
  std::uint32_t memory_overhead_basis_points = 0;
  std::vector<std::uint64_t> anchor_x;
  CoordinateStorage anchor_y;

  bool Empty() const noexcept { return anchor_x.empty(); }

  std::uint64_t SerializedBytes(std::uint8_t coordinate_width) const noexcept {
    const auto row_bytes = static_cast<std::uint64_t>(coordinate_width / 8);
    return 36ULL +
           static_cast<std::uint64_t>(anchor_x.size()) * (8ULL + row_bytes);
  }

  std::uint64_t KeyFor(EncodedView pattern) const {
    std::uint64_t key = 0;
    for (std::uint32_t index = 0; index < k; ++index) {
      const auto symbol = pattern[static_cast<std::size_t>(index)];
      if (symbol < detail::kA || symbol > detail::kT) {
        throw Error(ErrorCode::kInvalidInput,
                    "learned SA query contains a non-canonical base");
      }
      key = (key << 2U) | static_cast<std::uint64_t>(symbol - detail::kA);
    }
    return key;
  }

  std::uint64_t Predict(std::uint64_t key,
                        std::uint64_t sa_size) const noexcept {
    if (anchor_x.size() < 2 || sa_size == 0) {
      return 0;
    }
    const auto rows = ViewCoordinates(anchor_y);
    const auto key_bits = 2U * k;
    const auto shift = key_bits - bucket_bits;
    const auto bucket = bucket_bits == 0 ? 0ULL : key >> shift;
    const auto index = static_cast<std::size_t>(
        std::min<std::uint64_t>(bucket, anchor_x.size() - 2));
    const auto x_lo = anchor_x[index];
    const auto x_hi = anchor_x[index + 1];
    const auto y_lo = rows[index];
    const auto y_hi = rows[index + 1];
    if (x_hi <= x_lo || y_hi <= y_lo || key <= x_lo) {
      return std::min(y_lo, sa_size - 1);
    }
    if (key >= x_hi) {
      return std::min(y_hi, sa_size - 1);
    }
    const auto numerator =
        static_cast<WideUnsigned>(key - x_lo) * (y_hi - y_lo);
    const auto denominator = x_hi - x_lo;
    const auto interpolated =
        y_lo +
        static_cast<std::uint64_t>((numerator + denominator / 2) / denominator);
    return std::min(interpolated, sa_size - 1);
  }
};

std::uint32_t ChooseBucketBits(std::uint64_t suffix_count,
                               std::uint8_t coordinate_width,
                               const LearnedSaOptions& options) {
  if (options.k == 0 || options.k > 31) {
    throw Error(ErrorCode::kInvalidInput, "learned SA k must be in [1,31]");
  }
  const auto key_bits = 2U * options.k;
  if (options.bucket_bits) {
    if (*options.bucket_bits > key_bits || *options.bucket_bits > 31) {
      throw Error(ErrorCode::kInvalidInput,
                  "learned SA bucket bits are out of range");
    }
    return *options.bucket_bits;
  }
  if (options.memory_overhead_basis_points == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "learned SA memory budget must be greater than zero");
  }
  const auto coordinate_bytes =
      static_cast<std::uint64_t>(coordinate_width / 8);
  const auto sa_bytes =
      static_cast<WideUnsigned>(suffix_count) * coordinate_bytes;
  const auto budget = static_cast<std::uint64_t>(std::min<WideUnsigned>(
      sa_bytes * options.memory_overhead_basis_points / 10000U,
      std::numeric_limits<std::uint64_t>::max()));
  const auto bytes_per_anchor = 8ULL + coordinate_bytes;
  constexpr std::uint64_t kHeaderBytes = 36;
  const auto max_anchors =
      budget > kHeaderBytes ? (budget - kHeaderBytes) / bytes_per_anchor : 0;
  if (max_anchors < 2) {
    throw Error(ErrorCode::kInvalidInput,
                "learned SA memory budget is too small; specify "
                "--learned-bucket-bits for a small reference");
  }
  const auto max_buckets = max_anchors - 1;
  std::uint32_t bits = 0;
  while (bits < key_bits && bits < 31 && (1ULL << (bits + 1U)) <= max_buckets) {
    ++bits;
  }
  return bits;
}

LearnedSaIndex BuildLearnedIndex(const detail::ReferenceData& reference,
                                 const std::vector<std::uint8_t>& text,
                                 const SaStorage& suffix_array,
                                 const CoordinateStorage& isa,
                                 std::uint32_t sampling_rate,
                                 std::uint8_t coordinate_width,
                                 const LearnedSaOptions& options) {
  LearnedSaIndex model;
  model.k = options.k;
  const auto suffix_count = SaSize(suffix_array);
  model.bucket_bits = ChooseBucketBits(suffix_count, coordinate_width, options);
  model.memory_overhead_basis_points = options.memory_overhead_basis_points;
  const auto bucket_count = 1ULL << model.bucket_bits;
  const auto coordinate_max_size =
      coordinate_width == 32 ? Coordinate32().max_size()
                             : Coordinate64().max_size();
  if (bucket_count > coordinate_max_size - 1) {
    throw Error(ErrorCode::kInvalidInput,
                "learned SA bucket count is too large");
  }
  try {
    model.anchor_x.assign(static_cast<std::size_t>(bucket_count + 1),
                          std::numeric_limits<std::uint64_t>::max());
    if (coordinate_width == 32) {
      model.anchor_y = Coordinate32(
          static_cast<std::size_t>(bucket_count + 1),
          static_cast<std::uint32_t>(suffix_count));
    } else {
      model.anchor_y = Coordinate64(
          static_cast<std::size_t>(bucket_count + 1), suffix_count);
    }
  } catch (const std::bad_alloc&) {
    throw Error(ErrorCode::kBuildFailure,
                "cannot allocate learned SA anchor arrays");
  }
  const auto key_bits = 2U * model.k;
  const auto bucket_shift = key_bits - model.bucket_bits;
  const auto key_mask = (1ULL << key_bits) - 1;
  const auto isa_rows = ViewCoordinates(isa);
  bool found = false;

  std::visit(
      [&](auto& anchor_rows) {
        using Row = typename std::decay_t<decltype(anchor_rows)>::value_type;
        // Scan text in contig order so each k-mer reuses the previous rolling
        // key. ISA maps the canonical text position back to the identical SA
        // row used by the former row-order O(n*k) implementation.
        for (std::size_t sequence_id = 0;
             sequence_id < reference.contig_starts.size(); ++sequence_id) {
          const auto begin = reference.contig_starts[sequence_id];
          const auto length = reference.contig_lengths[sequence_id];
          std::uint64_t key = 0;
          std::uint32_t canonical_run = 0;
          for (std::uint64_t local = 0; local < length; ++local) {
            const auto symbol =
                text[static_cast<std::size_t>(begin + local)];
            if (symbol < detail::kA || symbol > detail::kT) {
              key = 0;
              canonical_run = 0;
              continue;
            }
            key = ((key << 2U) |
                   static_cast<std::uint64_t>(symbol - detail::kA)) &
                  key_mask;
            canonical_run = std::min<std::uint32_t>(canonical_run + 1,
                                                    model.k);
            if (canonical_run < model.k) {
              continue;
            }
            const auto start = begin + local + 1 - model.k;
            if (start % sampling_rate != 0) {
              continue;
            }
            const auto sample = start / sampling_rate;
            if (sample >= isa_rows.size) {
              throw Error(ErrorCode::kBuildFailure,
                          "learned SA position is outside ISA");
            }
            const auto row = isa_rows[static_cast<std::size_t>(sample)];
            if (row >= suffix_count) {
              throw Error(ErrorCode::kBuildFailure,
                          "learned SA row is outside the suffix array");
            }
                const auto bucket =
                    model.bucket_bits == 0 ? 0ULL : key >> bucket_shift;
                const auto index = static_cast<std::size_t>(bucket);
                if (key < model.anchor_x[index] ||
                    (key == model.anchor_x[index] &&
                     row < anchor_rows[index])) {
                  model.anchor_x[index] = key;
                  anchor_rows[index] = static_cast<Row>(row);
                }
                found = true;
          }
        }
        if (!found) {
          std::fill(model.anchor_x.begin(), std::prev(model.anchor_x.end()), 0);
          std::fill(anchor_rows.begin(), std::prev(anchor_rows.end()), 0);
          model.anchor_x.back() = 1ULL << key_bits;
          anchor_rows.back() = static_cast<Row>(suffix_count);
          return;
        }

        const auto first = std::find_if(
            model.anchor_x.begin(), std::prev(model.anchor_x.end()),
            [](std::uint64_t value) {
              return value != std::numeric_limits<std::uint64_t>::max();
            });
        const auto first_index = static_cast<std::size_t>(
            std::distance(model.anchor_x.begin(), first));
        for (std::size_t index = 0; index < first_index; ++index) {
          model.anchor_x[index] = 0;
          anchor_rows[index] = anchor_rows[first_index];
        }
        for (std::size_t index = first_index + 1;
             index < model.anchor_x.size() - 1; ++index) {
          if (model.anchor_x[index] ==
              std::numeric_limits<std::uint64_t>::max()) {
            model.anchor_x[index] = model.anchor_x[index - 1];
            anchor_rows[index] = anchor_rows[index - 1];
          }
        }
        model.anchor_x.back() = 1ULL << key_bits;
        anchor_rows.back() = static_cast<Row>(suffix_count);
        for (std::size_t index = 1; index < model.anchor_x.size(); ++index) {
          if (model.anchor_x[index] < model.anchor_x[index - 1] ||
              anchor_rows[index] < anchor_rows[index - 1]) {
            throw Error(ErrorCode::kBuildFailure,
                        "learned SA anchors are not monotonic");
          }
        }
      },
      model.anchor_y);
  return model;
}

template <class ChildCoordinate, class LcpVector>
std::vector<ChildCoordinate> BuildChildFor(const LcpVector& lcp) {
  const std::uint64_t count = lcp.size();
  const std::uint64_t none = count;
  std::vector<ChildCoordinate> child(static_cast<std::size_t>(count),
                                     static_cast<ChildCoordinate>(none));
  // The first monotone-stack pass records up/down links for nested LCP
  // intervals. `none` is a one-past-end sentinel, never a valid SA row.
  std::vector<std::uint64_t> stack{0};
  for (std::uint64_t index = 1; index < count; ++index) {
    const auto current = lcp[static_cast<std::size_t>(index)];
    std::uint64_t last = none;
    while (!stack.empty() &&
           current < lcp[static_cast<std::size_t>(stack.back())]) {
      last = stack.back();
      stack.pop_back();
      if (!stack.empty() &&
          current <= lcp[static_cast<std::size_t>(stack.back())] &&
          lcp[static_cast<std::size_t>(stack.back())] !=
              lcp[static_cast<std::size_t>(last)]) {
        child[static_cast<std::size_t>(stack.back())] =
            static_cast<ChildCoordinate>(last);
      }
    }
    if (last != none) {
      child[static_cast<std::size_t>(index - 1)] =
          static_cast<ChildCoordinate>(last);
    }
    stack.push_back(index);
  }
  while (!stack.empty() && lcp[static_cast<std::size_t>(stack.back())] > 0) {
    const auto last = stack.back();
    stack.pop_back();
    if (!stack.empty() && lcp[static_cast<std::size_t>(stack.back())] !=
                              lcp[static_cast<std::size_t>(last)]) {
      child[static_cast<std::size_t>(stack.back())] =
          static_cast<ChildCoordinate>(last);
    }
  }
  stack.clear();
  stack.push_back(0);
  // Equal LCP values form sibling intervals; this pass installs next-L links
  // without changing the compact one-entry-per-row representation.
  for (std::uint64_t index = 1; index < count; ++index) {
    while (!stack.empty() && lcp[static_cast<std::size_t>(index)] <
                                 lcp[static_cast<std::size_t>(stack.back())]) {
      stack.pop_back();
    }
    if (!stack.empty() && lcp[static_cast<std::size_t>(index)] ==
                              lcp[static_cast<std::size_t>(stack.back())]) {
      const auto last = stack.back();
      stack.pop_back();
      child[static_cast<std::size_t>(last)] =
          static_cast<ChildCoordinate>(index);
    }
    stack.push_back(index);
  }
  return child;
}

CoordinateStorage BuildChild(const CoordinateStorage& lcp,
                             std::uint8_t coordinate_width) {
  return std::visit(
      [&](const auto& lcp_values) -> CoordinateStorage {
        if (coordinate_width == 32) {
          return BuildChildFor<std::uint32_t>(lcp_values);
        }
        if (coordinate_width == 64) {
          return BuildChildFor<std::uint64_t>(lcp_values);
        }
        throw Error(ErrorCode::kBuildFailure, "invalid CHILD width");
      },
      lcp);
}

std::vector<std::uint8_t> EncodeRightMaximalQuery(std::string_view query) {
  std::vector<std::uint8_t> encoded;
  encoded.resize(query.size());
  for (std::size_t index = 0; index < query.size(); ++index) {
    // The impossible query sentinel makes every non-ACGT byte a hard break.
    encoded[index] =
        kRightMaximalEncoding[static_cast<unsigned char>(query[index])];
  }
  return encoded;
}

std::vector<std::uint8_t> ReverseComplementRightMaximal(
    const std::vector<std::uint8_t>& query) {
  std::vector<std::uint8_t> result;
  result.resize(query.size());
  for (std::size_t index = 0; index < query.size(); ++index) {
    const auto symbol = query[query.size() - index - 1];
    result[index] = symbol < kRightMaximalComplement.size()
                        ? kRightMaximalComplement[symbol]
                        : detail::kSentinel;
  }
  return result;
}

bool RightMaximalMatchLess(const RightMaximalMatch& left,
                           const RightMaximalMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) <
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

bool MemMatchLess(const MemMatch& left, const MemMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) <
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

bool MamMatchLess(const MamMatch& left, const MamMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) <
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

RightMaximalSearchAlgorithm AsRightMaximalAlgorithm(
    MemSearchAlgorithm algorithm) {
  static_assert(static_cast<std::uint8_t>(MemSearchAlgorithm::kFull) ==
                static_cast<std::uint8_t>(
                    RightMaximalSearchAlgorithm::kFull));
  return static_cast<RightMaximalSearchAlgorithm>(algorithm);
}

void PrepareMemSearch(const MemOptions& options) {
  if (options.min_length == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "MEM minimum length must be greater than zero");
  }
  if (options.skip_multiplier && *options.skip_multiplier == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "MEM skip multiplier must be greater than zero");
  }
}

void PrepareMamSearch(const MamOptions& options) {
  if (options.min_length == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "reference-MAM minimum length must be greater than zero");
  }
}

void PrepareRightMaximalSearch(const RightMaximalOptions& options) {
  if (options.min_length == 0) {
    throw Error(
        ErrorCode::kInvalidInput,
        "right-maximal exact match minimum length must be greater than zero");
  }
  if (options.statistics) {
    *options.statistics = {};
  }
}

IndexInfo BuiltInfo(const detail::ReferenceData& data,
                    detail::StoredBackend backend, std::uint8_t width,
                    std::uint64_t text_symbols, std::uint64_t suffix_count,
                    std::uint32_t sampling_rate, SaAcceleration acceleration,
                    std::uint64_t auxiliary_bytes,
                    const LearnedSaIndex& learned) {
  IndexInfo info;
  info.kind = IndexKind::kSuffixArray;
  info.format_version =
      sampling_rate > 1 ? "1.3"
      : !learned.Empty()
          ? "1.2"
          : (acceleration == SaAcceleration::kNone ? "1.0" : "1.1");
  info.library_version = SUFKIT_VERSION_STRING;
  info.backend = detail::StoredBackendName(backend);
  info.backend_signature = detail::StoredBackendSignature(backend);
  info.coordinate_width = width;
  info.sequence_count = data.sequences.size();
  info.total_bases = data.total_bases;
  info.text_symbols = text_symbols;
  info.suffix_count = suffix_count;
  info.sa_sampling_rate = sampling_rate;
  info.ambiguous_bases = data.ambiguous_bases;
  info.fingerprint = data.fingerprint;
  info.sa_acceleration = acceleration;
  info.auxiliary_bytes = auxiliary_bytes;
  if (!learned.Empty()) {
    info.sa_lookup_acceleration = SaLookupAcceleration::kSaplingPwl;
    info.learned_index_bytes = learned.SerializedBytes(width);
    info.learned_k = learned.k;
    info.learned_bucket_bits = learned.bucket_bits;
    info.learned_memory_overhead_basis_points =
        learned.memory_overhead_basis_points;
  }
  return info;
}

constexpr std::size_t kIntegerIoBufferBytes = 256U * 1024U;

bool NativeLittleEndian() noexcept {
  const std::uint16_t marker = 1;
  return *reinterpret_cast<const std::uint8_t*>(&marker) == 1;
}

template <class Values>
void WriteIntegerPayload(std::ostream& output, const Values& values,
                         std::uint8_t width, const char* label) {
  if (width != 32 && width != 64) {
    throw Error(ErrorCode::kBuildFailure,
                std::string("invalid ") + label + " width");
  }
  const std::size_t bytes_per_value = width / 8;
  const auto values_per_block = kIntegerIoBufferBytes / bytes_per_value;
  using Value = typename Values::value_type;
  if (NativeLittleEndian() && sizeof(Value) == bytes_per_value) {
    for (std::size_t begin = 0; begin < values.size();
         begin += values_per_block) {
      const auto count = std::min(values_per_block, values.size() - begin);
      const auto bytes = count * bytes_per_value;
      output.write(reinterpret_cast<const char*>(values.data() + begin),
                   static_cast<std::streamsize>(bytes));
      if (!output) {
        throw Error(ErrorCode::kIoError,
                    std::string("failed to write ") + label);
      }
    }
    return;
  }

  std::vector<unsigned char> buffer(values_per_block * bytes_per_value);
  for (std::size_t begin = 0; begin < values.size();
       begin += values_per_block) {
    const auto count = std::min(values_per_block, values.size() - begin);
    for (std::size_t index = 0; index < count; ++index) {
      const auto value =
          static_cast<std::uint64_t>(values[begin + index]);
      for (std::size_t byte = 0; byte < bytes_per_value; ++byte) {
        buffer[index * bytes_per_value + byte] =
            static_cast<unsigned char>((value >> (8U * byte)) & 0xffU);
      }
    }
    const auto bytes = count * bytes_per_value;
    output.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(bytes));
    if (!output) {
      throw Error(ErrorCode::kIoError,
                  std::string("failed to write ") + label);
    }
  }
}

struct AcceptAnyInteger {
  void operator()(std::uint64_t, std::uint64_t) const noexcept {}
};

template <class Values, class Validator = AcceptAnyInteger>
void ReadIntegerPayload(std::istream& input, Values& values,
                        std::uint8_t width, const char* label,
                        Validator&& validator = {}) {
  if (width != 32 && width != 64) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string("invalid ") + label + " width");
  }
  const std::size_t bytes_per_value = width / 8;
  const auto values_per_block = kIntegerIoBufferBytes / bytes_per_value;
  using Value = typename Values::value_type;
  if (NativeLittleEndian() && sizeof(Value) == bytes_per_value) {
    for (std::size_t begin = 0; begin < values.size();
         begin += values_per_block) {
      const auto block_count =
          std::min(values_per_block, values.size() - begin);
      const auto bytes = block_count * bytes_per_value;
      input.read(reinterpret_cast<char*>(values.data() + begin),
                 static_cast<std::streamsize>(bytes));
      if (input.gcount() != static_cast<std::streamsize>(bytes)) {
        throw Error(ErrorCode::kCorruptIndex,
                    std::string(label) + " is truncated");
      }
      for (std::size_t index = 0; index < block_count; ++index) {
        validator(static_cast<std::uint64_t>(begin + index),
                  static_cast<std::uint64_t>(values[begin + index]));
      }
    }
    return;
  }

  std::vector<unsigned char> buffer(values_per_block * bytes_per_value);
  std::size_t begin = 0;
  while (begin < values.size()) {
    const auto block_count =
        std::min(values_per_block, values.size() - begin);
    const auto bytes = block_count * bytes_per_value;
    input.read(reinterpret_cast<char*>(buffer.data()),
               static_cast<std::streamsize>(bytes));
    if (input.gcount() != static_cast<std::streamsize>(bytes)) {
      throw Error(ErrorCode::kCorruptIndex,
                  std::string(label) + " is truncated");
    }
    for (std::size_t index = 0; index < block_count; ++index) {
      std::uint64_t value = 0;
      for (std::size_t byte = 0; byte < bytes_per_value; ++byte) {
        value |= static_cast<std::uint64_t>(
                     buffer[index * bytes_per_value + byte])
                 << (8U * byte);
      }
      validator(static_cast<std::uint64_t>(begin + index), value);
      values[begin + index] = static_cast<Value>(value);
    }
    begin += block_count;
  }
}

void WriteLearnedIndex(std::ostream& output, const LearnedSaIndex& model,
                       std::uint8_t coordinate_width,
                       std::uint64_t fingerprint) {
  detail::WriteU32(output, LearnedSaIndex::kModelId);
  detail::WriteU32(output, model.k);
  detail::WriteU32(output, model.bucket_bits);
  detail::WriteU32(output, model.memory_overhead_basis_points);
  detail::WriteU32(output, coordinate_width);
  detail::WriteU64(output, model.anchor_x.size());
  detail::WriteU64(output, fingerprint);
  WriteIntegerPayload(output, model.anchor_x, 64, "learned SA anchor keys");
  std::visit(
      [&](const auto& rows) {
        WriteIntegerPayload(output, rows, coordinate_width,
                            "learned SA anchor rows");
      },
      model.anchor_y);
  if (!output) {
    throw Error(ErrorCode::kIoError, "failed to write learned SA index");
  }
}

LearnedSaIndex ReadLearnedIndex(const detail::ParsedContainer& container,
                                std::uint64_t suffix_count) {
  auto input =
      detail::OpenSectionStream(container, detail::SectionType::kLearnedSa);
  if (detail::ReadU32(*input, "learned SA model ID") !=
      LearnedSaIndex::kModelId) {
    throw Error(ErrorCode::kUnsupportedBackend,
                "unsupported learned SA model ID");
  }
  LearnedSaIndex model;
  model.k = detail::ReadU32(*input, "learned SA k");
  model.bucket_bits = detail::ReadU32(*input, "learned SA bucket bits");
  model.memory_overhead_basis_points =
      detail::ReadU32(*input, "learned SA memory budget");
  const auto coordinate_width =
      detail::ReadU32(*input, "learned SA coordinate width");
  const auto anchor_count = detail::ReadU64(*input, "learned SA anchor count");
  const auto fingerprint = detail::ReadU64(*input, "learned SA fingerprint");
  if (container.spec.format_minor < 2 || model.k == 0 || model.k > 31 ||
      model.bucket_bits > 2U * model.k || model.bucket_bits > 31 ||
      coordinate_width != container.spec.coordinate_width ||
      anchor_count != (1ULL << model.bucket_bits) + 1 ||
      fingerprint != container.spec.fingerprint ||
      anchor_count > (coordinate_width == 32 ? Coordinate32().max_size()
                                             : Coordinate64().max_size())) {
    throw Error(ErrorCode::kCorruptIndex, "invalid learned SA header");
  }
  model.anchor_x.resize(static_cast<std::size_t>(anchor_count));
  if (coordinate_width == 32) {
    model.anchor_y = Coordinate32(static_cast<std::size_t>(anchor_count));
  } else {
    model.anchor_y = Coordinate64(static_cast<std::size_t>(anchor_count));
  }
  ReadIntegerPayload(*input, model.anchor_x, 64,
                     "learned SA anchor keys");
  std::visit(
      [&](auto& rows) {
        ReadIntegerPayload(*input, rows,
                           static_cast<std::uint8_t>(coordinate_width),
                           "learned SA anchor rows");
      },
      model.anchor_y);
  if (input->peek() != std::char_traits<char>::eof()) {
    throw Error(ErrorCode::kCorruptIndex,
                "learned SA section has trailing bytes");
  }
  const auto anchor_rows = ViewCoordinates(model.anchor_y);
  if (model.anchor_x.back() != (1ULL << (2U * model.k)) ||
      anchor_rows[anchor_rows.size - 1] != suffix_count) {
    throw Error(ErrorCode::kCorruptIndex,
                "learned SA terminal anchor is invalid");
  }
  for (std::size_t index = 0; index < model.anchor_x.size(); ++index) {
    if (anchor_rows[index] > suffix_count ||
        (index != 0 && (model.anchor_x[index] < model.anchor_x[index - 1] ||
                        anchor_rows[index] < anchor_rows[index - 1]))) {
      throw Error(ErrorCode::kCorruptIndex, "learned SA anchors are invalid");
    }
  }
  return model;
}

void WriteIntegerVector(std::ostream& output,
                        const CoordinateStorage& storage,
                        std::uint8_t width) {
  detail::WriteU64(output, CoordinateSize(storage));
  output.put(static_cast<char>(width));
  std::visit(
      [&](const auto& values) {
        WriteIntegerPayload(output, values, width, "auxiliary index data");
      },
      storage);
  if (!output) {
    throw Error(ErrorCode::kIoError, "failed to write auxiliary index data");
  }
}

CoordinateStorage ReadIntegerVector(
    const detail::ParsedContainer& container, detail::SectionType type,
    std::uint64_t expected_count, std::uint8_t expected_width,
    const char* label) {
  auto input = detail::OpenSectionStream(container, type);
  const auto count = detail::ReadU64(*input, label);
  const int raw_width = input->get();
  if (count != expected_count || raw_width == std::char_traits<char>::eof() ||
      static_cast<std::uint8_t>(raw_width) != expected_width ||
      (expected_width != 32 && expected_width != 64)) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string("invalid ") + label + " header");
  }
  CoordinateStorage storage =
      expected_width == 32
          ? CoordinateStorage(Coordinate32(static_cast<std::size_t>(count)))
          : CoordinateStorage(Coordinate64(static_cast<std::size_t>(count)));
  std::visit(
      [&](auto& values) {
        ReadIntegerPayload(*input, values, expected_width, label);
      },
      storage);
  if (input->peek() != std::char_traits<char>::eof()) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(label) + " section has trailing bytes");
  }
  return storage;
}

}  // namespace

struct SuffixArray::Impl {
  detail::ReferenceData reference;
  std::vector<std::uint8_t> text;
  SaStorage suffix_array;
  CoordinateStorage isa;
  CoordinateStorage lcp;
  CoordinateStorage child;
  LearnedSaIndex learned;
  SaAcceleration acceleration = SaAcceleration::kNone;
  std::uint32_t sampling_rate = 1;
  detail::StoredBackend backend = detail::StoredBackend::kDivsufsort32;
  IndexInfo index_info;

  bool HasIsa() const noexcept { return !CoordinatesEmpty(isa); }
  bool HasLcp() const noexcept { return !CoordinatesEmpty(lcp); }
  bool HasChild() const noexcept { return !CoordinatesEmpty(child); }
  bool HasLearned() const noexcept { return !learned.Empty(); }

  SuffixRange BinaryRange(EncodedView pattern,
                          SaSearchStatistics* statistics = nullptr) const {
    return std::visit(
        [&](const auto& values) {
          return RangeFor(text, values, pattern, statistics);
        },
        suffix_array);
  }

  SuffixRange LcpBinaryRange(EncodedView pattern,
                             SaSearchStatistics* statistics = nullptr) const {
    return std::visit(
        [&](const auto& values) {
          return LcpRangeFor(text, values, pattern,
                             {0, static_cast<std::uint64_t>(values.size())},
                             statistics);
        },
        suffix_array);
  }

  SuffixRange LearnedRange(EncodedView pattern,
                           SaSearchStatistics* statistics = nullptr) const {
    if (!HasLearned()) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  "Sapling PWL data is unavailable in this index");
    }
    if (pattern.size() < learned.k) {
      // The model key requires k bases; a full binary search is the exact and
      // deterministic fallback for shorter patterns.
      if (statistics) {
        ++statistics->full_binary_fallbacks;
      }
      return BinaryRange(pattern, statistics);
    }
    const EncodedView prefix(pattern.data, learned.k);
    const auto prediction =
        learned.Predict(learned.KeyFor(prefix), SaSize(suffix_array));
    const auto prefix_range = std::visit(
        [&](const auto& values) {
          const SuffixRange whole{0, static_cast<std::uint64_t>(values.size())};
          const auto lower = GallopingBoundaryFor(
              text, values, prefix, whole, prediction, false, statistics);
          const auto upper =
              GallopingBoundaryFor(text, values, prefix, {lower, whole.end},
                                   prediction, true, statistics);
          return SuffixRange{lower, upper};
        },
        suffix_array);
    if (statistics) {
      const auto error = prediction > prefix_range.begin
                             ? prediction - prefix_range.begin
                             : prefix_range.begin - prediction;
      ++statistics->predictions;
      statistics->prediction_absolute_error_sum += error;
      statistics->prediction_absolute_error_max =
          std::max(statistics->prediction_absolute_error_max, error);
    }
    if (prefix_range.Empty() || pattern.size() == learned.k) {
      return prefix_range;
    }
    return std::visit(
        [&](const auto& values) {
          return LcpRangeFor(text, values, pattern, prefix_range, statistics);
        },
        suffix_array);
  }

  std::uint8_t SymbolAt(std::uint64_t row, std::uint64_t depth) const {
    const auto suffix = SaValue(suffix_array, row);
    if (suffix + depth >= text.size()) {
      return detail::kSentinel;
    }
    return text[static_cast<std::size_t>(suffix + depth)];
  }

  SuffixRange NarrowChar(SuffixRange range, std::uint64_t depth,
                         std::uint8_t symbol) const {
    auto lower = range.begin;
    auto upper = range.end;
    while (lower < upper) {
      const auto middle = lower + (upper - lower) / 2;
      if (SymbolAt(middle, depth) < symbol) {
        lower = middle + 1;
      } else {
        upper = middle;
      }
    }
    const auto begin = lower;
    upper = range.end;
    while (lower < upper) {
      const auto middle = lower + (upper - lower) / 2;
      if (SymbolAt(middle, depth) <= symbol) {
        lower = middle + 1;
      } else {
        upper = middle;
      }
    }
    return begin == lower ? SuffixRange{} : SuffixRange{begin, lower};
  }

  std::uint64_t IntervalDepth(SuffixRange range) const {
    if (range.Size() <= 1) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    const auto rows = SaSize(suffix_array);
    if (range.begin == 0 && range.end == rows) {
      return 0;
    }
    const auto none = rows;
    const auto child_values = ViewCoordinates(child);
    const auto lcp_values = ViewCoordinates(lcp);
    auto boundary = child_values[static_cast<std::size_t>(range.end - 1)];
    if (boundary == none || boundary <= range.begin || boundary >= range.end) {
      boundary = child_values[static_cast<std::size_t>(range.begin)];
    }
    if (boundary == none || boundary <= range.begin || boundary >= range.end) {
      return 0;
    }
    return lcp_values[static_cast<std::size_t>(boundary)];
  }

  SuffixRange ChildStep(SuffixRange range, std::uint64_t depth,
                        std::uint8_t symbol) const {
    // NarrowChar defines the exact answer. CHILD navigation is accepted only
    // when it reproduces that interval, keeping auxiliary data an optimization
    // rather than a correctness dependency.
    const auto verified = NarrowChar(range, depth, symbol);
    if (!HasChild() || range.Size() <= 1) {
      return verified;
    }
    const auto none = SaSize(suffix_array);
    const auto child_values = ViewCoordinates(child);
    auto first = child_values[static_cast<std::size_t>(range.end - 1)];
    if (first == none || first <= range.begin || first >= range.end) {
      first = child_values[static_cast<std::size_t>(range.begin)];
    }
    if (first == none || first <= range.begin || first >= range.end) {
      return verified;
    }
    std::uint64_t left = range.begin;
    std::uint64_t right = first;
    while (true) {
      if (SymbolAt(left, depth) == symbol) {
        const SuffixRange candidate{left, right};
        return candidate.begin == verified.begin &&
                       candidate.end == verified.end
                   ? candidate
                   : verified;
      }
      left = right;
      if (left >= range.end) {
        break;
      }
      const auto next = child_values[static_cast<std::size_t>(left)];
      right =
          next != none && next > left && next < range.end ? next : range.end;
    }
    return verified;
  }

  SuffixRange ChildRange(EncodedView pattern) const {
    SuffixRange range{0, SaSize(suffix_array)};
    std::uint64_t depth = 0;
    while (depth < pattern.size() && !range.Empty()) {
      if (range.Size() == 1) {
        const auto suffix = SaValue(suffix_array, range.begin);
        while (depth < pattern.size() && suffix + depth < text.size() &&
               text[static_cast<std::size_t>(suffix + depth)] ==
                   pattern[static_cast<std::size_t>(depth)]) {
          ++depth;
        }
        return depth == pattern.size() ? range : SuffixRange{};
      }
      const auto node_depth = IntervalDepth(range);
      const auto branch_end =
          std::min<std::uint64_t>(node_depth, pattern.size());
      const auto representative = SaValue(suffix_array, range.begin);
      while (depth < branch_end) {
        if (representative + depth >= text.size() ||
            text[static_cast<std::size_t>(representative + depth)] !=
                pattern[static_cast<std::size_t>(depth)]) {
          return {};
        }
        ++depth;
      }
      if (depth == pattern.size()) {
        return range;
      }
      range = ChildStep(range, depth, pattern[static_cast<std::size_t>(depth)]);
      ++depth;
    }
    return depth == pattern.size() ? range : SuffixRange{};
  }

  SaSearchAlgorithm ResolveSearchAlgorithm(SaSearchAlgorithm requested,
                                           std::size_t pattern_length) const {
    if (requested == SaSearchAlgorithm::kAutoSelect) {
      return HasLearned() && pattern_length >= learned.k
                 ? SaSearchAlgorithm::kSaplingPwl
                 : SaSearchAlgorithm::kBinary;
    }
    if (requested == SaSearchAlgorithm::kSaplingPwl && !HasLearned()) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  "Sapling PWL data is unavailable in this index");
    }
    if (requested == SaSearchAlgorithm::kChild && !HasChild()) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  "CHILD data is unavailable in this index");
    }
    return requested;
  }

  SuffixRange Range(
      EncodedView pattern,
      SaSearchAlgorithm requested = SaSearchAlgorithm::kAutoSelect,
      SaSearchStatistics* statistics = nullptr) const {
    SuffixRange result;
    switch (ResolveSearchAlgorithm(requested, pattern.size())) {
      case SaSearchAlgorithm::kAutoSelect:
        break;
      case SaSearchAlgorithm::kBinary:
        result = BinaryRange(pattern, statistics);
        break;
      case SaSearchAlgorithm::kLcpBinary:
        result = LcpBinaryRange(pattern, statistics);
        break;
      case SaSearchAlgorithm::kSaplingPwl:
        result = LearnedRange(pattern, statistics);
        break;
      case SaSearchAlgorithm::kChild:
        result = ChildRange(pattern);
        break;
      default:
        throw Error(ErrorCode::kInvalidInput, "invalid SA search algorithm");
    }
    if (result.Empty()) {
      return {};
    }
    return result;
  }

  template <class Callback>
  void ForEachStoredRow(SuffixRange interval, Callback&& callback) const {
    std::visit(
        [&](const auto& values) {
          for (std::uint64_t row = interval.begin; row < interval.end; ++row) {
            callback(row, static_cast<std::uint64_t>(
                              values[static_cast<std::size_t>(row)]));
          }
        },
        suffix_array);
  }

  template <class Callback>
  void ForEachStoredPosition(SuffixRange interval,
                             Callback&& callback) const {
    ForEachStoredRow(interval, [&](std::uint64_t, std::uint64_t position) {
      callback(position);
    });
  }

  template <class Callback>
  void ForEachExactGlobal(EncodedView pattern,
                          SaSearchAlgorithm algorithm,
                          SaSearchStatistics* statistics,
                          Callback&& callback) const {
    if (sampling_rate == 1) {
      const auto interval = Range(pattern, algorithm, statistics);
      ForEachStoredPosition(interval, callback);
      return;
    }

    if (pattern.size() < sampling_rate) {
      for (std::size_t sequence_id = 0;
           sequence_id < reference.contig_starts.size(); ++sequence_id) {
        const auto sequence_length = reference.contig_lengths[sequence_id];
        if (pattern.size() > sequence_length) {
          continue;
        }
        for (std::uint64_t local = 0;
             local + pattern.size() <= sequence_length; ++local) {
          const auto global = reference.contig_starts[sequence_id] + local;
          if (std::equal(pattern.begin(), pattern.end(),
                         text.begin() + static_cast<std::ptrdiff_t>(global))) {
            callback(global);
          }
        }
      }
      return;
    }

    // Every full match has exactly one offset modulo K whose shifted suffix is
    // stored. Verify the skipped prefix and contig containment before emitting
    // the recovered unsampled coordinate.
    for (std::uint32_t offset = 0; offset < sampling_rate; ++offset) {
      const EncodedView anchor(pattern.data + offset, pattern.size() - offset);
      const auto interval = Range(anchor, algorithm, statistics);
      ForEachStoredPosition(interval, [&](std::uint64_t sampled) {
        if (sampled < offset) {
          return;
        }
        const auto global = sampled - offset;
        if (!detail::MapGlobalPosition(reference, global, pattern.size())) {
          return;
        }
        if (std::equal(pattern.begin(),
                       pattern.begin() + static_cast<std::ptrdiff_t>(offset),
                       text.begin() + static_cast<std::ptrdiff_t>(global))) {
          callback(global);
        }
      });
    }
  }

  std::uint64_t ExactCount(EncodedView pattern,
                           SaSearchAlgorithm algorithm,
                           SaSearchStatistics* statistics) const {
    if (sampling_rate == 1) {
      return Range(pattern, algorithm, statistics).Size();
    }
    std::uint64_t count = 0;
    ForEachExactGlobal(pattern, algorithm, statistics,
                       [&](std::uint64_t) { ++count; });
    return count;
  }

  std::uint64_t Collect(EncodedView pattern, Strand strand,
                        const LocateOptions& options,
                        std::vector<GlobalMatch>& output, bool& heap_active,
                        SaSearchAlgorithm algorithm,
                        SaSearchStatistics* statistics) const {
    if (sampling_rate == 1) {
      const auto interval = Range(pattern, algorithm, statistics);
      if (options.max_hits && *options.max_hits == 0) {
        return interval.Size();
      }
      if (!options.max_hits) {
        const auto available = output.max_size() - output.size();
        if (interval.Size() <= available) {
          output.reserve(output.size() +
                         static_cast<std::size_t>(interval.Size()));
        }
      } else {
        // A caller-provided limit may be much larger than the number of hits.
        // Reserve only what this interval can contribute so an absent or
        // sparse pattern cannot trigger an enormous speculative allocation.
        const auto maximum = std::min<std::uint64_t>(
            *options.max_hits,
            static_cast<std::uint64_t>(output.max_size()));
        const auto current = static_cast<std::uint64_t>(output.size());
        const auto additional =
            current < maximum
                ? std::min(interval.Size(), maximum - current)
                : std::uint64_t{0};
        const auto desired = current + additional;
        if (desired > output.capacity()) {
          output.reserve(static_cast<std::size_t>(desired));
        }
      }
      ForEachStoredPosition(interval, [&](std::uint64_t global) {
        RetainGlobalMatch(output, {global, strand}, options, heap_active);
      });
      return interval.Size();
    }

    std::uint64_t total = 0;
    ForEachExactGlobal(
        pattern, algorithm, statistics, [&](std::uint64_t global) {
          RetainGlobalMatch(output, {global, strand}, options, heap_active);
          ++total;
        });
    return total;
  }

  RightMaximalSearchAlgorithm ResolveAlgorithm(
      RightMaximalSearchAlgorithm requested) const {
    if (requested == RightMaximalSearchAlgorithm::kAutoSelect) {
      if (HasIsa() && HasLcp()) {
        return RightMaximalSearchAlgorithm::kSuffixLink;
      }
      if (HasLcp()) {
        return RightMaximalSearchAlgorithm::kLcp;
      }
      return RightMaximalSearchAlgorithm::kBaseline;
    }
    const bool supported =
        requested == RightMaximalSearchAlgorithm::kBaseline ||
        (requested == RightMaximalSearchAlgorithm::kLcp && HasLcp()) ||
        (requested == RightMaximalSearchAlgorithm::kChild && HasLcp() &&
         HasChild()) ||
        (requested == RightMaximalSearchAlgorithm::kSuffixLink && HasIsa() &&
         HasLcp()) ||
        (requested == RightMaximalSearchAlgorithm::kFull && HasIsa() &&
         HasLcp() && HasChild());
    if (!supported) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  std::string("right-maximal exact match algorithm is "
                              "unavailable in this index: ") +
                      ToString(requested));
    }
    return requested;
  }

  template <class SaVector, class IsaVector, class LcpVector>
  SuffixRange SuffixLinkIntervalFor(
      const SaVector& sa_values, const IsaVector& isa_values,
      const LcpVector& lcp_values, SuffixRange previous, std::uint64_t depth,
      std::uint32_t shift, detail::SuffixLinkScanSink* scan_sink) const {
    // Shift both interval endpoints through ISA, then expand across LCP values
    // that still support the shortened prefix. Empty means reuse was not
    // proven; callers must restart from the root search path.
    if (previous.Empty() || depth <= shift) {
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
      if (scan_sink) {
        scan_sink->Record(0, 0, 0);
      }
#endif
      return {};
    }
    const auto left_suffix = static_cast<std::uint64_t>(
        sa_values[static_cast<std::size_t>(previous.begin)]);
    const auto right_suffix = static_cast<std::uint64_t>(
        sa_values[static_cast<std::size_t>(previous.end - 1)]);
    if (left_suffix + shift >= text.size() ||
        right_suffix + shift >= text.size()) {
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
      if (scan_sink) {
        scan_sink->Record(0, 0, 0);
      }
#endif
      return {};
    }
    const auto left_sample = (left_suffix + shift) / sampling_rate;
    const auto right_sample = (right_suffix + shift) / sampling_rate;
    if (left_sample >= isa_values.size() || right_sample >= isa_values.size()) {
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
      if (scan_sink) {
        scan_sink->Record(0, 0, 0);
      }
#endif
      return {};
    }
    auto left = static_cast<std::uint64_t>(
        std::min(isa_values[static_cast<std::size_t>(left_sample)],
                 isa_values[static_cast<std::size_t>(right_sample)]));
    const auto right_endpoint =
        std::max(isa_values[static_cast<std::size_t>(left_sample)],
                 isa_values[static_cast<std::size_t>(right_sample)]);
    auto right = static_cast<std::uint64_t>(right_endpoint) + 1;
    const auto sa_size = static_cast<std::uint64_t>(sa_values.size());
    const auto target = depth - shift;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    if (scan_sink) {
      std::uint64_t left_rows = 0;
      std::uint64_t right_rows = 0;
      const auto scan_begin = BuildClock::now();
      while (left > 0) {
        ++left_rows;
        if (static_cast<std::uint64_t>(
                lcp_values[static_cast<std::size_t>(left)]) < target) {
          break;
        }
        --left;
      }
      while (right < sa_size) {
        ++right_rows;
        if (static_cast<std::uint64_t>(
                lcp_values[static_cast<std::size_t>(right)]) < target) {
          break;
        }
        ++right;
      }
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               BuildClock::now() - scan_begin)
                               .count();
      scan_sink->Record(left_rows, right_rows,
                        elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0);
    } else {
#endif
      while (left > 0 &&
             static_cast<std::uint64_t>(
                 lcp_values[static_cast<std::size_t>(left)]) >= target) {
        --left;
      }
      while (right < sa_size &&
             static_cast<std::uint64_t>(
                 lcp_values[static_cast<std::size_t>(right)]) >= target) {
        ++right;
      }
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    }
#else
    static_cast<void>(scan_sink);
#endif
    return {left, right};
  }

  SuffixRange SuffixLinkInterval(SuffixRange previous, std::uint64_t depth,
                                 std::uint32_t shift,
                                 detail::SuffixLinkScanSink* scan_sink) const {
    // Coordinate width is selected once before the potentially long LCP scan,
    // so the hot loop contains no per-row 32/64-bit branch.
    return std::visit(
        [&](const auto& sa_values, const auto& isa_values,
            const auto& lcp_values) {
          return SuffixLinkIntervalFor(sa_values, isa_values, lcp_values,
                                       previous, depth, shift, scan_sink);
        },
        suffix_array, isa, lcp);
  }

  template <class Callback>
  void EnumerateOneStrand(const std::vector<std::uint8_t>& query,
                          std::uint64_t original_query_length, Strand strand,
                          std::uint64_t min_length,
                          RightMaximalSearchAlgorithm algorithm,
                          SaSearchAlgorithm lookup_algorithm,
                          RightMaximalSearchStatistics* statistics,
                          Callback& callback) const {
    const auto lcp_values = ViewCoordinates(lcp);
    detail::SuffixLinkScanSink* scan_sink = nullptr;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    scan_sink = detail::CurrentSuffixLinkScanSink();
#endif
    std::size_t run_begin = 0;
    while (run_begin < query.size()) {
      while (run_begin < query.size() &&
             query[run_begin] == detail::kSentinel) {
        ++run_begin;
      }
      std::size_t run_end = run_begin;
      while (run_end < query.size() && query[run_end] != detail::kSentinel) {
        ++run_end;
      }
      SuffixRange previous{};
      for (std::size_t query_position = run_begin;
           query_position + min_length <= run_end; ++query_position) {
        const EncodedView prefix(query.data() + query_position,
                                 static_cast<std::size_t>(min_length));
        SuffixRange interval;
        const bool links =
            algorithm == RightMaximalSearchAlgorithm::kSuffixLink ||
            algorithm == RightMaximalSearchAlgorithm::kFull;
        if (links && query_position != run_begin && !previous.Empty()) {
          if (statistics) {
            ++statistics->suffix_link_attempts;
          }
          interval =
              SuffixLinkInterval(previous, min_length, 1, scan_sink);
          if (!interval.Empty()) {
            interval = NarrowChar(interval, min_length - 1, prefix.back());
          }
          if (statistics && !interval.Empty()) {
            ++statistics->suffix_link_successes;
          }
          if (interval.Empty()) {
            // A failed suffix-link derivation is not evidence of no match.
            // Restart from the root path to preserve the complete result set.
            if (statistics) {
              ++statistics->suffix_link_fallbacks;
            }
            if (algorithm == RightMaximalSearchAlgorithm::kFull) {
              interval = ChildRange(prefix);
            } else {
              const auto selected =
                  ResolveSearchAlgorithm(lookup_algorithm, prefix.size());
              if (statistics) {
                ++statistics->lookup_calls;
                if (selected == SaSearchAlgorithm::kSaplingPwl) {
                  ++statistics->learned_lookup_calls;
                } else {
                  ++statistics->binary_lookup_calls;
                }
              }
              interval = Range(prefix, selected,
                               statistics ? &statistics->lookup : nullptr);
            }
          }
        } else if (algorithm == RightMaximalSearchAlgorithm::kChild ||
                   algorithm == RightMaximalSearchAlgorithm::kFull) {
          interval = ChildRange(prefix);
        } else {
          const auto selected =
              ResolveSearchAlgorithm(lookup_algorithm, prefix.size());
          if (statistics) {
            ++statistics->lookup_calls;
            if (query_position != run_begin && previous.Empty()) {
              ++statistics->previous_empty_lookups;
            }
            if (selected == SaSearchAlgorithm::kSaplingPwl) {
              ++statistics->learned_lookup_calls;
            } else {
              ++statistics->binary_lookup_calls;
            }
          }
          interval = Range(prefix, selected,
                           statistics ? &statistics->lookup : nullptr);
        }
        previous = interval;
        if (interval.Empty()) {
          continue;
        }

        std::uint64_t previous_lce = min_length;
        ForEachStoredRow(interval,
                         [&](std::uint64_t row, std::uint64_t global) {
          const auto mapped =
              detail::MapGlobalPosition(reference, global, min_length);
          if (!mapped) {
            return;
          }
          const auto sequence_id = static_cast<std::size_t>(mapped->first);
          const auto reference_limit = reference.contig_starts[sequence_id] +
                                       reference.contig_lengths[sequence_id];
          std::uint64_t length = min_length;
          if (HasLcp() && algorithm != RightMaximalSearchAlgorithm::kBaseline &&
              row != interval.begin) {
            length =
                std::min(previous_lce,
                         lcp_values[static_cast<std::size_t>(row)]);
            if (length < min_length) {
              length = min_length;
            }
          }
          if (query_position + length < run_end &&
              global + length < reference_limit) {
            const auto remaining = static_cast<std::size_t>(std::min(
                static_cast<std::uint64_t>(run_end - query_position) - length,
                reference_limit - global - length));
            length += detail::LongestCommonPrefixBytes(
                query.data() + query_position +
                    static_cast<std::size_t>(length),
                text.data() + static_cast<std::size_t>(global + length),
                remaining);
          }
          previous_lce = length;
          const bool left_extendable =
              query_position > run_begin && mapped->second > 0 &&
              query[query_position - 1] ==
                  text[static_cast<std::size_t>(global - 1)];
          if (left_extendable) {
            return;
          }
          const auto output_position =
              strand == Strand::kReverseComplement
                  ? original_query_length - (query_position + length)
                  : static_cast<std::uint64_t>(query_position);
          callback(
              {mapped->first, mapped->second, output_position, length, strand});
        });
      }
      run_begin = run_end;
    }
  }

  template <class Callback>
  void EnumerateSparseOneStrand(const std::vector<std::uint8_t>& query,
                                std::uint64_t original_query_length,
                                Strand strand, std::uint64_t min_length,
                                RightMaximalSearchAlgorithm algorithm,
                                SaSearchAlgorithm lookup_algorithm,
                                RightMaximalSearchStatistics* statistics,
                                Callback& callback) const {
    if (min_length < sampling_rate) {
      throw Error(ErrorCode::kInvalidInput,
                  "sampled SA right-maximal exact match search requires "
                  "min_length >= sampling_rate");
    }
    const auto anchor_length = min_length - sampling_rate + 1;
    detail::SuffixLinkScanSink* scan_sink = nullptr;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    scan_sink = detail::CurrentSuffixLinkScanSink();
#endif
    std::size_t run_begin = 0;
    while (run_begin < query.size()) {
      while (run_begin < query.size() &&
             query[run_begin] == detail::kSentinel) {
        ++run_begin;
      }
      std::size_t run_end = run_begin;
      while (run_end < query.size() && query[run_end] != detail::kSentinel) {
        ++run_end;
      }
      // Residue classes ensure every possible start can be recovered from one
      // stored suffix. Left extension then restores the original coordinate.
      for (std::uint32_t residue = 0; residue < sampling_rate; ++residue) {
        const auto first = run_begin + residue;
        if (first >= run_end) {
          break;
        }
        SuffixRange previous{};
        for (std::size_t anchor_position = first;
             anchor_position + anchor_length <= run_end;
             anchor_position += sampling_rate) {
          const EncodedView prefix(query.data() + anchor_position,
                                   static_cast<std::size_t>(anchor_length));
          SuffixRange interval;
          const bool links =
              algorithm == RightMaximalSearchAlgorithm::kSuffixLink ||
              algorithm == RightMaximalSearchAlgorithm::kFull;
          if (links && anchor_position != first && !previous.Empty() &&
              anchor_length > sampling_rate) {
            if (statistics) {
              ++statistics->suffix_link_attempts;
            }
            interval =
                SuffixLinkInterval(previous, anchor_length, sampling_rate,
                                   scan_sink);
            for (std::uint64_t depth = anchor_length - sampling_rate;
                 !interval.Empty() && depth < anchor_length; ++depth) {
              interval = NarrowChar(interval, depth,
                                    prefix[static_cast<std::size_t>(depth)]);
            }
            if (statistics && !interval.Empty()) {
              ++statistics->suffix_link_successes;
            }
          }
          if (interval.Empty()) {
            // As in the complete-SA path, failed reuse falls back to a fresh
            // root lookup; it never suppresses a possible match.
            if (links && anchor_position != first && statistics) {
              ++statistics->suffix_link_fallbacks;
            }
            if (algorithm == RightMaximalSearchAlgorithm::kChild ||
                algorithm == RightMaximalSearchAlgorithm::kFull) {
              interval = ChildRange(prefix);
            } else {
              const auto selected =
                  ResolveSearchAlgorithm(lookup_algorithm, prefix.size());
              if (statistics) {
                ++statistics->lookup_calls;
                if (selected == SaSearchAlgorithm::kSaplingPwl) {
                  ++statistics->learned_lookup_calls;
                } else {
                  ++statistics->binary_lookup_calls;
                }
              }
              interval = Range(prefix, selected,
                               statistics ? &statistics->lookup : nullptr);
            }
          }
          previous = interval;
          ForEachStoredPosition(interval, [&](std::uint64_t sampled) {
            const auto mapped =
                detail::MapGlobalPosition(reference, sampled, anchor_length);
            if (!mapped) {
              return;
            }
            const auto sequence_id = static_cast<std::size_t>(mapped->first);
            const auto reference_begin = reference.contig_starts[sequence_id];
            const auto reference_end =
                reference_begin + reference.contig_lengths[sequence_id];
            std::uint64_t left = 0;
            while (
                left < sampling_rate && anchor_position > run_begin + left &&
                sampled > reference_begin + left &&
                query[anchor_position - static_cast<std::size_t>(left) - 1] ==
                    text[static_cast<std::size_t>(sampled - left - 1)]) {
              ++left;
            }
            if (left == sampling_rate) {
              return;
            }
            std::uint64_t right = anchor_length;
            if (anchor_position + right < run_end &&
                sampled + right < reference_end) {
              const auto remaining = static_cast<std::size_t>(std::min(
                  static_cast<std::uint64_t>(run_end - anchor_position) -
                      right,
                  reference_end - sampled - right));
              right += detail::LongestCommonPrefixBytes(
                  query.data() + anchor_position +
                      static_cast<std::size_t>(right),
                  text.data() + static_cast<std::size_t>(sampled + right),
                  remaining);
            }
            const auto length = left + right;
            if (length < min_length) {
              return;
            }
            const auto query_start =
                anchor_position - static_cast<std::size_t>(left);
            const auto reference_start = sampled - left;
            const auto output_position =
                strand == Strand::kReverseComplement
                    ? original_query_length - (query_start + length)
                    : static_cast<std::uint64_t>(query_start);
            callback({mapped->first, reference_start - reference_begin,
                      output_position, length, strand});
          });
        }
      }
      run_begin = run_end;
    }
  }

  template <class Callback>
  void EnumerateEncodedRightMaximal(
      const std::vector<std::uint8_t>& encoded,
      const RightMaximalOptions& options,
      RightMaximalSearchAlgorithm algorithm, Callback& callback) const {
    const auto enumerate = [&](const std::vector<std::uint8_t>& value,
                               Strand strand) {
      if (sampling_rate == 1) {
        EnumerateOneStrand(value, encoded.size(), strand, options.min_length,
                           algorithm, options.lookup_algorithm,
                           options.statistics, callback);
      } else {
        EnumerateSparseOneStrand(
            value, encoded.size(), strand, options.min_length, algorithm,
            options.lookup_algorithm, options.statistics, callback);
      }
    };
    if (options.strands == StrandMode::kForward ||
        options.strands == StrandMode::kBoth) {
      enumerate(encoded, Strand::kForward);
    }
    if (options.strands == StrandMode::kReverseComplement ||
        options.strands == StrandMode::kBoth) {
      const auto reverse = ReverseComplementRightMaximal(encoded);
      enumerate(reverse, Strand::kReverseComplement);
    }
  }

  std::uint32_t ResolveMemSkip(
      const MemOptions& options,
      RightMaximalSearchAlgorithm effective_algorithm) const {
    if (options.min_length < sampling_rate) {
      throw Error(ErrorCode::kInvalidInput,
                  "sampled SA MEM search requires min_length >= "
                  "sampling_rate");
    }
    const auto maximum = options.min_length / sampling_rate;
    std::uint64_t skip = 1;
    if (options.skip_multiplier) {
      skip = *options.skip_multiplier;
      if (skip > maximum) {
        throw Error(ErrorCode::kInvalidInput,
                    "MEM skip multiplier makes the left-recovery window "
                    "longer than the minimum match length");
      }
    } else if (effective_algorithm !=
               RightMaximalSearchAlgorithm::kSuffixLink) {
      const auto reserve = sampling_rate >= 4 ? std::uint64_t{10}
                                              : std::uint64_t{12};
      const auto available =
          options.min_length > reserve ? options.min_length - reserve : 0;
      skip = std::max<std::uint64_t>(available / sampling_rate, 1);
    }
    skip = std::min(skip, maximum);
    if (skip == 0 || skip > std::numeric_limits<std::uint32_t>::max() ||
        skip > std::numeric_limits<std::uint64_t>::max() / sampling_rate ||
        skip * sampling_rate > options.min_length ||
        skip * sampling_rate > std::numeric_limits<std::uint32_t>::max()) {
      throw Error(ErrorCode::kInvalidInput,
                  "MEM skip multiplier is incompatible with minimum length "
                  "and sampling rate");
    }
    return static_cast<std::uint32_t>(skip);
  }

  template <class Callback>
  void EnumerateSkippedMemOneStrand(
      const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      const MemOptions& options, RightMaximalSearchAlgorithm algorithm,
      std::uint32_t skip_multiplier, Callback& callback) const {
    const auto window =
        static_cast<std::uint64_t>(skip_multiplier) * sampling_rate;
    const auto anchor_length = options.min_length - window + 1;
    const auto lcp_values = ViewCoordinates(lcp);
    detail::SuffixLinkScanSink* scan_sink = nullptr;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    scan_sink = detail::CurrentSuffixLinkScanSink();
#endif

    std::size_t run_begin = 0;
    while (run_begin < query.size()) {
      while (run_begin < query.size() &&
             query[run_begin] == detail::kSentinel) {
        ++run_begin;
      }
      std::size_t run_end = run_begin;
      while (run_end < query.size() && query[run_end] != detail::kSentinel) {
        ++run_end;
      }
      for (std::uint32_t residue = 0; residue < sampling_rate; ++residue) {
        const auto begin_mod = static_cast<std::uint64_t>(run_begin) % window;
        const auto delta = (static_cast<std::uint64_t>(residue) + window -
                            begin_mod) %
                           window;
        const auto first64 = static_cast<std::uint64_t>(run_begin) + delta;
        if (first64 >= run_end) {
          continue;
        }
        auto anchor_position = static_cast<std::size_t>(first64);
        SuffixRange previous{};
        while (anchor_position + anchor_length <= run_end) {
          const EncodedView prefix(
              query.data() + anchor_position,
              static_cast<std::size_t>(anchor_length));
          SuffixRange interval;
          const bool links =
              algorithm == RightMaximalSearchAlgorithm::kSuffixLink ||
              algorithm == RightMaximalSearchAlgorithm::kFull;
          if (links && anchor_position != first64 && !previous.Empty() &&
              anchor_length > window) {
            interval = SuffixLinkInterval(previous, anchor_length,
                                          static_cast<std::uint32_t>(window),
                                          scan_sink);
            for (std::uint64_t depth = anchor_length - window;
                 !interval.Empty() && depth < anchor_length; ++depth) {
              interval = NarrowChar(
                  interval, depth,
                  prefix[static_cast<std::size_t>(depth)]);
            }
          }
          if (interval.Empty()) {
            if (algorithm == RightMaximalSearchAlgorithm::kChild ||
                algorithm == RightMaximalSearchAlgorithm::kFull) {
              interval = ChildRange(prefix);
            } else {
              const auto selected = ResolveSearchAlgorithm(
                  options.lookup_algorithm, prefix.size());
              interval = Range(prefix, selected, nullptr);
            }
          }
          previous = interval;

          std::uint64_t previous_lce = anchor_length;
          ForEachStoredRow(
              interval, [&](std::uint64_t row, std::uint64_t sampled) {
                const auto mapped = detail::MapGlobalPosition(
                    reference, sampled, anchor_length);
                if (!mapped) {
                  return;
                }
                const auto sequence_id =
                    static_cast<std::size_t>(mapped->first);
                const auto reference_begin =
                    reference.contig_starts[sequence_id];
                const auto reference_end =
                    reference_begin + reference.contig_lengths[sequence_id];

                std::uint64_t right = anchor_length;
                if (HasLcp() &&
                    algorithm != RightMaximalSearchAlgorithm::kBaseline &&
                    row != interval.begin) {
                  right = std::min(
                      previous_lce,
                      static_cast<std::uint64_t>(
                          lcp_values[static_cast<std::size_t>(row)]));
                  right = std::max(right, anchor_length);
                }
                if (anchor_position + right < run_end &&
                    sampled + right < reference_end) {
                  const auto remaining = static_cast<std::size_t>(std::min(
                      static_cast<std::uint64_t>(run_end - anchor_position) -
                          right,
                      reference_end - sampled - right));
                  right += detail::LongestCommonPrefixBytes(
                      query.data() + anchor_position +
                          static_cast<std::size_t>(right),
                      text.data() + static_cast<std::size_t>(sampled + right),
                      remaining);
                }
                previous_lce = right;

                std::uint64_t left = 0;
                while (left < window &&
                       anchor_position > run_begin + left &&
                       sampled > reference_begin + left &&
                       query[anchor_position -
                             static_cast<std::size_t>(left) - 1] ==
                           text[static_cast<std::size_t>(sampled - left - 1)]) {
                  ++left;
                }
                // A full window is owned by the preceding query anchor.
                if (left == window || left + right < options.min_length) {
                  return;
                }
                const auto query_start =
                    anchor_position - static_cast<std::size_t>(left);
                const auto reference_start = sampled - left;
                const auto output_position =
                    strand == Strand::kReverseComplement
                        ? original_query_length - (query_start + left + right)
                        : static_cast<std::uint64_t>(query_start);
                callback(MemMatch{mapped->first,
                                  reference_start - reference_begin,
                                  output_position, left + right, strand});
              });

          if (window > run_end - anchor_position) {
            break;
          }
          anchor_position += static_cast<std::size_t>(window);
        }
      }
      run_begin = run_end;
    }
  }

  template <class Callback>
  void EnumerateEncodedMem(const std::vector<std::uint8_t>& encoded,
                           const MemOptions& options,
                           RightMaximalSearchAlgorithm algorithm,
                           Callback& callback) const {
    const auto skip = ResolveMemSkip(options, algorithm);
    if (skip == 1) {
      RightMaximalOptions legacy;
      legacy.min_length = options.min_length;
      legacy.strands = options.strands;
      legacy.algorithm = algorithm;
      legacy.lookup_algorithm = options.lookup_algorithm;
      auto convert = [&](const RightMaximalMatch& match) {
        callback(MemMatch{match.sequence_id, match.reference_position,
                          match.query_position, match.length, match.strand});
      };
      EnumerateEncodedRightMaximal(encoded, legacy, algorithm, convert);
      return;
    }

    const auto enumerate = [&](const std::vector<std::uint8_t>& value,
                               Strand strand) {
      EnumerateSkippedMemOneStrand(value, encoded.size(), strand, options,
                                   algorithm, skip, callback);
    };
    if (options.strands == StrandMode::kForward ||
        options.strands == StrandMode::kBoth) {
      enumerate(encoded, Strand::kForward);
    }
    if (options.strands == StrandMode::kReverseComplement ||
        options.strands == StrandMode::kBoth) {
      const auto reverse = ReverseComplementRightMaximal(encoded);
      enumerate(reverse, Strand::kReverseComplement);
    }
  }

  template <class Callback>
  void EnumerateEncodedMam(const std::vector<std::uint8_t>& encoded,
                           const MamOptions& options,
                           RightMaximalSearchAlgorithm algorithm,
                           Callback& callback) const {
    if (sampling_rate != 1) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  "reference-MAM search requires a complete suffix array");
    }
    MemOptions mem;
    mem.min_length = options.min_length;
    mem.strands = options.strands;
    mem.algorithm = static_cast<MemSearchAlgorithm>(algorithm);
    mem.lookup_algorithm = options.lookup_algorithm;
    mem.skip_multiplier = 1;
    auto filter = [&](const MemMatch& match) {
      const auto sequence_id = static_cast<std::size_t>(match.sequence_id);
      const auto global = reference.contig_starts[sequence_id] +
                          match.reference_position;
      const EncodedView pattern(
          text.data() + static_cast<std::size_t>(global),
          static_cast<std::size_t>(match.length));
      const auto lookup = HasLcp() ? SaSearchAlgorithm::kLcpBinary
                                   : SaSearchAlgorithm::kBinary;
      if (Range(pattern, lookup, nullptr).Size() == 1) {
        callback(MamMatch{match.sequence_id, match.reference_position,
                          match.query_position, match.length, match.strand});
      }
    };
    EnumerateEncodedMem(encoded, mem, algorithm, filter);
  }
};

SuffixArray::SuffixArray(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SuffixArray::SuffixArray(SuffixArray&&) noexcept = default;
SuffixArray& SuffixArray::operator=(SuffixArray&&) noexcept = default;
SuffixArray::~SuffixArray() = default;

SuffixArray SuffixArray::Build(const GenomeReference& reference,
                               const SuffixArrayBuildOptions& options) {
  if (options.threads == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "suffix-array thread count must be greater than zero");
  }
  if (options.sampling_rate == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "SA sampling rate must be greater than zero");
  }
  if (options.statistics) {
    *options.statistics = {};
  }
  auto impl = std::make_unique<Impl>();
  impl->reference = MetadataCopy(reference.impl_->data);
  const auto& encoded = reference.impl_->data.encoded;
  if (encoded.size() == std::numeric_limits<std::size_t>::max() ||
      encoded.size() + 1 > impl->text.max_size()) {
    throw Error(ErrorCode::kInvalidInput,
                "reference text size overflows suffix-array storage");
  }
  impl->text.reserve(encoded.size() + 1);
  impl->text.insert(impl->text.end(), encoded.begin(), encoded.end());
  // ReferenceData contains separators but never zero. Appending one zero here
  // gives every backend the same unique, lexicographically smallest sentinel.
  impl->text.push_back(detail::kSentinel);
  if (impl->text.size() < 2) {
    throw Error(ErrorCode::kInvalidInput, "reference text is empty");
  }
  const auto backend =
      detail::ResolveSaBackend(options.backend, impl->text.size(),
                               options.threads, detail::CapsBuildAvailable());
  if (backend == SaBackend::kCaps && !detail::CapsBuildAvailable()) {
    throw Error(ErrorCode::kUnsupportedBackend,
                "CaPS-SA support was disabled when sufkit was built");
  }
  if (backend != SaBackend::kCaps && backend != SaBackend::kDivsufsort) {
    throw Error(ErrorCode::kInvalidInput, "invalid suffix-array backend");
  }
  const CoordinateWidth width = detail::ResolveSaCoordinateWidth(
      backend, options.coordinate_width, impl->text.size());
  const auto coordinate_width = static_cast<std::uint8_t>(width);
  const auto lcp_width = static_cast<std::uint8_t>(
      impl->text.size() <= std::numeric_limits<std::uint32_t>::max() ? 32
                                                                    : 64);
  const auto sa_begin = BuildClock::now();
  const bool retain_lcp = options.acceleration != SaAcceleration::kNone;
  const bool retain_isa =
      options.acceleration == SaAcceleration::kLcpSuffixLink ||
      options.acceleration == SaAcceleration::kFull;
  std::vector<std::uint64_t> backend_isa;
  bool backend_reports_phases = false;
  if (backend == SaBackend::kCaps && width == CoordinateWidth::kBits32) {
    if (impl->text.size() >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference is too large for CaPS-SA uint32_t");
    }
    auto built = detail::BuildCaps32(impl->text, options.threads, retain_lcp);
    impl->suffix_array = std::move(built.suffix_array);
    impl->lcp = CompactCoordinates(std::move(built.lcp), lcp_width,
                                   ErrorCode::kBuildFailure, "LCP");
    impl->backend = detail::StoredBackend::kCaps32;
  } else if (backend == SaBackend::kCaps && width == CoordinateWidth::kBits64) {
    auto built = detail::BuildCaps64(impl->text, options.threads, retain_lcp);
    impl->suffix_array = std::move(built.suffix_array);
    impl->lcp = CompactCoordinates(std::move(built.lcp), lcp_width,
                                   ErrorCode::kBuildFailure, "LCP");
    impl->backend = detail::StoredBackend::kCaps64;
  } else if (backend == SaBackend::kDivsufsort &&
             width == CoordinateWidth::kBits32) {
    auto built =
        detail::BuildDivsufsort32(impl->text, options.sampling_rate, retain_lcp,
                                  retain_isa, options.threads);
    if (options.statistics) {
      options.statistics->sa_seconds = built.sa_seconds;
      options.statistics->isa_seconds = built.isa_seconds;
      options.statistics->lcp_seconds = built.lcp_seconds;
    }
    backend_reports_phases = true;
    impl->suffix_array = std::move(built.suffix_array);
    impl->lcp = CompactCoordinates(std::move(built.lcp), lcp_width,
                                   ErrorCode::kBuildFailure, "LCP");
    backend_isa = std::move(built.isa);
    impl->backend = detail::StoredBackend::kDivsufsort32;
  } else if (backend == SaBackend::kDivsufsort &&
             width == CoordinateWidth::kBits64) {
    auto built =
        detail::BuildDivsufsort64(impl->text, options.sampling_rate, retain_lcp,
                                  retain_isa, options.threads);
    if (options.statistics) {
      options.statistics->sa_seconds = built.sa_seconds;
      options.statistics->isa_seconds = built.isa_seconds;
      options.statistics->lcp_seconds = built.lcp_seconds;
    }
    backend_reports_phases = true;
    impl->suffix_array = std::move(built.suffix_array);
    impl->lcp = CompactCoordinates(std::move(built.lcp), lcp_width,
                                   ErrorCode::kBuildFailure, "LCP");
    backend_isa = std::move(built.isa);
    impl->backend = detail::StoredBackend::kDivsufsort64;
  } else {
    throw Error(ErrorCode::kInvalidInput,
                "invalid suffix-array coordinate width");
  }
  impl->sampling_rate = options.sampling_rate;
  if (backend == SaBackend::kCaps && impl->sampling_rate > 1) {
    // CaPS supplies complete-SA LCP. Sampling must reduce intervening LCP
    // values by interval minimum so adjacent retained suffixes remain valid.
    if (impl->HasLcp()) {
      SampleSaLcp(impl->suffix_array, impl->lcp, impl->sampling_rate);
    } else {
      SampleSa(impl->suffix_array, impl->sampling_rate);
    }
  }
  if (options.statistics && !backend_reports_phases) {
    options.statistics->sa_seconds = BuildElapsed(sa_begin);
  }

  impl->acceleration = options.acceleration;
  CoordinateStorage temporary_isa = CompactCoordinates(
      std::move(backend_isa), coordinate_width, ErrorCode::kBuildFailure,
      "inverse suffix array");
  if (((retain_lcp && !impl->HasLcp()) || retain_isa ||
       options.learned_index.enabled) &&
      CoordinatesEmpty(temporary_isa)) {
    const auto begin = BuildClock::now();
    temporary_isa = BuildIsa(impl->suffix_array, impl->text.size(),
                             impl->sampling_rate, options.threads,
                             coordinate_width);
    if (options.statistics) {
      options.statistics->isa_seconds = BuildElapsed(begin);
    }
  }
  if (retain_lcp) {
    if (!impl->HasLcp()) {
      const auto lcp_begin = BuildClock::now();
      impl->lcp = BuildLcp(impl->text, impl->suffix_array, temporary_isa,
                           impl->sampling_rate, lcp_width);
      if (options.statistics) {
        options.statistics->lcp_seconds = BuildElapsed(lcp_begin);
      }
    }
    if (retain_isa) {
      // LCP construction is the final temporary consumer. Move ISA into the
      // immutable index instead of retaining a second full-size copy.
      impl->isa = std::move(temporary_isa);
    }
    if (options.acceleration == SaAcceleration::kLcpChild ||
        options.acceleration == SaAcceleration::kFull) {
      const auto child_begin = BuildClock::now();
      impl->child = BuildChild(impl->lcp, coordinate_width);
      if (options.statistics) {
        options.statistics->child_seconds = BuildElapsed(child_begin);
      }
    }
  }
  if (options.learned_index.enabled) {
    const auto learned_begin = BuildClock::now();
    const auto& learned_isa = retain_isa ? impl->isa : temporary_isa;
    impl->learned = BuildLearnedIndex(
        impl->reference, impl->text, impl->suffix_array, learned_isa,
        impl->sampling_rate,
        static_cast<std::uint8_t>(width), options.learned_index);
    if (options.statistics) {
      options.statistics->learned_index_seconds = BuildElapsed(learned_begin);
    }
  }
  const auto aux_bytes = CoordinateBytes(impl->isa) +
                         CoordinateBytes(impl->lcp) +
                         CoordinateBytes(impl->child);
  impl->index_info = BuiltInfo(
      impl->reference, impl->backend, static_cast<std::uint8_t>(width),
      impl->text.size(), SaSize(impl->suffix_array), impl->sampling_rate,
      impl->acceleration, aux_bytes, impl->learned);
  return SuffixArray(std::move(impl));
}

SuffixRange SuffixArray::EqualRange(std::string_view pattern) const {
  return EqualRange(pattern, SaSearchAlgorithm::kAutoSelect);
}

SuffixRange SuffixArray::EqualRange(std::string_view pattern,
                                    SaSearchAlgorithm algorithm,
                                    SaSearchStatistics* statistics) const {
  if (impl_->sampling_rate != 1) {
    throw Error(ErrorCode::kUnsupportedBackend,
                "equal_range is not representable as one interval for a "
                "sampled suffix array; use count or locate");
  }
  return impl_->Range(detail::EncodePattern(pattern), algorithm, statistics);
}

std::uint64_t SuffixArray::Count(std::string_view pattern,
                                 StrandMode strands) const {
  return Count(pattern, strands, SaSearchAlgorithm::kAutoSelect);
}

std::uint64_t SuffixArray::Count(std::string_view pattern, StrandMode strands,
                                 SaSearchAlgorithm algorithm,
                                 SaSearchStatistics* statistics) const {
  const auto encoded = detail::EncodePattern(pattern);
  if (strands == StrandMode::kForward) {
    return impl_->ExactCount(encoded, algorithm, statistics);
  }
  const auto reverse = detail::ReverseComplement(encoded);
  if (strands == StrandMode::kReverseComplement) {
    return impl_->ExactCount(reverse, algorithm, statistics);
  }
  if (encoded == reverse) {
    return impl_->ExactCount(encoded, algorithm, statistics);
  }
  return impl_->ExactCount(encoded, algorithm, statistics) +
         impl_->ExactCount(reverse, algorithm, statistics);
}

QueryResult SuffixArray::Locate(std::string_view pattern,
                                const LocateOptions& options) const {
  return Locate(pattern, options, SaSearchAlgorithm::kAutoSelect);
}

QueryResult SuffixArray::Locate(std::string_view pattern,
                                const LocateOptions& options,
                                SaSearchAlgorithm algorithm,
                                SaSearchStatistics* statistics) const {
  const auto encoded = detail::EncodePattern(pattern);
  std::vector<GlobalMatch> matches;
  bool heap_active = false;
  std::uint64_t total_hits = 0;
  if (options.strands == StrandMode::kForward) {
    total_hits = impl_->Collect(encoded, Strand::kForward, options, matches,
                                heap_active, algorithm, statistics);
  } else {
    const auto reverse = detail::ReverseComplement(encoded);
    if (options.strands == StrandMode::kReverseComplement) {
      total_hits = impl_->Collect(reverse, Strand::kReverseComplement, options,
                                  matches, heap_active, algorithm, statistics);
    } else if (encoded == reverse) {
      total_hits = impl_->Collect(encoded, Strand::kBoth, options, matches,
                                  heap_active, algorithm, statistics);
    } else {
      total_hits = impl_->Collect(encoded, Strand::kForward, options, matches,
                                  heap_active, algorithm, statistics);
      total_hits += impl_->Collect(reverse, Strand::kReverseComplement, options,
                                   matches, heap_active, algorithm, statistics);
    }
  }
  return FinalizeGlobalMatches(std::move(matches), impl_->reference,
                               encoded.size(), total_hits);
}

void SuffixArray::ForEachRightMaximalMatch(
    std::string_view query, const RightMaximalOptions& options,
    const RightMaximalCallback& callback) const {
  // The public contract deliberately guarantees right maximality only. Some
  // paths may also reject left-extendable candidates, but callers must not
  // treat this API as a complete MEM implementation.
  PrepareRightMaximalSearch(options);
  if (!callback) {
    throw Error(ErrorCode::kInvalidInput,
                "right-maximal exact match callback must not be empty");
  }
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveAlgorithm(options.algorithm);
  impl_->EnumerateEncodedRightMaximal(encoded, options, algorithm, callback);
}

RightMaximalResult SuffixArray::FindRightMaximalMatches(
    std::string_view query, const RightMaximalOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareRightMaximalSearch(options);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveAlgorithm(options.algorithm);
  RightMaximalResult result;
  if (!max_matches) {
    auto collect = [&](const RightMaximalMatch& match) {
      ++result.total_matches;
      result.matches.push_back(match);
    };
    impl_->EnumerateEncodedRightMaximal(encoded, options, algorithm, collect);
    std::sort(result.matches.begin(), result.matches.end(),
              RightMaximalMatchLess);
    result.truncated = false;
    return result;
  }

  const auto retain = *max_matches;
  bool heap_active = false;
  auto collect = [&](const RightMaximalMatch& match) {
    ++result.total_matches;
    if (retain == 0) {
      return;
    }
    if (!heap_active && result.matches.size() < retain) {
      result.matches.push_back(match);
      return;
    }
    if (!heap_active) {
      // Do not build a heap unless an additional result proves that truncation
      // is necessary. Common max_matches values above the true result count
      // retain the append-and-sort behavior of the unlimited path.
      std::make_heap(result.matches.begin(), result.matches.end(),
                     RightMaximalMatchLess);
      heap_active = true;
    }
    if (RightMaximalMatchLess(match, result.matches.front())) {
      std::pop_heap(result.matches.begin(), result.matches.end(),
                    RightMaximalMatchLess);
      result.matches.back() = match;
      std::push_heap(result.matches.begin(), result.matches.end(),
                     RightMaximalMatchLess);
    }
  };
  impl_->EnumerateEncodedRightMaximal(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(),
            RightMaximalMatchLess);
  result.truncated = result.matches.size() < result.total_matches;
  return result;
}

void SuffixArray::ForEachMem(std::string_view query,
                             const MemOptions& options,
                             const MemCallback& callback) const {
  PrepareMemSearch(options);
  if (!callback) {
    throw Error(ErrorCode::kInvalidInput, "MEM callback must not be empty");
  }
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm =
      impl_->ResolveAlgorithm(AsRightMaximalAlgorithm(options.algorithm));
  impl_->EnumerateEncodedMem(encoded, options, algorithm, callback);
}

MemResult SuffixArray::FindMems(
    std::string_view query, const MemOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareMemSearch(options);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm =
      impl_->ResolveAlgorithm(AsRightMaximalAlgorithm(options.algorithm));
  MemResult result;
  auto collect = [&](const MemMatch& match) { result.matches.push_back(match); };
  impl_->EnumerateEncodedMem(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(), MemMatchLess);
  result.matches.erase(
      std::unique(result.matches.begin(), result.matches.end(),
                  [](const MemMatch& left, const MemMatch& right) {
                    return std::tie(left.query_position, left.sequence_id,
                                    left.reference_position, left.length,
                                    left.strand) ==
                           std::tie(right.query_position, right.sequence_id,
                                    right.reference_position, right.length,
                                    right.strand);
                  }),
      result.matches.end());
  result.total_matches = result.matches.size();
  if (max_matches && result.matches.size() > *max_matches) {
    result.matches.resize(static_cast<std::size_t>(*max_matches));
  }
  result.truncated = result.matches.size() < result.total_matches;
  return result;
}

void SuffixArray::ForEachMam(std::string_view query,
                             const MamOptions& options,
                             const MamCallback& callback) const {
  PrepareMamSearch(options);
  if (!callback) {
    throw Error(ErrorCode::kInvalidInput,
                "reference-MAM callback must not be empty");
  }
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm =
      impl_->ResolveAlgorithm(AsRightMaximalAlgorithm(options.algorithm));
  impl_->EnumerateEncodedMam(encoded, options, algorithm, callback);
}

MamResult SuffixArray::FindMams(
    std::string_view query, const MamOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareMamSearch(options);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm =
      impl_->ResolveAlgorithm(AsRightMaximalAlgorithm(options.algorithm));
  MamResult result;
  auto collect = [&](const MamMatch& match) { result.matches.push_back(match); };
  impl_->EnumerateEncodedMam(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(), MamMatchLess);
  result.matches.erase(
      std::unique(result.matches.begin(), result.matches.end(),
                  [](const MamMatch& left, const MamMatch& right) {
                    return std::tie(left.query_position, left.sequence_id,
                                    left.reference_position, left.length,
                                    left.strand) ==
                           std::tie(right.query_position, right.sequence_id,
                                    right.reference_position, right.length,
                                    right.strand);
                  }),
      result.matches.end());
  result.total_matches = result.matches.size();
  if (max_matches && result.matches.size() > *max_matches) {
    result.matches.resize(static_cast<std::size_t>(*max_matches));
  }
  result.truncated = result.matches.size() < result.total_matches;
  return result;
}

SaAcceleration SuffixArray::Acceleration() const noexcept {
  return impl_->acceleration;
}
SaLookupAcceleration SuffixArray::LookupAcceleration() const noexcept {
  return impl_->HasLearned() ? SaLookupAcceleration::kSaplingPwl
                             : SaLookupAcceleration::kBinary;
}
std::uint32_t SuffixArray::SamplingRate() const noexcept {
  return impl_->sampling_rate;
}
Position SuffixArray::SuffixAt(std::uint64_t row) const {
  if (row >= SaSize(impl_->suffix_array)) {
    throw Error(ErrorCode::kInvalidInput, "suffix-array row is out of range");
  }
  return SaValue(impl_->suffix_array, row);
}

SequenceInfo SuffixArray::GetSequenceInfo(SequenceId id) const {
  const auto index = static_cast<std::size_t>(id);
  if (index >= impl_->reference.sequences.size()) {
    throw Error(ErrorCode::kInvalidInput, "sequence id is out of range");
  }
  return impl_->reference.sequences[index];
}

IndexInfo SuffixArray::GetInfo() const { return impl_->index_info; }

void SuffixArray::Save(const std::filesystem::path& path,
                       const SaveOptions& options) const {
  detail::ContainerSpec spec;
  spec.format_minor =
      impl_->sampling_rate > 1 ? 3
      : impl_->HasLearned()
          ? 2
          : (impl_->acceleration == SaAcceleration::kNone ? 0 : 1);
  spec.kind = IndexKind::kSuffixArray;
  spec.backend = impl_->backend;
  spec.coordinate_width = impl_->index_info.coordinate_width;
  spec.sequence_count = impl_->reference.sequences.size();
  spec.total_bases = impl_->reference.total_bases;
  spec.text_symbols = impl_->text.size();
  spec.ambiguous_bases = impl_->reference.ambiguous_bases;
  spec.fingerprint = impl_->reference.fingerprint;
  std::vector<detail::SectionWriter> writers{
      {detail::SectionType::kMetadata,
       [&](std::ostream& out) {
         detail::WriteMetadata(out, impl_->reference);
       }},
      {detail::SectionType::kText,
       [&](std::ostream& out) {
         out.write(reinterpret_cast<const char*>(impl_->text.data()),
                   static_cast<std::streamsize>(impl_->text.size()));
       }},
      {detail::SectionType::kSuffixArray, [&](std::ostream& out) {
         std::visit(
             [&](const auto& values) {
               detail::WriteU64(out, values.size());
               WriteIntegerPayload(
                   out, values,
                   static_cast<std::uint8_t>(sizeof(typename std::decay_t<
                                                      decltype(values)>::
                                                      value_type) *
                                             8),
                   "suffix-array values");
             },
             impl_->suffix_array);
       }}};
  if (impl_->HasIsa()) {
    writers.push_back(
        {detail::SectionType::kInverseSuffixArray, [&](std::ostream& out) {
           WriteIntegerVector(out, impl_->isa, spec.coordinate_width);
         }});
  }
  if (impl_->HasLcp()) {
    writers.push_back({detail::SectionType::kLcp, [&](std::ostream& out) {
                         WriteIntegerVector(
                             out, impl_->lcp,
                             impl_->text.size() <=
                                     std::numeric_limits<std::uint32_t>::max()
                                 ? 32
                                 : 64);
                       }});
  }
  if (impl_->HasChild()) {
    writers.push_back({detail::SectionType::kChild, [&](std::ostream& out) {
                         WriteIntegerVector(out, impl_->child,
                                            spec.coordinate_width);
                       }});
  }
  if (impl_->HasLearned()) {
    writers.push_back({detail::SectionType::kLearnedSa, [&](std::ostream& out) {
                         WriteLearnedIndex(out, impl_->learned,
                                           spec.coordinate_width,
                                           impl_->reference.fingerprint);
                       }});
  }
  if (impl_->sampling_rate > 1) {
    writers.push_back(
        {detail::SectionType::kSaSampling, [&](std::ostream& out) {
           detail::WriteU32(out, impl_->sampling_rate);
           detail::WriteU64(out, SaSize(impl_->suffix_array));
         }});
  }
  detail::WriteContainer(path, options, spec, writers);
}

SuffixArray SuffixArray::Load(const std::filesystem::path& path) {
  const auto container = detail::ReadContainer(path);
  if (container.spec.kind != IndexKind::kSuffixArray) {
    throw Error(ErrorCode::kCorruptIndex,
                "index does not contain a suffix array");
  }
  if (container.spec.backend != detail::StoredBackend::kDivsufsort32 &&
      container.spec.backend != detail::StoredBackend::kDivsufsort64 &&
      container.spec.backend != detail::StoredBackend::kCaps32 &&
      container.spec.backend != detail::StoredBackend::kCaps64) {
    throw Error(ErrorCode::kUnsupportedBackend,
                "unsupported suffix-array payload");
  }
  auto impl = std::make_unique<Impl>();
  impl->reference = detail::ReadMetadata(container);
  impl->backend = container.spec.backend;
  const auto& text_section =
      detail::RequireSection(container, detail::SectionType::kText);
  if (text_section.size != container.spec.text_symbols ||
      text_section.size < 2) {
    throw Error(ErrorCode::kCorruptIndex,
                "invalid suffix-array text section size");
  }
  auto text_input =
      detail::OpenSectionStream(container, detail::SectionType::kText);
  impl->text.resize(static_cast<std::size_t>(text_section.size));
  text_input->read(reinterpret_cast<char*>(impl->text.data()),
                   static_cast<std::streamsize>(impl->text.size()));
  if (text_input->gcount() != static_cast<std::streamsize>(impl->text.size()) ||
      impl->text.back() != detail::kSentinel ||
      std::find(impl->text.begin(), std::prev(impl->text.end()),
                detail::kSentinel) != std::prev(impl->text.end())) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array text has an invalid sentinel");
  }
  for (const auto& sequence : impl->reference.sequences) {
    const auto separator = sequence.global_offset + sequence.length;
    if (separator >= impl->text.size() ||
        impl->text[static_cast<std::size_t>(separator)] != detail::kSeparator) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array text has an invalid contig separator");
    }
  }
  if (detail::ContentFingerprint(impl->text.data(), impl->text.size() - 1) !=
      impl->reference.fingerprint) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array text fingerprint mismatch");
  }
  const auto has_section = [&](detail::SectionType type) {
    return std::any_of(
        container.sections.begin(), container.sections.end(),
        [type](const auto& section) { return section.type == type; });
  };
  if (has_section(detail::SectionType::kSaSampling)) {
    if (container.spec.format_minor < 3) {
      throw Error(ErrorCode::kCorruptIndex,
                  "sampled SA data requires sufidx format 1.3");
    }
    auto sampling_input =
        detail::OpenSectionStream(container, detail::SectionType::kSaSampling);
    impl->sampling_rate = detail::ReadU32(*sampling_input, "SA sampling rate");
    const auto recorded_count =
        detail::ReadU64(*sampling_input, "sampled suffix count");
    if (impl->sampling_rate <= 1) {
      throw Error(ErrorCode::kCorruptIndex, "invalid SA sampling rate");
    }
    const auto expected =
        SampledSuffixCount(impl->text.size(), impl->sampling_rate);
    if (recorded_count != expected ||
        sampling_input->peek() != std::char_traits<char>::eof()) {
      throw Error(ErrorCode::kCorruptIndex, "invalid sampled SA metadata");
    }
  }
  auto sa_input =
      detail::OpenSectionStream(container, detail::SectionType::kSuffixArray);
  const auto count = detail::ReadU64(*sa_input, "suffix-array length");
  const auto expected_count =
      SampledSuffixCount(impl->text.size(), impl->sampling_rate);
  if (count != expected_count) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array length does not match sampling metadata");
  }
  std::vector<bool> seen(static_cast<std::size_t>(count), false);
  const auto accept_position = [&](std::uint64_t raw) {
    if (raw >= impl->text.size() || raw % impl->sampling_rate != 0) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix array contains a non-sampled position");
    }
    const auto sample = raw / impl->sampling_rate;
    if (sample >= count || seen[static_cast<std::size_t>(sample)]) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix array is not a sampled permutation");
    }
    seen[static_cast<std::size_t>(sample)] = true;
  };
  const bool backend_is_32 =
      container.spec.backend == detail::StoredBackend::kDivsufsort32 ||
      container.spec.backend == detail::StoredBackend::kCaps32;
  const bool backend_is_caps =
      container.spec.backend == detail::StoredBackend::kCaps32 ||
      container.spec.backend == detail::StoredBackend::kCaps64;
  if (backend_is_32) {
    if (container.spec.coordinate_width != 32) {
      throw Error(ErrorCode::kCorruptIndex,
                  "invalid 32-bit suffix-array coordinate width");
    }
    if (backend_is_caps) {
      impl->suffix_array = CapsSa32(static_cast<std::size_t>(count));
    } else {
      impl->suffix_array = Sa32(static_cast<std::size_t>(count));
    }
  } else {
    if (container.spec.coordinate_width != 64) {
      throw Error(ErrorCode::kCorruptIndex,
                  "invalid 64-bit suffix-array coordinate width");
    }
    if (backend_is_caps) {
      impl->suffix_array = CapsSa64(static_cast<std::size_t>(count));
    } else {
      impl->suffix_array = Sa64(static_cast<std::size_t>(count));
    }
  }
  std::visit(
      [&](auto& values) {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        constexpr auto width =
            static_cast<std::uint8_t>(sizeof(Value) * 8);
        ReadIntegerPayload(*sa_input, values, width, "suffix-array values",
                           [&](std::uint64_t, std::uint64_t raw) {
                             accept_position(raw);
                           });
      },
      impl->suffix_array);
  if (sa_input->peek() != std::char_traits<char>::eof()) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array section has trailing bytes");
  }

  const bool has_isa = has_section(detail::SectionType::kInverseSuffixArray);
  const bool has_lcp = has_section(detail::SectionType::kLcp);
  const bool has_child = has_section(detail::SectionType::kChild);
  const bool has_learned = has_section(detail::SectionType::kLearnedSa);
  if (!has_isa && !has_lcp && !has_child) {
    impl->acceleration = SaAcceleration::kNone;
  } else if (!has_isa && has_lcp && !has_child) {
    impl->acceleration = SaAcceleration::kLcp;
  } else if (!has_isa && has_lcp && has_child) {
    impl->acceleration = SaAcceleration::kLcpChild;
  } else if (has_isa && has_lcp && !has_child) {
    impl->acceleration = SaAcceleration::kLcpSuffixLink;
  } else if (has_isa && has_lcp && has_child) {
    impl->acceleration = SaAcceleration::kFull;
  } else {
    throw Error(ErrorCode::kCorruptIndex,
                "invalid suffix-array auxiliary section combination");
  }
  if (impl->acceleration != SaAcceleration::kNone &&
      container.spec.format_minor < 1) {
    throw Error(ErrorCode::kCorruptIndex,
                "auxiliary sections require sufidx format 1.1");
  }
  if (has_learned && container.spec.format_minor < 2) {
    throw Error(ErrorCode::kCorruptIndex,
                "learned SA data requires sufidx format 1.2");
  }
  if (has_isa) {
    impl->isa = ReadIntegerVector(
        container, detail::SectionType::kInverseSuffixArray, count,
        container.spec.coordinate_width, "inverse suffix array");
    const auto isa_values = ViewCoordinates(impl->isa);
    for (std::uint64_t position = 0; position < count; ++position) {
      const auto row = isa_values[static_cast<std::size_t>(position)];
      if (row >= count ||
          SaValue(impl->suffix_array, row) != position * impl->sampling_rate) {
        throw Error(ErrorCode::kCorruptIndex,
                    "inverse suffix array is inconsistent with suffix array");
      }
    }
  }
  if (has_lcp) {
    const auto lcp_width = static_cast<std::uint8_t>(
        container.spec.text_symbols <=
                std::numeric_limits<std::uint32_t>::max()
            ? 32
            : 64);
    impl->lcp = ReadIntegerVector(container, detail::SectionType::kLcp, count,
                                  lcp_width, "LCP");
    const auto lcp_values = ViewCoordinates(impl->lcp);
    if (lcp_values.size == 0 || lcp_values[0] != 0) {
      throw Error(ErrorCode::kCorruptIndex, "LCP[0] must be zero");
    }
    for (std::uint64_t row = 1; row < count; ++row) {
      const auto suffix = SaValue(impl->suffix_array, row);
      const auto previous = SaValue(impl->suffix_array, row - 1);
      if (lcp_values[static_cast<std::size_t>(row)] >
          std::min(impl->text.size() - suffix, impl->text.size() - previous)) {
        throw Error(ErrorCode::kCorruptIndex,
                    "LCP value exceeds suffix bounds");
      }
    }
  }
  if (has_child) {
    impl->child =
        ReadIntegerVector(container, detail::SectionType::kChild, count,
                          container.spec.coordinate_width, "CHILD");
    const auto child_values = ViewCoordinates(impl->child);
    for (std::size_t index = 0; index < child_values.size; ++index) {
      if (child_values[index] > count) {
        throw Error(ErrorCode::kCorruptIndex, "CHILD index is out of range");
      }
    }
    if (impl->child !=
        BuildChild(impl->lcp, container.spec.coordinate_width)) {
      throw Error(ErrorCode::kCorruptIndex,
                  "CHILD table is inconsistent with LCP");
    }
  }
  if (has_learned) {
    impl->learned = ReadLearnedIndex(container, count);
  }
  impl->index_info = detail::IndexInfoFromContainer(container);
  if (impl->index_info.sa_sampling_rate != impl->sampling_rate ||
      impl->index_info.suffix_count != count) {
    throw Error(ErrorCode::kCorruptIndex,
                "sampled SA metadata is inconsistent");
  }
  impl->index_info.sa_acceleration = impl->acceleration;
  impl->index_info.auxiliary_bytes = CoordinateBytes(impl->isa) +
                                     CoordinateBytes(impl->lcp) +
                                     CoordinateBytes(impl->child);
  if (has_learned) {
    impl->index_info.sa_lookup_acceleration = SaLookupAcceleration::kSaplingPwl;
    impl->index_info.learned_index_bytes =
        impl->learned.SerializedBytes(container.spec.coordinate_width);
    impl->index_info.learned_k = impl->learned.k;
    impl->index_info.learned_bucket_bits = impl->learned.bucket_bits;
    impl->index_info.learned_memory_overhead_basis_points =
        impl->learned.memory_overhead_basis_points;
  }
  return SuffixArray(std::move(impl));
}

}  // namespace sufkit
