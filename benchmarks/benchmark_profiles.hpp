#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace sufkit::benchmark {

struct ProfileDefinition {
    std::uint64_t reference_bases;
    std::uint64_t query_count;
    std::uint32_t build_repetitions;
    std::uint32_t query_warmups;
    std::uint32_t query_repetitions;
};

inline const ProfileDefinition& profile_definition(std::string_view name) {
    static constexpr ProfileDefinition smoke{16ULL << 10U, 100, 1, 1, 3};
    static constexpr ProfileDefinition quick{4ULL << 20U, 1000, 3, 1, 5};
    static constexpr ProfileDefinition standard{32ULL << 20U, 5000, 3, 1, 7};
    static constexpr ProfileDefinition full{256ULL << 20U, 10000, 1, 1, 5};
    static constexpr ProfileDefinition user{0, 1000, 1, 1, 5};

    if (name == "smoke") return smoke;
    if (name == "quick") return quick;
    if (name == "standard") return standard;
    if (name == "full") return full;
    if (name == "user") return user;
    throw std::invalid_argument("unknown benchmark profile");
}

} // namespace sufkit::benchmark
