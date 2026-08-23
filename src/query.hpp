// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::detail {

std::vector<std::uint8_t> EncodePattern(std::string_view pattern);
std::vector<std::uint8_t> ReverseComplement(
    const std::vector<std::uint8_t>& pattern);
bool IsReverseComplementPalindrome(const std::vector<std::uint8_t>& pattern);
void RetainMatch(std::vector<Match>& matches, Match match,
                 const LocateOptions& options);
QueryResult FinalizeMatches(std::vector<Match> matches,
                            std::uint64_t total_hits);

}  // namespace sufkit::detail
