#include "caps_backend.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#if SUFKIT_HAS_CAPS
#include "Suffix_Array.hpp"
#include "parlay/parallel.h"
#endif

namespace sufkit::detail {
namespace {

template <class Index>
std::vector<Index> build_caps(
    const std::vector<std::uint8_t>& text,
    std::uint32_t threads) {
#if SUFKIT_HAS_CAPS
    if (text.size() < 16) {
        throw Error(
            ErrorCode::invalid_input,
            "CaPS-SA requires a logical text containing at least 16 symbols");
    }
    if (text.size() > static_cast<std::uint64_t>(std::numeric_limits<Index>::max())) {
        throw Error(
            ErrorCode::invalid_input,
            sizeof(Index) == 4
                ? "reference is too large for CaPS-SA uint32_t"
                : "reference is too large for CaPS-SA uint64_t");
    }

    const auto subproblems = caps_subproblem_count(text.size(), threads);
    try {
        CaPS_SA::Suffix_Array<Index> suffix_array(
            reinterpret_cast<const char*>(text.data()),
            static_cast<Index>(text.size()),
            static_cast<Index>(subproblems),
            0);
        parlay::execute_with_scheduler(threads, [&] {
            suffix_array.construct();
        });
        return std::vector<Index>(
            suffix_array.SA(),
            suffix_array.SA() + text.size());
    } catch (const std::bad_alloc&) {
        throw Error(ErrorCode::build_failure, "CaPS-SA allocation failed");
    } catch (const std::exception& error) {
        throw Error(
            ErrorCode::build_failure,
            std::string("CaPS-SA construction failed: ") + error.what());
    } catch (...) {
        throw Error(ErrorCode::build_failure, "CaPS-SA construction failed");
    }
#else
    (void)text;
    (void)threads;
    throw Error(
        ErrorCode::unsupported_backend,
        "CaPS-SA support was disabled when sufkit was built");
#endif
}

} // namespace

bool caps_build_available() noexcept {
#if SUFKIT_HAS_CAPS
    return true;
#else
    return false;
#endif
}

std::vector<std::uint32_t> build_caps32(
    const std::vector<std::uint8_t>& text,
    std::uint32_t threads) {
    return build_caps<std::uint32_t>(text, threads);
}

std::vector<std::uint64_t> build_caps64(
    const std::vector<std::uint8_t>& text,
    std::uint32_t threads) {
    return build_caps<std::uint64_t>(text, threads);
}

} // namespace sufkit::detail
