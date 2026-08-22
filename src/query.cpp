#include "query.hpp"

#include <algorithm>
#include <cctype>
#include <tuple>

#include "reference_data.hpp"

namespace sufkit::detail {
namespace {

bool match_less(const Match& left, const Match& right) {
    return std::tie(left.sequence_id, left.position, left.length, left.strand) <
           std::tie(right.sequence_id, right.position, right.length, right.strand);
}

} // namespace

std::vector<std::uint8_t> encode_pattern(std::string_view pattern) {
    if (pattern.empty()) {
        throw Error(ErrorCode::invalid_input, "pattern must not be empty");
    }
    std::vector<std::uint8_t> encoded;
    encoded.reserve(pattern.size());
    for (const char raw : pattern) {
        const auto value = static_cast<unsigned char>(raw);
        switch (static_cast<char>(std::toupper(value))) {
        case 'A': encoded.push_back(kA); break;
        case 'C': encoded.push_back(kC); break;
        case 'G': encoded.push_back(kG); break;
        case 'T': encoded.push_back(kT); break;
        default:
            throw Error(
                ErrorCode::invalid_input,
                "pattern contains a non-ACGT character");
        }
    }
    return encoded;
}

std::vector<std::uint8_t> reverse_complement(const std::vector<std::uint8_t>& pattern) {
    std::vector<std::uint8_t> result;
    result.reserve(pattern.size());
    for (auto it = pattern.rbegin(); it != pattern.rend(); ++it) {
        switch (*it) {
        case kA: result.push_back(kT); break;
        case kC: result.push_back(kG); break;
        case kG: result.push_back(kC); break;
        case kT: result.push_back(kA); break;
        default:
            throw Error(ErrorCode::invalid_input, "internal pattern encoding is invalid");
        }
    }
    return result;
}

bool is_reverse_complement_palindrome(const std::vector<std::uint8_t>& pattern) {
    return pattern == reverse_complement(pattern);
}

void retain_match(
    std::vector<Match>& matches,
    Match match,
    const LocateOptions& options) {
    if (!options.max_hits) {
        matches.push_back(std::move(match));
        return;
    }
    const auto limit = *options.max_hits;
    if (limit == 0) {
        return;
    }
    if (matches.size() < limit) {
        matches.push_back(std::move(match));
        std::push_heap(matches.begin(), matches.end(), match_less);
        return;
    }
    if (match_less(match, matches.front())) {
        std::pop_heap(matches.begin(), matches.end(), match_less);
        matches.back() = std::move(match);
        std::push_heap(matches.begin(), matches.end(), match_less);
    }
}

QueryResult finalize_matches(
    std::vector<Match> matches,
    std::uint64_t total_hits) {
    std::sort(matches.begin(), matches.end(), match_less);

    std::vector<Match> merged;
    merged.reserve(matches.size());
    for (const auto& match : matches) {
        if (!merged.empty() &&
            merged.back().sequence_id == match.sequence_id &&
            merged.back().position == match.position &&
            merged.back().length == match.length) {
            if (merged.back().strand != match.strand) {
                merged.back().strand = Strand::both;
            }
            continue;
        }
        merged.push_back(match);
    }

    QueryResult result;
    result.total_hits = total_hits;
    result.hits = std::move(merged);
    result.truncated = result.hits.size() < result.total_hits;
    return result;
}

} // namespace sufkit::detail
