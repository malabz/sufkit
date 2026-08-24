#include "benchmark_common.hpp"
#include "benchmark_profiles.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "app_support.hpp"

namespace sufkit::app::bench {
namespace {

using Clock = std::chrono::steady_clock;

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31U);
    }

private:
    std::uint64_t state_;
};

double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

char random_base(SplitMix64& random, double gc_fraction) {
    const auto scaled = static_cast<std::uint64_t>(gc_fraction * 1000000.0);
    const auto draw = random.next() % 1000000ULL;
    if (draw < scaled) {
        return (random.next() & 1U) == 0 ? 'G' : 'C';
    }
    return (random.next() & 1U) == 0 ? 'A' : 'T';
}

std::string reverse_complement(const std::string& pattern) {
    std::string result;
    result.reserve(pattern.size());
    for (auto it = pattern.rbegin(); it != pattern.rend(); ++it) {
        switch (*it) {
        case 'A': result.push_back('T'); break;
        case 'C': result.push_back('G'); break;
        case 'G': result.push_back('C'); break;
        case 'T': result.push_back('A'); break;
        default: throw Error(ErrorCode::kInvalidInput, "benchmark pattern is not A/C/G/T");
        }
    }
    return result;
}

std::uint64_t occurrence_count(
    const std::vector<SequenceRecord>& records,
    const std::string& pattern,
    std::uint64_t stop_after = std::numeric_limits<std::uint64_t>::max()) {
    std::uint64_t count = 0;
    for (const auto& record : records) {
        auto position = record.sequence.find(pattern);
        while (position != std::string::npos) {
            ++count;
            if (count >= stop_after) return count;
            position = record.sequence.find(pattern, position + 1);
        }
    }
    return count;
}

std::optional<std::pair<std::size_t, std::size_t>> sample_window(
    const std::vector<SequenceRecord>& records,
    std::size_t length,
    SplitMix64& random) {
    if (records.empty()) return std::nullopt;
    for (int attempt = 0; attempt < 512; ++attempt) {
        const auto sequence_id = static_cast<std::size_t>(random.next() % records.size());
        const auto& sequence = records[sequence_id].sequence;
        if (sequence.size() < length) continue;
        const auto start = static_cast<std::size_t>(
            random.next() % (sequence.size() - length + 1));
        if (sequence.compare(start, length, std::string(length, 'N')) == 0) continue;
        if (sequence.substr(start, length).find('N') == std::string::npos) {
            return std::make_pair(sequence_id, start);
        }
    }
    return std::nullopt;
}

std::string make_random_no_hit(
    const std::vector<SequenceRecord>& records,
    std::size_t length,
    SplitMix64& random) {
    for (int attempt = 0; attempt < 128; ++attempt) {
        std::string pattern(length, 'A');
        for (auto& base : pattern) base = random_base(random, 0.5);
        if (occurrence_count(records, pattern, 1) == 0 &&
            occurrence_count(records, reverse_complement(pattern), 1) == 0) return pattern;
    }
    throw Error(ErrorCode::kBuildFailure, "cannot generate a deterministic no-hit query");
}

std::string make_boundary_pattern(
    const std::vector<SequenceRecord>& records,
    std::size_t length,
    bool contig_boundary,
    SplitMix64& random) {
    if (length < 2 || records.empty()) return {};
    const auto left_length = length / 2;
    const auto right_length = length - left_length;
    std::string pattern;
    if (contig_boundary) {
        for (std::size_t index = 0; index + 1 < records.size(); ++index) {
            const auto& left = records[index].sequence;
            const auto& right = records[index + 1].sequence;
            if (left.size() < left_length || right.size() < right_length) continue;
            pattern = left.substr(left.size() - left_length) + right.substr(0, right_length);
            if (pattern.find('N') == std::string::npos &&
                occurrence_count(records, pattern, 1) == 0 &&
                occurrence_count(records, reverse_complement(pattern), 1) == 0) {
                return pattern;
            }
        }
    } else {
        for (const auto& record : records) {
            const auto first_n = record.sequence.find('N');
            if (first_n == std::string::npos || first_n < left_length) continue;
            auto after_n = first_n;
            while (after_n < record.sequence.size() && record.sequence[after_n] == 'N') ++after_n;
            if (after_n + right_length > record.sequence.size()) continue;
            pattern = record.sequence.substr(first_n - left_length, left_length) +
                      record.sequence.substr(after_n, right_length);
            if (pattern.find('N') == std::string::npos &&
                occurrence_count(records, pattern, 1) == 0 &&
                occurrence_count(records, reverse_complement(pattern), 1) == 0) {
                return pattern;
            }
        }
    }
    if (pattern.empty()) return {};
    for (int attempt = 0; attempt < 64; ++attempt) {
        const auto position = static_cast<std::size_t>(random.next() % pattern.size());
        pattern[position] = pattern[position] == 'A' ? 'C' : 'A';
        if (occurrence_count(records, pattern, 1) == 0 &&
            occurrence_count(records, reverse_complement(pattern), 1) == 0) return pattern;
    }
    return {};
}

void add_query(
    Dataset& dataset,
    std::string group,
    std::uint64_t length,
    std::string sequence,
    std::string source) {
    if (sequence.empty()) return;
    dataset.queries.push_back({
        "q" + std::to_string(dataset.queries.size()),
        std::move(sequence),
        std::move(group),
        std::to_string(length),
        std::move(source)});
}

void add_group_specs(Dataset& dataset, const std::vector<std::uint64_t>& lengths) {
    static const std::array<const char*, 7> groups{{
        "exact_unique", "exact_repetitive", "mutated_low_hit", "random_no_hit",
        "n_boundary", "contig_boundary", "reverse_complement"}};
    for (const auto* group : groups) {
        for (const auto length : lengths) {
            dataset.groups.push_back({group, std::to_string(length)});
        }
    }
}

void finalize_dataset(Dataset& dataset, double repeat_fraction) {
    std::uint64_t gc = 0;
    std::uint64_t ambiguous = 0;
    dataset.total_bases = 0;
    for (const auto& record : dataset.records) {
        dataset.total_bases += record.sequence.size();
        for (const char base : record.sequence) {
            if (base == 'G' || base == 'C') ++gc;
            if (base == 'N') ++ambiguous;
        }
    }
    dataset.contigs = dataset.records.size();
    const auto canonical_bases = dataset.total_bases - ambiguous;
    dataset.gc_fraction = canonical_bases == 0
        ? 0.0 : static_cast<double>(gc) / static_cast<double>(canonical_bases);
    dataset.ambiguous_fraction = dataset.total_bases == 0
        ? 0.0 : static_cast<double>(ambiguous) / static_cast<double>(dataset.total_bases);
    dataset.repeat_fraction = repeat_fraction;
    const auto begin = Clock::now();
    auto reference = GenomeReference::FromRecords(dataset.records);
    dataset.normalization_seconds = elapsed(begin);
    dataset.fingerprint = reference.Fingerprint();
}

std::string hit_bucket(std::uint64_t hits) {
    if (hits == 0) return "user_hit_0";
    if (hits == 1) return "user_hit_1";
    if (hits <= 10) return "user_hit_2_10";
    if (hits <= 1000) return "user_hit_11_1000";
    return "user_hit_gt_1000";
}

} // namespace

const char* to_string(Profile value) noexcept {
    switch (value) {
    case Profile::smoke: return "smoke";
    case Profile::quick: return "quick";
    case Profile::standard: return "standard";
    case Profile::full: return "full";
    case Profile::user: return "user";
    }
    return "unknown";
}

const char* to_string(Scenario value) noexcept {
    switch (value) {
    case Scenario::mixed: return "mixed";
    case Scenario::balanced: return "balanced";
    case Scenario::gc_skewed: return "gc-skewed";
    case Scenario::repeat_rich: return "repeat-rich";
    case Scenario::n_islands: return "n-islands";
    case Scenario::many_contig: return "many-contig";
    case Scenario::user: return "user";
    }
    return "unknown";
}

Profile parse_profile(const std::string& value) {
    if (value == "smoke") return Profile::smoke;
    if (value == "quick") return Profile::quick;
    if (value == "standard") return Profile::standard;
    if (value == "full") return Profile::full;
    throw Error(ErrorCode::kInvalidInput, "invalid benchmark profile: " + value);
}

Scenario parse_scenario(const std::string& value) {
    if (value == "mixed") return Scenario::mixed;
    if (value == "balanced") return Scenario::balanced;
    if (value == "gc-skewed") return Scenario::gc_skewed;
    if (value == "repeat-rich") return Scenario::repeat_rich;
    if (value == "n-islands") return Scenario::n_islands;
    if (value == "many-contig") return Scenario::many_contig;
    throw Error(ErrorCode::kInvalidInput, "invalid benchmark scenario: " + value);
}

ProfileSpec profile_spec(Profile value) {
    const auto& definition = sufkit::benchmark::profile_definition(to_string(value));
    return {definition.reference_bases, definition.query_count,
            definition.build_repetitions, definition.query_warmups,
            definition.query_repetitions};
}

std::string fingerprint_hex(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << value;
    return output.str();
}

std::uint64_t checksum_seed() noexcept { return 14695981039346656037ULL; }

void mix_checksum(std::uint64_t& hash, std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
        hash *= 1099511628211ULL;
    }
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    if ((values.size() & 1U) != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

double minimum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end());
}

double maximum(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

Dataset generate_dataset(
    Profile profile,
    Scenario scenario,
    std::uint64_t seed,
    const std::vector<std::uint64_t>& pattern_lengths) {
    const auto begin = Clock::now();
    const auto spec = profile_spec(profile);
    Dataset dataset;
    dataset.name = std::string("synthetic-") + to_string(profile) + "-" + to_string(scenario);
    dataset.scenario = scenario;
    add_group_specs(dataset, pattern_lengths);
    SplitMix64 random(seed ^ (static_cast<std::uint64_t>(scenario) * 0x9e3779b97f4a7c15ULL));

    const std::size_t contig_count = scenario == Scenario::many_contig ? 1024 : 4;
    const auto base_size = static_cast<std::size_t>(spec.total_bases / contig_count);
    const auto remainder = static_cast<std::size_t>(spec.total_bases % contig_count);
    for (std::size_t contig = 0; contig < contig_count; ++contig) {
        const auto length = base_size + (contig < remainder ? 1U : 0U);
        double gc_fraction = 0.5;
        if (scenario == Scenario::gc_skewed) gc_fraction = (contig & 1U) == 0 ? 0.25 : 0.75;
        if (scenario == Scenario::mixed) {
            static const std::array<double, 4> values{{0.30, 0.45, 0.55, 0.70}};
            gc_fraction = values[contig % values.size()];
        }
        std::string sequence(length, 'A');
        for (auto& base : sequence) base = random_base(random, gc_fraction);
        dataset.records.push_back({
            "contig" + std::to_string(contig), "synthetic", std::move(sequence)});
    }

    std::string repeat_template(1024, 'A');
    for (auto& base : repeat_template) base = random_base(random, 0.5);
    double repeat_fraction = 0.0;
    const double target_repeat = scenario == Scenario::repeat_rich
        ? 0.40 : (scenario == Scenario::mixed ? 0.15 : 0.01);
    std::uint64_t repeated = 0;
    std::uint64_t max_repeated_span = 0;
    for (auto& record : dataset.records) {
        if (record.sequence.size() < repeat_template.size()) continue;
        const auto target = static_cast<std::uint64_t>(
            static_cast<double>(record.sequence.size()) * target_repeat);
        std::uint64_t record_repeated = 0;
        for (std::size_t position = 257;
             position + repeat_template.size() <= record.sequence.size() && record_repeated < target;
             position += repeat_template.size() * 2) {
            const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(
                repeat_template.size(), target - record_repeated));
            std::copy_n(repeat_template.begin(), amount, record.sequence.begin() + static_cast<std::ptrdiff_t>(position));
            record_repeated += amount;
            repeated += amount;
        }
        max_repeated_span = std::max(max_repeated_span, record_repeated);
    }
    if (spec.total_bases != 0) {
        repeat_fraction = static_cast<double>(repeated) / static_cast<double>(spec.total_bases);
    }

    const double n_fraction = scenario == Scenario::n_islands
        ? 0.05 : (scenario == Scenario::mixed ? 0.01 : 0.0005);
    for (std::size_t contig = 0; contig < dataset.records.size(); ++contig) {
        auto& sequence = dataset.records[contig].sequence;
        if (sequence.size() < 64) continue;
        const auto target_n = std::max<std::size_t>(1, static_cast<std::size_t>(
            static_cast<double>(sequence.size()) * n_fraction));
        std::size_t written = 0;
        std::size_t cursor = 31 + (contig * 97U) % std::max<std::size_t>(1, sequence.size() / 4);
        while (written < target_n && cursor < sequence.size()) {
            const auto island = std::min<std::size_t>(written == 0 ? 16 : 64, target_n - written);
            if (cursor + island > sequence.size()) break;
            std::fill_n(sequence.begin() + static_cast<std::ptrdiff_t>(cursor), island, 'N');
            written += island;
            cursor += std::max<std::size_t>(island + 1, sequence.size() / std::max<std::size_t>(2, target_n / 32));
        }
    }

    for (const auto length_value : pattern_lengths) {
        const auto length = static_cast<std::size_t>(length_value);
        const auto n_boundary = make_boundary_pattern(dataset.records, length, false, random);
        add_query(dataset, "n_boundary", length_value, n_boundary, "synthetic N island");
        const auto contig_boundary = make_boundary_pattern(dataset.records, length, true, random);
        add_query(dataset, "contig_boundary", length_value, contig_boundary, "synthetic contig edge");
    }

    const auto reserved = dataset.queries.size();
    const auto remaining = spec.query_count > reserved ? spec.query_count - reserved : 0;
    static const std::array<const char*, 5> main_groups{{
        "exact_unique", "exact_repetitive", "mutated_low_hit", "random_no_hit", "reverse_complement"}};
    for (std::uint64_t index = 0; index < remaining; ++index) {
        const auto* group = main_groups[static_cast<std::size_t>(index % main_groups.size())];
        const auto length_value = pattern_lengths[static_cast<std::size_t>(
            (index / main_groups.size()) % pattern_lengths.size())];
        const auto length = static_cast<std::size_t>(length_value);
        std::string pattern;
        std::string source;
        if (std::string(group) == "exact_repetitive") {
            const auto available_repeat_span = std::min<std::uint64_t>(
                repeat_template.size(), max_repeated_span);
            if (length <= available_repeat_span) {
                const auto start = static_cast<std::size_t>(
                    random.next() % (static_cast<std::size_t>(available_repeat_span) - length + 1));
                pattern = repeat_template.substr(start, length);
                source = "repeat-template:" + std::to_string(start);
                if (occurrence_count(dataset.records, pattern, 1) == 0) pattern.clear();
            }
        } else if (std::string(group) == "random_no_hit") {
            pattern = make_random_no_hit(dataset.records, length, random);
            source = "deterministic random no-hit";
        } else {
            const auto sampled = sample_window(dataset.records, length, random);
            if (!sampled) continue;
            pattern = dataset.records[sampled->first].sequence.substr(sampled->second, length);
            source = dataset.records[sampled->first].name + ":" + std::to_string(sampled->second);
            if (std::string(group) == "exact_unique") {
                for (int attempt = 0; occurrence_count(dataset.records, pattern, 2) != 1 && attempt < 128; ++attempt) {
                    const auto replacement = sample_window(dataset.records, length, random);
                    if (!replacement) break;
                    pattern = dataset.records[replacement->first].sequence.substr(replacement->second, length);
                    source = dataset.records[replacement->first].name + ":" + std::to_string(replacement->second);
                }
                if (occurrence_count(dataset.records, pattern, 2) != 1) pattern.clear();
            } else if (std::string(group) == "mutated_low_hit") {
                const auto position = pattern.size() / 2;
                pattern[position] = pattern[position] == 'A' ? 'C' : 'A';
                source += ":mutated";
            } else if (std::string(group) == "reverse_complement" && (index & 1U) != 0 && length % 2 == 0) {
                std::string half(length / 2, 'A');
                for (auto& base : half) base = random_base(random, 0.5);
                pattern = half + reverse_complement(half);
                source = "deterministic reverse-complement palindrome";
            }
        }
        add_query(dataset, group, length_value, std::move(pattern), std::move(source));
    }

    dataset.reference_seconds = elapsed(begin);
    finalize_dataset(dataset, repeat_fraction);
    return dataset;
}

Dataset load_user_dataset(
    const std::filesystem::path& reference,
    const std::optional<std::filesystem::path>& queries,
    std::uint64_t seed,
    std::uint64_t generated_query_count,
    const std::vector<std::uint64_t>& pattern_lengths) {
    const auto begin = Clock::now();
    Dataset dataset;
    dataset.name = reference.filename().string();
    dataset.scenario = Scenario::user;
    dataset.records = ReadFastaRecords(reference);
    dataset.reference_seconds = elapsed(begin);
    if (dataset.records.empty()) {
        throw Error(ErrorCode::kInvalidInput, "benchmark reference contains no records");
    }
    SplitMix64 random(seed);
    if (queries) {
        const auto records = ReadFastaRecords(*queries);
        for (const auto& record : records) {
            dataset.queries.push_back({
                record.name, record.sequence, "user", std::to_string(record.sequence.size()), "user query"});
        }
    } else {
        static const std::array<const char*, 3> groups{{
            "exact_unique", "mutated_low_hit", "random_no_hit"}};
        for (std::uint64_t index = 0; index < generated_query_count; ++index) {
            const auto* group = groups[static_cast<std::size_t>(index % groups.size())];
            const auto length_value = pattern_lengths[static_cast<std::size_t>(
                (index / groups.size()) % pattern_lengths.size())];
            const auto length = static_cast<std::size_t>(length_value);
            std::string pattern;
            std::string source;
            if (std::string(group) == "random_no_hit") {
                pattern = make_random_no_hit(dataset.records, length, random);
                source = "generated no-hit";
            } else {
                const auto sampled = sample_window(dataset.records, length, random);
                if (!sampled) continue;
                pattern = dataset.records[sampled->first].sequence.substr(sampled->second, length);
                source = dataset.records[sampled->first].name + ":" + std::to_string(sampled->second);
                if (std::string(group) == "mutated_low_hit") {
                    const auto position = pattern.size() / 2;
                    pattern[position] = pattern[position] == 'A' ? 'C' : 'A';
                    source += ":mutated";
                }
            }
            add_query(dataset, group, length_value, std::move(pattern), std::move(source));
        }
    }
    if (dataset.queries.empty()) {
        throw Error(ErrorCode::kInvalidInput, "benchmark query set contains no usable records");
    }
    std::set<std::pair<std::string, std::string>> groups;
    for (const auto& query : dataset.queries) groups.emplace(query.group, query.pattern_length);
    for (const auto& group : groups) dataset.groups.push_back({group.first, group.second});
    finalize_dataset(dataset, -1.0);
    return dataset;
}

void classify_user_queries(Dataset& dataset, const std::filesystem::path& classifier_index) {
    if (dataset.queries.empty() || dataset.queries.front().group != "user") return;
    const auto info = InspectIndex(classifier_index);
    std::set<std::pair<std::string, std::string>> groups;
    if (info.kind == IndexKind::kSuffixArray) {
        auto index = SuffixArray::Load(classifier_index);
        for (auto& query : dataset.queries) {
            query.group = hit_bucket(index.Count(query.sequence));
            groups.emplace(query.group, query.pattern_length);
        }
    } else {
        auto index = FmIndex::Load(classifier_index);
        for (auto& query : dataset.queries) {
            query.group = hit_bucket(index.Count(query.sequence));
            groups.emplace(query.group, query.pattern_length);
        }
    }
    dataset.groups.clear();
    for (const auto& group : groups) dataset.groups.push_back({group.first, group.second});
}

} // namespace sufkit::app::bench
