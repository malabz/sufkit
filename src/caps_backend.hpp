// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::detail {

inline constexpr std::uint64_t kCapsAutoThresholdSymbols = 1ULL << 30;

bool CapsBuildAvailable() noexcept;

template <class Index>
struct CapsBuildResult {
  std::vector<Index> suffix_array;
  // CaPS can expose the complete adjacent-suffix LCP during construction.
  std::vector<Index> lcp;
};

inline SaBackend ResolveSaBackend(SaBackend requested,
                                  std::uint64_t text_symbols,
                                  std::uint32_t threads,
                                  bool caps_available) noexcept {
  if (requested != SaBackend::kAutoSelect) {
    return requested;
  }
  return caps_available && threads > 1 &&
                 text_symbols >= kCapsAutoThresholdSymbols
             ? SaBackend::kCaps
             : SaBackend::kDivsufsort;
}

inline CoordinateWidth ResolveSaCoordinateWidth(
    SaBackend effective_backend, CoordinateWidth requested,
    std::uint64_t text_symbols) noexcept {
  if (requested != CoordinateWidth::kAutoSelect) {
    return requested;
  }
  const auto limit = effective_backend == SaBackend::kCaps
                         ? static_cast<std::uint64_t>(
                               std::numeric_limits<std::uint32_t>::max())
                         : static_cast<std::uint64_t>(
                               std::numeric_limits<std::int32_t>::max());
  return text_symbols <= limit ? CoordinateWidth::kBits32
                               : CoordinateWidth::kBits64;
}

inline std::uint64_t MaximumSaConstructionTextSymbols(
    SaBackend backend, CoordinateWidth width) noexcept {
  if (width == CoordinateWidth::kBits32) {
    if (backend == SaBackend::kCaps) {
      return std::numeric_limits<std::uint32_t>::max();
    }
    if (backend == SaBackend::kDivsufsort) {
      return static_cast<std::uint64_t>(
          std::numeric_limits<std::int32_t>::max());
    }
  }
  if (width == CoordinateWidth::kBits64) {
    if (backend == SaBackend::kCaps) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    if (backend == SaBackend::kDivsufsort) {
      return static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max());
    }
  }
  return 0;
}

inline bool SaConstructionCanRepresent(SaBackend backend,
                                       CoordinateWidth width,
                                       std::uint64_t text_symbols) noexcept {
  const auto limit = MaximumSaConstructionTextSymbols(backend, width);
  return limit != 0 && text_symbols != 0 && text_symbols <= limit;
}

inline std::uint64_t CapsSubproblemCount(std::uint64_t text_symbols,
                                         std::uint32_t threads) noexcept {
  // Keep enough independent work for the scheduler without creating a large
  // task-metadata overhead on short references.
  const auto by_text = std::max<std::uint64_t>(1, text_symbols / 4096);
  const auto by_threads =
      std::max<std::uint64_t>(1, static_cast<std::uint64_t>(threads) * 128);
  return std::min<std::uint64_t>({8192, by_text, by_threads});
}

CapsBuildResult<std::uint32_t> BuildCaps32(
    const std::vector<std::uint8_t>& text, std::uint32_t threads,
    bool retain_lcp);

CapsBuildResult<std::uint64_t> BuildCaps64(
    const std::vector<std::uint8_t>& text, std::uint32_t threads,
    bool retain_lcp);

}  // namespace sufkit::detail
