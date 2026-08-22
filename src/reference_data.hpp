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

    std::uint64_t logical_text_size() const noexcept {
        return static_cast<std::uint64_t>(encoded.size()) + 1;
    }
};

inline std::uint64_t content_fingerprint(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::uint64_t content_fingerprint(const std::vector<std::uint8_t>& data) {
    return content_fingerprint(data.data(), data.size());
}

inline std::string fingerprint_hex(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

inline std::optional<std::pair<SequenceId, Position>> map_global_position(
    const std::vector<SequenceInfo>& sequences,
    Position global,
    std::uint64_t pattern_length) {
    if (sequences.empty()) {
        return std::nullopt;
    }
    const auto it = std::upper_bound(
        sequences.begin(), sequences.end(), global,
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
    if (local > sequence.length || pattern_length > sequence.length - local) {
        return std::nullopt;
    }
    return std::make_pair(sequence.id, local);
}

} // namespace sufkit::detail
