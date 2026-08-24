// SPDX-License-Identifier: MIT

#include "query.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

#include "reference_data.hpp"

namespace sufkit::detail {
namespace {

constexpr std::uint8_t kInvalidQuerySymbol =
    std::numeric_limits<std::uint8_t>::max();

constexpr std::array<std::uint8_t, 256> MakeQueryEncodingTable() {
  std::array<std::uint8_t, 256> table{};
  for (auto& value : table) {
    value = kInvalidQuerySymbol;
  }
  table[static_cast<unsigned char>('A')] = kA;
  table[static_cast<unsigned char>('a')] = kA;
  table[static_cast<unsigned char>('C')] = kC;
  table[static_cast<unsigned char>('c')] = kC;
  table[static_cast<unsigned char>('G')] = kG;
  table[static_cast<unsigned char>('g')] = kG;
  table[static_cast<unsigned char>('T')] = kT;
  table[static_cast<unsigned char>('t')] = kT;
  return table;
}

constexpr std::array<std::uint8_t, 7> kComplement = {
    kInvalidQuerySymbol, kInvalidQuerySymbol, kT, kG, kC, kA,
    kInvalidQuerySymbol};
constexpr auto kQueryEncoding = MakeQueryEncodingTable();

bool MatchLess(const Match& left, const Match& right) {
  return std::tie(left.sequence_id, left.position, left.length, left.strand) <
         std::tie(right.sequence_id, right.position, right.length,
                  right.strand);
}

}  // namespace

std::vector<std::uint8_t> EncodePattern(std::string_view pattern) {
  if (pattern.empty()) {
    throw Error(ErrorCode::kInvalidInput, "pattern must not be empty");
  }
  std::vector<std::uint8_t> encoded;
  encoded.resize(pattern.size());
  // Keeping query symbols in ACGT ensures that N, separators, and the sentinel
  // remain hard boundaries in every index backend.
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    const auto symbol =
        kQueryEncoding[static_cast<unsigned char>(pattern[index])];
    if (symbol == kInvalidQuerySymbol) {
      throw Error(ErrorCode::kInvalidInput,
                  "pattern contains a non-ACGT character");
    }
    encoded[index] = symbol;
  }
  return encoded;
}

std::vector<std::uint8_t> ReverseComplement(
    const std::vector<std::uint8_t>& pattern) {
  std::vector<std::uint8_t> result;
  result.resize(pattern.size());
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    const auto symbol = pattern[pattern.size() - index - 1];
    if (symbol >= kComplement.size() ||
        kComplement[symbol] == kInvalidQuerySymbol) {
      throw Error(ErrorCode::kInvalidInput,
                  "internal pattern encoding is invalid");
    }
    result[index] = kComplement[symbol];
  }
  return result;
}

bool IsReverseComplementPalindrome(const std::vector<std::uint8_t>& pattern) {
  for (std::size_t index = 0; index < pattern.size() / 2; ++index) {
    const auto left = pattern[index];
    const auto right = pattern[pattern.size() - index - 1];
    if (left >= kComplement.size() || right >= kComplement.size() ||
        kComplement[right] != left) {
      return false;
    }
  }
  return pattern.size() % 2 == 0;
}

void RetainMatch(std::vector<Match>& matches, Match match,
                 const LocateOptions& options, bool& heap_active) {
  if (!options.max_hits) {
    matches.push_back(std::move(match));
    return;
  }
  const auto limit = *options.max_hits;
  if (limit == 0) {
    return;
  }
  if (!heap_active && matches.size() < limit) {
    matches.push_back(std::move(match));
    return;
  }
  if (!heap_active) {
    // Delay heap construction until the first omitted candidate. A generous
    // max_hits therefore retains the same append-only path as an unlimited
    // locate instead of paying O(log N) for results that are never truncated.
    std::make_heap(matches.begin(), matches.end(), MatchLess);
    heap_active = true;
  }
  // The max-heap keeps only the lexicographically smallest requested hits;
  // callers count all occurrences separately to retain an exact total.
  if (MatchLess(match, matches.front())) {
    std::pop_heap(matches.begin(), matches.end(), MatchLess);
    matches.back() = std::move(match);
    std::push_heap(matches.begin(), matches.end(), MatchLess);
  }
}

QueryResult FinalizeMatches(std::vector<Match> matches,
                            std::uint64_t total_hits) {
  // Public results are deterministic regardless of SA row order or backend.
  std::sort(matches.begin(), matches.end(), MatchLess);

  std::size_t output = 0;
  for (auto& match : matches) {
    if (output != 0 && matches[output - 1].sequence_id == match.sequence_id &&
        matches[output - 1].position == match.position &&
        matches[output - 1].length == match.length) {
      // Identical coordinates found on both strands retain that provenance
      // without exposing duplicate public hits.
      if (matches[output - 1].strand != match.strand) {
        matches[output - 1].strand = Strand::kBoth;
      }
      continue;
    }
    const auto input = static_cast<std::size_t>(&match - matches.data());
    if (output != input) {
      matches[output] = std::move(match);
    }
    ++output;
  }
  matches.resize(output);

  QueryResult result;
  result.total_hits = total_hits;
  result.hits = std::move(matches);
  result.truncated = result.hits.size() < result.total_hits;
  return result;
}

}  // namespace sufkit::detail
