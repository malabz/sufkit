// SPDX-License-Identifier: MIT

#include "sufkit/suffix_array.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "caps_backend.hpp"
#include "coordinate_storage.hpp"
#include "divsufsort_backend.hpp"
#include "fast_prefix_index.hpp"
#include "genome_reference_internal.hpp"
#include "lcp_storage.hpp"
#include "query.hpp"
#include "reference_data.hpp"
#include "sa_codec.hpp"
#include "sequence_compare.hpp"
#include "serialization.hpp"
#include "suffix_link_diagnostics.hpp"
#include <sufkit/version.hpp>

namespace sufkit {
namespace {

using Sa32 = std::vector<std::int32_t>;
using Sa64 = detail::DivSufsort64Buffer;
using CapsSa32 = std::vector<std::uint32_t>;
using CapsSa64 = std::vector<std::uint64_t>;
using Coordinate40 = detail::Coordinate40Storage;
using Coordinate48 = detail::Coordinate48Storage;
using RawSaStorage = std::variant<Sa32, Sa64, CapsSa32, CapsSa64>;
using SaStorage = detail::CoordinateStorage;

using Coordinate32 = std::vector<std::uint32_t>;
using Coordinate64 = std::vector<std::uint64_t>;
using RawCoordinateStorage = std::variant<Coordinate32, Coordinate64>;
using CoordinateStorage = detail::CoordinateStorage;
using BuildClock = std::chrono::steady_clock;

#if defined(SUFKIT_INTERNAL_FORCE_RAW_LCP)
inline constexpr bool kForceRawLcpForDeveloperBenchmark = true;
#else
inline constexpr bool kForceRawLcpForDeveloperBenchmark = false;
#endif

std::uint64_t SaValue(const SaStorage& storage, std::uint64_t row);

template <class Values>
inline constexpr bool kIsPackedCoordinateStorage =
    std::is_same_v<std::decay_t<Values>, Coordinate40> ||
    std::is_same_v<std::decay_t<Values>, Coordinate48>;

struct CoordinateView {
  const void* low = nullptr;
  const void* high = nullptr;
  std::size_t size = 0;
  CoordinateStorageWidth width = CoordinateStorageWidth::kBits32;

  std::uint64_t operator[](std::size_t index) const noexcept {
    switch (width) {
      case CoordinateStorageWidth::kBits32:
        return static_cast<const std::uint32_t*>(low)[index];
      case CoordinateStorageWidth::kBits40:
        return static_cast<const std::uint32_t*>(low)[index] |
               (static_cast<std::uint64_t>(
                    static_cast<const std::uint8_t*>(high)[index])
                << 32U);
      case CoordinateStorageWidth::kBits48:
        return static_cast<const std::uint32_t*>(low)[index] |
               (static_cast<std::uint64_t>(
                    static_cast<const std::uint16_t*>(high)[index])
                << 32U);
      case CoordinateStorageWidth::kBits64:
        return static_cast<const std::uint64_t*>(low)[index];
      case CoordinateStorageWidth::kAutoSelect:
        break;
    }
    return 0;
  }
};

template <class Values>
std::size_t CoordinateCount(const Values& values) noexcept {
  if constexpr (std::is_same_v<std::decay_t<Values>, CoordinateView>) {
    return values.size;
  } else {
    return values.size();
  }
}

// Resolve the immutable LCP representation once per query. Raw values and the
// common (<255) byte-coded case then use direct pointer loads; only an actual
// overflow marker enters the anchor decoder in LcpStorage.
struct LcpAccess {
  const detail::LcpStorage* storage = nullptr;
  const std::uint32_t* raw32 = nullptr;
  const std::uint64_t* raw64 = nullptr;
  const std::uint8_t* primary = nullptr;

  template <class SuffixPosition>
  std::uint64_t ExactLazy(std::size_t row,
                          SuffixPosition&& suffix_position) const {
    if (raw32 != nullptr) {
      return raw32[row];
    }
    if (raw64 != nullptr) {
      return raw64[row];
    }
    if (primary != nullptr &&
        primary[row] < detail::LcpStorage::kByteLimit) {
      return primary[row];
    }
    return storage->Exact(row, suffix_position());
  }

  std::uint64_t Exact(std::size_t row,
                      std::uint64_t suffix_position) const {
    return ExactLazy(row, [suffix_position] { return suffix_position; });
  }

  template <class SuffixPosition>
  bool AtLeastLazy(std::size_t row, SuffixPosition&& suffix_position,
                   std::uint64_t target) const {
    if (raw32 != nullptr) {
      return raw32[row] >= target;
    }
    if (raw64 != nullptr) {
      return raw64[row] >= target;
    }
    if (primary != nullptr) {
      const auto value = primary[row];
      if (value < detail::LcpStorage::kByteLimit) {
        return value >= target;
      }
      if (target <= detail::LcpStorage::kByteLimit) {
        return true;
      }
      return storage->AtLeast(row, suffix_position(), target);
    }
    return storage->AtLeast(row, suffix_position(), target);
  }

  bool AtLeast(std::size_t row, std::uint64_t suffix_position,
               std::uint64_t target) const {
    return AtLeastLazy(row, [suffix_position] { return suffix_position; },
                       target);
  }
};

struct LcpView {
  LcpAccess access;
  const SaStorage* suffix_array = nullptr;
  std::size_t row_count = 0;

  std::size_t size() const noexcept { return row_count; }

  std::uint64_t operator[](std::size_t row) const {
    return access.ExactLazy(
        row, [&] { return SaValue(*suffix_array, row); });
  }
};

template <class SaVector>
struct TypedLcpView {
  LcpAccess access;
  const SaVector* suffix_array = nullptr;

  std::uint64_t operator[](std::size_t row) const {
    return access.ExactLazy(row, [&] {
      return static_cast<std::uint64_t>((*suffix_array)[row]);
    });
  }

  bool AtLeast(std::size_t row, std::uint64_t target) const {
    return access.AtLeastLazy(
        row,
        [&] { return static_cast<std::uint64_t>((*suffix_array)[row]); },
        target);
  }
};

struct RawLcpView {
  const std::uint32_t* raw32 = nullptr;
  const std::uint64_t* raw64 = nullptr;

  bool AtLeast(std::size_t row, std::uint64_t target) const noexcept {
    return raw32 != nullptr ? raw32[row] >= target : raw64[row] >= target;
  }
};

LcpAccess MakeLcpAccess(const detail::LcpStorage& storage) noexcept {
  LcpAccess result;
  result.storage = &storage;
  if (const auto* raw32 = storage.Raw32Values()) {
    result.raw32 = raw32->data();
  } else if (const auto* raw64 = storage.Raw64Values()) {
    result.raw64 = raw64->data();
  } else if (const auto* primary = storage.BytePrimary()) {
    result.primary = primary->data();
  }
  return result;
}

CoordinateView ViewCoordinates(const CoordinateStorage& storage) noexcept {
  return storage.Visit(
      [](const auto& values) {
        using Values = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Values, Coordinate40>) {
          return CoordinateView{values.low.data(), values.high.data(),
                                values.size(),
                                CoordinateStorageWidth::kBits40};
        } else if constexpr (std::is_same_v<Values, Coordinate48>) {
          return CoordinateView{values.low.data(), values.high.data(),
                                values.size(),
                                CoordinateStorageWidth::kBits48};
        } else {
          using Value = typename Values::value_type;
          return CoordinateView{
              values.data(), nullptr, values.size(),
              sizeof(Value) == sizeof(std::uint64_t)
                  ? CoordinateStorageWidth::kBits64
                  : CoordinateStorageWidth::kBits32};
        }
      });
}

bool CoordinatesEmpty(const CoordinateStorage& storage) noexcept {
  return storage.Empty();
}

std::uint64_t CoordinateBytes(const CoordinateStorage& storage) noexcept {
  return storage.Bytes();
}

RawCoordinateStorage CompactRawCoordinates(
    std::vector<std::uint64_t>&& values, std::uint8_t width,
    ErrorCode error_code, const char* label) {
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
  return compact;
}

RawCoordinateStorage CompactRawCoordinates(
    std::vector<std::uint32_t>&& values, std::uint8_t width,
    ErrorCode error_code, const char* label) {
  if (width == 32) {
    return Coordinate32(std::move(values));
  }
  if (width != 64) {
    throw Error(error_code, std::string("invalid ") + label + " width");
  }
  Coordinate64 expanded;
  try {
    expanded.assign(values.begin(), values.end());
  } catch (const std::bad_alloc&) {
    throw Error(error_code, std::string("cannot allocate ") + label);
  }
  return expanded;
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

// Exact SA queries up to 512 bases use stack storage. This removes one heap
// allocation from the common count/equal-range path and two from a both-strand
// query, while retaining an unbounded heap fallback for long patterns.
class ExactPatternBuffer {
 public:
  explicit ExactPatternBuffer(std::string_view pattern)
      : ExactPatternBuffer(pattern.size()) {
    if (pattern.empty()) {
      throw Error(ErrorCode::kInvalidInput, "pattern must not be empty");
    }
    auto* output = MutableData();
    for (std::size_t index = 0; index < pattern.size(); ++index) {
      const auto symbol = kRightMaximalEncoding[
          static_cast<unsigned char>(pattern[index])];
      if (symbol == detail::kSentinel) {
        throw Error(ErrorCode::kInvalidInput,
                    "pattern contains a non-ACGT character");
      }
      output[index] = symbol;
    }
  }

  ExactPatternBuffer ReverseComplement() const {
    ExactPatternBuffer result(size_);
    auto* output = result.MutableData();
    const auto* input = Data();
    for (std::size_t index = 0; index < size_; ++index) {
      output[index] = kRightMaximalComplement[input[size_ - index - 1U]];
    }
    return result;
  }

  bool Equals(const ExactPatternBuffer& other) const noexcept {
    return size_ == other.size_ &&
           std::equal(Data(), Data() + size_, other.Data());
  }

  EncodedView View() const noexcept { return {Data(), size_}; }
  std::size_t Size() const noexcept { return size_; }

 private:
  static constexpr std::size_t kInlineBases = 512;

  explicit ExactPatternBuffer(std::size_t size) : size_(size) {
    if (size_ > kInlineBases) {
      heap_.resize(size_);
    }
  }

  const std::uint8_t* Data() const noexcept {
    return size_ <= kInlineBases ? inline_.data() : heap_.data();
  }
  std::uint8_t* MutableData() noexcept {
    return size_ <= kInlineBases ? inline_.data() : heap_.data();
  }

  // Constructors initialize exactly the active prefix. Leaving the remaining
  // stack capacity untouched avoids clearing 512 bytes for every short query.
  std::array<std::uint8_t, kInlineBases> inline_;
  std::vector<std::uint8_t> heap_;
  std::size_t size_ = 0;
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

void ReplaceGlobalMatchHeapRoot(std::vector<GlobalMatch>& matches,
                                GlobalMatch replacement) {
  // The replacement is smaller than the current maximum. Sift it down once
  // instead of traversing the bounded heap for both pop_heap and push_heap.
  std::size_t parent = 0;
  while (true) {
    const auto left = parent * 2 + 1;
    if (left >= matches.size()) {
      break;
    }
    auto child = left;
    const auto right = left + 1;
    if (right < matches.size() &&
        GlobalMatchLess(matches[left], matches[right])) {
      child = right;
    }
    if (!GlobalMatchLess(replacement, matches[child])) {
      break;
    }
    matches[parent] = matches[child];
    parent = child;
  }
  matches[parent] = replacement;
}

void RetainBoundedGlobalMatch(std::vector<GlobalMatch>& matches,
                              GlobalMatch match, std::uint64_t limit,
                              bool& heap_active) {
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
    ReplaceGlobalMatchHeapRoot(matches, match);
  }
}

void RetainSmallestGlobalMatch(std::optional<GlobalMatch>& smallest,
                               GlobalMatch match) {
  if (!smallest || GlobalMatchLess(match, *smallest)) {
    smallest = match;
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

QueryResult FinalizeSmallestGlobalMatch(
    const std::optional<GlobalMatch>& global,
    const detail::ReferenceData& reference, std::uint64_t pattern_length,
    std::uint64_t total_hits) {
  QueryResult result;
  result.total_hits = total_hits;
  if (!global) {
    result.truncated = total_hits != 0;
    return result;
  }

  const auto& starts = reference.contig_starts;
  const auto& lengths = reference.contig_lengths;
  if (starts.empty() || starts.size() != lengths.size()) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array reference coordinates are invalid");
  }
  const auto sequence =
      std::upper_bound(starts.begin(), starts.end(), global->global_position);
  if (sequence == starts.begin()) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array hit is outside a reference contig");
  }
  const auto sequence_index =
      static_cast<std::size_t>(sequence - starts.begin() - 1);
  const Position local = global->global_position - starts[sequence_index];
  if (local > lengths[sequence_index] ||
      pattern_length > lengths[sequence_index] - local) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array hit is outside a reference contig");
  }
  result.hits.push_back(
      {static_cast<SequenceId>(sequence_index), local, pattern_length,
       global->strand});
  result.truncated = total_hits > 1;
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
  const auto compare = [&](std::uint64_t row) {
    const auto suffix = static_cast<std::uint64_t>(
        suffix_array[static_cast<std::size_t>(row)]);
    return statistics
               ? CompareSuffixPatternLcp(text, suffix, pattern, 0, statistics)
                     .order
               : CompareSuffixPattern(text, suffix, pattern);
  };
  std::uint64_t begin = 0;
  std::uint64_t end = suffix_array.size();
  while (begin < end) {
    const auto middle = begin + (end - begin) / 2;
    if (compare(middle) < 0) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  const auto lower = begin;
  end = suffix_array.size();
  while (begin < end) {
    const auto middle = begin + (end - begin) / 2;
    if (compare(middle) <= 0) {
      begin = middle + 1;
    } else {
      end = middle;
    }
  }
  return {lower, begin};
}

template <class SaVector>
std::uint64_t LcpBoundaryFor(const std::vector<std::uint8_t>& text,
                             const SaVector& suffix_array,
                             EncodedView pattern,
                             std::uint64_t begin, std::uint64_t end, bool upper,
                             SaSearchStatistics* statistics,
                             std::uint64_t known_prefix = 0) {
  std::uint64_t left_lcp = known_prefix;
  std::uint64_t right_lcp = known_prefix;
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
                        SaSearchStatistics* statistics,
                        std::uint64_t known_prefix = 0) {
  const auto lower =
      LcpBoundaryFor(text, suffix_array, pattern, search_range.begin,
                     search_range.end, false, statistics, known_prefix);
  const auto upper = LcpBoundaryFor(text, suffix_array, pattern, lower,
                                    search_range.end, true, statistics,
                                    known_prefix);
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
  return storage.At(row);
}

std::uint64_t SaSize(const SaStorage& storage) noexcept {
  return storage.Size();
}

CoordinateStorageWidth SaStoredWidth(const SaStorage& storage) noexcept {
  return storage.Width();
}

std::uint64_t SaBytes(const SaStorage& storage) noexcept {
  return storage.Bytes();
}

CoordinateStorageWidth ResolveProfileStorageWidth(
    CoordinateStorageWidth requested, SaResourceProfile profile,
    std::uint64_t symbol_count) {
  if (profile != SaResourceProfile::kFast &&
      profile != SaResourceProfile::kLowMemory) {
    throw Error(ErrorCode::kInvalidInput,
                "invalid suffix-array resource profile");
  }
  if (requested != CoordinateStorageWidth::kAutoSelect) {
    return detail::ResolveCoordinateStorageWidth(requested, symbol_count);
  }
  const auto narrowest = detail::SelectCoordinateStorageWidth(symbol_count);
  if (profile == SaResourceProfile::kFast &&
      narrowest != CoordinateStorageWidth::kBits32) {
    return CoordinateStorageWidth::kBits64;
  }
  return narrowest;
}

CoordinateStorageWidth ResolveAuxiliaryStorageWidth(
    CoordinateStorageWidth preferred, std::uint64_t symbol_count) {
  if (symbol_count - 1U <= detail::MaxCoordinateForWidth(preferred)) {
    return preferred;
  }
  return detail::SelectCoordinateStorageWidth(symbol_count);
}

template <class Source>
std::uint64_t CheckedSaValue(const Source& source, std::size_t index,
                             std::uint64_t text_size,
                             std::uint32_t sampling_rate) {
  using Value = std::decay_t<decltype(source[index])>;
  if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>) {
    if (source[index] < 0) {
      throw Error(ErrorCode::kBuildFailure,
                  "suffix array contains a negative position");
    }
  }
  const auto value = static_cast<std::uint64_t>(source[index]);
  if (value >= text_size || value % sampling_rate != 0) {
    throw Error(ErrorCode::kBuildFailure,
                "suffix array contains an invalid sampled position");
  }
  return value;
}

SaStorage RepackSuffixArray(RawSaStorage&& storage,
                            CoordinateStorageWidth width,
                            std::uint64_t text_size,
                            std::uint32_t sampling_rate) {
  const auto count = std::visit(
      [](const auto& values) {
        return static_cast<std::uint64_t>(values.size());
      },
      storage);
  const auto expected_count =
      text_size == 0 ? 0 : 1 + (text_size - 1) / sampling_rate;
  if (count != expected_count) {
    throw Error(ErrorCode::kBuildFailure,
                "suffix-array length disagrees with sampling metadata");
  }
  std::vector<bool> seen(static_cast<std::size_t>(count), false);
  const auto validate = [&](std::uint64_t value) {
    const auto sample = value / sampling_rate;
    if (sample >= count || seen[static_cast<std::size_t>(sample)]) {
      throw Error(ErrorCode::kBuildFailure,
                  "suffix array is not a sampled permutation");
    }
    seen[static_cast<std::size_t>(sample)] = true;
  };

  return std::visit(
      [&](auto& source) -> SaStorage {
        using Source = std::decay_t<decltype(source)>;
        for (std::size_t index = 0; index < source.size(); ++index) {
          validate(CheckedSaValue(source, index, text_size, sampling_rate));
        }
        // The sampled-permutation bitmap is no longer needed once every row
        // is validated. Release it before a 64-bit result allocates the
        // split40/split48 high plane, preserving the 8N+high down-pack peak.
        std::vector<bool>{}.swap(seen);
        if constexpr (std::is_same_v<Source, CapsSa32>) {
          return SaStorage::FromUInt32(std::move(source), width, text_size,
                                      ErrorCode::kBuildFailure,
                                      "suffix array");
        } else if constexpr (std::is_same_v<Source, CapsSa64>) {
          return SaStorage::FromUInt64(std::move(source), width, text_size,
                                      ErrorCode::kBuildFailure,
                                      "suffix array");
        } else if constexpr (std::is_same_v<Source, Sa32>) {
          CapsSa32 values(source.size());
          for (std::size_t index = 0; index < source.size(); ++index) {
            values[index] = static_cast<std::uint32_t>(source[index]);
          }
          return SaStorage::FromUInt32(std::move(values), width, text_size,
                                      ErrorCode::kBuildFailure,
                                      "suffix array");
        } else {
          return SaStorage::FromDivSufsort64(
              std::move(source), width, text_size, ErrorCode::kBuildFailure,
              "suffix array");
        }
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

void SampleSa(RawSaStorage& storage, std::uint32_t sampling_rate) {
  std::visit([&](auto& values) { SampleSaInPlace(values, sampling_rate); },
             storage);
}

void SampleSaLcp(RawSaStorage& storage, RawCoordinateStorage& lcp,
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
  const auto rows_per_worker = count / thread_count;
  const auto workers_with_extra_row = count % thread_count;
  for (std::uint64_t worker = 0; worker < thread_count; ++worker) {
    // Quotient/remainder partitioning avoids overflowing count * worker for
    // very large coordinate domains and still covers [0, count) exactly.
    const auto begin = rows_per_worker * worker +
                       std::min(worker, workers_with_extra_row);
    const auto end = begin + rows_per_worker +
                     (worker < workers_with_extra_row ? 1U : 0U);
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
                           CoordinateStorageWidth storage_width) {
  const auto count = sa.Size();
  const bool source_fits_32 =
      count - 1U <= std::numeric_limits<std::uint32_t>::max();
  return sa.Visit(
      [&](const auto& values) -> CoordinateStorage {
        if (source_fits_32) {
          auto isa = BuildIsaFor<std::uint32_t>(
              values, text_size, sampling_rate, requested_threads);
          return CoordinateStorage::FromUInt32(
              std::move(isa), storage_width, count,
              ErrorCode::kBuildFailure, "inverse suffix array");
        }
        auto isa = BuildIsaFor<std::uint64_t>(
            values, text_size, sampling_rate, requested_threads);
        return CoordinateStorage::FromUInt64(
            std::move(isa), storage_width, count,
            ErrorCode::kBuildFailure, "inverse suffix array");
      });
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

RawCoordinateStorage BuildLcp(const std::vector<std::uint8_t>& text,
                              const SaStorage& sa,
                              const CoordinateStorage& isa,
                              std::uint32_t sampling_rate,
                              std::uint8_t lcp_width) {
  return sa.Visit([&](const auto& sa_values) {
    return isa.Visit(
        [&](const auto& isa_values) -> RawCoordinateStorage {
          if (lcp_width == 32) {
            return BuildLcpFor<std::uint32_t>(text, sa_values, isa_values,
                                              sampling_rate);
          }
          if (lcp_width == 64) {
            return BuildLcpFor<std::uint64_t>(text, sa_values, isa_values,
                                              sampling_rate);
          }
          throw Error(ErrorCode::kBuildFailure, "invalid LCP width");
        });
  });
}

detail::LcpStorage BuildByteCodedLcpDirect(
    const std::vector<std::uint8_t>& text, const SaStorage& sa,
    const CoordinateStorage& isa, std::uint32_t sampling_rate,
    std::uint8_t lcp_width) {
  return sa.Visit([&](const auto& sa_values) {
    return isa.Visit([&](const auto& isa_values) {
      return detail::LcpStorage::BuildByteCodedDirect(
          text.data(), text.size(), detail::IntegerArrayView(sa_values),
          detail::IntegerArrayView(isa_values), sampling_rate, lcp_width,
          ErrorCode::kBuildFailure);
    });
  });
}

LcpView ViewLcp(const detail::LcpStorage& lcp,
                const SaStorage& suffix_array) noexcept {
  return LcpView{MakeLcpAccess(lcp), &suffix_array,
                 static_cast<std::size_t>(lcp.Size())};
}

detail::LcpStorage FinalizeLcpStorage(RawCoordinateStorage&& raw_lcp,
                                      const CoordinateStorage& isa,
                                      std::uint32_t sampling_rate,
                                      std::uint8_t lcp_width,
                                      std::uint64_t text_symbols,
                                      bool require_byte_coded,
                                      bool require_raw) {
  if (std::visit([](const auto& values) { return values.empty(); }, raw_lcp)) {
    return {};
  }
  const auto retain_raw = [&]() -> detail::LcpStorage {
    return std::visit(
        [&](auto& values) -> detail::LcpStorage {
          using Values = std::decay_t<decltype(values)>;
          if constexpr (std::is_same_v<Values, Coordinate32>) {
            return detail::LcpStorage::FromRaw32(
                std::move(values), sampling_rate, ErrorCode::kBuildFailure);
          } else {
            return detail::LcpStorage::FromRaw64(
                std::move(values), sampling_rate, ErrorCode::kBuildFailure);
          }
        },
        raw_lcp);
  };
  if (require_raw) {
    return retain_raw();
  }
  detail::LcpStorage byte_coded = std::visit(
      [&](const auto& lcp_values) {
        return isa.Visit([&](const auto& isa_values) {
          return detail::LcpStorage::BuildByteCoded(
              detail::IntegerArrayView(lcp_values),
              detail::IntegerArrayView(isa_values), sampling_rate, lcp_width,
              text_symbols, ErrorCode::kBuildFailure);
        });
      },
      raw_lcp);
  const auto raw_bytes = std::visit(
      [](const auto& values) {
        return static_cast<std::uint64_t>(values.size()) *
               sizeof(typename std::decay_t<decltype(values)>::value_type);
      },
      raw_lcp);
  if (require_byte_coded || byte_coded.SerializedDataBytes() < raw_bytes) {
    return byte_coded;
  }
  return retain_raw();
}

struct LearnedSaIndex {
  static constexpr std::uint32_t kModelId = 1;

  std::uint32_t k = 0;
  std::uint32_t bucket_bits = 0;
  std::uint32_t memory_overhead_basis_points = 0;
  std::vector<std::uint64_t> anchor_x;
  CoordinateStorage anchor_y;

  bool Empty() const noexcept { return anchor_x.empty(); }

  std::uint64_t SerializedBytes() const noexcept {
    const auto row_bytes =
        static_cast<std::uint64_t>(anchor_y.Width()) / 8U;
    constexpr std::uint64_t kCoordinateCodecHeaderBytes = 56;
    return 36ULL + kCoordinateCodecHeaderBytes +
           static_cast<std::uint64_t>(anchor_x.size()) * (8ULL + row_bytes);
  }

  std::uint64_t ResidentBytes() const noexcept {
    return static_cast<std::uint64_t>(anchor_x.size()) * 8ULL +
           anchor_y.Bytes();
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
                               CoordinateStorageWidth storage_width,
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
      static_cast<std::uint64_t>(storage_width) / 8U;
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
                                 CoordinateStorageWidth storage_width,
                                 const LearnedSaOptions& options) {
  LearnedSaIndex model;
  model.k = options.k;
  const auto suffix_count = SaSize(suffix_array);
  if (suffix_count == std::numeric_limits<std::uint64_t>::max()) {
    throw Error(ErrorCode::kBuildFailure,
                "learned SA coordinate domain overflows");
  }
  const auto model_width = ResolveAuxiliaryStorageWidth(
      storage_width, suffix_count + 1U);
  model.bucket_bits = ChooseBucketBits(suffix_count, model_width, options);
  model.memory_overhead_basis_points = options.memory_overhead_basis_points;
  const auto bucket_count = 1ULL << model.bucket_bits;
  const auto coordinate_max_size = Coordinate64().max_size();
  if (bucket_count > coordinate_max_size - 1) {
    throw Error(ErrorCode::kInvalidInput,
                "learned SA bucket count is too large");
  }
  RawCoordinateStorage raw_anchor_y = Coordinate32{};
  try {
    model.anchor_x.assign(static_cast<std::size_t>(bucket_count + 1),
                          std::numeric_limits<std::uint64_t>::max());
    if (model_width == CoordinateStorageWidth::kBits32) {
      raw_anchor_y = Coordinate32(
          static_cast<std::size_t>(bucket_count + 1),
          static_cast<std::uint32_t>(suffix_count));
    } else {
      raw_anchor_y = Coordinate64(
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
      raw_anchor_y);
  model.anchor_y = std::visit(
      [&](auto& values) -> CoordinateStorage {
        using Values = std::decay_t<decltype(values)>;
        const auto domain = suffix_count + 1U;
        if constexpr (std::is_same_v<Values, Coordinate32>) {
          return CoordinateStorage::FromUInt32(
              std::move(values), model_width, domain,
              ErrorCode::kBuildFailure, "learned SA anchors");
        } else {
          return CoordinateStorage::FromUInt64(
              std::move(values), model_width, domain,
              ErrorCode::kBuildFailure, "learned SA anchors");
        }
      },
      raw_anchor_y);
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

CoordinateStorage BuildChild(const RawCoordinateStorage& lcp,
                             CoordinateStorageWidth storage_width) {
  return std::visit(
      [&](const auto& lcp_values) -> CoordinateStorage {
        const auto count = static_cast<std::uint64_t>(lcp_values.size());
        if (count == std::numeric_limits<std::uint64_t>::max()) {
          throw Error(ErrorCode::kBuildFailure,
                      "CHILD coordinate domain overflows");
        }
        if (count <= std::numeric_limits<std::uint32_t>::max()) {
          auto child = BuildChildFor<std::uint32_t>(lcp_values);
          const auto domain = static_cast<std::uint64_t>(child.size()) + 1U;
          return CoordinateStorage::FromUInt32(
              std::move(child), storage_width, domain,
              ErrorCode::kBuildFailure, "CHILD");
        }
        auto child = BuildChildFor<std::uint64_t>(lcp_values);
        const auto domain = static_cast<std::uint64_t>(child.size()) + 1U;
        return CoordinateStorage::FromUInt64(
            std::move(child), storage_width, domain,
            ErrorCode::kBuildFailure, "CHILD");
      },
      lcp);
}

CoordinateStorage BuildChild(const detail::LcpStorage& lcp,
                             const SaStorage& suffix_array,
                             CoordinateStorageWidth storage_width,
                             ErrorCode error_code) {
  const auto values = ViewLcp(lcp, suffix_array);
  if (lcp.Size() == std::numeric_limits<std::uint64_t>::max()) {
    throw Error(error_code, "CHILD coordinate domain overflows");
  }
  if (lcp.Size() <= std::numeric_limits<std::uint32_t>::max()) {
    auto child = BuildChildFor<std::uint32_t>(values);
    const auto domain = static_cast<std::uint64_t>(child.size()) + 1U;
    return CoordinateStorage::FromUInt32(
        std::move(child), storage_width, domain, error_code, "CHILD");
  }
  auto child = BuildChildFor<std::uint64_t>(values);
  const auto domain = static_cast<std::uint64_t>(child.size()) + 1U;
  return CoordinateStorage::FromUInt64(
      std::move(child), storage_width, domain, error_code, "CHILD");
}

bool CoordinatesEqual(const CoordinateStorage& left,
                      const CoordinateStorage& right) {
  if (left.Size() != right.Size()) {
    return false;
  }
  for (std::uint64_t index = 0; index < left.Size(); ++index) {
    if (left.At(index) != right.At(index)) {
      return false;
    }
  }
  return true;
}

void ValidateLcpAgainstSuffixArray(const detail::LcpStorage& lcp,
                                   const SaStorage& suffix_array,
                                   std::uint64_t text_symbols) {
  suffix_array.Visit([&](const auto& values) {
    lcp.Validate(detail::IntegerArrayView(values), text_symbols,
                 ErrorCode::kCorruptIndex);
  });
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

bool SmemMatchLess(const SmemMatch& left, const SmemMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) <
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

bool MumMatchLess(const MumMatch& left, const MumMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) <
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

bool MemMatchEqual(const MemMatch& left, const MemMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) ==
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

bool MamMatchEqual(const MamMatch& left, const MamMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) ==
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

bool SmemMatchEqual(const SmemMatch& left, const SmemMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length,
                  left.reference_occurrences, left.strand) ==
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length,
                  right.reference_occurrences, right.strand);
}

bool MumMatchEqual(const MumMatch& left, const MumMatch& right) {
  return std::tie(left.query_position, left.sequence_id,
                  left.reference_position, left.length, left.strand) ==
         std::tie(right.query_position, right.sequence_id,
                  right.reference_position, right.length, right.strand);
}

void ValidateStrandMode(StrandMode strands) {
  switch (strands) {
    case StrandMode::kForward:
    case StrandMode::kReverseComplement:
    case StrandMode::kBoth:
      return;
  }
  throw Error(ErrorCode::kInvalidInput, "invalid suffix-array strand mode");
}

void ValidateSaAcceleration(SaAcceleration acceleration) {
  switch (acceleration) {
    case SaAcceleration::kNone:
    case SaAcceleration::kLcp:
    case SaAcceleration::kLcpChild:
    case SaAcceleration::kLcpSuffixLink:
    case SaAcceleration::kFull:
      return;
  }
  throw Error(ErrorCode::kInvalidInput,
              "invalid suffix-array acceleration");
}

RightMaximalSearchAlgorithm AsRightMaximalAlgorithm(
    MemSearchAlgorithm algorithm) {
  static_assert(static_cast<std::uint8_t>(MemSearchAlgorithm::kFull) ==
                static_cast<std::uint8_t>(
                    RightMaximalSearchAlgorithm::kFull));
  return static_cast<RightMaximalSearchAlgorithm>(algorithm);
}

void PrepareMemSearch(const MemOptions& options) {
  ValidateStrandMode(options.strands);
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
  ValidateStrandMode(options.strands);
  if (options.min_length == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "reference-MAM minimum length must be greater than zero");
  }
}

void PrepareSmemSearch(const SmemOptions& options) {
  ValidateStrandMode(options.strands);
  if (options.min_length == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "SMEM minimum length must be greater than zero");
  }
  if (options.min_occurrences == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "SMEM minimum occurrence count must be greater than zero");
  }
}

void PrepareMumSearch(const MumOptions& options) {
  ValidateStrandMode(options.strands);
  if (options.min_length == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "MUM minimum length must be greater than zero");
  }
}

void PrepareRightMaximalSearch(const RightMaximalOptions& options) {
  ValidateStrandMode(options.strands);
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
  info.format_version = "1.4";
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
    info.learned_index_bytes = learned.SerializedBytes();
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
                       std::uint64_t fingerprint,
                       std::uint64_t suffix_count) {
  if (suffix_count == std::numeric_limits<std::uint64_t>::max()) {
    throw Error(ErrorCode::kBuildFailure,
                "learned SA coordinate domain overflows");
  }
  coordinate_width = static_cast<std::uint8_t>(model.anchor_y.Width());
  detail::WriteU32(output, LearnedSaIndex::kModelId);
  detail::WriteU32(output, model.k);
  detail::WriteU32(output, model.bucket_bits);
  detail::WriteU32(output, model.memory_overhead_basis_points);
  detail::WriteU32(output, coordinate_width);
  detail::WriteU64(output, model.anchor_x.size());
  detail::WriteU64(output, fingerprint);
  WriteIntegerPayload(output, model.anchor_x, 64, "learned SA anchor keys");
  detail::WriteCoordinateSectionV14(output, model.anchor_y,
                                    suffix_count + 1U);
  if (!output) {
    throw Error(ErrorCode::kIoError, "failed to write learned SA index");
  }
}

LearnedSaIndex ReadLearnedIndex(const detail::ParsedContainer& container,
                                std::uint64_t suffix_count) {
  if (suffix_count == std::numeric_limits<std::uint64_t>::max()) {
    throw Error(ErrorCode::kCorruptIndex,
                "learned SA coordinate domain overflows");
  }
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
      (container.spec.format_minor < 4 &&
       coordinate_width != container.spec.coordinate_width) ||
      (container.spec.format_minor < 4 && coordinate_width != 32 &&
       coordinate_width != 64) ||
      (container.spec.format_minor >= 4 && coordinate_width != 32 &&
       coordinate_width != 40 && coordinate_width != 48 &&
       coordinate_width != 64) ||
      anchor_count != (1ULL << model.bucket_bits) + 1 ||
      fingerprint != container.spec.fingerprint ||
      anchor_count > Coordinate64().max_size()) {
    throw Error(ErrorCode::kCorruptIndex, "invalid learned SA header");
  }
  model.anchor_x.resize(static_cast<std::size_t>(anchor_count));
  ReadIntegerPayload(*input, model.anchor_x, 64,
                     "learned SA anchor keys");
  const auto anchor_domain = suffix_count + 1U;
  if (container.spec.format_minor >= 4) {
    model.anchor_y = detail::ReadCoordinateSectionV14(
        *input, anchor_count, anchor_domain, "learned SA anchors");
    if (static_cast<std::uint8_t>(model.anchor_y.Width()) !=
        coordinate_width) {
      throw Error(ErrorCode::kCorruptIndex,
                  "learned SA coordinate codec disagrees with its header");
    }
  } else {
    RawCoordinateStorage raw_anchor_y =
        coordinate_width == 32
            ? RawCoordinateStorage(
                  Coordinate32(static_cast<std::size_t>(anchor_count)))
            : RawCoordinateStorage(
                  Coordinate64(static_cast<std::size_t>(anchor_count)));
    std::visit(
        [&](auto& rows) {
          ReadIntegerPayload(*input, rows,
                             static_cast<std::uint8_t>(coordinate_width),
                             "learned SA anchor rows");
        },
        raw_anchor_y);
    model.anchor_y = std::visit(
        [&](auto& rows) -> CoordinateStorage {
          using Rows = std::decay_t<decltype(rows)>;
          if constexpr (std::is_same_v<Rows, Coordinate32>) {
            return CoordinateStorage::FromUInt32(
                std::move(rows), CoordinateStorageWidth::kBits32,
                anchor_domain, ErrorCode::kCorruptIndex,
                "learned SA anchors");
          } else {
            return CoordinateStorage::FromUInt64(
                std::move(rows), CoordinateStorageWidth::kBits64,
                anchor_domain, ErrorCode::kCorruptIndex,
                "learned SA anchors");
          }
        },
        raw_anchor_y);
  }
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

RawCoordinateStorage ReadLegacyIntegerVector(
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
  RawCoordinateStorage raw_storage =
      expected_width == 32
          ? RawCoordinateStorage(
                Coordinate32(static_cast<std::size_t>(count)))
          : RawCoordinateStorage(
                Coordinate64(static_cast<std::size_t>(count)));
  std::visit(
      [&](auto& values) {
        ReadIntegerPayload(*input, values, expected_width, label);
      },
      raw_storage);
  if (input->peek() != std::char_traits<char>::eof()) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(label) + " section has trailing bytes");
  }
  return raw_storage;
}

CoordinateStorage ReadLegacyCoordinateVector(
    const detail::ParsedContainer& container, detail::SectionType type,
    std::uint64_t expected_count, std::uint8_t expected_width,
    std::uint64_t symbol_count, const char* label) {
  auto raw_storage = ReadLegacyIntegerVector(
      container, type, expected_count, expected_width, label);
  return std::visit(
      [&](auto& values) -> CoordinateStorage {
        using Values = std::decay_t<decltype(values)>;
        if constexpr (std::is_same_v<Values, Coordinate32>) {
          return CoordinateStorage::FromUInt32(
              std::move(values), CoordinateStorageWidth::kBits32,
              symbol_count, ErrorCode::kCorruptIndex, label);
        } else {
          return CoordinateStorage::FromUInt64(
              std::move(values), CoordinateStorageWidth::kBits64,
              symbol_count, ErrorCode::kCorruptIndex, label);
        }
      },
      raw_storage);
}

}  // namespace

struct SuffixArray::Impl {
  detail::ReferenceData reference;
  std::vector<std::uint8_t> text;
  SaStorage suffix_array;
  CoordinateStorage isa;
  detail::LcpStorage lcp;
  LcpAccess lcp_access;
  CoordinateStorage child;
  LearnedSaIndex learned;
  detail::FastPrefixIndex fast_prefix;
  SaAcceleration acceleration = SaAcceleration::kNone;
  std::uint32_t sampling_rate = 1;
  detail::StoredBackend backend = detail::StoredBackend::kDivsufsort32;
  IndexInfo index_info;

  bool HasIsa() const noexcept { return !CoordinatesEmpty(isa); }
  bool HasLcp() const noexcept { return !lcp.Empty(); }
  bool HasChild() const noexcept { return !CoordinatesEmpty(child); }
  bool HasLearned() const noexcept { return !learned.Empty(); }
  bool HasFastPrefix() const noexcept { return !fast_prefix.Empty(); }

  void RefreshLcpAccess() noexcept { lcp_access = MakeLcpAccess(lcp); }

  void BuildFastPrefix(std::uint32_t threads = 1) {
    if (index_info.sa_resource_profile != SaResourceProfile::kFast ||
        sampling_rate != 1 || !HasIsa() || HasLearned()) {
      return;
    }
    detail::FastPrefixIndexOptions prefix_options;
    prefix_options.threads = threads;
    fast_prefix = detail::FastPrefixIndex::Build(
        text, suffix_array, isa, reference.contig_starts,
        reference.contig_lengths, index_info.resident_core_bytes, prefix_options);
    const auto bytes = fast_prefix.ResidentBytes();
    if (bytes > std::numeric_limits<std::uint64_t>::max() -
                    index_info.auxiliary_bytes ||
        bytes > std::numeric_limits<std::uint64_t>::max() -
                    index_info.resident_core_bytes) {
      throw Error(ErrorCode::kBuildFailure,
                  "Fast prefix-index memory accounting overflows");
    }
    index_info.auxiliary_bytes += bytes;
    index_info.resident_core_bytes += bytes;
  }

  SuffixRange BinaryRange(EncodedView pattern,
                          SaSearchStatistics* statistics = nullptr) const {
    return suffix_array.Visit(
        [&](const auto& values) {
          return RangeFor(text, values, pattern, statistics);
        });
  }

  SuffixRange LcpBinaryRange(EncodedView pattern,
                             SaSearchStatistics* statistics = nullptr) const {
    return suffix_array.Visit(
        [&](const auto& values) {
          return LcpRangeFor(text, values, pattern,
                             {0, static_cast<std::uint64_t>(values.size())},
                             statistics);
        });
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
    const auto prefix_range = suffix_array.Visit(
        [&](const auto& values) {
          const SuffixRange whole{0, static_cast<std::uint64_t>(values.size())};
          const auto lower = GallopingBoundaryFor(
              text, values, prefix, whole, prediction, false, statistics);
          const auto upper =
              GallopingBoundaryFor(text, values, prefix, {lower, whole.end},
                                   prediction, true, statistics);
          return SuffixRange{lower, upper};
        });
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
    return suffix_array.Visit(
        [&](const auto& values) {
          return LcpRangeFor(text, values, pattern, prefix_range, statistics,
                             learned.k);
        });
  }

  SuffixRange FastPrefixRange(
      EncodedView pattern,
      SaSearchStatistics* statistics = nullptr) const {
    const auto prefix_range = fast_prefix.Lookup(pattern.data, pattern.size());
    if (!prefix_range) {
      return LcpBinaryRange(pattern, statistics);
    }
    const SuffixRange range{prefix_range->begin, prefix_range->end};
    if (range.Empty() || pattern.size() == fast_prefix.K()) {
      return range;
    }
    return suffix_array.Visit([&](const auto& values) {
      return LcpRangeFor(text, values, pattern, range, statistics,
                         fast_prefix.K());
    });
  }

  std::uint8_t SymbolAt(std::uint64_t row, std::uint64_t depth) const {
    const auto suffix = SaValue(suffix_array, row);
    if (suffix >= text.size() ||
        depth >= static_cast<std::uint64_t>(text.size()) - suffix) {
      return detail::kSentinel;
    }
    return text[static_cast<std::size_t>(suffix + depth)];
  }

  template <class SaVector>
  std::uint8_t SymbolAtFor(const SaVector& sa_values, std::uint64_t row,
                           std::uint64_t depth) const {
    const auto suffix = static_cast<std::uint64_t>(
        sa_values[static_cast<std::size_t>(row)]);
    if (suffix >= text.size() ||
        depth >= static_cast<std::uint64_t>(text.size()) - suffix) {
      return detail::kSentinel;
    }
    return text[static_cast<std::size_t>(suffix + depth)];
  }

  template <class SaVector>
  SuffixRange NarrowCharFor(const SaVector& sa_values, SuffixRange range,
                            std::uint64_t depth,
                            std::uint8_t symbol) const {
    auto lower = range.begin;
    auto upper = range.end;
    while (lower < upper) {
      const auto middle = lower + (upper - lower) / 2;
      if (SymbolAtFor(sa_values, middle, depth) < symbol) {
        lower = middle + 1;
      } else {
        upper = middle;
      }
    }
    const auto begin = lower;
    upper = range.end;
    while (lower < upper) {
      const auto middle = lower + (upper - lower) / 2;
      if (SymbolAtFor(sa_values, middle, depth) <= symbol) {
        lower = middle + 1;
      } else {
        upper = middle;
      }
    }
    return begin == lower ? SuffixRange{} : SuffixRange{begin, lower};
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

  std::uint64_t LcpCommonDepth(SuffixRange range,
                               std::uint64_t known_depth) const {
    if (range.Size() <= 1) {
      return std::numeric_limits<std::uint64_t>::max();
    }

    // For a contiguous SA interval, the common prefix of every suffix is the
    // minimum LCP value on the internal row boundaries. Stop as soon as the
    // interval branches at the already matched depth; an exact minimum is not
    // needed in that case.
    return suffix_array.Visit([&](const auto& sa_values) {
      const TypedLcpView<std::decay_t<decltype(sa_values)>> lcp_values{
          lcp_access, &sa_values};
      std::uint64_t common_depth =
          std::numeric_limits<std::uint64_t>::max();
      for (auto row = range.begin + 1; row < range.end; ++row) {
        common_depth = std::min(
            common_depth,
            lcp_values[static_cast<std::size_t>(row)]);
        if (common_depth <= known_depth) {
          break;
        }
      }
      return common_depth;
    });
  }

  std::uint64_t ExtendSmemLcp(
      const std::vector<std::uint8_t>& query, std::size_t query_position,
      std::size_t run_end, std::uint64_t min_occurrences,
      SuffixRange& interval, std::uint64_t depth) const {
    const auto query_limit =
        static_cast<std::uint64_t>(run_end - query_position);
    while (depth < query_limit) {
      if (interval.Size() == 1) {
        // Once the interval is a singleton, no further SA navigation is
        // necessary. A bounded LCE consumes the remainder in one pass while
        // encoded N/separator/sentinel bytes remain hard mismatches.
        const auto suffix = SaValue(suffix_array, interval.begin);
        if (suffix >= text.size() ||
            depth > static_cast<std::uint64_t>(text.size()) - suffix) {
          return depth;
        }
        const auto text_offset = suffix + depth;
        const auto available = static_cast<std::size_t>(std::min(
            query_limit - depth,
            static_cast<std::uint64_t>(text.size()) - text_offset));
        const auto matched = detail::LongestCommonPrefixBytesLong(
            text.data() + static_cast<std::size_t>(text_offset),
            query.data() + query_position + static_cast<std::size_t>(depth),
            available);
        return depth + matched;
      }

      const auto common_depth = LcpCommonDepth(interval, depth);
      if (common_depth > depth) {
        // Every suffix in the interval shares this compact edge. Compare one
        // representative and jump over the edge instead of narrowing one
        // character at a time.
        const auto edge_end = std::min(common_depth, query_limit);
        const auto suffix = SaValue(suffix_array, interval.begin);
        if (suffix >= text.size() ||
            depth > static_cast<std::uint64_t>(text.size()) - suffix) {
          return depth;
        }
        const auto text_offset = suffix + depth;
        const auto available = static_cast<std::size_t>(std::min(
            edge_end - depth,
            static_cast<std::uint64_t>(text.size()) - text_offset));
        const auto matched = detail::LongestCommonPrefixBytesLong(
            text.data() + static_cast<std::size_t>(text_offset),
            query.data() + query_position + static_cast<std::size_t>(depth),
            available);
        depth += matched;
        if (depth != edge_end) {
          return depth;
        }
        if (depth == query_limit) {
          return depth;
        }
      }

      // The compact edge ends at `depth`; selecting the query symbol now
      // descends to the only child interval that can still qualify.
      const auto narrowed = NarrowChar(
          interval, depth,
          query[query_position + static_cast<std::size_t>(depth)]);
      if (narrowed.Size() < min_occurrences) {
        return depth;
      }
      interval = narrowed;
      ++depth;
    }
    return depth;
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
    const LcpView lcp_values{lcp_access, &suffix_array,
                            static_cast<std::size_t>(lcp.Size())};
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

  void ValidateExplicitSearchAlgorithm(
      SaSearchAlgorithm requested) const {
    switch (requested) {
      case SaSearchAlgorithm::kAutoSelect:
      case SaSearchAlgorithm::kBinary:
      case SaSearchAlgorithm::kLcpBinary:
        return;
      case SaSearchAlgorithm::kSaplingPwl:
        if (!HasLearned()) {
          throw Error(ErrorCode::kUnsupportedBackend,
                      "Sapling PWL data is unavailable in this index");
        }
        return;
      case SaSearchAlgorithm::kChild:
        if (!HasChild()) {
          throw Error(ErrorCode::kUnsupportedBackend,
                      "CHILD data is unavailable in this index");
        }
        return;
    }
    throw Error(ErrorCode::kInvalidInput, "invalid SA search algorithm");
  }

  SaSearchAlgorithm ResolveSearchAlgorithm(SaSearchAlgorithm requested,
                                           std::size_t pattern_length) const {
    if (requested == SaSearchAlgorithm::kAutoSelect) {
      return HasLearned() && pattern_length >= learned.k
                 ? SaSearchAlgorithm::kSaplingPwl
                 : SaSearchAlgorithm::kLcpBinary;
    }
    ValidateExplicitSearchAlgorithm(requested);
    return requested;
  }

  SaSearchAlgorithm ResolveMamLookupAlgorithm(
      SaSearchAlgorithm requested, std::size_t pattern_length) const {
    if (requested != SaSearchAlgorithm::kAutoSelect) {
      return ResolveSearchAlgorithm(requested, pattern_length);
    }
    // MAM advances one query position at a time and usually reaches the root
    // only after a failed suffix-link reuse. On this latency-sensitive path,
    // ordinary binary search is consistently faster than maintaining two LCP
    // boundary states. PWL remains the strongest automatic choice when its
    // model can encode the full prefix key; otherwise Fast's exact prefix
    // directory can remove the root search entirely.
    if (HasFastPrefix() && pattern_length >= fast_prefix.K()) {
      return SaSearchAlgorithm::kAutoSelect;
    }
    return HasLearned() && pattern_length >= learned.k
               ? SaSearchAlgorithm::kSaplingPwl
               : SaSearchAlgorithm::kBinary;
  }

  SuffixRange Range(
      EncodedView pattern,
      SaSearchAlgorithm requested = SaSearchAlgorithm::kAutoSelect,
      SaSearchStatistics* statistics = nullptr) const {
    // An explicitly built Sapling model retains priority for auto queries.
    // Otherwise Fast's exact k-mer directory removes the global SA search
    // without changing semantics; its interval is exact, not a prediction.
    if (requested == SaSearchAlgorithm::kAutoSelect &&
        !(HasLearned() && pattern.size() >= learned.k) && HasFastPrefix() &&
        pattern.size() >= fast_prefix.K()) {
      return FastPrefixRange(pattern, statistics);
    }
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

  bool IsReferencePrefixUnique(std::uint64_t row, std::uint64_t global,
                               std::uint64_t length) const {
    if (!HasLcp()) {
      const EncodedView pattern(
          text.data() + static_cast<std::size_t>(global),
          static_cast<std::size_t>(length));
      return BinaryRange(pattern).Size() == 1;
    }

    // All suffixes sharing a prefix form one contiguous SA interval. A prefix
    // at `row` is therefore unique exactly when neither adjacent suffix has
    // an LCP at least as long. This replaces a full equal-range lookup per MAM
    // candidate with at most two compressed-LCP probes.
    if (row > 0 && lcp_access.AtLeast(row, global, length)) {
      return false;
    }
    const auto rows = SaSize(suffix_array);
    if (row + 1 < rows) {
      if (lcp_access.AtLeastLazy(
              row + 1, [&] { return SaValue(suffix_array, row + 1); },
              length)) {
        return false;
      }
    }
    return true;
  }

  template <class Callback>
  void ForEachStoredRow(SuffixRange interval, Callback&& callback) const {
    // Native coordinates remain a single direct-pointer walk. Packed
    // coordinates are decoded in bounded blocks so locate and maximal-match
    // enumeration do not pay two plane loads throughout the surrounding hot
    // loop or allocate storage proportional to the match interval.
    const auto stored_width = suffix_array.Width();
    if (stored_width == CoordinateStorageWidth::kBits40 ||
        stored_width == CoordinateStorageWidth::kBits48) {
      constexpr std::uint64_t kDecodeRows = 256;
      std::array<std::uint64_t, kDecodeRows> decoded{};
      for (std::uint64_t begin = interval.begin; begin < interval.end;) {
        const auto count = std::min(kDecodeRows, interval.end - begin);
        suffix_array.DecodeSpan(begin, count, decoded.data());
        for (std::uint64_t offset = 0; offset < count; ++offset) {
          callback(begin + offset,
                   decoded[static_cast<std::size_t>(offset)]);
        }
        begin += count;
      }
      return;
    }
    suffix_array.Visit(
        [&](const auto& values) {
          using Values = std::decay_t<decltype(values)>;
          if constexpr (!kIsPackedCoordinateStorage<Values>) {
            const auto* current =
                values.data() + static_cast<std::size_t>(interval.begin);
            for (std::uint64_t row = interval.begin; row < interval.end;
                 ++row, ++current) {
              callback(row, static_cast<std::uint64_t>(*current));
            }
          }
        });
  }

  std::uint64_t SmallestStoredPosition(SuffixRange interval) const {
    if (interval.Empty()) {
      throw Error(ErrorCode::kCorruptIndex,
                  "cannot select a suffix from an empty interval");
    }
    const auto stored_width = suffix_array.Width();
    if (stored_width == CoordinateStorageWidth::kBits40 ||
        stored_width == CoordinateStorageWidth::kBits48) {
      constexpr std::uint64_t kDecodeRows = 256;
      std::array<std::uint64_t, kDecodeRows> decoded{};
      auto smallest = std::numeric_limits<std::uint64_t>::max();
      for (std::uint64_t begin = interval.begin; begin < interval.end;) {
        const auto count = std::min(kDecodeRows, interval.end - begin);
        suffix_array.DecodeSpan(begin, count, decoded.data());
        smallest = std::min(
            smallest,
            *std::min_element(decoded.begin(),
                              decoded.begin() +
                                  static_cast<std::ptrdiff_t>(count)));
        begin += count;
      }
      return smallest;
    }
    return suffix_array.Visit([&](const auto& values) -> std::uint64_t {
      using Values = std::decay_t<decltype(values)>;
      if constexpr (kIsPackedCoordinateStorage<Values>) {
        return 0;
      } else {
        const auto first =
            values.begin() + static_cast<std::ptrdiff_t>(interval.begin);
        const auto last =
            values.begin() + static_cast<std::ptrdiff_t>(interval.end);
        return static_cast<std::uint64_t>(*std::min_element(first, last));
      }
    });
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
      if (!options.max_hits) {
        ForEachStoredPosition(interval, [&](std::uint64_t global) {
          output.push_back({global, strand});
        });
      } else {
        const auto limit = *options.max_hits;
        ForEachStoredPosition(interval, [&](std::uint64_t global) {
          RetainBoundedGlobalMatch(output, {global, strand}, limit,
                                   heap_active);
        });
      }
      return interval.Size();
    }

    std::uint64_t total = 0;
    if (!options.max_hits) {
      ForEachExactGlobal(
          pattern, algorithm, statistics, [&](std::uint64_t global) {
            output.push_back({global, strand});
            ++total;
          });
    } else {
      const auto limit = *options.max_hits;
      ForEachExactGlobal(
          pattern, algorithm, statistics, [&](std::uint64_t global) {
            RetainBoundedGlobalMatch(output, {global, strand}, limit,
                                     heap_active);
            ++total;
          });
    }
    return total;
  }

  std::uint64_t CollectSmallest(
      EncodedView pattern, Strand strand,
      std::optional<GlobalMatch>& smallest, SaSearchAlgorithm algorithm,
      SaSearchStatistics* statistics) const {
    if (sampling_rate == 1) {
      const auto interval = Range(pattern, algorithm, statistics);
      if (!interval.Empty()) {
        RetainSmallestGlobalMatch(
            smallest, {SmallestStoredPosition(interval), strand});
      }
      return interval.Size();
    }

    std::uint64_t total = 0;
    ForEachExactGlobal(
        pattern, algorithm, statistics, [&](std::uint64_t global) {
          RetainSmallestGlobalMatch(smallest, {global, strand});
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

  RightMaximalSearchAlgorithm ResolveMemAlgorithm(
      MemSearchAlgorithm requested) const {
    const auto algorithm = AsRightMaximalAlgorithm(requested);
    if (algorithm != RightMaximalSearchAlgorithm::kAutoSelect) {
      return ResolveAlgorithm(algorithm);
    }
    // Sparse query anchors make LCP traversal consistently faster for MEMs.
    // Retained ISA remains valuable for MAM and explicit suffix-link queries.
    return HasLcp() ? RightMaximalSearchAlgorithm::kLcp
                    : RightMaximalSearchAlgorithm::kBaseline;
  }

  RightMaximalSearchAlgorithm ResolveMamAlgorithm(
      MemSearchAlgorithm requested) const {
    // MAM examines adjacent query positions for reference uniqueness, where
    // ISA+LCP interval reuse remains the strongest available automatic path.
    return ResolveAlgorithm(AsRightMaximalAlgorithm(requested));
  }

  RightMaximalSearchAlgorithm ResolveSmemAlgorithm(
      MemSearchAlgorithm requested) const {
    // SMEM advances one query position at a time. Fast indexes can reuse the
    // previous prefix interval through ISA+LCP; Low-memory indexes retain the
    // exact LCP/root path without requiring a persistent ISA.
    return ResolveAlgorithm(AsRightMaximalAlgorithm(requested));
  }

  RightMaximalSearchAlgorithm ResolveMumAlgorithm(
      MemSearchAlgorithm requested) const {
    // Strict MUM starts from the same reference-unique candidates as MAM, so
    // it uses the same workload-specific capability selection.
    return ResolveMamAlgorithm(requested);
  }

  template <class SaVector, class IsaVector, class LcpVector>
  SuffixRange SuffixLinkIntervalFor(
      const SaVector& sa_values, const IsaVector& isa_values,
      const LcpVector& lcp_values, SuffixRange previous, std::uint64_t depth,
      std::uint32_t shift, detail::SuffixLinkScanSink* scan_sink) const {
    static constexpr std::uint64_t kLcpProbeBudget = 4096;
    // Shift both interval endpoints through ISA, then expand across LCP values
    // that still support the shortened prefix. Empty means reuse was not
    // proven; callers must restart from the root search path.
    if (previous.Empty() || depth <= shift) {
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
      if (scan_sink) {
        scan_sink->Record(0, 0, 0, false);
      }
#endif
      return {};
    }
    const auto left_suffix = static_cast<std::uint64_t>(
        sa_values[static_cast<std::size_t>(previous.begin)]);
    const auto right_suffix = static_cast<std::uint64_t>(
        sa_values[static_cast<std::size_t>(previous.end - 1)]);
    const auto text_size = static_cast<std::uint64_t>(text.size());
    if (left_suffix >= text_size || right_suffix >= text_size ||
        static_cast<std::uint64_t>(shift) >= text_size - left_suffix ||
        static_cast<std::uint64_t>(shift) >= text_size - right_suffix) {
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
      if (scan_sink) {
        scan_sink->Record(0, 0, 0, false);
      }
#endif
      return {};
    }
    const auto left_sample = (left_suffix + shift) / sampling_rate;
    const auto right_sample = (right_suffix + shift) / sampling_rate;
    if (left_sample >= CoordinateCount(isa_values) ||
        right_sample >= CoordinateCount(isa_values)) {
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
      if (scan_sink) {
        scan_sink->Record(0, 0, 0, false);
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
    const auto sa_size =
        static_cast<std::uint64_t>(CoordinateCount(sa_values));
    const auto target = depth - shift;
    std::uint64_t left_rows = 0;
    std::uint64_t right_rows = 0;
    bool budget_exhausted = false;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    const auto scan_begin =
        scan_sink ? BuildClock::now() : BuildClock::time_point{};
#endif

    while (left > 0) {
      if (left_rows + right_rows >= kLcpProbeBudget) {
        budget_exhausted = true;
        break;
      }
      ++left_rows;
      if (!lcp_values.AtLeast(static_cast<std::size_t>(left), target)) {
        break;
      }
      --left;
    }
    while (!budget_exhausted && right < sa_size) {
      if (left_rows + right_rows >= kLcpProbeBudget) {
        budget_exhausted = true;
        break;
      }
      ++right_rows;
      if (!lcp_values.AtLeast(static_cast<std::size_t>(right), target)) {
        break;
      }
      ++right;
    }

#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    if (scan_sink) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               BuildClock::now() - scan_begin)
                               .count();
      scan_sink->Record(left_rows, right_rows,
                        elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0,
                        budget_exhausted);
    }
#else
    static_cast<void>(scan_sink);
#endif
    // A partially expanded range is not a proven suffix-link interval.
    // Returning empty makes every caller use its exact root-search fallback.
    if (budget_exhausted) {
      return {};
    }
    return {left, right};
  }

  SuffixRange SuffixLinkInterval(SuffixRange previous, std::uint64_t depth,
                                 std::uint32_t shift,
                                 detail::SuffixLinkScanSink* scan_sink) const {
    // Coordinate width is selected once before the potentially long LCP scan,
    // so the hot loop contains no per-row 32/64-bit branch.
    return suffix_array.Visit([&](const auto& sa_values) {
      return isa.Visit(
          [&](const auto& isa_values) {
            const TypedLcpView<std::decay_t<decltype(sa_values)>> lcp_values{
                lcp_access, &sa_values};
            return SuffixLinkIntervalFor(sa_values, isa_values, lcp_values,
                                         previous, depth, shift, scan_sink);
          });
    });
  }

  template <class LinkInterval, class Callback>
  void EnumerateOneStrandWithLinker(
      const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      std::uint64_t min_length, RightMaximalSearchAlgorithm algorithm,
      SaSearchAlgorithm lookup_algorithm,
      RightMaximalSearchStatistics* statistics, LinkInterval& link_interval,
      Callback& callback) const {
    const LcpView lcp_values{lcp_access, &suffix_array,
                            static_cast<std::size_t>(lcp.Size())};
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
           min_length <=
           static_cast<std::uint64_t>(run_end - query_position);
           ++query_position) {
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
          interval = link_interval(previous, min_length, 1);
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
              if (statistics) {
                const auto selected =
                    ResolveSearchAlgorithm(lookup_algorithm, prefix.size());
                ++statistics->lookup_calls;
                if (selected == SaSearchAlgorithm::kSaplingPwl) {
                  ++statistics->learned_lookup_calls;
                } else {
                  ++statistics->binary_lookup_calls;
                }
              }
              interval = Range(prefix, lookup_algorithm,
                               statistics ? &statistics->lookup : nullptr);
            }
          }
        } else if (algorithm == RightMaximalSearchAlgorithm::kChild ||
                   algorithm == RightMaximalSearchAlgorithm::kFull) {
          interval = ChildRange(prefix);
        } else {
          if (statistics) {
            const auto selected =
                ResolveSearchAlgorithm(lookup_algorithm, prefix.size());
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
          interval = Range(prefix, lookup_algorithm,
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
          callback(row, global, interval.Size() == 1,
                   RightMaximalMatch{mapped->first, mapped->second,
                                     output_position, length, strand});
        });
      }
      run_begin = run_end;
    }
  }

  template <class Coordinate, class Callback>
  void EnumerateOneStrandNativeSuffixLink(
      const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      std::uint64_t min_length, RightMaximalSearchAlgorithm algorithm,
      SaSearchAlgorithm lookup_algorithm,
      RightMaximalSearchStatistics* statistics, RawLcpView lcp_values,
      detail::SuffixLinkScanSink* scan_sink, Callback& callback) const {
    using NativeCoordinates = std::vector<Coordinate>;
    suffix_array.Visit([&](const auto& sa_values) {
      using SaValues = std::decay_t<decltype(sa_values)>;
      if constexpr (std::is_same_v<SaValues, NativeCoordinates>) {
        isa.Visit([&](const auto& isa_values) {
          using IsaValues = std::decay_t<decltype(isa_values)>;
          if constexpr (std::is_same_v<IsaValues, NativeCoordinates>) {
            auto link_interval = [&](SuffixRange previous,
                                     std::uint64_t depth,
                                     std::uint32_t shift) {
              return SuffixLinkIntervalFor(sa_values, isa_values, lcp_values,
                                           previous, depth, shift, scan_sink);
            };
            EnumerateOneStrandWithLinker(
                query, original_query_length, strand, min_length, algorithm,
                lookup_algorithm, statistics, link_interval, callback);
          }
        });
      }
    });
  }

  template <class Callback>
  void EnumerateOneStrand(const std::vector<std::uint8_t>& query,
                          std::uint64_t original_query_length, Strand strand,
                          std::uint64_t min_length,
                          RightMaximalSearchAlgorithm algorithm,
                          SaSearchAlgorithm lookup_algorithm,
                          RightMaximalSearchStatistics* statistics,
                          Callback& callback) const {
    detail::SuffixLinkScanSink* scan_sink = nullptr;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    scan_sink = detail::CurrentSuffixLinkScanSink();
#endif
    const bool uses_suffix_links =
        algorithm == RightMaximalSearchAlgorithm::kSuffixLink ||
        algorithm == RightMaximalSearchAlgorithm::kFull;
    const RawLcpView raw_lcp{lcp_access.raw32, lcp_access.raw64};
    if (uses_suffix_links &&
        (raw_lcp.raw32 != nullptr || raw_lcp.raw64 != nullptr) &&
        suffix_array.Width() == CoordinateStorageWidth::kBits32 &&
        isa.Width() == CoordinateStorageWidth::kBits32) {
      EnumerateOneStrandNativeSuffixLink<std::uint32_t>(
          query, original_query_length, strand, min_length, algorithm,
          lookup_algorithm, statistics, raw_lcp, scan_sink, callback);
      return;
    }
    if (uses_suffix_links &&
        (raw_lcp.raw32 != nullptr || raw_lcp.raw64 != nullptr) &&
        suffix_array.Width() == CoordinateStorageWidth::kBits64 &&
        isa.Width() == CoordinateStorageWidth::kBits64) {
      EnumerateOneStrandNativeSuffixLink<std::uint64_t>(
          query, original_query_length, strand, min_length, algorithm,
          lookup_algorithm, statistics, raw_lcp, scan_sink, callback);
      return;
    }

    auto link_interval = [&](SuffixRange previous, std::uint64_t depth,
                             std::uint32_t shift) {
      return SuffixLinkInterval(previous, depth, shift, scan_sink);
    };
    EnumerateOneStrandWithLinker(
        query, original_query_length, strand, min_length, algorithm,
        lookup_algorithm, statistics, link_interval, callback);
  }

  template <class SaVector, class LinkInterval, class Callback>
  void EnumerateMamOneStrandFullDepthFor(
      const SaVector& sa_values, const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      std::uint64_t min_length, RightMaximalSearchAlgorithm algorithm,
      SaSearchAlgorithm lookup_algorithm, LinkInterval& link_interval,
      Callback& callback) const {
    std::size_t run_begin = 0;
    while (run_begin < query.size()) {
      while (run_begin < query.size() &&
             query[run_begin] == detail::kSentinel) {
        ++run_begin;
      }
      std::size_t run_end = run_begin;
      while (run_end < query.size() &&
             query[run_end] != detail::kSentinel) {
        ++run_end;
      }

      // `interval` is the complete SA interval for query
      // [query_position, query_position + depth). Unlike the generic
      // right-maximal kernel, this state retains the full match depth. After
      // deleting one query character, ISA+LCP therefore resumes from almost
      // the mismatch point instead of rebuilding a fixed min-length prefix.
      SuffixRange interval{};
      std::uint64_t depth = 0;
      std::size_t query_position = run_begin;
      while (query_position < run_end &&
             min_length <=
                 static_cast<std::uint64_t>(run_end - query_position)) {
        const auto remaining =
            static_cast<std::uint64_t>(run_end - query_position);
        if (interval.Empty()) {
          const EncodedView prefix(query.data() + query_position,
                                   static_cast<std::size_t>(min_length));
          interval = algorithm == RightMaximalSearchAlgorithm::kFull
                         ? ChildRange(prefix)
                         : Range(prefix, lookup_algorithm, nullptr);
          depth = interval.Empty() ? 0 : min_length;
        }

        if (!interval.Empty()) {
          // A shifted state can be one character shorter than min_length.
          // More generally, retain the last non-empty interval on a mismatch
          // so it can still seed the next suffix-link transition.
          while (depth < remaining && interval.Size() != 1) {
            const auto narrowed = NarrowCharFor(
                sa_values, interval, depth,
                query[query_position + static_cast<std::size_t>(depth)]);
            if (narrowed.Empty()) {
              break;
            }
            interval = narrowed;
            ++depth;
          }

          if (interval.Size() == 1 && depth < remaining) {
            const auto global = static_cast<std::uint64_t>(
                sa_values[static_cast<std::size_t>(interval.begin)]);
            if (global < text.size() &&
                depth <= static_cast<std::uint64_t>(text.size()) - global) {
              const auto text_offset = global + depth;
              const auto available = static_cast<std::size_t>(std::min(
                  remaining - depth,
                  static_cast<std::uint64_t>(text.size()) - text_offset));
              // Query runs contain only A/C/G/T, so N, separator and sentinel
              // bytes terminate this LCE without a metadata lookup.
              depth += detail::LongestCommonPrefixBytesLong(
                  query.data() + query_position +
                      static_cast<std::size_t>(depth),
                  text.data() + static_cast<std::size_t>(text_offset),
                  available);
            }
          }

          if (interval.Size() == 1 && depth >= min_length) {
            const auto row = interval.begin;
            const auto global = static_cast<std::uint64_t>(
                sa_values[static_cast<std::size_t>(row)]);
            const auto mapped =
                detail::MapGlobalPosition(reference, global, depth);
            if (mapped) {
              const bool left_extendable =
                  query_position > run_begin && mapped->second > 0 &&
                  query[query_position - 1] ==
                      text[static_cast<std::size_t>(global - 1)];
              if (!left_extendable) {
                const auto output_position =
                    strand == Strand::kReverseComplement
                        ? original_query_length - (query_position + depth)
                        : static_cast<std::uint64_t>(query_position);
                callback(MamMatch{mapped->first, mapped->second,
                                  output_position, depth, strand});
              }
            }
          }
        }

        if (!interval.Empty() && depth > 1) {
          bool predecessor_was_unique = interval.Size() == 1;
          do {
            interval = link_interval(interval, depth, 1);
            ++query_position;
            if (interval.Empty()) {
              depth = 0;
              break;
            }
            --depth;
            if (!predecessor_was_unique || interval.Size() != 1) {
              break;
            }

            // Removing the first base from a unique exact match preserves
            // the same right endpoint. While the shifted interval is still a
            // singleton, that query start is necessarily left-extendable by
            // the base just removed and cannot be a MAM. Skip the start, but
            // stop immediately when the interval expands: another reference
            // suffix can then extend beyond the old mismatch and may yield a
            // valid internal MAM.
            predecessor_was_unique = true;
          } while (query_position < run_end && depth > 1 &&
                   min_length <= static_cast<std::uint64_t>(
                                     run_end - query_position));

          if (!interval.Empty() && interval.Size() == 1 && depth <= 1 &&
              query_position < run_end &&
              min_length <= static_cast<std::uint64_t>(
                                run_end - query_position)) {
            // The current singleton start was already proven left-extendable
            // by the preceding shift, but depth is now too small to link
            // again. Advance once and restart from the root.
            ++query_position;
            interval = {};
            depth = 0;
          }
        } else {
          interval = {};
          depth = 0;
          ++query_position;
        }
      }
      run_begin = run_end;
    }
  }

  template <class Coordinate, class Callback>
  bool EnumerateMamOneStrandNativeFullDepth(
      const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      std::uint64_t min_length, RightMaximalSearchAlgorithm algorithm,
      SaSearchAlgorithm lookup_algorithm, RawLcpView lcp_values,
      detail::SuffixLinkScanSink* scan_sink, Callback& callback) const {
    using NativeCoordinates = std::vector<Coordinate>;
    bool handled = false;
    suffix_array.Visit([&](const auto& sa_values) {
      using SaValues = std::decay_t<decltype(sa_values)>;
      if constexpr (std::is_same_v<SaValues, NativeCoordinates>) {
        isa.Visit([&](const auto& isa_values) {
          using IsaValues = std::decay_t<decltype(isa_values)>;
          if constexpr (std::is_same_v<IsaValues, NativeCoordinates>) {
            auto link_interval = [&](SuffixRange previous,
                                     std::uint64_t depth,
                                     std::uint32_t shift) {
              return SuffixLinkIntervalFor(sa_values, isa_values, lcp_values,
                                           previous, depth, shift, scan_sink);
            };
            EnumerateMamOneStrandFullDepthFor(
                sa_values, query, original_query_length, strand, min_length,
                algorithm, lookup_algorithm, link_interval, callback);
            handled = true;
          }
        });
      }
    });
    return handled;
  }

  template <class Callback>
  void EnumerateMamOneStrandFullDepth(
      const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      std::uint64_t min_length, RightMaximalSearchAlgorithm algorithm,
      SaSearchAlgorithm lookup_algorithm, Callback& callback) const {
    detail::SuffixLinkScanSink* scan_sink = nullptr;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    scan_sink = detail::CurrentSuffixLinkScanSink();
#endif
    const RawLcpView raw_lcp{lcp_access.raw32, lcp_access.raw64};
    if ((raw_lcp.raw32 != nullptr || raw_lcp.raw64 != nullptr) &&
        EnumerateMamOneStrandNativeFullDepth<std::uint32_t>(
            query, original_query_length, strand, min_length, algorithm,
            lookup_algorithm, raw_lcp, scan_sink, callback)) {
      return;
    }
    if ((raw_lcp.raw32 != nullptr || raw_lcp.raw64 != nullptr) &&
        EnumerateMamOneStrandNativeFullDepth<std::uint64_t>(
            query, original_query_length, strand, min_length, algorithm,
            lookup_algorithm, raw_lcp, scan_sink, callback)) {
      return;
    }

    const auto sa_values = ViewCoordinates(suffix_array);
    const auto isa_values = ViewCoordinates(isa);
    const TypedLcpView<CoordinateView> lcp_values{lcp_access, &sa_values};
    auto link_interval = [&](SuffixRange previous, std::uint64_t depth,
                             std::uint32_t shift) {
      return SuffixLinkIntervalFor(sa_values, isa_values, lcp_values,
                                   previous, depth, shift, scan_sink);
    };
    EnumerateMamOneStrandFullDepthFor(
        sa_values, query, original_query_length, strand, min_length,
        algorithm, lookup_algorithm, link_interval, callback);
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
        auto emit = [&](std::uint64_t, std::uint64_t, bool,
                        const RightMaximalMatch& match) { callback(match); };
        EnumerateOneStrand(value, encoded.size(), strand, options.min_length,
                           algorithm, options.lookup_algorithm,
                           options.statistics, emit);
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
    const LcpView lcp_values{lcp_access, &suffix_array,
                            static_cast<std::size_t>(lcp.Size())};
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
              // Preserve kAutoSelect so Fast's exact prefix directory can
              // seed the root lookup. Explicit lookup algorithms retain their
              // existing behavior for ablation and reproducibility.
              interval = Range(prefix, options.lookup_algorithm, nullptr);
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
                  right += detail::LongestCommonPrefixBytesLong(
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
    const auto enumerate = [&](const std::vector<std::uint8_t>& value,
                               Strand strand) {
      const auto lookup = ResolveMamLookupAlgorithm(
          options.lookup_algorithm,
          static_cast<std::size_t>(options.min_length));
      if (algorithm == RightMaximalSearchAlgorithm::kSuffixLink ||
          algorithm == RightMaximalSearchAlgorithm::kFull) {
        EnumerateMamOneStrandFullDepth(
            value, encoded.size(), strand, options.min_length, algorithm,
            lookup, callback);
        return;
      }

      auto filter = [&](std::uint64_t row, std::uint64_t global,
                        bool minimum_prefix_is_unique,
                        const RightMaximalMatch& match) {
        if (!minimum_prefix_is_unique &&
            !IsReferencePrefixUnique(row, global, match.length)) {
          return;
        }
        callback(MamMatch{match.sequence_id, match.reference_position,
                          match.query_position, match.length, match.strand});
      };
      EnumerateOneStrand(value, encoded.size(), strand, options.min_length,
                         algorithm, lookup, nullptr, filter);
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
  std::uint64_t EnumerateSmemOneStrand(
      const std::vector<std::uint8_t>& query,
      std::uint64_t original_query_length, Strand strand,
      const SmemOptions& options, RightMaximalSearchAlgorithm algorithm,
      Callback& callback) const {
    detail::SuffixLinkScanSink* scan_sink = nullptr;
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    scan_sink = detail::CurrentSuffixLinkScanSink();
#endif
    std::uint64_t total_smems = 0;
    std::size_t run_begin = 0;
    while (run_begin < query.size()) {
      while (run_begin < query.size() &&
             query[run_begin] == detail::kSentinel) {
        ++run_begin;
      }
      std::size_t run_end = run_begin;
      while (run_end < query.size() &&
             query[run_end] != detail::kSentinel) {
        ++run_end;
      }

      SuffixRange previous_prefix{};
      std::uint64_t maximum_end = run_begin;
      for (std::size_t query_position = run_begin;
           options.min_length <=
           static_cast<std::uint64_t>(run_end - query_position);
           ++query_position) {
        const EncodedView prefix(
            query.data() + query_position,
            static_cast<std::size_t>(options.min_length));
        SuffixRange interval;
        const bool uses_links =
            algorithm == RightMaximalSearchAlgorithm::kSuffixLink ||
            algorithm == RightMaximalSearchAlgorithm::kFull;
        if (uses_links && query_position != run_begin &&
            !previous_prefix.Empty()) {
          interval = SuffixLinkInterval(previous_prefix, options.min_length, 1,
                                        scan_sink);
          if (!interval.Empty()) {
            interval = NarrowChar(interval, options.min_length - 1,
                                  prefix.back());
          }
        }
        if (interval.Empty()) {
          if (algorithm == RightMaximalSearchAlgorithm::kChild ||
              algorithm == RightMaximalSearchAlgorithm::kFull) {
            interval = ChildRange(prefix);
          } else {
            interval = Range(prefix, options.lookup_algorithm, nullptr);
          }
        }

        // Keep the exact minimum-prefix interval for suffix-linking the next
        // query start. Its occurrence threshold does not affect correctness of
        // the shifted interval.
        previous_prefix = interval;
        if (interval.Size() < options.min_occurrences) {
          continue;
        }

        std::uint64_t length = options.min_length;
        if (algorithm == RightMaximalSearchAlgorithm::kLcp) {
          length = ExtendSmemLcp(query, query_position, run_end,
                                 options.min_occurrences, interval, length);
        } else {
          while (query_position + length < run_end) {
            const auto narrowed = NarrowChar(
                interval, length,
                query[query_position + static_cast<std::size_t>(length)]);
            if (narrowed.Size() < options.min_occurrences) {
              break;
            }
            interval = narrowed;
            ++length;
          }
        }

        const auto query_end =
            static_cast<std::uint64_t>(query_position) + length;
        // For each query start only the longest c-supported match matters.
        // Starts are visited in ascending order, so an earlier candidate
        // contains this one exactly when its right end reaches at least as far.
        if (query_end <= maximum_end) {
          continue;
        }
        maximum_end = query_end;
        ++total_smems;

        const auto output_position =
            strand == Strand::kReverseComplement
                ? original_query_length - query_end
                : static_cast<std::uint64_t>(query_position);
        const auto occurrences = interval.Size();
        ForEachStoredPosition(interval, [&](std::uint64_t global) {
          const auto mapped =
              detail::MapGlobalPosition(reference, global, length);
          if (!mapped) {
            return;
          }
          callback(SmemMatch{mapped->first, mapped->second, output_position,
                             length, occurrences, strand});
        });
      }
      run_begin = run_end;
    }
    return total_smems;
  }

  template <class Callback>
  std::uint64_t EnumerateEncodedSmem(
      const std::vector<std::uint8_t>& encoded, const SmemOptions& options,
      RightMaximalSearchAlgorithm algorithm, Callback& callback) const {
    if (sampling_rate != 1) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  "SMEM search requires a complete suffix array");
    }
    if (options.min_occurrences > SaSize(suffix_array)) {
      return 0;
    }
    std::uint64_t total_smems = 0;
    if (options.strands == StrandMode::kForward ||
        options.strands == StrandMode::kBoth) {
      total_smems += EnumerateSmemOneStrand(
          encoded, encoded.size(), Strand::kForward, options, algorithm,
          callback);
    }
    if (options.strands == StrandMode::kReverseComplement ||
        options.strands == StrandMode::kBoth) {
      const auto reverse = ReverseComplementRightMaximal(encoded);
      total_smems += EnumerateSmemOneStrand(
          reverse, encoded.size(), Strand::kReverseComplement, options,
          algorithm, callback);
    }
    return total_smems;
  }

  template <class Callback>
  void EnumerateEncodedMum(const std::vector<std::uint8_t>& encoded,
                           const MumOptions& options,
                           RightMaximalSearchAlgorithm algorithm,
                           Callback& callback) const {
    if (sampling_rate != 1) {
      throw Error(ErrorCode::kUnsupportedBackend,
                  "MUM search requires a complete suffix array");
    }

    MamOptions mam_options;
    mam_options.min_length = options.min_length;
    mam_options.strands = options.strands;
    mam_options.algorithm = options.algorithm;
    mam_options.lookup_algorithm = options.lookup_algorithm;
    std::vector<MamMatch> candidates;
    auto collect = [&](const MamMatch& match) { candidates.push_back(match); };
    EnumerateEncodedMam(encoded, mam_options, algorithm, collect);

    // Remove accidental duplicate tuples before interpreting equal reference
    // intervals as distinct query occurrences.
    std::sort(candidates.begin(), candidates.end(), MamMatchLess);
    candidates.erase(
        std::unique(candidates.begin(), candidates.end(), MamMatchEqual),
        candidates.end());
    const auto interval_less = [](const MamMatch& left,
                                  const MamMatch& right) {
      if (left.strand != right.strand) {
        return left.strand < right.strand;
      }
      if (left.sequence_id != right.sequence_id) {
        return left.sequence_id < right.sequence_id;
      }
      if (left.reference_position != right.reference_position) {
        return left.reference_position < right.reference_position;
      }
      if (left.length != right.length) {
        return left.length > right.length;
      }
      return left.query_position < right.query_position;
    };
    std::sort(candidates.begin(), candidates.end(), interval_less);

    std::size_t begin = 0;
    while (begin < candidates.size()) {
      const auto strand = candidates[begin].strand;
      const auto sequence_id = candidates[begin].sequence_id;
      std::size_t group_end = begin;
      while (group_end < candidates.size() &&
             candidates[group_end].strand == strand &&
             candidates[group_end].sequence_id == sequence_id) {
        ++group_end;
      }

      std::uint64_t furthest_end = 0;
      for (std::size_t index = begin; index < group_end;) {
        const auto& first = candidates[index];
        if (first.length >
            std::numeric_limits<std::uint64_t>::max() -
                first.reference_position) {
          throw Error(ErrorCode::kCorruptIndex,
                      "MUM reference interval overflows");
        }
        const auto reference_end = first.reference_position + first.length;
        std::size_t equal_end = index + 1;
        while (equal_end < group_end &&
               candidates[equal_end].reference_position ==
                   first.reference_position &&
               candidates[equal_end].length == first.length) {
          ++equal_end;
        }

        const bool duplicated = equal_end - index > 1;
        const bool contained = reference_end <= furthest_end;
        // Equal intervals are all rejected. Even a rejected duplicate still
        // participates in containment: any shorter interval inside it also
        // occurs at least twice in the query and cannot be a strict MUM.
        if (!duplicated && !contained) {
          callback(MumMatch{first.sequence_id, first.reference_position,
                            first.query_position, first.length,
                            first.strand});
        }
        furthest_end = std::max(furthest_end, reference_end);
        index = equal_end;
      }
      begin = group_end;
    }
  }
};

SuffixArray::SuffixArray(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SuffixArray::SuffixArray(SuffixArray&&) noexcept = default;
SuffixArray& SuffixArray::operator=(SuffixArray&&) noexcept = default;
SuffixArray::~SuffixArray() = default;

SuffixArray SuffixArray::Build(const GenomeReference& reference,
                               const SuffixArrayBuildOptions& options) {
  const auto total_begin = BuildClock::now();
  const auto notify = [&](const char* stage) {
    if (options.stage_callback) options.stage_callback(stage, options.stage_context);
  };
  notify("index-text-prepare");
  if (options.threads == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "suffix-array thread count must be greater than zero");
  }
  if (options.sampling_rate == 0) {
    throw Error(ErrorCode::kInvalidInput,
                "SA sampling rate must be greater than zero");
  }
  ValidateSaAcceleration(options.acceleration);
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
  if (!detail::SaConstructionCanRepresent(backend, width,
                                          impl->text.size())) {
    throw Error(ErrorCode::kInvalidInput,
                "reference exceeds the requested SA construction width");
  }
  const auto stored_width = ResolveProfileStorageWidth(
      options.storage_width, options.resource_profile, impl->text.size());
  SaAcceleration effective_acceleration = options.acceleration;
  bool learned_enabled = options.learned_index.enabled;
  if (options.resource_profile == SaResourceProfile::kLowMemory) {
    if (options.sampling_rate != 1) {
      throw Error(ErrorCode::kInvalidInput,
                  "low-memory profile requires a complete suffix array");
    }
    // The profile is authoritative: temporary ISA may build compressed LCP,
    // but no ISA, CHILD, or learned model remains resident afterward.
    effective_acceleration = SaAcceleration::kLcp;
    learned_enabled = false;
  }
  const auto lcp_width = static_cast<std::uint8_t>(
      impl->text.size() - 1U <=
              std::numeric_limits<std::uint32_t>::max()
          ? 32
          : 64);
  const auto sa_begin = BuildClock::now();
  if (options.statistics) {
    options.statistics->text_prepare_seconds = BuildElapsed(total_begin);
  }
  notify("index-sa-lcp");
  const bool retain_lcp = effective_acceleration != SaAcceleration::kNone;
  const bool retain_isa =
      effective_acceleration == SaAcceleration::kLcpSuffixLink ||
      effective_acceleration == SaAcceleration::kFull;
  const bool needs_child =
      effective_acceleration == SaAcceleration::kLcpChild ||
      effective_acceleration == SaAcceleration::kFull;
  // Byte-coded LCP reduces the Low-memory resident set substantially, but the
  // pinned quick A/B did not satisfy Fast's <=3% per-workload regression
  // gate. Fast therefore keeps native raw LCP; the private override also lets
  // maintainers compare a raw Low-memory build from the same source revision.
  const bool persist_raw_lcp =
      retain_lcp &&
      (options.resource_profile == SaResourceProfile::kFast ||
       kForceRawLcpForDeveloperBenchmark);
  const bool needs_raw_lcp =
      retain_lcp && (needs_child || persist_raw_lcp);
  RawCoordinateStorage raw_lcp;
  RawSaStorage built_sa = Sa32{};
  bool backend_reports_phases = false;
  if (backend == SaBackend::kCaps && width == CoordinateWidth::kBits32) {
    if (impl->text.size() >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference is too large for CaPS-SA uint32_t");
    }
    auto built =
        detail::BuildCaps32(impl->text, options.threads, needs_raw_lcp, &options);
    built_sa = std::move(built.suffix_array);
    raw_lcp = CompactRawCoordinates(std::move(built.lcp), lcp_width,
                                    ErrorCode::kBuildFailure, "LCP");
    impl->backend = detail::StoredBackend::kCaps32;
  } else if (backend == SaBackend::kCaps && width == CoordinateWidth::kBits64) {
    auto built =
        detail::BuildCaps64(impl->text, options.threads, needs_raw_lcp, &options);
    built_sa = std::move(built.suffix_array);
    raw_lcp = CompactRawCoordinates(std::move(built.lcp), lcp_width,
                                    ErrorCode::kBuildFailure, "LCP");
    impl->backend = detail::StoredBackend::kCaps64;
  } else if (backend == SaBackend::kDivsufsort &&
             width == CoordinateWidth::kBits32) {
    // Rebuild ISA directly in its resolved resident width after SA repacking.
    // The adapter still creates a private ISA when raw LCP is requested, but
    // does not carry that 8-byte coordinate plane into compact ISA
    // construction.
    auto built = detail::BuildDivsufsort32(
        impl->text, options.sampling_rate, needs_raw_lcp, false,
        options.threads);
    if (options.statistics) {
      options.statistics->sa_seconds = built.sa_seconds;
      options.statistics->lcp_seconds = built.lcp_seconds;
    }
    backend_reports_phases = true;
    built_sa = std::move(built.suffix_array);
    raw_lcp = CompactRawCoordinates(std::move(built.lcp), lcp_width,
                                    ErrorCode::kBuildFailure, "LCP");
    impl->backend = detail::StoredBackend::kDivsufsort32;
  } else if (backend == SaBackend::kDivsufsort &&
             width == CoordinateWidth::kBits64) {
    auto built = detail::BuildDivsufsort64(
        impl->text, options.sampling_rate, needs_raw_lcp, false,
        options.threads);
    if (options.statistics) {
      options.statistics->sa_seconds = built.sa_seconds;
      options.statistics->lcp_seconds = built.lcp_seconds;
    }
    backend_reports_phases = true;
    built_sa = std::move(built.suffix_array);
    raw_lcp = CompactRawCoordinates(std::move(built.lcp), lcp_width,
                                    ErrorCode::kBuildFailure, "LCP");
    impl->backend = detail::StoredBackend::kDivsufsort64;
  } else {
    throw Error(ErrorCode::kInvalidInput,
                "invalid suffix-array coordinate width");
  }
  impl->sampling_rate = options.sampling_rate;
  if (backend == SaBackend::kCaps && impl->sampling_rate > 1) {
    // CaPS supplies complete-SA LCP. Sampling must reduce intervening LCP
    // values by interval minimum so adjacent retained suffixes remain valid.
    if (!std::visit([](const auto& values) { return values.empty(); },
                    raw_lcp)) {
      SampleSaLcp(built_sa, raw_lcp, impl->sampling_rate);
    } else {
      SampleSa(built_sa, impl->sampling_rate);
    }
  }
  if (options.statistics && !backend_reports_phases) {
    options.statistics->sa_seconds = BuildElapsed(sa_begin);
  }

  const auto storage_compaction_begin = BuildClock::now();
  notify("index-storage-layout");
  impl->suffix_array = RepackSuffixArray(
      std::move(built_sa), stored_width, impl->text.size(),
      impl->sampling_rate);
  if (options.statistics) {
    options.statistics->storage_compaction_seconds =
        BuildElapsed(storage_compaction_begin);
  }
  impl->acceleration = effective_acceleration;
  const auto isa_domain = SaSize(impl->suffix_array);
  const bool native_isa_fits_32 =
      isa_domain - 1U <= std::numeric_limits<std::uint32_t>::max();
  const auto temporary_isa_width =
      retain_isa
          ? ResolveAuxiliaryStorageWidth(stored_width, isa_domain)
          : (native_isa_fits_32 ? CoordinateStorageWidth::kBits32
                                : CoordinateStorageWidth::kBits64);
  auto child_storage_width = stored_width;
  if (needs_child) {
    if (isa_domain == std::numeric_limits<std::uint64_t>::max()) {
      throw Error(ErrorCode::kInvalidInput,
                  "CHILD coordinate domain overflows");
    }
    child_storage_width =
        ResolveAuxiliaryStorageWidth(stored_width, isa_domain + 1U);
  }
  CoordinateStorage temporary_isa;
  if ((retain_lcp || retain_isa || learned_enabled) &&
      CoordinatesEmpty(temporary_isa)) {
    const auto begin = BuildClock::now();
    notify("index-isa");
    temporary_isa = BuildIsa(impl->suffix_array, impl->text.size(),
                             impl->sampling_rate, options.threads,
                             temporary_isa_width);
    if (options.statistics) {
      options.statistics->isa_seconds = BuildElapsed(begin);
    }
  }
  if (retain_lcp) {
    const bool has_raw_lcp = !std::visit(
        [](const auto& values) { return values.empty(); }, raw_lcp);
    if (!has_raw_lcp && !needs_child) {
      const auto lcp_begin = BuildClock::now();
      impl->lcp = BuildByteCodedLcpDirect(
          impl->text, impl->suffix_array, temporary_isa,
          impl->sampling_rate, lcp_width);
      if (options.statistics) {
        options.statistics->lcp_seconds = BuildElapsed(lcp_begin);
      }
    } else {
      if (!has_raw_lcp) {
        const auto lcp_begin = BuildClock::now();
        raw_lcp = BuildLcp(impl->text, impl->suffix_array, temporary_isa,
                           impl->sampling_rate, lcp_width);
        if (options.statistics) {
          options.statistics->lcp_seconds = BuildElapsed(lcp_begin);
        }
      }
      if (needs_child) {
        const auto child_begin = BuildClock::now();
        impl->child = BuildChild(raw_lcp, child_storage_width);
        if (options.statistics) {
          options.statistics->child_seconds = BuildElapsed(child_begin);
        }
      }
      notify("index-lcp-layout");
      const auto finalize_begin = BuildClock::now();
      impl->lcp = FinalizeLcpStorage(
          std::move(raw_lcp), temporary_isa, impl->sampling_rate, lcp_width,
          impl->text.size(), !persist_raw_lcp, persist_raw_lcp);
      if (options.statistics) {
        options.statistics->lcp_finalize_seconds = BuildElapsed(finalize_begin);
      }
    }
    if (retain_isa) {
      // LCP compression is the final temporary consumer. Move ISA into the
      // immutable index instead of retaining a second full-size copy.
      impl->isa = std::move(temporary_isa);
    }
  }
  if (learned_enabled) {
    const auto learned_begin = BuildClock::now();
    const auto& learned_isa = retain_isa ? impl->isa : temporary_isa;
    impl->learned = BuildLearnedIndex(
        impl->reference, impl->text, impl->suffix_array, learned_isa,
        impl->sampling_rate, stored_width, options.learned_index);
    if (options.statistics) {
      options.statistics->learned_index_seconds = BuildElapsed(learned_begin);
    }
  }
  impl->RefreshLcpAccess();
  const auto aux_bytes = CoordinateBytes(impl->isa) +
                         impl->lcp.ResidentBytes() +
                         CoordinateBytes(impl->child);
  impl->index_info = BuiltInfo(
      impl->reference, impl->backend, static_cast<std::uint8_t>(width),
      impl->text.size(), SaSize(impl->suffix_array), impl->sampling_rate,
      impl->acceleration, aux_bytes, impl->learned);
  impl->index_info.stored_coordinate_width =
      static_cast<std::uint8_t>(SaStoredWidth(impl->suffix_array));
  impl->index_info.sa_resource_profile = options.resource_profile;
  impl->index_info.lcp_encoding =
      !impl->HasLcp()
          ? SaLcpEncoding::kNone
          : (impl->lcp.Encoding() == detail::LcpEncoding::kRaw32 ||
                     impl->lcp.Encoding() == detail::LcpEncoding::kRaw64
                 ? SaLcpEncoding::kRaw
                 : SaLcpEncoding::kByteCoded);
  impl->index_info.text_bytes = impl->text.size();
  impl->index_info.sa_bytes = SaBytes(impl->suffix_array);
  impl->index_info.isa_bytes = CoordinateBytes(impl->isa);
  impl->index_info.lcp_bytes = impl->lcp.ResidentBytes();
  impl->index_info.lcp_primary_bytes =
      impl->lcp.BytePrimary()
          ? static_cast<std::uint64_t>(impl->lcp.BytePrimary()->size())
          : 0;
  impl->index_info.lcp_overflow_anchors = impl->lcp.AnchorCount();
  impl->index_info.lcp_overflow_bytes =
      impl->index_info.lcp_encoding == SaLcpEncoding::kByteCoded
          ? impl->lcp.SerializedDataBytes() -
                impl->index_info.lcp_primary_bytes
          : 0;
  impl->index_info.lcp_guide_bytes = impl->lcp.GuideBytes();
  impl->index_info.child_bytes = CoordinateBytes(impl->child);
  impl->index_info.resident_core_bytes =
      impl->index_info.text_bytes + impl->index_info.sa_bytes +
      impl->index_info.isa_bytes + impl->index_info.lcp_bytes +
      impl->index_info.child_bytes + impl->learned.ResidentBytes();
  notify("index-prefix-directory");
  const auto prefix_begin = BuildClock::now();
  impl->BuildFastPrefix(options.threads);
  if (options.statistics) {
    options.statistics->prefix_directory_seconds = BuildElapsed(prefix_begin);
    options.statistics->total_seconds = BuildElapsed(total_begin);
  }
  notify("index-build-return");
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
  const ExactPatternBuffer encoded(pattern);
  return impl_->Range(encoded.View(), algorithm, statistics);
}

std::uint64_t SuffixArray::Count(std::string_view pattern,
                                 StrandMode strands) const {
  return Count(pattern, strands, SaSearchAlgorithm::kAutoSelect);
}

std::uint64_t SuffixArray::Count(std::string_view pattern, StrandMode strands,
                                 SaSearchAlgorithm algorithm,
                                 SaSearchStatistics* statistics) const {
  ValidateStrandMode(strands);
  const ExactPatternBuffer encoded(pattern);
  if (strands == StrandMode::kForward) {
    return impl_->ExactCount(encoded.View(), algorithm, statistics);
  }
  const auto reverse = encoded.ReverseComplement();
  if (strands == StrandMode::kReverseComplement) {
    return impl_->ExactCount(reverse.View(), algorithm, statistics);
  }
  if (encoded.Equals(reverse)) {
    return impl_->ExactCount(encoded.View(), algorithm, statistics);
  }
  return impl_->ExactCount(encoded.View(), algorithm, statistics) +
         impl_->ExactCount(reverse.View(), algorithm, statistics);
}

QueryResult SuffixArray::Locate(std::string_view pattern,
                                const LocateOptions& options) const {
  return Locate(pattern, options, SaSearchAlgorithm::kAutoSelect);
}

QueryResult SuffixArray::Locate(std::string_view pattern,
                                const LocateOptions& options,
                                SaSearchAlgorithm algorithm,
                                SaSearchStatistics* statistics) const {
  ValidateStrandMode(options.strands);
  const ExactPatternBuffer encoded(pattern);
  if (options.max_hits && *options.max_hits == 1) {
    std::optional<GlobalMatch> smallest;
    std::uint64_t total_hits = 0;
    if (options.strands == StrandMode::kForward) {
      total_hits = impl_->CollectSmallest(encoded.View(), Strand::kForward,
                                          smallest, algorithm, statistics);
    } else {
      const auto reverse = encoded.ReverseComplement();
      if (options.strands == StrandMode::kReverseComplement) {
        total_hits = impl_->CollectSmallest(
            reverse.View(), Strand::kReverseComplement, smallest, algorithm,
            statistics);
      } else if (encoded.Equals(reverse)) {
        total_hits = impl_->CollectSmallest(
            encoded.View(), Strand::kBoth, smallest, algorithm, statistics);
      } else {
        total_hits = impl_->CollectSmallest(
            encoded.View(), Strand::kForward, smallest, algorithm,
            statistics);
        total_hits += impl_->CollectSmallest(
            reverse.View(), Strand::kReverseComplement, smallest, algorithm,
            statistics);
      }
    }
    return FinalizeSmallestGlobalMatch(smallest, impl_->reference,
                                       encoded.Size(), total_hits);
  }

  std::vector<GlobalMatch> matches;
  bool heap_active = false;
  std::uint64_t total_hits = 0;
  if (options.strands == StrandMode::kForward) {
    total_hits = impl_->Collect(encoded.View(), Strand::kForward, options,
                                matches, heap_active, algorithm, statistics);
  } else {
    const auto reverse = encoded.ReverseComplement();
    if (options.strands == StrandMode::kReverseComplement) {
      total_hits = impl_->Collect(reverse.View(), Strand::kReverseComplement,
                                  options, matches, heap_active, algorithm,
                                  statistics);
    } else if (encoded.Equals(reverse)) {
      total_hits = impl_->Collect(encoded.View(), Strand::kBoth, options,
                                  matches, heap_active, algorithm, statistics);
    } else {
      total_hits = impl_->Collect(encoded.View(), Strand::kForward, options,
                                  matches, heap_active, algorithm, statistics);
      total_hits += impl_->Collect(
          reverse.View(), Strand::kReverseComplement, options, matches,
          heap_active, algorithm, statistics);
    }
  }
  return FinalizeGlobalMatches(std::move(matches), impl_->reference,
                               encoded.Size(), total_hits);
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
  const auto algorithm = impl_->ResolveMemAlgorithm(options.algorithm);
  impl_->EnumerateEncodedMem(encoded, options, algorithm, callback);
}

MemResult SuffixArray::FindMems(
    std::string_view query, const MemOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareMemSearch(options);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveMemAlgorithm(options.algorithm);
  MemResult result;
  if (!max_matches) {
    auto collect = [&](const MemMatch& match) {
      result.matches.push_back(match);
    };
    impl_->EnumerateEncodedMem(encoded, options, algorithm, collect);
    std::sort(result.matches.begin(), result.matches.end(), MemMatchLess);
    result.matches.erase(
        std::unique(result.matches.begin(), result.matches.end(),
                    MemMatchEqual),
        result.matches.end());
    result.total_matches = result.matches.size();
    return result;
  }

  const auto retain = *max_matches;
  bool heap_active = false;
  auto collect = [&](const MemMatch& match) {
    // Every MEM has one anchor/residue owner. The streaming kernel therefore
    // emits each directional tuple once, making this an exact count without a
    // result-sized deduplication set.
    ++result.total_matches;
    if (retain == 0) {
      return;
    }
    if (!heap_active && result.matches.size() < retain) {
      result.matches.push_back(match);
      return;
    }
    if (!heap_active) {
      std::make_heap(result.matches.begin(), result.matches.end(),
                     MemMatchLess);
      heap_active = true;
    }
    if (MemMatchLess(match, result.matches.front())) {
      std::pop_heap(result.matches.begin(), result.matches.end(), MemMatchLess);
      result.matches.back() = match;
      std::push_heap(result.matches.begin(), result.matches.end(),
                     MemMatchLess);
    }
  };
  impl_->EnumerateEncodedMem(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(), MemMatchLess);
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
  const auto algorithm = impl_->ResolveMamAlgorithm(options.algorithm);
  impl_->EnumerateEncodedMam(encoded, options, algorithm, callback);
}

MamResult SuffixArray::FindMams(
    std::string_view query, const MamOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareMamSearch(options);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveMamAlgorithm(options.algorithm);
  MamResult result;
  if (!max_matches) {
    auto collect = [&](const MamMatch& match) {
      result.matches.push_back(match);
    };
    impl_->EnumerateEncodedMam(encoded, options, algorithm, collect);
    std::sort(result.matches.begin(), result.matches.end(), MamMatchLess);
    result.matches.erase(
        std::unique(result.matches.begin(), result.matches.end(),
                    MamMatchEqual),
        result.matches.end());
    result.total_matches = result.matches.size();
    return result;
  }

  const auto retain = *max_matches;
  bool heap_active = false;
  auto collect = [&](const MamMatch& match) {
    // Complete-SA query positions and SA rows form a unique owner for every
    // directional reference-MAM tuple.
    ++result.total_matches;
    if (retain == 0) {
      return;
    }
    if (!heap_active && result.matches.size() < retain) {
      result.matches.push_back(match);
      return;
    }
    if (!heap_active) {
      std::make_heap(result.matches.begin(), result.matches.end(),
                     MamMatchLess);
      heap_active = true;
    }
    if (MamMatchLess(match, result.matches.front())) {
      std::pop_heap(result.matches.begin(), result.matches.end(), MamMatchLess);
      result.matches.back() = match;
      std::push_heap(result.matches.begin(), result.matches.end(),
                     MamMatchLess);
    }
  };
  impl_->EnumerateEncodedMam(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(), MamMatchLess);
  result.truncated = result.matches.size() < result.total_matches;
  return result;
}

void SuffixArray::ForEachSmem(std::string_view query,
                              const SmemOptions& options,
                              const SmemCallback& callback) const {
  PrepareSmemSearch(options);
  if (!callback) {
    throw Error(ErrorCode::kInvalidInput, "SMEM callback must not be empty");
  }
  impl_->ValidateExplicitSearchAlgorithm(options.lookup_algorithm);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveSmemAlgorithm(options.algorithm);
  impl_->EnumerateEncodedSmem(encoded, options, algorithm, callback);
}

SmemResult SuffixArray::FindSmems(
    std::string_view query, const SmemOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareSmemSearch(options);
  impl_->ValidateExplicitSearchAlgorithm(options.lookup_algorithm);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveSmemAlgorithm(options.algorithm);
  SmemResult result;
  if (!max_matches) {
    auto collect = [&](const SmemMatch& match) {
      result.matches.push_back(match);
    };
    result.total_smems =
        impl_->EnumerateEncodedSmem(encoded, options, algorithm, collect);
    std::sort(result.matches.begin(), result.matches.end(), SmemMatchLess);
    result.matches.erase(
        std::unique(result.matches.begin(), result.matches.end(),
                    SmemMatchEqual),
        result.matches.end());
    result.total_matches = result.matches.size();
    return result;
  }

  const auto retain = *max_matches;
  bool heap_active = false;
  auto collect = [&](const SmemMatch& match) {
    ++result.total_matches;
    if (retain == 0) {
      return;
    }
    if (!heap_active && result.matches.size() < retain) {
      result.matches.push_back(match);
      return;
    }
    if (!heap_active) {
      std::make_heap(result.matches.begin(), result.matches.end(),
                     SmemMatchLess);
      heap_active = true;
    }
    if (SmemMatchLess(match, result.matches.front())) {
      std::pop_heap(result.matches.begin(), result.matches.end(),
                    SmemMatchLess);
      result.matches.back() = match;
      std::push_heap(result.matches.begin(), result.matches.end(),
                     SmemMatchLess);
    }
  };
  result.total_smems =
      impl_->EnumerateEncodedSmem(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(), SmemMatchLess);
  result.truncated = result.matches.size() < result.total_matches;
  return result;
}

void SuffixArray::ForEachMum(std::string_view query,
                             const MumOptions& options,
                             const MumCallback& callback) const {
  PrepareMumSearch(options);
  if (!callback) {
    throw Error(ErrorCode::kInvalidInput, "MUM callback must not be empty");
  }
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveMumAlgorithm(options.algorithm);
  impl_->EnumerateEncodedMum(encoded, options, algorithm, callback);
}

MumResult SuffixArray::FindMums(
    std::string_view query, const MumOptions& options,
    std::optional<std::uint64_t> max_matches) const {
  PrepareMumSearch(options);
  const auto encoded = EncodeRightMaximalQuery(query);
  const auto algorithm = impl_->ResolveMumAlgorithm(options.algorithm);
  MumResult result;
  if (!max_matches) {
    auto collect = [&](const MumMatch& match) {
      result.matches.push_back(match);
    };
    impl_->EnumerateEncodedMum(encoded, options, algorithm, collect);
    std::sort(result.matches.begin(), result.matches.end(), MumMatchLess);
    result.matches.erase(
        std::unique(result.matches.begin(), result.matches.end(),
                    MumMatchEqual),
        result.matches.end());
    result.total_matches = result.matches.size();
    return result;
  }

  const auto retain = *max_matches;
  bool heap_active = false;
  auto collect = [&](const MumMatch& match) {
    ++result.total_matches;
    if (retain == 0) {
      return;
    }
    if (!heap_active && result.matches.size() < retain) {
      result.matches.push_back(match);
      return;
    }
    if (!heap_active) {
      std::make_heap(result.matches.begin(), result.matches.end(),
                     MumMatchLess);
      heap_active = true;
    }
    if (MumMatchLess(match, result.matches.front())) {
      std::pop_heap(result.matches.begin(), result.matches.end(), MumMatchLess);
      result.matches.back() = match;
      std::push_heap(result.matches.begin(), result.matches.end(),
                     MumMatchLess);
    }
  };
  impl_->EnumerateEncodedMum(encoded, options, algorithm, collect);
  std::sort(result.matches.begin(), result.matches.end(), MumMatchLess);
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
  spec.format_minor = 4;
  spec.kind = IndexKind::kSuffixArray;
  spec.backend = impl_->backend;
  spec.coordinate_width = impl_->index_info.coordinate_width;
  spec.sa_resource_profile = impl_->index_info.sa_resource_profile;
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
         detail::WriteCoordinateSectionV14(out, impl_->suffix_array,
                                           impl_->text.size());
       }}};
  if (impl_->HasIsa()) {
    writers.push_back(
        {detail::SectionType::kInverseSuffixArray, [&](std::ostream& out) {
           detail::WriteCoordinateSectionV14(
               out, impl_->isa, SaSize(impl_->suffix_array));
         }});
  }
  if (impl_->HasLcp()) {
    writers.push_back({detail::SectionType::kLcp, [&](std::ostream& out) {
                         detail::WriteLcpSectionV14(out, impl_->lcp,
                                                    impl_->text.size());
                       }});
  }
  if (impl_->HasChild()) {
    writers.push_back({detail::SectionType::kChild, [&](std::ostream& out) {
                         detail::WriteCoordinateSectionV14(
                             out, impl_->child,
                             SaSize(impl_->suffix_array) + 1U);
                       }});
  }
  if (impl_->HasLearned()) {
    writers.push_back({detail::SectionType::kLearnedSa, [&](std::ostream& out) {
                         WriteLearnedIndex(out, impl_->learned,
                                           spec.coordinate_width,
                                           impl_->reference.fingerprint,
                                           SaSize(impl_->suffix_array));
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
  // Validate section combinations and exact codec payload sizes before any
  // genome-scale allocation. A codec header is untrusted even after its CRC
  // has been verified because a deliberately constructed corrupt file can
  // carry a matching checksum.
  auto container_info = detail::IndexInfoFromContainer(container);
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
  const auto count =
      SampledSuffixCount(impl->text.size(), impl->sampling_rate);
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
  const auto expected_construction_width =
      static_cast<std::uint8_t>(backend_is_32 ? 32 : 64);
  if (container.spec.coordinate_width != expected_construction_width) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array backend and construction width disagree");
  }
  const bool backend_is_caps =
      container.spec.backend == detail::StoredBackend::kCaps32 ||
      container.spec.backend == detail::StoredBackend::kCaps64;
  const auto construction_backend =
      backend_is_caps ? SaBackend::kCaps : SaBackend::kDivsufsort;
  const auto construction_width =
      backend_is_32 ? CoordinateWidth::kBits32 : CoordinateWidth::kBits64;
  if (!detail::SaConstructionCanRepresent(
          construction_backend, construction_width, impl->text.size())) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array text exceeds its recorded constructor width");
  }
  auto sa_input =
      detail::OpenSectionStream(container, detail::SectionType::kSuffixArray);
  if (container.spec.format_minor >= 4) {
    impl->suffix_array = detail::ReadCoordinateSectionV14(
        *sa_input, count, impl->text.size(), "suffix array");
    for (std::uint64_t row = 0; row < count; ++row) {
      accept_position(SaValue(impl->suffix_array, row));
    }
  } else {
    const auto recorded_count =
        detail::ReadU64(*sa_input, "suffix-array length");
    if (recorded_count != count) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array length does not match sampling metadata");
    }
    RawCoordinateStorage legacy_sa =
        expected_construction_width == 32
            ? RawCoordinateStorage(
                  Coordinate32(static_cast<std::size_t>(count)))
            : RawCoordinateStorage(
                  Coordinate64(static_cast<std::size_t>(count)));
    std::visit(
        [&](auto& values) {
          ReadIntegerPayload(*sa_input, values,
                             expected_construction_width,
                             "suffix-array values",
                             [&](std::uint64_t, std::uint64_t raw) {
                               accept_position(raw);
                             });
        },
        legacy_sa);
    if (sa_input->peek() != std::char_traits<char>::eof()) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array section has trailing bytes");
    }
    impl->suffix_array = std::visit(
        [&](auto& values) -> SaStorage {
          using Values = std::decay_t<decltype(values)>;
          if constexpr (std::is_same_v<Values, Coordinate32>) {
            return SaStorage::FromUInt32(
                std::move(values), CoordinateStorageWidth::kBits32,
                impl->text.size(), ErrorCode::kCorruptIndex,
                "suffix array");
          } else {
            return SaStorage::FromUInt64(
                std::move(values), CoordinateStorageWidth::kBits64,
                impl->text.size(), ErrorCode::kCorruptIndex,
                "suffix array");
          }
        },
        legacy_sa);
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
    if (container.spec.format_minor >= 4) {
      auto input = detail::OpenSectionStream(
          container, detail::SectionType::kInverseSuffixArray);
      impl->isa = detail::ReadCoordinateSectionV14(
          *input, count, count, "inverse suffix array");
    } else {
      impl->isa = ReadLegacyCoordinateVector(
          container, detail::SectionType::kInverseSuffixArray, count,
          container.spec.coordinate_width, count, "inverse suffix array");
    }
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
    if (container.spec.format_minor >= 4) {
      auto input =
          detail::OpenSectionStream(container, detail::SectionType::kLcp);
      impl->lcp = detail::ReadLcpSectionV14(
          *input, count, impl->sampling_rate, impl->text.size());
    } else {
      const auto lcp_width = static_cast<std::uint8_t>(
          container.spec.text_symbols <=
                  std::numeric_limits<std::uint32_t>::max()
              ? 32
              : 64);
      auto raw_lcp = ReadLegacyIntegerVector(
          container, detail::SectionType::kLcp, count, lcp_width, "LCP");
      impl->lcp = std::visit(
          [&](auto& values) -> detail::LcpStorage {
            using Values = std::decay_t<decltype(values)>;
            if constexpr (std::is_same_v<Values, Coordinate32>) {
              return detail::LcpStorage::FromRaw32(
                  std::move(values), impl->sampling_rate,
                  ErrorCode::kCorruptIndex);
            } else {
              return detail::LcpStorage::FromRaw64(
                  std::move(values), impl->sampling_rate,
                  ErrorCode::kCorruptIndex);
            }
          },
          raw_lcp);
    }
    ValidateLcpAgainstSuffixArray(impl->lcp, impl->suffix_array,
                                  impl->text.size());
  }
  impl->RefreshLcpAccess();
  if (has_child) {
    if (container.spec.format_minor >= 4) {
      auto input =
          detail::OpenSectionStream(container, detail::SectionType::kChild);
      impl->child = detail::ReadCoordinateSectionV14(
          *input, count, count + 1U, "CHILD");
    } else {
      impl->child = ReadLegacyCoordinateVector(
          container, detail::SectionType::kChild, count,
          container.spec.coordinate_width, count + 1U, "CHILD");
    }
    const auto child_values = ViewCoordinates(impl->child);
    for (std::size_t index = 0; index < child_values.size; ++index) {
      if (child_values[index] > count) {
        throw Error(ErrorCode::kCorruptIndex, "CHILD index is out of range");
      }
    }
    const auto rebuilt_child = BuildChild(
        impl->lcp, impl->suffix_array, impl->child.Width(),
        ErrorCode::kCorruptIndex);
    if (!CoordinatesEqual(impl->child, rebuilt_child)) {
      throw Error(ErrorCode::kCorruptIndex,
                  "CHILD table is inconsistent with LCP");
    }
  }
  if (has_learned) {
    impl->learned = ReadLearnedIndex(container, count);
  }
  impl->index_info = std::move(container_info);
  if (impl->index_info.sa_sampling_rate != impl->sampling_rate ||
      impl->index_info.suffix_count != count) {
    throw Error(ErrorCode::kCorruptIndex,
                "sampled SA metadata is inconsistent");
  }
  impl->index_info.sa_acceleration = impl->acceleration;
  impl->index_info.auxiliary_bytes = CoordinateBytes(impl->isa) +
                                     impl->lcp.ResidentBytes() +
                                     CoordinateBytes(impl->child);
  impl->index_info.stored_coordinate_width =
      static_cast<std::uint8_t>(impl->suffix_array.Width());
  impl->index_info.lcp_encoding =
      !has_lcp
          ? SaLcpEncoding::kNone
          : (impl->lcp.Encoding() == detail::LcpEncoding::kRaw32 ||
                     impl->lcp.Encoding() == detail::LcpEncoding::kRaw64
                 ? SaLcpEncoding::kRaw
                 : SaLcpEncoding::kByteCoded);
  impl->index_info.text_bytes = impl->text.size();
  impl->index_info.sa_bytes = impl->suffix_array.Bytes();
  impl->index_info.isa_bytes = impl->isa.Bytes();
  impl->index_info.lcp_bytes = impl->lcp.ResidentBytes();
  impl->index_info.lcp_primary_bytes =
      impl->lcp.BytePrimary()
          ? static_cast<std::uint64_t>(impl->lcp.BytePrimary()->size())
          : 0;
  impl->index_info.lcp_overflow_anchors = impl->lcp.AnchorCount();
  impl->index_info.lcp_overflow_bytes =
      impl->index_info.lcp_encoding == SaLcpEncoding::kByteCoded
          ? impl->lcp.SerializedDataBytes() -
                impl->index_info.lcp_primary_bytes
          : 0;
  impl->index_info.lcp_guide_bytes = impl->lcp.GuideBytes();
  impl->index_info.child_bytes = impl->child.Bytes();
  if (has_learned) {
    impl->index_info.sa_lookup_acceleration = SaLookupAcceleration::kSaplingPwl;
    impl->index_info.learned_index_bytes =
        impl->learned.SerializedBytes();
    impl->index_info.learned_k = impl->learned.k;
    impl->index_info.learned_bucket_bits = impl->learned.bucket_bits;
    impl->index_info.learned_memory_overhead_basis_points =
        impl->learned.memory_overhead_basis_points;
  }
  impl->index_info.resident_core_bytes =
      impl->index_info.text_bytes + impl->index_info.sa_bytes +
      impl->index_info.isa_bytes + impl->index_info.lcp_bytes +
      impl->index_info.child_bytes + impl->learned.ResidentBytes();
  impl->BuildFastPrefix();
  return SuffixArray(std::move(impl));
}

}  // namespace sufkit
