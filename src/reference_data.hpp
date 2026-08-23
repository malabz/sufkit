// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::detail {

// Zero is reserved for the unique terminal sentinel appended during index
// construction. Separators delimit contigs, while N is a reference hard break;
// legal queries contain only kA..kT and therefore cannot match any boundary.
constexpr std::uint8_t kSentinel = 0;
constexpr std::uint8_t kSeparator = 1;
constexpr std::uint8_t kA = 2;
constexpr std::uint8_t kC = 3;
constexpr std::uint8_t kG = 4;
constexpr std::uint8_t kT = 5;
constexpr std::uint8_t kN = 6;
constexpr std::uint8_t kNormalizationId = 1;

struct ReferenceData {
  std::vector<std::uint8_t> encoded;
  std::vector<SequenceInfo> sequences;
  std::uint64_t total_bases = 0;
  std::uint64_t ambiguous_bases = 0;
  std::uint64_t fingerprint = 0;

  std::uint64_t LogicalTextSize() const noexcept {
    return static_cast<std::uint64_t>(encoded.size()) + 1;
  }
};

inline std::uint64_t ContentFingerprint(const std::uint8_t* data,
                                        std::size_t size) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= data[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline std::uint64_t ContentFingerprint(const std::vector<std::uint8_t>& data) {
  return ContentFingerprint(data.data(), data.size());
}

inline std::string FingerprintHex(std::uint64_t value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value;
  return out.str();
}

inline std::optional<std::pair<SequenceId, Position>> MapGlobalPosition(
    const std::vector<SequenceInfo>& sequences, Position global,
    std::uint64_t pattern_length) {
  if (sequences.empty()) {
    return std::nullopt;
  }
  const auto it =
      std::upper_bound(sequences.begin(), sequences.end(), global,
                       [](Position value, const SequenceInfo& sequence) {
                         return value < sequence.global_offset;
                       });
  if (it == sequences.begin()) {
    return std::nullopt;
  }
  const auto& sequence = *std::prev(it);
  if (global < sequence.global_offset) {
    return std::nullopt;
  }
  const Position local = global - sequence.global_offset;
  // This final containment check prevents hits from crossing a contig
  // separator, the terminal sentinel, or malformed persisted coordinates.
  if (local > sequence.length || pattern_length > sequence.length - local) {
    return std::nullopt;
  }
  return std::make_pair(sequence.id, local);
}

}  // namespace sufkit::detail
