#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::detail {

std::vector<std::uint8_t> encode_pattern(std::string_view pattern);
std::vector<std::uint8_t> reverse_complement(const std::vector<std::uint8_t>& pattern);
bool is_reverse_complement_palindrome(const std::vector<std::uint8_t>& pattern);
void retain_match(
    std::vector<Match>& matches,
    Match match,
    const LocateOptions& options);
QueryResult finalize_matches(
    std::vector<Match> matches,
    std::uint64_t total_hits);

} // namespace sufkit::detail
