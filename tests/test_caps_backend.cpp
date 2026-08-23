#include <sufkit/sufkit.hpp>

#include "caps_backend.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
        ++failures; \
    } \
} while (false)

template <class Function>
void check_error(sufkit::ErrorCode expected, Function&& function) {
    try {
        function();
        std::cerr << "expected sufkit::Error was not thrown\n";
        ++failures;
    } catch (const sufkit::Error& error) {
        CHECK(error.code() == expected);
    }
}

sufkit::SuffixArrayBuildOptions options(
    sufkit::SaBackend backend,
    sufkit::CoordinateWidth width,
    std::uint32_t threads,
    sufkit::SaAcceleration acceleration = sufkit::SaAcceleration::none) {
    sufkit::SuffixArrayBuildOptions result;
    result.backend = backend;
    result.coordinate_width = width;
    result.threads = threads;
    result.acceleration = acceleration;
    return result;
}

sufkit::GenomeReference make_reference() {
    std::string first;
    std::string second;
    first.reserve(12288);
    second.reserve(8192);
    std::uint64_t state = 0x9e3779b97f4a7c15ULL;
    constexpr char bases[] = {'A', 'C', 'G', 'T'};
    for (std::size_t index = 0; index < 12288; ++index) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        first.push_back(index % 521 == 0 ? 'N' : bases[(state * 2685821657736338717ULL) & 3ULL]);
    }
    const std::string repeat = "ACGTACGTGATTACATTTTCCCCAAAAGGGG";
    while (second.size() < 8192) second += repeat;
    second.resize(8192);
    return sufkit::GenomeReference::from_records({
        {"random", "deterministic", std::move(first)},
        {"repeat", "repeat-rich", std::move(second)}
    });
}

void compare_suffix_arrays(const sufkit::SuffixArray& left, const sufkit::SuffixArray& right) {
    CHECK(left.info().text_symbols == right.info().text_symbols);
    const auto count = std::min(left.info().text_symbols, right.info().text_symbols);
    for (std::uint64_t row = 0; row < count; ++row) {
        if (left.suffix_at(row) != right.suffix_at(row)) {
            std::cerr << "suffix-array mismatch at row " << row << '\n';
            ++failures;
            break;
        }
    }
}

std::uint64_t suffix_checksum(const sufkit::SuffixArray& index) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint64_t row = 0; row < index.info().text_symbols; ++row) {
        hash ^= index.suffix_at(row);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool same_right_maximal_result(const sufkit::RightMaximalResult& left, const sufkit::RightMaximalResult& right) {
    if (left.total_matches != right.total_matches ||
        left.truncated != right.truncated ||
        left.matches.size() != right.matches.size()) return false;
    for (std::size_t index = 0; index < left.matches.size(); ++index) {
        const auto& a = left.matches[index];
        const auto& b = right.matches[index];
        if (a.sequence_id != b.sequence_id ||
            a.reference_position != b.reference_position ||
            a.query_position != b.query_position ||
            a.length != b.length || a.strand != b.strand) return false;
    }
    return true;
}

void test_selection_policy() {
    using sufkit::SaBackend;
    using sufkit::detail::resolve_sa_backend;
    constexpr auto threshold = sufkit::detail::kCapsAutoThresholdSymbols;
    CHECK(resolve_sa_backend(SaBackend::auto_select, threshold - 1, 2, true) == SaBackend::divsufsort);
    CHECK(resolve_sa_backend(SaBackend::auto_select, threshold, 1, true) == SaBackend::divsufsort);
    CHECK(resolve_sa_backend(SaBackend::auto_select, threshold, 2, true) == SaBackend::caps);
    CHECK(resolve_sa_backend(SaBackend::auto_select, threshold, 2, false) == SaBackend::divsufsort);
    CHECK(resolve_sa_backend(SaBackend::caps, 32, 1, false) == SaBackend::caps);
    CHECK(sufkit::detail::resolve_sa_coordinate_width(
        SaBackend::divsufsort,
        sufkit::CoordinateWidth::auto_select,
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) ==
        sufkit::CoordinateWidth::bits32);
    CHECK(sufkit::detail::resolve_sa_coordinate_width(
        SaBackend::divsufsort,
        sufkit::CoordinateWidth::auto_select,
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1) ==
        sufkit::CoordinateWidth::bits64);
    CHECK(sufkit::detail::resolve_sa_coordinate_width(
        SaBackend::caps,
        sufkit::CoordinateWidth::auto_select,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) ==
        sufkit::CoordinateWidth::bits32);
    CHECK(sufkit::detail::resolve_sa_coordinate_width(
        SaBackend::caps,
        sufkit::CoordinateWidth::auto_select,
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1) ==
        sufkit::CoordinateWidth::bits64);
    CHECK(sufkit::detail::caps_subproblem_count(16, 1) == 1);
    CHECK(sufkit::detail::caps_subproblem_count(1ULL << 30, 64) == 8192);
    CHECK(sufkit::detail::caps_subproblem_count(1ULL << 30, 2) == 256);
}

void test_availability() {
    const auto backends = sufkit::available_sa_backends();
    const auto caps = std::find_if(backends.begin(), backends.end(), [](const auto& backend) {
        return backend.name == "caps";
    });
    CHECK(caps != backends.end());
    if (caps != backends.end()) {
        CHECK(caps->supports_threads);
        CHECK(caps->available == static_cast<bool>(SUFKIT_TEST_CAPS_ENABLED));
    }
}

#if SUFKIT_TEST_CAPS_ENABLED
void test_caps_builds(const std::filesystem::path& directory) {
    const auto reference = make_reference();
    auto div32 = sufkit::SuffixArray::build(
        reference,
        options(sufkit::SaBackend::divsufsort, sufkit::CoordinateWidth::bits32, 1));
    auto caps32_1 = sufkit::SuffixArray::build(
        reference,
        options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits32, 1));
    auto caps32_2 = sufkit::SuffixArray::build(
        reference,
        options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits32, 2));
    auto caps64 = sufkit::SuffixArray::build(
        reference,
        options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits64, 4));

    CHECK(caps32_1.info().backend == "caps32");
    CHECK(caps32_1.info().coordinate_width == 32);
    CHECK(caps64.info().backend == "caps64");
    CHECK(caps64.info().coordinate_width == 64);
    compare_suffix_arrays(div32, caps32_1);
    compare_suffix_arrays(div32, caps32_2);
    compare_suffix_arrays(div32, caps64);
    CHECK(caps32_1.count("ACGTACGT") == div32.count("ACGTACGT"));
    CHECK(caps64.locate("GATTACA").total_hits == div32.locate("GATTACA").total_hits);

    auto automatic = sufkit::SuffixArray::build(
        reference,
        options(sufkit::SaBackend::auto_select, sufkit::CoordinateWidth::auto_select, 8));
    CHECK(automatic.info().backend == "divsufsort32");

    const auto path = directory / "caps32.sufidx";
    caps32_2.save(path);
    const auto inspected = sufkit::inspect_index(path);
    CHECK(inspected.backend == "caps32");
    CHECK(inspected.backend_signature.find("2597b373") != std::string::npos);
    auto loaded = sufkit::SuffixArray::load(path);
    CHECK(loaded.info().backend == "caps32");
    compare_suffix_arrays(caps32_2, loaded);
    CHECK(loaded.locate("ACGTACGT").total_hits == caps32_2.locate("ACGTACGT").total_hits);

    sufkit::SuffixArrayBuildStatistics build_statistics;
    auto learned_options = options(
        sufkit::SaBackend::caps,
        sufkit::CoordinateWidth::bits32,
        2,
        sufkit::SaAcceleration::full);
    learned_options.learned_index.enabled = true;
    learned_options.learned_index.k = 4;
    learned_options.learned_index.bucket_bits = 4;
    learned_options.statistics = &build_statistics;
    auto learned_caps = sufkit::SuffixArray::build(reference, learned_options);
    CHECK(learned_caps.info().backend == "caps32");
    CHECK(learned_caps.lookup_acceleration() == sufkit::SaLookupAcceleration::sapling_pwl);
    CHECK(build_statistics.sa_seconds >= 0.0);
    for (const std::string pattern : {"ACGT", "GATTACA", "CCCCAAAA", "TGCATGCA"}) {
        const auto binary = learned_caps.equal_range(pattern, sufkit::SaSearchAlgorithm::binary);
        const auto learned = learned_caps.equal_range(pattern, sufkit::SaSearchAlgorithm::sapling_pwl);
        CHECK(binary.begin == learned.begin && binary.end == learned.end);
    }
    const auto learned_path = directory / "caps32-learned.sufidx";
    learned_caps.save(learned_path);
    auto loaded_learned = sufkit::SuffixArray::load(learned_path);
    CHECK(loaded_learned.info().backend == "caps32");
    CHECK(loaded_learned.lookup_acceleration() == sufkit::SaLookupAcceleration::sapling_pwl);
    const auto loaded_binary = loaded_learned.equal_range("ACGT", sufkit::SaSearchAlgorithm::binary);
    const auto loaded_sapling = loaded_learned.equal_range("ACGT", sufkit::SaSearchAlgorithm::sapling_pwl);
    CHECK(loaded_binary.begin == loaded_sapling.begin && loaded_binary.end == loaded_sapling.end);

    const std::vector<sufkit::SaAcceleration> accelerations{
        sufkit::SaAcceleration::lcp,
        sufkit::SaAcceleration::lcp_child,
        sufkit::SaAcceleration::lcp_suffix_link,
        sufkit::SaAcceleration::full
    };
    for (const auto acceleration : accelerations) {
        auto div = sufkit::SuffixArray::build(
            reference,
            options(sufkit::SaBackend::divsufsort, sufkit::CoordinateWidth::bits32, 2, acceleration));
        auto caps = sufkit::SuffixArray::build(
            reference,
            options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits32, 2, acceleration));
        compare_suffix_arrays(div, caps);
        CHECK(div.count("ACGTACGT") == caps.count("ACGTACGT"));
        sufkit::RightMaximalOptions right_maximal;
        right_maximal.min_length = 8;
        right_maximal.strands = sufkit::StrandMode::both;
        CHECK(same_right_maximal_result(div.find_right_maximal_matches("TTACACGTACGTGATTACATTTT"),
                       caps.find_right_maximal_matches("TTACACGTACGTGATTACATTTT")));
    }

    const auto tiny = sufkit::GenomeReference::from_records({{"tiny", "", "ACGT"}});
    check_error(sufkit::ErrorCode::invalid_input, [&] {
        (void)sufkit::SuffixArray::build(
            tiny,
            options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits32, 2));
    });
}

void test_concurrent_builds() {
    const auto reference = make_reference();
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::exception_ptr first_error;
    std::exception_ptr second_error;
    std::thread left([&] {
        try {
            auto index = sufkit::SuffixArray::build(
                reference,
                options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits32, 2));
            first = suffix_checksum(index);
        } catch (...) { first_error = std::current_exception(); }
    });
    std::thread right([&] {
        try {
            auto index = sufkit::SuffixArray::build(
                reference,
                options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits64, 3));
            second = suffix_checksum(index);
        } catch (...) { second_error = std::current_exception(); }
    });
    left.join();
    right.join();
    CHECK(first_error == nullptr);
    CHECK(second_error == nullptr);
    CHECK(first != 0);
    CHECK(first == second);
}
#else
void test_disabled_build() {
    const auto reference = make_reference();
    check_error(sufkit::ErrorCode::unsupported_backend, [&] {
        (void)sufkit::SuffixArray::build(
            reference,
            options(sufkit::SaBackend::caps, sufkit::CoordinateWidth::bits32, 2));
    });
}
#endif

} // namespace

int main() {
    test_selection_policy();
    test_availability();
    const auto directory = std::filesystem::path("/tmp") /
        ("sufkit-caps-tests-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
#if SUFKIT_TEST_CAPS_ENABLED
    test_caps_builds(directory);
    test_concurrent_builds();
#else
    test_disabled_build();
#endif
    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "CaPS backend tests passed\n";
    return 0;
}
