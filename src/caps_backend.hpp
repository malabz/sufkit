#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::detail {

inline constexpr std::uint64_t kCapsAutoThresholdSymbols = 1ULL << 30;

bool caps_build_available() noexcept;

inline SaBackend resolve_sa_backend(
    SaBackend requested,
    std::uint64_t text_symbols,
    std::uint32_t threads,
    bool caps_available) noexcept {
    if (requested != SaBackend::auto_select) return requested;
    return caps_available && threads > 1 && text_symbols >= kCapsAutoThresholdSymbols
        ? SaBackend::caps
        : SaBackend::divsufsort;
}

inline CoordinateWidth resolve_sa_coordinate_width(
    SaBackend effective_backend,
    CoordinateWidth requested,
    std::uint64_t text_symbols) noexcept {
    if (requested != CoordinateWidth::auto_select) return requested;
    const auto limit = effective_backend == SaBackend::caps
        ? static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
        : static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    return text_symbols <= limit
        ? CoordinateWidth::bits32
        : CoordinateWidth::bits64;
}

inline std::uint64_t caps_subproblem_count(
    std::uint64_t text_symbols,
    std::uint32_t threads) noexcept {
    const auto by_text = std::max<std::uint64_t>(1, text_symbols / 4096);
    const auto by_threads = std::max<std::uint64_t>(
        1,
        static_cast<std::uint64_t>(threads) * 128);
    return std::min<std::uint64_t>({8192, by_text, by_threads});
}

std::vector<std::uint32_t> build_caps32(
    const std::vector<std::uint8_t>& text,
    std::uint32_t threads);

std::vector<std::uint64_t> build_caps64(
    const std::vector<std::uint8_t>& text,
    std::uint32_t threads);

} // namespace sufkit::detail
