#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <tuple>
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
void expect_error(sufkit::ErrorCode code, Function&& function) {
    try {
        function();
        CHECK(false);
    } catch (const sufkit::Error& error) {
        CHECK(error.code() == code);
    }
}

sufkit::GenomeReference reference() {
    return sufkit::GenomeReference::from_records({
        {"alpha", "", "ACGTACGTGATTACACCCCGGGGTTTTACGTACGTNNACGTGATTACA"},
        {"beta", "", "TTTTACGTACGTAAAACCCCGATTACAGATTACATGCATGCAACGT"},
        {"repeat", "", "ACGTACGTACGTACGTACGTGATTACAGATTACAGATTACATTTT"}
    });
}

sufkit::SuffixArrayBuildOptions build_options(
    sufkit::SaBackend backend,
    std::uint32_t sampling_rate,
    bool learned = false) {
    sufkit::SuffixArrayBuildOptions options;
    options.backend = backend;
    options.coordinate_width = sufkit::CoordinateWidth::bits32;
    options.threads = backend == sufkit::SaBackend::caps ? 2 : 1;
    options.sampling_rate = sampling_rate;
    options.acceleration = sufkit::SaAcceleration::full;
    options.learned_index.enabled = learned;
    options.learned_index.k = 4;
    options.learned_index.bucket_bits = 4;
    return options;
}

bool same_query_result(const sufkit::QueryResult& left, const sufkit::QueryResult& right) {
    if (left.total_hits != right.total_hits || left.truncated != right.truncated ||
        left.hits.size() != right.hits.size()) return false;
    for (std::size_t index = 0; index < left.hits.size(); ++index) {
        const auto& a = left.hits[index];
        const auto& b = right.hits[index];
        if (std::tie(a.sequence_id, a.position, a.length, a.strand) !=
            std::tie(b.sequence_id, b.position, b.length, b.strand)) return false;
    }
    return true;
}

bool same_mem_result(const sufkit::MemResult& left, const sufkit::MemResult& right) {
    if (left.total_matches != right.total_matches || left.matches.size() != right.matches.size())
        return false;
    for (std::size_t index = 0; index < left.matches.size(); ++index) {
        const auto& a = left.matches[index];
        const auto& b = right.matches[index];
        if (std::tie(a.sequence_id, a.reference_position, a.query_position, a.length, a.strand) !=
            std::tie(b.sequence_id, b.reference_position, b.query_position, b.length, b.strand))
            return false;
    }
    return true;
}

void check_exact(const sufkit::SuffixArray& full, const sufkit::SuffixArray& sampled) {
    const std::vector<std::string> patterns{
        "A", "AC", "ACG", "ACGT", "GATTACA", "TTTTACGT", "TGCATGCA",
        "CCCCGGGG", "AAAAAAAA", "ACGTGATTACAC"};
    for (const auto& pattern : patterns) {
        for (const auto strands : {sufkit::StrandMode::forward,
                                   sufkit::StrandMode::reverse_complement,
                                   sufkit::StrandMode::both}) {
            CHECK(full.count(pattern, strands) == sampled.count(pattern, strands));
            sufkit::LocateOptions options;
            options.strands = strands;
            CHECK(same_query_result(full.locate(pattern, options), sampled.locate(pattern, options)));
        }
    }
}

void check_mems(const sufkit::SuffixArray& full, const sufkit::SuffixArray& sampled,
                std::uint64_t min_length) {
    const std::vector<std::string> queries{
        "GGACGTACGTGATTACATTTT", "NNACGTGATTACANNTGCATGCA", "GATTACAGATTACAGG",
        "ACGTACGTACGTACGT"};
    for (const auto algorithm : {
            sufkit::MemSearchAlgorithm::baseline,
            sufkit::MemSearchAlgorithm::lcp,
            sufkit::MemSearchAlgorithm::child,
            sufkit::MemSearchAlgorithm::suffix_link,
            sufkit::MemSearchAlgorithm::full}) {
        sufkit::MemOptions options;
        options.min_length = min_length;
        options.strands = sufkit::StrandMode::both;
        options.algorithm = algorithm;
        for (const auto& query : queries)
            CHECK(same_mem_result(full.find_mems(query, options), sampled.find_mems(query, options)));
    }
}

void run_backend(sufkit::SaBackend backend, const std::filesystem::path& directory) {
    const auto ref = reference();
    auto full = sufkit::SuffixArray::build(ref, build_options(backend, 1));
    for (const std::uint32_t rate : {2U, 4U, 8U}) {
        auto sampled = sufkit::SuffixArray::build(ref, build_options(backend, rate, true));
        const auto expected = (sampled.info().text_symbols + rate - 1) / rate;
        CHECK(sampled.sampling_rate() == rate);
        CHECK(sampled.info().sa_sampling_rate == rate);
        CHECK(sampled.info().suffix_count == expected);
        CHECK(sampled.info().format_version == "1.3");
        for (std::uint64_t row = 0; row < expected; ++row)
            CHECK(sampled.suffix_at(row) % rate == 0);
        expect_error(sufkit::ErrorCode::unsupported_backend, [&] {
            (void)sampled.equal_range("ACGT");
        });
        check_exact(full, sampled);
        check_mems(full, sampled, std::max<std::uint64_t>(8, rate));

        const auto path = directory /
            (std::string(backend == sufkit::SaBackend::caps ? "caps" : "div") +
             "-sample-" + std::to_string(rate) + ".sufidx");
        sampled.save(path);
        const auto inspected = sufkit::inspect_index(path);
        CHECK(inspected.sa_sampling_rate == rate);
        CHECK(inspected.suffix_count == expected);
        auto loaded = sufkit::SuffixArray::load(path);
        CHECK(loaded.sampling_rate() == rate);
        check_exact(full, loaded);
        check_mems(full, loaded, std::max<std::uint64_t>(8, rate));
    }

    sufkit::SuffixArrayBuildOptions invalid = build_options(backend, 1);
    invalid.sampling_rate = 0;
    expect_error(sufkit::ErrorCode::invalid_input, [&] {
        (void)sufkit::SuffixArray::build(ref, invalid);
    });
    auto sampled4 = sufkit::SuffixArray::build(ref, build_options(backend, 4));
    sufkit::MemOptions too_short;
    too_short.min_length = 3;
    expect_error(sufkit::ErrorCode::invalid_input, [&] {
        (void)sampled4.find_mems("ACGTACGT", too_short);
    });
}

void randomized_differential() {
    std::mt19937_64 generator(20260823);
    const auto base = [&] { return "ACGT"[generator() & 3U]; };
    std::vector<sufkit::SequenceRecord> records;
    for (int record = 0; record < 3; ++record) {
        std::string sequence;
        for (int position = 0; position < 320; ++position)
            sequence.push_back(position % 79 == 0 ? 'N' : base());
        records.push_back({"random-" + std::to_string(record), "", std::move(sequence)});
    }
    const auto ref = sufkit::GenomeReference::from_records(records);
    auto full = sufkit::SuffixArray::build(
        ref, build_options(sufkit::SaBackend::divsufsort, 1));
    for (const std::uint32_t rate : {2U, 3U, 5U}) {
        auto sampled = sufkit::SuffixArray::build(
            ref, build_options(sufkit::SaBackend::divsufsort, rate));
        for (int trial = 0; trial < 200; ++trial) {
            const auto length = static_cast<std::size_t>(1 + generator() % 15);
            std::string pattern;
            for (std::size_t index = 0; index < length; ++index) pattern.push_back(base());
            CHECK(full.count(pattern, sufkit::StrandMode::both) ==
                  sampled.count(pattern, sufkit::StrandMode::both));
            sufkit::LocateOptions locate;
            locate.strands = sufkit::StrandMode::both;
            CHECK(same_query_result(full.locate(pattern, locate), sampled.locate(pattern, locate)));
        }
        for (int trial = 0; trial < 30; ++trial) {
            std::string query;
            for (int position = 0; position < 96; ++position)
                query.push_back(position % 37 == 0 ? 'N' : base());
            sufkit::MemOptions options;
            options.min_length = std::max<std::uint64_t>(rate, 8);
            options.strands = sufkit::StrandMode::both;
            options.algorithm = sufkit::MemSearchAlgorithm::full;
            CHECK(same_mem_result(full.find_mems(query, options),
                                  sampled.find_mems(query, options)));
        }
    }
}

} // namespace

int main() {
    const auto directory = std::filesystem::path("/tmp") /
        ("sufkit-sampled-sa-" + std::to_string(static_cast<long long>(getpid())));
    std::filesystem::create_directories(directory);
    run_backend(sufkit::SaBackend::divsufsort, directory);
    const auto backends = sufkit::available_sa_backends();
    const auto caps = std::find_if(backends.begin(), backends.end(), [](const auto& value) {
        return value.name == "caps";
    });
    if (caps != backends.end() && caps->available)
        run_backend(sufkit::SaBackend::caps, directory);
    randomized_differential();
    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " sampled SA assertion(s) failed\n";
        return 1;
    }
    std::cout << "sampled SA tests passed\n";
    return 0;
}
