// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <vector>

namespace sufkit::detail {

template <class Index>
struct DivSufsortBuildResult {
  std::vector<Index> suffix_array;
  // For sampled SA, ISA is dense over positions position / sampling_rate.
  std::vector<std::uint64_t> isa;
  std::vector<std::uint64_t> lcp;
  double sa_seconds = 0.0;
  double isa_seconds = 0.0;
  double lcp_seconds = 0.0;
};

DivSufsortBuildResult<std::int32_t> BuildDivsufsort32(
    const std::vector<std::uint8_t>& text, std::uint32_t sampling_rate,
    bool retain_lcp, bool retain_isa, std::uint32_t threads);

DivSufsortBuildResult<std::int64_t> BuildDivsufsort64(
    const std::vector<std::uint8_t>& text, std::uint32_t sampling_rate,
    bool retain_lcp, bool retain_isa, std::uint32_t threads);

}  // namespace sufkit::detail
