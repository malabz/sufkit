#include "right_maximal_benchmark.hpp"
#include "app_support.hpp"
#include "benchmark_profiles.hpp"

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <tuple>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sufkit::app::right_maximal_bench {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kVectorMaterializationMatchThreshold = 1'000'000;

struct Options {
    std::string workload = "right-maximal";
    std::string profile = "smoke";
    std::vector<std::string> scenarios{"mixed"};
    std::vector<std::string> methods{"right-maximal-baseline", "right-maximal-lcp", "right-maximal-child", "right-maximal-suffix-link", "right-maximal-full"};
    std::vector<std::uint64_t> min_lengths{20, 50};
    std::vector<std::uint64_t> min_occurrences{1};
    std::filesystem::path output_directory;
    std::optional<std::filesystem::path> mummer4;
    std::optional<std::filesystem::path> mummer4_runtime;
    std::optional<std::filesystem::path> minibwa;
    std::optional<std::filesystem::path> reference;
    std::optional<std::filesystem::path> query_file;
    std::string mummer_version;
    std::string mummer_launcher_sha256;
    std::string mummer_runtime_sha256;
    std::string minibwa_version;
    std::string minibwa_sha256;
    std::uint64_t seed = 20260822;
    std::uint32_t learned_k = 20;
    std::uint32_t learned_memory_overhead_basis_points = 100;
    std::optional<std::uint32_t> learned_bucket_bits;
    std::uint32_t build_repetitions = 1;
    std::uint32_t query_repetitions = 3;
    std::uint32_t warmups = 1;
    bool build_repetitions_explicit = false;
    bool query_repetitions_explicit = false;
    bool warmups_explicit = false;
    bool min_occurrences_explicit = false;
};

struct Dataset {
    std::string name;
    std::string scenario;
    std::vector<SequenceRecord> reference;
    std::vector<SequenceRecord> queries;
    std::uint64_t total_bases = 0;
    std::uint64_t query_bases = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t oracle_reference_bases = 0;
    std::uint64_t oracle_query_bases = 0;
    std::string oracle_status = "not_run";
};

struct CorrectnessResult {
    std::string dataset;
    std::string scenario;
    std::string oracle = "naive-right-maximal-forward";
    std::uint64_t min_length = 0;
    std::uint64_t min_occurrences = 1;
    std::uint64_t reference_bases = 0;
    std::uint64_t query_count = 0;
    std::uint64_t query_bases = 0;
    std::uint64_t total_matches = 0;
    std::uint64_t total_smems = 0;
    std::uint64_t checksum = 0;
    std::string status = "ok";
};

struct BuildResult {
    std::string dataset;
    std::string method;
    std::string algorithm;
    std::string acceleration;
    double build_seconds = 0;
    double sa_build_seconds = 0;
    double storage_compaction_seconds = 0;
    double isa_build_seconds = 0;
    double lcp_build_seconds = 0;
    double child_build_seconds = 0;
    double learned_index_build_seconds = 0;
    double save_seconds = 0;
    double load_seconds = 0;
    double build_peak_rss_mb = 0;
    double save_peak_rss_mb = 0;
    double load_peak_rss_mb = 0;
    std::string build_peak_rss_scope =
        "build_worker_clean_exec_reference_plus_build";
    std::string save_peak_rss_scope =
        "save_worker_clean_exec_load_plus_save";
    std::string load_peak_rss_scope =
        "load_worker_clean_exec_load";
    std::uint64_t serialized_bytes = 0;
    std::uint64_t allocated_disk_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    std::uint64_t sa_bytes = 0;
    std::uint64_t isa_bytes = 0;
    std::uint64_t lcp_bytes = 0;
    std::uint64_t resident_core_bytes = 0;
    std::uint8_t construction_coordinate_width = 0;
    std::uint8_t stored_coordinate_width = 0;
    SaResourceProfile resource_profile = SaResourceProfile::kFast;
    SaLcpEncoding lcp_encoding = SaLcpEncoding::kNone;
    std::uint32_t repetitions = 1;
    std::uint32_t sampling_rate = 1;
    std::string status = "ok";
};

struct QueryResultRow {
    std::string dataset;
    std::string method;
    std::string algorithm;
    std::string acceleration;
    std::string operation;
    std::uint64_t min_length = 0;
    std::uint64_t min_occurrences = 1;
    std::uint64_t query_count = 0;
    std::uint64_t query_bases = 0;
    std::vector<double> seconds;
    std::uint64_t total_matches = 0;
    std::uint64_t total_smems = 0;
    std::uint64_t reported_matches = 0;
    std::uint64_t count_checksum = 0;
    std::uint64_t checksum = 0;
    double peak_rss_mb = 0;
    std::string peak_rss_scope =
        "query_worker_clean_exec_queries_plus_load_plus_query";
    std::uint64_t materialization_match_threshold =
        kVectorMaterializationMatchThreshold;
    bool vector_skipped = false;
    RightMaximalSearchStatistics statistics;
    std::string status = "ok";
};

struct RawRow {
    std::string dataset;
    std::string method;
    std::string operation;
    std::uint64_t min_length = 0;
    std::uint64_t min_occurrences = 1;
    std::uint32_t repetition = 0;
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    double peak_rss_mb = 0;
    std::string peak_rss_scope;
    std::uint64_t query_bases = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t allocated_disk_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    std::uint64_t materialization_match_threshold =
        kVectorMaterializationMatchThreshold;
    bool vector_skipped = false;
    std::uint64_t total_matches = 0;
    std::uint64_t total_smems = 0;
    std::uint64_t reported_matches = 0;
    std::uint64_t count_checksum = 0;
    std::uint64_t checksum = 0;
    RightMaximalSearchStatistics statistics;
    std::string status = "ok";
};

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        const auto value = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (value.empty()) throw Error(ErrorCode::kInvalidInput, "empty comma-separated benchmark value");
        result.push_back(value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::string join_numbers(const std::vector<std::uint64_t>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ',';
        output << values[index];
    }
    return output.str();
}

std::uint64_t parse_number(const std::string& value, const char* name) {
    std::size_t consumed = 0;
    std::uint64_t result = 0;
    try { result = std::stoull(value, &consumed); }
    catch (...) { throw Error(ErrorCode::kInvalidInput, std::string(name) + " requires an integer"); }
    if (consumed != value.size()) throw Error(ErrorCode::kInvalidInput, std::string(name) + " requires an integer");
    return result;
}

Options parse(const std::vector<std::string>& arguments) {
    Options result;
    bool methods_explicit = false;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& name = arguments[index];
        if (name == "--workload") {
            if (++index >= arguments.size())
                throw Error(ErrorCode::kInvalidInput,
                            "maximal match benchmark requires a workload");
            result.workload = arguments[index];
            if (result.workload != "right-maximal" &&
                result.workload != "mem" && result.workload != "mam" &&
                result.workload != "smem" && result.workload != "mum")
                throw Error(ErrorCode::kInvalidInput,
                            "unknown maximal match benchmark workload: " +
                                result.workload);
        } else {
            if (++index >= arguments.size()) throw Error(ErrorCode::kInvalidInput, "missing value for " + name);
            const auto& value = arguments[index];
            if (name == "--profile") result.profile = value;
            else if (name == "--scenarios") result.scenarios = split(value);
            else if (name == "--methods") {
                result.methods = split(value);
                methods_explicit = true;
            }
            else if (name == "--min-lengths") {
                result.min_lengths.clear();
                for (const auto& item : split(value)) result.min_lengths.push_back(parse_number(item, "--min-lengths"));
            } else if (name == "--min-occurrences") {
                result.min_occurrences.clear();
                for (const auto& item : split(value)) {
                    result.min_occurrences.push_back(
                        parse_number(item, "--min-occurrences"));
                }
                result.min_occurrences_explicit = true;
            } else if (name == "--output-dir") result.output_directory = value;
            else if (name == "--mummer4") result.mummer4 = std::filesystem::path(value);
            else if (name == "--mummer4-runtime")
                result.mummer4_runtime = std::filesystem::path(value);
            else if (name == "--minibwa") result.minibwa = std::filesystem::path(value);
            else if (name == "--reference") result.reference = std::filesystem::path(value);
            else if (name == "--queries") result.query_file = std::filesystem::path(value);
            else if (name == "--seed") result.seed = parse_number(value, "--seed");
            else if (name == "--build-repetitions") {
                const auto parsed = parse_number(value, "--build-repetitions");
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
                    throw Error(ErrorCode::kInvalidInput, "--build-repetitions must be a positive 32-bit integer");
                result.build_repetitions = static_cast<std::uint32_t>(parsed);
                result.build_repetitions_explicit = true;
            } else if (name == "--query-repetitions") {
                const auto parsed = parse_number(value, "--query-repetitions");
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
                    throw Error(ErrorCode::kInvalidInput, "--query-repetitions must be a positive 32-bit integer");
                result.query_repetitions = static_cast<std::uint32_t>(parsed);
                result.query_repetitions_explicit = true;
            } else if (name == "--warmups") {
                const auto parsed = parse_number(value, "--warmups");
                if (parsed > std::numeric_limits<std::uint32_t>::max())
                    throw Error(ErrorCode::kInvalidInput, "--warmups must be a 32-bit integer");
                result.warmups = static_cast<std::uint32_t>(parsed);
                result.warmups_explicit = true;
            }
            else if (name == "--learned-k") {
                const auto parsed = parse_number(value, "--learned-k");
                if (parsed == 0 || parsed > 31)
                    throw Error(ErrorCode::kInvalidInput, "--learned-k must be in [1,31]");
                result.learned_k = static_cast<std::uint32_t>(parsed);
            } else if (name == "--learned-memory-bp") {
                const auto parsed = parse_number(value, "--learned-memory-bp");
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
                    throw Error(ErrorCode::kInvalidInput, "--learned-memory-bp is out of range");
                result.learned_memory_overhead_basis_points = static_cast<std::uint32_t>(parsed);
            } else if (name == "--learned-bucket-bits") {
                const auto parsed = parse_number(value, "--learned-bucket-bits");
                if (parsed > 31)
                    throw Error(ErrorCode::kInvalidInput, "--learned-bucket-bits must be at most 31");
                result.learned_bucket_bits = static_cast<std::uint32_t>(parsed);
            }
            else throw Error(ErrorCode::kInvalidInput, "unknown right-maximal exact match benchmark option: " + name);
        }
    }
    if (!methods_explicit && result.workload != "right-maximal") {
        const auto prefix = result.workload + "-";
        result.methods = {prefix + "baseline", prefix + "lcp",
                          prefix + "child", prefix + "suffix-link",
                          prefix + "full"};
    }
    if (result.profile != "smoke" && result.profile != "quick" &&
        result.profile != "standard" && result.profile != "full")
        throw Error(ErrorCode::kInvalidInput,
            "right-maximal exact match benchmark currently runs smoke, quick, standard, or full profiles");
    const auto& profile = sufkit::benchmark::profile_definition(result.profile);
    if (!result.build_repetitions_explicit)
        result.build_repetitions = profile.build_repetitions;
    if (!result.query_repetitions_explicit)
        result.query_repetitions = profile.query_repetitions;
    if (!result.warmups_explicit) result.warmups = profile.query_warmups;
    if (result.query_file && !result.reference)
        throw Error(ErrorCode::kInvalidInput, "--queries requires --reference");
    if (result.mummer4_runtime && !result.mummer4)
        throw Error(ErrorCode::kInvalidInput,
                    "--mummer4-runtime requires --mummer4");
    if (result.output_directory.empty()) throw Error(ErrorCode::kInvalidInput, "--output-dir is required");
    if (result.min_lengths.empty() || std::any_of(result.min_lengths.begin(), result.min_lengths.end(), [](auto value) { return value == 0; }))
        throw Error(ErrorCode::kInvalidInput, "right-maximal exact match minimum lengths must be positive");
    if (result.min_occurrences.empty() ||
        std::any_of(result.min_occurrences.begin(), result.min_occurrences.end(),
                    [](auto value) { return value == 0; }))
        throw Error(ErrorCode::kInvalidInput,
                    "SMEM minimum occurrence counts must be positive");
    if (result.min_occurrences_explicit && result.workload != "smem")
        throw Error(ErrorCode::kInvalidInput,
                    "--min-occurrences is only valid for the SMEM workload");
    const std::set<std::string> valid_scenarios{
        "mixed", "balanced", "gc-skewed", "repeat-rich", "n-islands", "many-contig"};
    for (const auto& scenario : result.scenarios)
        if (valid_scenarios.count(scenario) == 0)
            throw Error(ErrorCode::kInvalidInput, "unknown right-maximal exact match benchmark scenario: " + scenario);
    const std::set<std::string> right_maximal_methods{
        "right-maximal-baseline", "right-maximal-lcp", "right-maximal-child", "right-maximal-suffix-link", "right-maximal-full",
        "right-maximal-suffix-link-binary", "right-maximal-suffix-link-sapling",
        "right-maximal-sampled-k4", "right-maximal-sampled-k8", "mummer4"};
    const std::set<std::string> mem_methods{
        "mem-baseline", "mem-lcp", "mem-child", "mem-suffix-link",
        "mem-full", "mem-auto-fast", "mem-auto-low-memory",
        "mem-suffix-link-fast",
        "mem-lcp-fast-auto-skip", "mem-lcp-fast-skip1",
        "mem-suffix-link-fast-auto-skip", "mem-lcp-low-memory", "mummer4"};
    const std::set<std::string> mam_methods{
        "mam-baseline", "mam-lcp", "mam-child", "mam-suffix-link",
        "mam-full", "mam-auto-fast", "mam-auto-low-memory",
        "mam-suffix-link-fast", "mam-lcp-low-memory", "mummer4"};
    const std::set<std::string> smem_methods{
        "smem-baseline", "smem-lcp", "smem-child", "smem-suffix-link",
        "smem-full", "smem-auto-fast", "smem-auto-low-memory", "minibwa"};
    const std::set<std::string> mum_methods{
        "mum-baseline", "mum-lcp", "mum-child", "mum-suffix-link",
        "mum-full", "mum-auto-fast", "mum-auto-low-memory", "mummer4"};
    const auto& valid_methods = result.workload == "right-maximal"
                                    ? right_maximal_methods
                                : result.workload == "mem" ? mem_methods
                                : result.workload == "mam" ? mam_methods
                                : result.workload == "smem" ? smem_methods
                                                             : mum_methods;
    for (const auto& method : result.methods) {
        if (valid_methods.count(method) == 0) throw Error(ErrorCode::kInvalidInput, "unknown right-maximal exact match benchmark method: " + method);
        if (method == "mummer4" && !result.mummer4) throw Error(ErrorCode::kInvalidInput, "mummer4 method requires --mummer4 PATH");
        if (method == "minibwa" && !result.minibwa)
            throw Error(ErrorCode::kInvalidInput,
                        "minibwa method requires --minibwa PATH");
    }
    return result;
}

std::uint64_t next_random(std::uint64_t& state) {
    state ^= state >> 12U;
    state ^= state << 25U;
    state ^= state >> 27U;
    return state * 2685821657736338717ULL;
}

std::uint64_t hash_bytes(std::uint64_t hash, std::string_view text) {
    for (const unsigned char value : text) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

Dataset generate_dataset(const Options& options, const std::string& scenario) {
    Dataset dataset;
    dataset.name = "synthetic-" + options.workload + "-" + options.profile +
                   "-" + scenario;
    dataset.scenario = scenario;
    const auto& profile = sufkit::benchmark::profile_definition(options.profile);
    const std::uint64_t total_bases = profile.reference_bases;
    const std::size_t query_count = static_cast<std::size_t>(profile.query_count);
    const std::size_t query_length = options.profile == "smoke" ? 128 : 256;
    std::uint64_t state = options.seed ^ hash_bytes(1469598103934665603ULL, scenario);
    const std::array<char, 4> bases{{'A', 'C', 'G', 'T'}};
    const std::size_t contigs = scenario == "many-contig" ? 1024 : 4;
    const std::size_t per_contig = static_cast<std::size_t>(total_bases / contigs);
    dataset.reference.reserve(contigs);
    const std::size_t repeat_template_length = std::min<std::size_t>(
        4096, std::max<std::size_t>(64, per_contig / 16));
    const auto repeat_slot_count = std::max<std::size_t>(
        1, static_cast<std::size_t>(total_bases * 2 / 5) /
            repeat_template_length);
    const std::size_t repeat_template_count = std::min<std::size_t>(
        64, std::max<std::size_t>(1, repeat_slot_count / 4));
    std::uint64_t repeat_state = options.seed ^
        hash_bytes(1469598103934665603ULL,
            "right-maximal-deterministic-repeat-template-family-v1");
    std::vector<std::string> repeat_templates(
        repeat_template_count, std::string(repeat_template_length, 'A'));
    for (auto& repeat_template : repeat_templates) {
        for (auto& base : repeat_template)
            base = bases[static_cast<std::size_t>(
                next_random(repeat_state) & 3U)];
    }
    for (std::size_t id = 0; id < contigs; ++id) {
        std::string sequence(per_contig, 'A');
        double gc_fraction = 0.5;
        if (scenario == "gc-skewed") gc_fraction = id % 2 == 0 ? 0.25 : 0.75;
        else if (scenario == "mixed") {
            const std::array<double, 4> fractions{{0.30, 0.45, 0.60, 0.75}};
            gc_fraction = fractions[id % fractions.size()];
        }
        for (auto& base : sequence) {
            const auto draw = static_cast<double>(next_random(state) % 10000) / 10000.0;
            if (draw < gc_fraction) base = (next_random(state) & 1U) == 0 ? 'G' : 'C';
            else base = (next_random(state) & 1U) == 0 ? 'A' : 'T';
        }
        if (scenario == "repeat-rich") {
            const auto repeat_end = sequence.size() * 2 / 5;
            for (std::size_t offset = 0;
                 offset + repeat_template_length <= repeat_end;
                 offset += repeat_template_length) {
                const auto template_id =
                    (id * 17 + offset / repeat_template_length) %
                    repeat_templates.size();
                sequence.replace(
                    offset, repeat_template_length,
                    repeat_templates[template_id]);
            }
        } else if (scenario == "mixed") {
            const auto repeat_end = std::min(
                sequence.size(), std::max(
                    sequence.size() / 100, repeat_template_length));
            for (std::size_t offset = 0;
                 offset + repeat_template_length <= repeat_end;
                 offset += repeat_template_length) {
                const auto template_id =
                    (offset / repeat_template_length) %
                    repeat_templates.size();
                sequence.replace(
                    offset, repeat_template_length,
                    repeat_templates[template_id]);
            }
        }
        if (scenario == "mixed") {
            for (std::size_t offset = 4096; offset + 32 < sequence.size(); offset += 16384)
                sequence.replace(offset, 32, 32, 'N');
        } else if (scenario == "n-islands") {
            for (std::size_t offset = 950; offset + 50 < sequence.size(); offset += 1000)
                sequence.replace(offset, 50, 50, 'N');
        }
        dataset.total_bases += sequence.size();
        dataset.reference.push_back({"ref" + std::to_string(id), "", std::move(sequence)});
    }
    dataset.queries.reserve(query_count);
    const auto repeat_query_count = std::min<std::size_t>(
        8, std::max<std::size_t>(1, (query_count + 999) / 1000));
    for (std::size_t id = 0; id < query_count; ++id) {
        const auto contig = static_cast<std::size_t>(next_random(state) % dataset.reference.size());
        const auto& source = dataset.reference[contig].sequence;
        const auto effective_query_length = std::min(query_length, source.size());
        if (effective_query_length == 0) continue;
        const bool repeat_query = scenario == "repeat-rich" &&
            id < repeat_query_count;
        std::size_t position = 0;
        do {
            if (repeat_query) {
                const auto repeat_extent = source.size() * 2 / 5;
                const auto repeat_slots = repeat_extent > effective_query_length
                    ? std::max<std::size_t>(1, (repeat_extent - effective_query_length) /
                        repeat_template_length)
                    : 1;
                position = static_cast<std::size_t>(
                    next_random(state) % repeat_slots) * repeat_template_length;
            } else if (scenario == "repeat-rich") {
                const auto random_begin = source.size() * 2 / 5;
                position = random_begin + static_cast<std::size_t>(next_random(state) %
                    (source.size() - random_begin - effective_query_length + 1));
            } else {
                position = static_cast<std::size_t>(next_random(state) %
                    (source.size() - effective_query_length + 1));
            }
        } while (source.substr(position, effective_query_length).find('N') != std::string::npos);
        auto query = source.substr(position, effective_query_length);
        if (!repeat_query && id % 3 != 0) {
            const auto mutation = std::min<std::size_t>(
                query.size() - 1, query.size() / 2 + id % 7);
            const auto original = query[mutation];
            query[mutation] = bases[(static_cast<std::size_t>(original) + id + 1) & 3U];
            if (query[mutation] == original) query[mutation] = original == 'A' ? 'C' : 'A';
        }
        dataset.query_bases += query.size();
        dataset.queries.push_back({"q" + std::to_string(id), "", std::move(query)});
    }
    std::uint64_t fingerprint = 1469598103934665603ULL;
    for (const auto& record : dataset.reference) fingerprint = hash_bytes(fingerprint, record.sequence);
    dataset.fingerprint = fingerprint;
    return dataset;
}

Dataset load_dataset(const Options& options) {
    Dataset dataset;
    dataset.name = "user-" + options.workload + "-reference";
    dataset.scenario = "user-reference";
    dataset.reference = ReadFastaRecords(*options.reference);
    if (dataset.reference.empty()) throw Error(ErrorCode::kInvalidInput, "reference FASTA contains no records");
    std::uint64_t state = options.seed;
    for (auto& record : dataset.reference) {
        for (auto& base : record.sequence) {
            base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
            if (base != 'A' && base != 'C' && base != 'G' && base != 'T') base = 'N';
        }
        dataset.total_bases += record.sequence.size();
    }
    if (options.query_file) {
        dataset.queries = ReadFastaRecords(*options.query_file);
    } else {
        const auto& profile = sufkit::benchmark::profile_definition(options.profile);
        const std::size_t desired = static_cast<std::size_t>(profile.query_count);
        const std::size_t length = 256;
        std::vector<std::size_t> eligible_contigs;
        for (std::size_t sequence_id = 0;
             sequence_id < dataset.reference.size(); ++sequence_id) {
            if (dataset.reference[sequence_id].sequence.size() >= length)
                eligible_contigs.push_back(sequence_id);
        }
        if (eligible_contigs.empty())
            throw Error(ErrorCode::kInvalidInput,
                "reference has no contig long enough for 256 bp generated queries");
        for (std::size_t id = 0; id < desired; ++id) {
            const auto contig = eligible_contigs[static_cast<std::size_t>(
                next_random(state) % eligible_contigs.size())];
            const auto& sequence = dataset.reference[contig].sequence;
            bool found = false;
            for (std::size_t attempt = 0; attempt < 100; ++attempt) {
                const auto position = static_cast<std::size_t>(next_random(state) % (sequence.size() - length + 1));
                auto query = sequence.substr(position, length);
                if (query.find('N') != std::string::npos) continue;
                if (id % 3 != 0) query[length / 2] = query[length / 2] == 'A' ? 'C' : 'A';
                dataset.queries.push_back({"q" + std::to_string(id), "", std::move(query)});
                found = true;
                break;
            }
            if (!found)
                throw Error(ErrorCode::kInvalidInput,
                    "cannot generate the profile-requested number of canonical 256 bp queries");
        }
    }
    if (dataset.queries.empty()) throw Error(ErrorCode::kInvalidInput, "no usable right-maximal exact match benchmark queries");
    for (const auto& query : dataset.queries) dataset.query_bases += query.sequence.size();
    std::uint64_t fingerprint = 1469598103934665603ULL;
    for (const auto& record : dataset.reference) fingerprint = hash_bytes(fingerprint, record.sequence);
    dataset.fingerprint = fingerprint;
    return dataset;
}

void write_fasta(const std::filesystem::path& path, const std::vector<SequenceRecord>& records) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw Error(ErrorCode::kIoError, "cannot create benchmark FASTA: " + path.string());
    for (const auto& record : records) output << '>' << record.name << '\n' << record.sequence << '\n';
}

double seconds_since(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

std::uint64_t checksum_seed() { return 1469598103934665603ULL; }
void mix(std::uint64_t& checksum, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        checksum ^= (value >> (byte * 8U)) & 0xffU;
        checksum *= 1099511628211ULL;
    }
}

void mix_match(std::uint64_t& checksum, const RightMaximalMatch& match) {
    mix(checksum, match.query_position);
    mix(checksum, match.sequence_id);
    mix(checksum, match.reference_position);
    mix(checksum, match.length);
    mix(checksum, static_cast<std::uint8_t>(match.strand));
}

void mix_match(std::uint64_t& checksum, const MemMatch& match) {
    mix(checksum, match.query_position);
    mix(checksum, match.sequence_id);
    mix(checksum, match.reference_position);
    mix(checksum, match.length);
    mix(checksum, static_cast<std::uint8_t>(match.strand));
}

void mix_match(std::uint64_t& checksum, const MamMatch& match) {
    mix(checksum, match.query_position);
    mix(checksum, match.sequence_id);
    mix(checksum, match.reference_position);
    mix(checksum, match.length);
    mix(checksum, static_cast<std::uint8_t>(match.strand));
}

void mix_match(std::uint64_t& checksum, const SmemMatch& match) {
    mix(checksum, match.query_position);
    mix(checksum, match.sequence_id);
    mix(checksum, match.reference_position);
    mix(checksum, match.length);
    mix(checksum, match.reference_occurrences);
    mix(checksum, static_cast<std::uint8_t>(match.strand));
}

void mix_match(std::uint64_t& checksum, const MumMatch& match) {
    mix(checksum, match.query_position);
    mix(checksum, match.sequence_id);
    mix(checksum, match.reference_position);
    mix(checksum, match.length);
    mix(checksum, static_cast<std::uint8_t>(match.strand));
}

template <class MatchType>
std::uint64_t unordered_match_hash(std::size_t query_id,
                                   const MatchType& match) {
    auto checksum = checksum_seed();
    mix(checksum, query_id);
    mix_match(checksum, match);
    return checksum;
}

bool right_maximal_match_less(
    const RightMaximalMatch& left,
    const RightMaximalMatch& right) {
    return std::tie(left.query_position, left.sequence_id,
                    left.reference_position, left.length, left.strand) <
           std::tie(right.query_position, right.sequence_id,
                    right.reference_position, right.length, right.strand);
}

std::string canonical_sequence(std::string sequence) {
    for (auto& base : sequence) {
        base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
        if (base != 'A' && base != 'C' && base != 'G' && base != 'T') base = 'N';
    }
    return sequence;
}

std::vector<RightMaximalMatch> naive_right_maximal_matches(
    const std::vector<SequenceRecord>& records,
    const std::string& raw_query,
    std::uint64_t min_length) {
    const auto query = canonical_sequence(raw_query);
    std::vector<RightMaximalMatch> result;
    for (std::size_t sequence_id = 0; sequence_id < records.size(); ++sequence_id) {
        const auto reference = canonical_sequence(records[sequence_id].sequence);
        for (std::size_t query_position = 0;
             query_position < query.size(); ++query_position) {
            if (query[query_position] == 'N') continue;
            for (std::size_t reference_position = 0;
                 reference_position < reference.size(); ++reference_position) {
                if (reference[reference_position] == 'N') continue;
                std::size_t length = 0;
                while (query_position + length < query.size() &&
                       reference_position + length < reference.size() &&
                       query[query_position + length] != 'N' &&
                       reference[reference_position + length] != 'N' &&
                       query[query_position + length] ==
                           reference[reference_position + length])
                    ++length;
                if (length < min_length) continue;
                const bool left_extendable = query_position != 0 &&
                    reference_position != 0 && query[query_position - 1] != 'N' &&
                    reference[reference_position - 1] != 'N' &&
                    query[query_position - 1] == reference[reference_position - 1];
                if (left_extendable) continue;
                result.push_back({
                    static_cast<SequenceId>(sequence_id), reference_position,
                    query_position, length, Strand::kForward});
            }
        }
    }
    std::sort(result.begin(), result.end(), right_maximal_match_less);
    return result;
}

std::uint64_t reference_occurrences(
    const std::vector<SequenceRecord>& records, std::string_view pattern) {
    std::uint64_t count = 0;
    for (const auto& record : records) {
        const auto reference = canonical_sequence(record.sequence);
        std::size_t position = reference.find(pattern);
        while (position != std::string::npos) {
            ++count;
            position = reference.find(pattern, position + 1);
        }
    }
    return count;
}

std::uint64_t query_occurrences(std::string_view query,
                                std::string_view pattern) {
    std::uint64_t count = 0;
    std::size_t position = query.find(pattern);
    while (position != std::string_view::npos) {
        ++count;
        position = query.find(pattern, position + 1);
    }
    return count;
}

std::vector<RightMaximalMatch> naive_matches(
    const std::vector<SequenceRecord>& records, const std::string& query,
    std::uint64_t min_length, const std::string& workload) {
    auto matches = naive_right_maximal_matches(records, query, min_length);
    if (workload != "mam" && workload != "mum") return matches;
    const auto canonical_query = canonical_sequence(query);
    matches.erase(std::remove_if(matches.begin(), matches.end(),
        [&](const auto& match) {
            const auto pattern = std::string_view(canonical_query).substr(
                static_cast<std::size_t>(match.query_position),
                static_cast<std::size_t>(match.length));
            return reference_occurrences(records, pattern) != 1 ||
                   (workload == "mum" &&
                    query_occurrences(canonical_query, pattern) != 1);
        }), matches.end());
    return matches;
}

std::vector<SmemMatch> naive_smem_matches(
    const std::vector<SequenceRecord>& records, const std::string& raw_query,
    std::uint64_t min_length, std::uint64_t min_occurrences) {
    struct Occurrence {
        SequenceId sequence_id;
        Position reference_position;
        std::uint64_t lce;
    };

    std::vector<std::string> references;
    references.reserve(records.size());
    for (const auto& record : records) {
        references.push_back(canonical_sequence(record.sequence));
    }
    const auto query = canonical_sequence(raw_query);
    std::vector<SmemMatch> result;
    std::uint64_t maximum_end = 0;
    for (std::size_t query_position = 0; query_position < query.size();
         ++query_position) {
        if (query[query_position] == 'N') {
            maximum_end = query_position + 1;
            continue;
        }
        std::vector<Occurrence> occurrences;
        for (std::size_t sequence_id = 0; sequence_id < references.size();
             ++sequence_id) {
            const auto& reference = references[sequence_id];
            for (std::size_t reference_position = 0;
                 reference_position < reference.size(); ++reference_position) {
                std::size_t length = 0;
                while (query_position + length < query.size() &&
                       reference_position + length < reference.size() &&
                       query[query_position + length] != 'N' &&
                       reference[reference_position + length] != 'N' &&
                       query[query_position + length] ==
                           reference[reference_position + length]) {
                    ++length;
                }
                if (length >= min_length) {
                    occurrences.push_back({
                        static_cast<SequenceId>(sequence_id),
                        reference_position, length});
                }
            }
        }
        if (occurrences.size() < min_occurrences) continue;
        std::vector<std::uint64_t> lengths;
        lengths.reserve(occurrences.size());
        for (const auto& occurrence : occurrences) {
            lengths.push_back(occurrence.lce);
        }
        const auto rank = static_cast<std::size_t>(min_occurrences - 1);
        std::nth_element(lengths.begin(), lengths.begin() + rank,
                         lengths.end(), std::greater<>());
        const auto length = lengths[rank];
        const auto end = query_position + length;
        if (end <= maximum_end) continue;
        maximum_end = end;
        const auto count = static_cast<std::uint64_t>(std::count_if(
            occurrences.begin(), occurrences.end(),
            [&](const auto& occurrence) { return occurrence.lce >= length; }));
        for (const auto& occurrence : occurrences) {
            if (occurrence.lce < length) continue;
            result.push_back({occurrence.sequence_id,
                              occurrence.reference_position, query_position,
                              length, count, Strand::kForward});
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& left,
                                                const auto& right) {
        return std::tie(left.query_position, left.sequence_id,
                        left.reference_position, left.length, left.strand) <
               std::tie(right.query_position, right.sequence_id,
                        right.reference_position, right.length, right.strand);
    });
    return result;
}

template <class Match>
RightMaximalMatch AsComparableMatch(const Match& match) {
    return {match.sequence_id, match.reference_position, match.query_position,
            match.length, match.strand};
}

std::vector<CorrectnessResult> run_naive_oracle(
    Dataset& dataset,
    const Options& options) {
    constexpr std::uint64_t kReferenceLimit = 4096;
    constexpr std::uint64_t kQueryLimit = 512;
    std::optional<std::tuple<std::size_t, std::size_t, std::size_t>> anchor;
    const auto query_search_limit = std::min<std::size_t>(8, dataset.queries.size());
    for (std::size_t query_id = 0; query_id < query_search_limit && !anchor; ++query_id) {
        const auto query = canonical_sequence(dataset.queries[query_id].sequence);
        if (query.empty() || query.find('N') != std::string::npos) continue;
        for (std::size_t sequence_id = 0;
             sequence_id < dataset.reference.size(); ++sequence_id) {
            const auto position = dataset.reference[sequence_id].sequence.find(query);
            if (position != std::string::npos) {
                anchor = std::make_tuple(query_id, sequence_id, position);
                break;
            }
        }
    }
    std::vector<SequenceRecord> reference_subset;
    std::uint64_t reference_bases = 0;
    if (anchor) {
        const auto sequence_id = std::get<1>(*anchor);
        const auto position = std::get<2>(*anchor);
        const auto& record = dataset.reference[sequence_id];
        const auto retained = static_cast<std::size_t>(std::min<std::uint64_t>(
            record.sequence.size(), kReferenceLimit));
        const auto start = std::min<std::size_t>(
            position > retained / 4 ? position - retained / 4 : 0,
            record.sequence.size() - retained);
        reference_subset.push_back({
            record.name, record.description, record.sequence.substr(start, retained)});
        reference_bases = retained;
    } else {
        for (const auto& record : dataset.reference) {
            if (reference_bases == kReferenceLimit) break;
            const auto retained = static_cast<std::size_t>(std::min<std::uint64_t>(
                record.sequence.size(), kReferenceLimit - reference_bases));
            if (retained == 0) continue;
            reference_subset.push_back({
                record.name, record.description, record.sequence.substr(0, retained)});
            reference_bases += retained;
        }
    }
    std::vector<SequenceRecord> query_subset;
    std::uint64_t query_bases = 0;
    const auto append_query = [&](const SequenceRecord& query) {
        if (query_bases == kQueryLimit) return;
        const auto retained = static_cast<std::size_t>(std::min<std::uint64_t>(
            query.sequence.size(), kQueryLimit - query_bases));
        if (retained == 0) return;
        query_subset.push_back({
            query.name, query.description, query.sequence.substr(0, retained)});
        query_bases += retained;
    };
    if (anchor) append_query(dataset.queries[std::get<0>(*anchor)]);
    for (std::size_t query_id = 0; query_id < dataset.queries.size(); ++query_id) {
        if (query_bases == kQueryLimit) break;
        if (anchor && query_id == std::get<0>(*anchor)) continue;
        append_query(dataset.queries[query_id]);
    }
    if (reference_subset.empty() || query_subset.empty())
        throw Error(ErrorCode::kBuildFailure,
            "cannot construct fixed naive right-maximal oracle subset");

    const auto reference = GenomeReference::FromRecords(reference_subset);
    SuffixArrayBuildOptions build_options;
    build_options.acceleration = SaAcceleration::kNone;
    const auto index = SuffixArray::Build(reference, build_options);
    std::vector<CorrectnessResult> rows;
    const auto thresholds = options.workload == "smem"
        ? options.min_occurrences : std::vector<std::uint64_t>{1};
    for (const auto min_length : options.min_lengths) {
        for (const auto min_occurrences : thresholds) {
            CorrectnessResult row;
            row.dataset = dataset.name;
            row.scenario = dataset.scenario;
            row.min_length = min_length;
            row.min_occurrences = min_occurrences;
            row.reference_bases = reference_bases;
            row.query_count = query_subset.size();
            row.query_bases = query_bases;
            row.oracle = options.workload == "mem" ? "naive-mem-forward" :
                         options.workload == "mam" ?
                             "naive-reference-mam-forward" :
                         options.workload == "smem" ?
                             "naive-generalized-smem-forward" :
                         options.workload == "mum" ? "naive-mum-forward" :
                             "naive-right-maximal-forward";
            RightMaximalOptions right_maximal_options;
            right_maximal_options.min_length = min_length;
            right_maximal_options.algorithm =
                RightMaximalSearchAlgorithm::kBaseline;
            std::uint64_t checksum = 0;
            for (std::size_t query_id = 0; query_id < query_subset.size();
                 ++query_id) {
                if (options.workload == "smem") {
                    const auto expected = naive_smem_matches(
                        reference_subset, query_subset[query_id].sequence,
                        min_length, min_occurrences);
                    SmemOptions smem_options;
                    smem_options.min_length = min_length;
                    smem_options.min_occurrences = min_occurrences;
                    smem_options.algorithm = MemSearchAlgorithm::kBaseline;
                    const auto observed = index.FindSmems(
                        query_subset[query_id].sequence, smem_options);
                    std::set<std::tuple<Position, std::uint64_t, Strand>>
                        expected_smems;
                    for (const auto& match : expected) {
                        expected_smems.emplace(match.query_position,
                                               match.length, match.strand);
                    }
                    const auto equal_match = [](const auto& left,
                                                const auto& right) {
                        return std::tie(
                                   left.query_position, left.sequence_id,
                                   left.reference_position, left.length,
                                   left.reference_occurrences, left.strand) ==
                               std::tie(
                                   right.query_position, right.sequence_id,
                                   right.reference_position, right.length,
                                   right.reference_occurrences, right.strand);
                    };
                    if (observed.total_smems != expected_smems.size() ||
                        observed.total_matches != expected.size() ||
                        observed.matches.size() != expected.size() ||
                        !std::equal(observed.matches.begin(),
                                    observed.matches.end(), expected.begin(),
                                    equal_match)) {
                        throw Error(ErrorCode::kBuildFailure,
                            "naive SMEM oracle mismatch for " + dataset.name +
                            " min_length=" + std::to_string(min_length) +
                            " min_occurrences=" +
                            std::to_string(min_occurrences) + " query=" +
                            std::to_string(query_id));
                    }
                    row.total_matches += expected.size();
                    row.total_smems += expected_smems.size();
                    for (const auto& match : expected) {
                        checksum += unordered_match_hash(query_id, match);
                    }
                    continue;
                }

                const auto expected = naive_matches(
                    reference_subset, query_subset[query_id].sequence,
                    min_length, options.workload);
                std::vector<RightMaximalMatch> observed_matches;
                std::uint64_t observed_total = 0;
                if (options.workload == "mem") {
                    MemOptions mem_options;
                    mem_options.min_length = min_length;
                    mem_options.algorithm = MemSearchAlgorithm::kBaseline;
                    const auto observed = index.FindMems(
                        query_subset[query_id].sequence, mem_options);
                    observed_total = observed.total_matches;
                    for (const auto& match : observed.matches)
                        observed_matches.push_back(AsComparableMatch(match));
                } else if (options.workload == "mam") {
                    MamOptions mam_options;
                    mam_options.min_length = min_length;
                    mam_options.algorithm = MemSearchAlgorithm::kBaseline;
                    const auto observed = index.FindMams(
                        query_subset[query_id].sequence, mam_options);
                    observed_total = observed.total_matches;
                    for (const auto& match : observed.matches)
                        observed_matches.push_back(AsComparableMatch(match));
                } else if (options.workload == "mum") {
                    MumOptions mum_options;
                    mum_options.min_length = min_length;
                    mum_options.algorithm = MemSearchAlgorithm::kBaseline;
                    const auto observed = index.FindMums(
                        query_subset[query_id].sequence, mum_options);
                    observed_total = observed.total_matches;
                    for (const auto& match : observed.matches)
                        observed_matches.push_back(AsComparableMatch(match));
                } else {
                    const auto observed = index.FindRightMaximalMatches(
                        query_subset[query_id].sequence,
                        right_maximal_options);
                    observed_total = observed.total_matches;
                    observed_matches = observed.matches;
                }
                if (observed_total != expected.size() ||
                    observed_matches.size() != expected.size() ||
                    !std::equal(observed_matches.begin(),
                        observed_matches.end(), expected.begin(),
                        [](const auto& left, const auto& right) {
                            return !right_maximal_match_less(left, right) &&
                                   !right_maximal_match_less(right, left);
                        })) {
                    throw Error(ErrorCode::kBuildFailure,
                        "naive maximal-match oracle mismatch for " +
                        dataset.name + " min_length=" +
                        std::to_string(min_length) + " query=" +
                        std::to_string(query_id));
                }
                row.total_matches += expected.size();
                for (const auto& match : expected)
                    checksum += unordered_match_hash(query_id, match);
            }
            row.checksum = checksum;
            rows.push_back(std::move(row));
        }
    }
    dataset.oracle_reference_bases = reference_bases;
    dataset.oracle_query_bases = query_bases;
    dataset.oracle_status = "passed";
    return rows;
}

void accumulate_statistics(
    RightMaximalSearchStatistics& aggregate,
    const RightMaximalSearchStatistics& value) {
    aggregate.lookup_calls += value.lookup_calls;
    aggregate.binary_lookup_calls += value.binary_lookup_calls;
    aggregate.learned_lookup_calls += value.learned_lookup_calls;
    aggregate.suffix_link_attempts += value.suffix_link_attempts;
    aggregate.suffix_link_successes += value.suffix_link_successes;
    aggregate.suffix_link_fallbacks += value.suffix_link_fallbacks;
    aggregate.previous_empty_lookups += value.previous_empty_lookups;
    aggregate.lookup.suffix_comparisons += value.lookup.suffix_comparisons;
    aggregate.lookup.character_comparisons += value.lookup.character_comparisons;
    aggregate.lookup.gallop_probes += value.lookup.gallop_probes;
    aggregate.lookup.local_window_rows += value.lookup.local_window_rows;
    aggregate.lookup.local_window_rows_max = std::max(
        aggregate.lookup.local_window_rows_max, value.lookup.local_window_rows_max);
    aggregate.lookup.predictions += value.lookup.predictions;
    aggregate.lookup.prediction_absolute_error_sum +=
        value.lookup.prediction_absolute_error_sum;
    aggregate.lookup.prediction_absolute_error_max = std::max(
        aggregate.lookup.prediction_absolute_error_max,
        value.lookup.prediction_absolute_error_max);
    aggregate.lookup.full_binary_fallbacks += value.lookup.full_binary_fallbacks;
}

SaAcceleration acceleration_for(const std::string& method) {
    if (method.find("baseline") != std::string::npos)
        return SaAcceleration::kNone;
    if (method == "mem-auto-fast" || method == "mam-auto-fast" ||
        method == "smem-auto-fast" || method == "mum-auto-fast")
        return SaAcceleration::kLcpSuffixLink;
    if (method == "mem-auto-low-memory" ||
        method == "mam-auto-low-memory" ||
        method == "smem-auto-low-memory" ||
        method == "mum-auto-low-memory")
        return SaAcceleration::kLcp;
    // Query-strategy ablations with "-fast-" must use the same retained
    // SA+ISA+raw-LCP index. Only the query algorithm and skip policy differ.
    if (method.find("mem-lcp-fast-") != std::string::npos)
        return SaAcceleration::kLcpSuffixLink;
    if ((method.size() >= 4 && method.substr(method.size() - 4) == "-lcp") ||
        method.find("-lcp-") != std::string::npos)
        return SaAcceleration::kLcp;
    if (method.find("child") != std::string::npos)
        return SaAcceleration::kLcpChild;
    if (method.find("suffix-link") != std::string::npos ||
        method.find("sampled-k") != std::string::npos)
        return SaAcceleration::kLcpSuffixLink;
    return SaAcceleration::kFull;
}

RightMaximalSearchAlgorithm algorithm_for(const std::string& method) {
    if (method.find("baseline") != std::string::npos)
        return RightMaximalSearchAlgorithm::kBaseline;
    if (method == "mem-auto-fast" || method == "mem-auto-low-memory" ||
        method == "mam-auto-fast" || method == "mam-auto-low-memory" ||
        method == "smem-auto-fast" || method == "smem-auto-low-memory" ||
        method == "mum-auto-fast" || method == "mum-auto-low-memory")
        return RightMaximalSearchAlgorithm::kAutoSelect;
    if ((method.size() >= 4 && method.substr(method.size() - 4) == "-lcp") ||
        method.find("-lcp-") != std::string::npos)
        return RightMaximalSearchAlgorithm::kLcp;
    if (method.find("child") != std::string::npos)
        return RightMaximalSearchAlgorithm::kChild;
    if (method.find("suffix-link") != std::string::npos ||
        method.find("sampled-k") != std::string::npos)
        return RightMaximalSearchAlgorithm::kSuffixLink;
    return RightMaximalSearchAlgorithm::kFull;
}

std::uint32_t mummer_style_skip(std::uint64_t min_length,
                                std::uint32_t sampling_rate) {
    const auto maximum = min_length / sampling_rate;
    const auto reserve = sampling_rate >= 4 ? std::uint64_t{10}
                                            : std::uint64_t{12};
    const auto available =
        min_length > reserve ? min_length - reserve : std::uint64_t{0};
    const auto skip = std::min(
        std::max<std::uint64_t>(available / sampling_rate, 1), maximum);
    if (skip == 0 || skip > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(ErrorCode::kInvalidInput,
                    "benchmark MEM skip is incompatible with min_length");
    }
    return static_cast<std::uint32_t>(skip);
}

void configure_mem_skip(const std::string& method, std::uint64_t min_length,
                        std::uint32_t sampling_rate, MemOptions& options) {
    if (method == "mem-lcp-fast-skip1" ||
        method == "mem-suffix-link-fast") {
        options.skip_multiplier = 1;
    } else if (method == "mem-lcp-fast-auto-skip" ||
               method == "mem-suffix-link-fast-auto-skip") {
        options.skip_multiplier =
            mummer_style_skip(min_length, sampling_rate);
    }
}

std::uint32_t sampling_rate_for(const std::string& method) {
    if (method == "right-maximal-sampled-k4") return 4;
    if (method == "right-maximal-sampled-k8") return 8;
    return 1;
}

SuffixArrayBuildOptions internal_build_options(
    const std::string& method, const Options& options,
    SuffixArrayBuildStatistics* statistics) {
    SuffixArrayBuildOptions value;
    value.acceleration = acceleration_for(method);
    value.sampling_rate = sampling_rate_for(method);
    value.statistics = statistics;
    value.resource_profile =
        method.find("low-memory") == std::string::npos
            ? SaResourceProfile::kFast
            : SaResourceProfile::kLowMemory;
    if (method.find("-suffix-link-sapling") != std::string::npos) {
        value.learned_index.enabled = true;
        value.learned_index.k = options.learned_k;
        value.learned_index.memory_overhead_basis_points =
            options.learned_memory_overhead_basis_points;
        value.learned_index.bucket_bits = options.learned_bucket_bits;
    }
    return value;
}

std::uint64_t serialized_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

std::uint64_t allocated_disk_size(const std::filesystem::path& path) {
    struct stat value {};
    if (stat(path.c_str(), &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.st_blocks) * 512ULL;
}

double median(std::vector<double> values);

struct OperationSpec {
    const char* name;
    bool streaming = false;
    std::optional<std::uint64_t> max_matches;
};

const std::array<OperationSpec, 4>& operation_specs() {
    static const std::array<OperationSpec, 4> result{{
        {"streaming", true, {}},
        {"vector", false, {}},
        {"max_matches=0", false, 0},
        {"max_matches=1000", false, 1000},
    }};
    return result;
}

struct OperationMeasurement {
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    std::uint64_t total_matches = 0;
    std::uint64_t total_smems = 0;
    std::uint64_t reported_matches = 0;
    std::uint64_t count_checksum = checksum_seed();
    std::uint64_t result_checksum = 0;
    RightMaximalSearchStatistics statistics;
};

struct ChildUsage {
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    double peak_rss_mb = 0;
};

double timeval_seconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) +
        static_cast<double>(value.tv_usec) / 1'000'000.0;
}

struct CpuUsage {
    double user_seconds = 0;
    double system_seconds = 0;
};

CpuUsage current_cpu_usage() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw Error(ErrorCode::kIoError,
            "cannot read right-maximal benchmark CPU usage");
    return {timeval_seconds(usage.ru_utime), timeval_seconds(usage.ru_stime)};
}

CpuUsage cpu_usage_delta(const CpuUsage& begin, const CpuUsage& end) {
    return {end.user_seconds - begin.user_seconds,
            end.system_seconds - begin.system_seconds};
}

ChildUsage run_clean_exec_worker(
    const std::filesystem::path& error_path,
    const std::vector<std::string>& worker_arguments) {
    const pid_t pid = fork();
    if (pid < 0)
        throw Error(ErrorCode::kIoError,
            "cannot fork isolated right-maximal benchmark phase worker");
    if (pid == 0) {
        const auto error_descriptor = open(
            error_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (error_descriptor < 0 ||
            dup2(error_descriptor, STDERR_FILENO) < 0) {
            _exit(126);
        }
        std::vector<std::string> arguments{
            "/proc/self/exe", "__maximal-benchmark-worker"};
        arguments.insert(
            arguments.end(), worker_arguments.begin(), worker_arguments.end());
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        execv(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    rusage usage{};
    if (wait4(pid, &status, 0, &usage) < 0)
        throw Error(ErrorCode::kIoError,
            "cannot wait for isolated right-maximal benchmark phase worker");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ifstream input(error_path);
        std::string message;
        std::getline(input, message);
        throw Error(ErrorCode::kBuildFailure,
            message.empty()
                ? "clean-exec maximal-match benchmark phase worker failed"
                : message);
    }
    return {
        timeval_seconds(usage.ru_utime),
        timeval_seconds(usage.ru_stime),
        static_cast<double>(usage.ru_maxrss) / 1024.0,
    };
}

template <class Value>
void write_phase_result(const std::filesystem::path& path, const Value& value) {
    static_assert(std::is_trivially_copyable<Value>::value,
                  "phase result must be trivially copyable");
    std::ofstream output(path, std::ios::binary);
    if (!output)
        throw Error(ErrorCode::kIoError,
            "cannot create isolated right-maximal phase result");
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output)
        throw Error(ErrorCode::kIoError,
            "cannot finish isolated right-maximal phase result");
}

template <class Value>
Value read_phase_result(const std::filesystem::path& path) {
    static_assert(std::is_trivially_copyable<Value>::value,
                  "phase result must be trivially copyable");
    std::ifstream input(path, std::ios::binary);
    Value value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input || input.peek() != std::char_traits<char>::eof())
        throw Error(ErrorCode::kBuildFailure,
            "invalid isolated right-maximal phase result");
    return value;
}

struct BuildPhaseResult {
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    double sa_seconds = 0;
    double storage_compaction_seconds = 0;
    double isa_seconds = 0;
    double lcp_seconds = 0;
    double child_seconds = 0;
    double learned_seconds = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    std::uint64_t sa_bytes = 0;
    std::uint64_t isa_bytes = 0;
    std::uint64_t lcp_bytes = 0;
    std::uint64_t resident_core_bytes = 0;
    std::uint8_t construction_coordinate_width = 0;
    std::uint8_t stored_coordinate_width = 0;
    std::uint8_t resource_profile = 0;
    std::uint8_t lcp_encoding = 0;
};

struct SavePhaseResult {
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t allocated_disk_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
};

struct LoadPhaseResult {
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t allocated_disk_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
};

struct QueryPhaseResult {
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    std::uint64_t total_matches = 0;
    std::uint64_t total_smems = 0;
    std::uint64_t reported_matches = 0;
    std::uint64_t count_checksum = 0;
    std::uint64_t result_checksum = 0;
    RightMaximalSearchStatistics statistics;
};

void append_phase_raw(
    std::vector<RawRow>& raw,
    const std::string& dataset,
    const std::string& method,
    const char* operation,
    std::uint32_t repetition,
    double seconds,
    const ChildUsage& usage,
    std::string peak_rss_scope,
    std::uint64_t query_bases = 0,
    std::uint64_t serialized_bytes = 0,
    std::uint64_t allocated_disk_bytes = 0,
    std::uint64_t auxiliary_bytes = 0,
    std::uint64_t learned_index_bytes = 0) {
    RawRow row;
    row.dataset = dataset;
    row.method = method;
    row.operation = operation;
    row.repetition = repetition;
    row.seconds = seconds;
    row.user_cpu_seconds = usage.user_cpu_seconds;
    row.system_cpu_seconds = usage.system_cpu_seconds;
    row.peak_rss_mb = usage.peak_rss_mb;
    row.peak_rss_scope = std::move(peak_rss_scope);
    row.query_bases = query_bases;
    row.serialized_bytes = serialized_bytes;
    row.allocated_disk_bytes = allocated_disk_bytes;
    row.auxiliary_bytes = auxiliary_bytes;
    row.learned_index_bytes = learned_index_bytes;
    raw.push_back(std::move(row));
}

OperationMeasurement measure_operation(
    const SuffixArray& index,
    const Dataset& dataset,
    const Options& benchmark_options,
    const std::string& method,
    std::uint64_t min_length,
    std::uint64_t min_occurrences,
    StrandMode strands,
    const OperationSpec& operation) {
    OperationMeasurement measured;
    const auto cpu_begin = current_cpu_usage();
    const auto begin = Clock::now();
    for (std::size_t query_id = 0; query_id < dataset.queries.size(); ++query_id) {
        std::uint64_t query_total = 0;
        std::set<std::tuple<Position, std::uint64_t, Strand>> smem_keys;
        const auto consume = [&](const auto& match) {
            ++query_total;
            ++measured.reported_matches;
            measured.result_checksum += unordered_match_hash(query_id, match);
            using MatchType = std::decay_t<decltype(match)>;
            if constexpr (std::is_same_v<MatchType, SmemMatch>) {
                smem_keys.emplace(match.query_position, match.length,
                                  match.strand);
            }
        };
        if (benchmark_options.workload == "mem") {
            MemOptions options;
            options.min_length = min_length;
            options.strands = strands;
            options.algorithm = static_cast<MemSearchAlgorithm>(
                static_cast<std::uint8_t>(algorithm_for(method)));
            configure_mem_skip(method, min_length, index.SamplingRate(),
                               options);
            if (method.find("-binary") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kBinary;
            else if (method.find("-sapling") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kSaplingPwl;
            if (operation.streaming) {
                index.ForEachMem(dataset.queries[query_id].sequence, options,
                                 consume);
            } else {
                const auto result = index.FindMems(
                    dataset.queries[query_id].sequence, options,
                    operation.max_matches);
                query_total = result.total_matches;
                measured.reported_matches += result.matches.size();
                for (const auto& match : result.matches) {
                    measured.result_checksum +=
                        unordered_match_hash(query_id, match);
                }
            }
        } else if (benchmark_options.workload == "mam") {
            MamOptions options;
            options.min_length = min_length;
            options.strands = strands;
            options.algorithm = static_cast<MemSearchAlgorithm>(
                static_cast<std::uint8_t>(algorithm_for(method)));
            if (method.find("-binary") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kBinary;
            else if (method.find("-sapling") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kSaplingPwl;
            if (operation.streaming) {
                index.ForEachMam(dataset.queries[query_id].sequence, options,
                                 consume);
            } else {
                const auto result = index.FindMams(
                    dataset.queries[query_id].sequence, options,
                    operation.max_matches);
                query_total = result.total_matches;
                measured.reported_matches += result.matches.size();
                for (const auto& match : result.matches) {
                    measured.result_checksum +=
                        unordered_match_hash(query_id, match);
                }
            }
        } else if (benchmark_options.workload == "smem") {
            SmemOptions options;
            options.min_length = min_length;
            options.min_occurrences = min_occurrences;
            options.strands = strands;
            options.algorithm = static_cast<MemSearchAlgorithm>(
                static_cast<std::uint8_t>(algorithm_for(method)));
            if (method.find("-binary") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kBinary;
            else if (method.find("-sapling") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kSaplingPwl;
            if (operation.streaming) {
                index.ForEachSmem(dataset.queries[query_id].sequence, options,
                                  consume);
                measured.total_smems += smem_keys.size();
            } else {
                const auto result = index.FindSmems(
                    dataset.queries[query_id].sequence, options,
                    operation.max_matches);
                query_total = result.total_matches;
                measured.total_smems += result.total_smems;
                measured.reported_matches += result.matches.size();
                for (const auto& match : result.matches) {
                    measured.result_checksum +=
                        unordered_match_hash(query_id, match);
                }
            }
        } else if (benchmark_options.workload == "mum") {
            MumOptions options;
            options.min_length = min_length;
            options.strands = strands;
            options.algorithm = static_cast<MemSearchAlgorithm>(
                static_cast<std::uint8_t>(algorithm_for(method)));
            if (method.find("-binary") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kBinary;
            else if (method.find("-sapling") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kSaplingPwl;
            if (operation.streaming) {
                index.ForEachMum(dataset.queries[query_id].sequence, options,
                                 consume);
            } else {
                const auto result = index.FindMums(
                    dataset.queries[query_id].sequence, options,
                    operation.max_matches);
                query_total = result.total_matches;
                measured.reported_matches += result.matches.size();
                for (const auto& match : result.matches) {
                    measured.result_checksum +=
                        unordered_match_hash(query_id, match);
                }
            }
        } else {
            RightMaximalOptions options;
            options.min_length = min_length;
            options.strands = strands;
            options.algorithm = algorithm_for(method);
            if (method.find("-binary") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kBinary;
            else if (method.find("-sapling") != std::string::npos)
                options.lookup_algorithm = SaSearchAlgorithm::kSaplingPwl;
            RightMaximalSearchStatistics search_statistics;
            options.statistics = &search_statistics;
            if (operation.streaming) {
                index.ForEachRightMaximalMatch(
                    dataset.queries[query_id].sequence, options, consume);
            } else {
                const auto result = index.FindRightMaximalMatches(
                    dataset.queries[query_id].sequence, options,
                    operation.max_matches);
                query_total = result.total_matches;
                measured.reported_matches += result.matches.size();
                for (const auto& match : result.matches) {
                    measured.result_checksum +=
                        unordered_match_hash(query_id, match);
                }
            }
            accumulate_statistics(measured.statistics, search_statistics);
        }
        mix(measured.count_checksum, query_id);
        mix(measured.count_checksum, query_total);
        measured.total_matches += query_total;
    }
    measured.seconds = seconds_since(begin);
    const auto cpu = cpu_usage_delta(cpu_begin, current_cpu_usage());
    measured.user_cpu_seconds = cpu.user_seconds;
    measured.system_cpu_seconds = cpu.system_seconds;
    return measured;
}

const std::string& require_worker_argument(
    const std::vector<std::string>& arguments, std::size_t& index,
    const char* label) {
    if (index >= arguments.size()) {
        throw Error(ErrorCode::kInvalidInput,
            std::string("missing maximal-match worker argument: ") + label);
    }
    return arguments[index++];
}

std::optional<std::uint32_t> parse_worker_optional_u32(
    const std::string& value, const char* label) {
    if (value == "NA") return std::nullopt;
    const auto parsed = parse_number(value, label);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(ErrorCode::kInvalidInput,
            std::string(label) + " exceeds uint32_t");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::uint32_t parse_worker_u32(
    const std::string& value, const char* label) {
    const auto parsed = parse_number(value, label);
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(ErrorCode::kInvalidInput,
            std::string(label) + " exceeds uint32_t");
    }
    return static_cast<std::uint32_t>(parsed);
}

OperationSpec worker_operation(const std::string& name) {
    for (const auto& operation : operation_specs()) {
        if (name == operation.name) return operation;
    }
    throw Error(ErrorCode::kInvalidInput,
        "invalid maximal-match worker operation: " + name);
}

int run_clean_exec_phase(const std::vector<std::string>& arguments) {
    std::size_t index = 0;
    const auto action = require_worker_argument(arguments, index, "action");
    if (action == "build" || action == "canonical") {
        const auto method = require_worker_argument(arguments, index, "method");
        const std::filesystem::path reference_path =
            require_worker_argument(arguments, index, "reference");
        const std::filesystem::path output_path =
            require_worker_argument(arguments, index, "output");
        Options options;
        options.learned_k = parse_worker_u32(
            require_worker_argument(arguments, index, "learned k"),
            "learned k");
        options.learned_memory_overhead_basis_points = parse_worker_u32(
            require_worker_argument(arguments, index, "learned memory"),
            "learned memory");
        options.learned_bucket_bits = parse_worker_optional_u32(
            require_worker_argument(arguments, index, "learned bucket bits"),
            "learned bucket bits");
        if (index != arguments.size()) {
            throw Error(ErrorCode::kInvalidInput,
                "too many maximal-match build worker arguments");
        }
        const auto reference = GenomeReference::FromFasta(reference_path);
        if (action == "canonical") {
            const auto built = SuffixArray::Build(
                reference, internal_build_options(method, options, nullptr));
            built.Save(output_path, {true});
            return 0;
        }
        SuffixArrayBuildStatistics statistics;
        const auto build_options =
            internal_build_options(method, options, &statistics);
        const auto cpu_begin = current_cpu_usage();
        const auto begin = Clock::now();
        const auto built = SuffixArray::Build(reference, build_options);
        BuildPhaseResult phase;
        phase.seconds = seconds_since(begin);
        const auto cpu = cpu_usage_delta(cpu_begin, current_cpu_usage());
        phase.user_cpu_seconds = cpu.user_seconds;
        phase.system_cpu_seconds = cpu.system_seconds;
        phase.sa_seconds = statistics.sa_seconds;
        phase.storage_compaction_seconds =
            statistics.storage_compaction_seconds;
        phase.isa_seconds = statistics.isa_seconds;
        phase.lcp_seconds = statistics.lcp_seconds;
        phase.child_seconds = statistics.child_seconds;
        phase.learned_seconds = statistics.learned_index_seconds;
        const auto info = built.GetInfo();
        phase.auxiliary_bytes = info.auxiliary_bytes;
        phase.learned_index_bytes = info.learned_index_bytes;
        phase.sa_bytes = info.sa_bytes;
        phase.isa_bytes = info.isa_bytes;
        phase.lcp_bytes = info.lcp_bytes;
        phase.resident_core_bytes = info.resident_core_bytes;
        phase.construction_coordinate_width = info.coordinate_width;
        phase.stored_coordinate_width = info.stored_coordinate_width;
        phase.resource_profile =
            static_cast<std::uint8_t>(info.sa_resource_profile);
        phase.lcp_encoding = static_cast<std::uint8_t>(info.lcp_encoding);
        write_phase_result(output_path, phase);
        return 0;
    }
    if (action == "save") {
        const std::filesystem::path canonical_path =
            require_worker_argument(arguments, index, "canonical index");
        const std::filesystem::path saved_path =
            require_worker_argument(arguments, index, "saved index");
        const std::filesystem::path result_path =
            require_worker_argument(arguments, index, "result");
        if (index != arguments.size()) {
            throw Error(ErrorCode::kInvalidInput,
                "too many maximal-match save worker arguments");
        }
        const auto loaded = SuffixArray::Load(canonical_path);
        const auto cpu_begin = current_cpu_usage();
        const auto begin = Clock::now();
        loaded.Save(saved_path, {true});
        SavePhaseResult phase;
        phase.seconds = seconds_since(begin);
        const auto cpu = cpu_usage_delta(cpu_begin, current_cpu_usage());
        phase.user_cpu_seconds = cpu.user_seconds;
        phase.system_cpu_seconds = cpu.system_seconds;
        phase.serialized_bytes = serialized_size(saved_path);
        phase.allocated_disk_bytes = allocated_disk_size(saved_path);
        phase.auxiliary_bytes = loaded.GetInfo().auxiliary_bytes;
        phase.learned_index_bytes = loaded.GetInfo().learned_index_bytes;
        write_phase_result(result_path, phase);
        return 0;
    }
    if (action == "load") {
        const std::filesystem::path canonical_path =
            require_worker_argument(arguments, index, "canonical index");
        const std::filesystem::path result_path =
            require_worker_argument(arguments, index, "result");
        if (index != arguments.size()) {
            throw Error(ErrorCode::kInvalidInput,
                "too many maximal-match load worker arguments");
        }
        const auto cpu_begin = current_cpu_usage();
        const auto begin = Clock::now();
        const auto loaded = SuffixArray::Load(canonical_path);
        LoadPhaseResult phase;
        phase.seconds = seconds_since(begin);
        const auto cpu = cpu_usage_delta(cpu_begin, current_cpu_usage());
        phase.user_cpu_seconds = cpu.user_seconds;
        phase.system_cpu_seconds = cpu.system_seconds;
        phase.serialized_bytes = serialized_size(canonical_path);
        phase.allocated_disk_bytes = allocated_disk_size(canonical_path);
        phase.auxiliary_bytes = loaded.GetInfo().auxiliary_bytes;
        phase.learned_index_bytes = loaded.GetInfo().learned_index_bytes;
        write_phase_result(result_path, phase);
        return 0;
    }
    if (action == "query") {
        const auto method = require_worker_argument(arguments, index, "method");
        const auto workload =
            require_worker_argument(arguments, index, "workload");
        const std::filesystem::path query_path =
            require_worker_argument(arguments, index, "queries");
        const std::filesystem::path canonical_path =
            require_worker_argument(arguments, index, "canonical index");
        const std::filesystem::path result_path =
            require_worker_argument(arguments, index, "result");
        const auto min_length = parse_number(
            require_worker_argument(arguments, index, "minimum length"),
            "minimum length");
        const auto min_occurrences = parse_number(
            require_worker_argument(arguments, index, "minimum occurrences"),
            "minimum occurrences");
        const auto operation = worker_operation(
            require_worker_argument(arguments, index, "operation"));
        const auto warmups = parse_worker_u32(
            require_worker_argument(arguments, index, "warmups"),
            "warmups");
        if (index != arguments.size()) {
            throw Error(ErrorCode::kInvalidInput,
                "too many maximal-match query worker arguments");
        }
        Dataset dataset;
        dataset.queries = ReadFastaRecords(query_path);
        for (const auto& query : dataset.queries) {
            dataset.query_bases += query.sequence.size();
        }
        Options options;
        options.workload = workload;
        const auto loaded = SuffixArray::Load(canonical_path);
        for (std::uint32_t warmup = 0; warmup < warmups; ++warmup) {
            (void)measure_operation(
                loaded, dataset, options, method, min_length,
                min_occurrences, StrandMode::kForward, operation);
        }
        const auto measured = measure_operation(
            loaded, dataset, options, method, min_length,
            min_occurrences, StrandMode::kForward, operation);
        QueryPhaseResult phase;
        phase.seconds = measured.seconds;
        phase.user_cpu_seconds = measured.user_cpu_seconds;
        phase.system_cpu_seconds = measured.system_cpu_seconds;
        phase.total_matches = measured.total_matches;
        phase.total_smems = measured.total_smems;
        phase.reported_matches = measured.reported_matches;
        phase.count_checksum = measured.count_checksum;
        phase.result_checksum = measured.result_checksum;
        phase.statistics = measured.statistics;
        write_phase_result(result_path, phase);
        return 0;
    }
    throw Error(ErrorCode::kInvalidInput,
        "unknown maximal-match benchmark worker action: " + action);
}

void benchmark_internal(const Dataset& dataset, const Options& options, const std::string& method,
    const std::filesystem::path& scratch, std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries, std::vector<RawRow>& raw) {
    const auto reference_path = scratch / (method + ".reference.fa");
    const auto query_path = scratch / (method + ".queries.fa");
    write_fasta(reference_path, dataset.reference);
    write_fasta(query_path, dataset.queries);
    BuildResult build;
    build.dataset = dataset.name;
    build.method = method;
    build.algorithm = ToString(algorithm_for(method));
    build.acceleration = ToString(acceleration_for(method));
    build.repetitions = options.build_repetitions;
    build.sampling_rate = sampling_rate_for(method);
    const auto learned_bucket_bits = options.learned_bucket_bits
        ? std::to_string(*options.learned_bucket_bits) : std::string("NA");
    const auto build_worker_arguments = [&](const char* action,
                                             const auto& output) {
        return std::vector<std::string>{
            action, method, reference_path.string(), output.string(),
            std::to_string(options.learned_k),
            std::to_string(options.learned_memory_overhead_basis_points),
            learned_bucket_bits};
    };
    std::vector<double> build_seconds;
    std::vector<double> sa_seconds;
    std::vector<double> storage_compaction_seconds;
    std::vector<double> isa_seconds;
    std::vector<double> lcp_seconds;
    std::vector<double> child_seconds;
    std::vector<double> learned_seconds;
    std::vector<double> save_seconds;
    std::vector<double> load_seconds;
    std::vector<double> build_peak_rss;
    std::vector<double> save_peak_rss;
    std::vector<double> load_peak_rss;

    for (std::uint32_t repetition = 0; repetition < options.build_repetitions; ++repetition) {
        const auto result_path = scratch /
            (method + ".build." + std::to_string(repetition) + ".bin");
        const auto error_path = scratch /
            (method + ".build." + std::to_string(repetition) + ".error");
        const auto usage = run_clean_exec_worker(
            error_path, build_worker_arguments("build", result_path));
        const auto phase = read_phase_result<BuildPhaseResult>(result_path);
        build_seconds.push_back(phase.seconds);
        sa_seconds.push_back(phase.sa_seconds);
        storage_compaction_seconds.push_back(
            phase.storage_compaction_seconds);
        isa_seconds.push_back(phase.isa_seconds);
        lcp_seconds.push_back(phase.lcp_seconds);
        child_seconds.push_back(phase.child_seconds);
        learned_seconds.push_back(phase.learned_seconds);
        build_peak_rss.push_back(usage.peak_rss_mb);
        build.auxiliary_bytes = phase.auxiliary_bytes;
        build.learned_index_bytes = phase.learned_index_bytes;
        build.sa_bytes = phase.sa_bytes;
        build.isa_bytes = phase.isa_bytes;
        build.lcp_bytes = phase.lcp_bytes;
        build.resident_core_bytes = phase.resident_core_bytes;
        build.construction_coordinate_width =
            phase.construction_coordinate_width;
        build.stored_coordinate_width = phase.stored_coordinate_width;
        build.resource_profile =
            static_cast<SaResourceProfile>(phase.resource_profile);
        build.lcp_encoding =
            static_cast<SaLcpEncoding>(phase.lcp_encoding);
        auto phase_usage = usage;
        phase_usage.user_cpu_seconds = phase.user_cpu_seconds;
        phase_usage.system_cpu_seconds = phase.system_cpu_seconds;
        append_phase_raw(raw, dataset.name, method, "build", repetition,
            phase.seconds, phase_usage, build.build_peak_rss_scope, 0, 0, 0,
            phase.auxiliary_bytes, phase.learned_index_bytes);
    }

    // Measured build workers intentionally do not serialize an index. Prepare
    // one canonical artifact in a separate unmeasured worker so subsequent
    // save, load, and query phases can each start in a fresh process.
    const auto canonical_path = scratch / (method + ".canonical.sufidx");
    const auto canonical_error = scratch / (method + ".canonical.error");
    (void)run_clean_exec_worker(
        canonical_error,
        build_worker_arguments("canonical", canonical_path));

    for (std::uint32_t repetition = 0; repetition < options.build_repetitions; ++repetition) {
        const auto saved_path = scratch /
            (method + ".save." + std::to_string(repetition) + ".sufidx");
        const auto result_path = scratch /
            (method + ".save." + std::to_string(repetition) + ".bin");
        const auto error_path = scratch /
            (method + ".save." + std::to_string(repetition) + ".error");
        const auto usage = run_clean_exec_worker(error_path, {
            "save", canonical_path.string(), saved_path.string(),
            result_path.string()});
        const auto phase = read_phase_result<SavePhaseResult>(result_path);
        save_seconds.push_back(phase.seconds);
        save_peak_rss.push_back(usage.peak_rss_mb);
        build.serialized_bytes = phase.serialized_bytes;
        build.allocated_disk_bytes = phase.allocated_disk_bytes;
        build.auxiliary_bytes = phase.auxiliary_bytes;
        build.learned_index_bytes = phase.learned_index_bytes;
        auto phase_usage = usage;
        phase_usage.user_cpu_seconds = phase.user_cpu_seconds;
        phase_usage.system_cpu_seconds = phase.system_cpu_seconds;
        append_phase_raw(raw, dataset.name, method, "save", repetition,
            phase.seconds, phase_usage, build.save_peak_rss_scope, 0,
            phase.serialized_bytes, phase.allocated_disk_bytes,
            phase.auxiliary_bytes, phase.learned_index_bytes);
    }

    const auto canonical_serialized_bytes = serialized_size(canonical_path);
    const auto canonical_allocated_bytes = allocated_disk_size(canonical_path);
    for (std::uint32_t repetition = 0; repetition < options.build_repetitions; ++repetition) {
        const auto result_path = scratch /
            (method + ".load." + std::to_string(repetition) + ".bin");
        const auto error_path = scratch /
            (method + ".load." + std::to_string(repetition) + ".error");
        const auto usage = run_clean_exec_worker(error_path, {
            "load", canonical_path.string(), result_path.string()});
        const auto phase = read_phase_result<LoadPhaseResult>(result_path);
        load_seconds.push_back(phase.seconds);
        load_peak_rss.push_back(usage.peak_rss_mb);
        auto phase_usage = usage;
        phase_usage.user_cpu_seconds = phase.user_cpu_seconds;
        phase_usage.system_cpu_seconds = phase.system_cpu_seconds;
        append_phase_raw(raw, dataset.name, method, "load", repetition,
            phase.seconds, phase_usage, build.load_peak_rss_scope, 0,
            phase.serialized_bytes, phase.allocated_disk_bytes,
            phase.auxiliary_bytes, phase.learned_index_bytes);
    }

    build.build_seconds = median(build_seconds);
    build.sa_build_seconds = median(sa_seconds);
    build.storage_compaction_seconds = median(storage_compaction_seconds);
    build.isa_build_seconds = median(isa_seconds);
    build.lcp_build_seconds = median(lcp_seconds);
    build.child_build_seconds = median(child_seconds);
    build.learned_index_build_seconds = median(learned_seconds);
    build.save_seconds = median(save_seconds);
    build.load_seconds = median(load_seconds);
    build.build_peak_rss_mb = median(build_peak_rss);
    build.save_peak_rss_mb = median(save_peak_rss);
    build.load_peak_rss_mb = median(load_peak_rss);
    builds.push_back(build);

    const auto occurrence_thresholds = options.workload == "smem"
        ? options.min_occurrences : std::vector<std::uint64_t>{1};
    for (const auto min_length : options.min_lengths) {
      for (const auto min_occurrences : occurrence_thresholds) {
        std::optional<std::uint64_t> streaming_total_matches;
        std::optional<std::uint64_t> streaming_total_smems;
        std::optional<std::uint64_t> streaming_count_checksum;
        for (const auto& operation : operation_specs()) {
            if (!operation.streaming && !operation.max_matches &&
                streaming_total_matches &&
                *streaming_total_matches > kVectorMaterializationMatchThreshold) {
                QueryResultRow summary;
                summary.dataset = dataset.name;
                summary.method = method;
                summary.algorithm = build.algorithm;
                summary.acceleration = build.acceleration;
                summary.operation = operation.name;
                summary.min_length = min_length;
                summary.min_occurrences = min_occurrences;
                summary.query_count = dataset.queries.size();
                summary.query_bases = dataset.query_bases;
                summary.total_matches = *streaming_total_matches;
                summary.total_smems = streaming_total_smems.value_or(0);
                summary.count_checksum = *streaming_count_checksum;
                summary.vector_skipped = true;
                summary.peak_rss_scope = "not_applicable";
                summary.status = "skipped_high_frequency";
                queries.push_back(summary);
                for (std::uint32_t repetition = 0;
                     repetition < options.query_repetitions; ++repetition) {
                    RawRow raw_row;
                    raw_row.dataset = dataset.name;
                    raw_row.method = method;
                    raw_row.operation = operation.name;
                    raw_row.min_length = min_length;
                    raw_row.min_occurrences = min_occurrences;
                    raw_row.repetition = repetition;
                    raw_row.peak_rss_scope = "not_applicable";
                    raw_row.query_bases = dataset.query_bases;
                    raw_row.serialized_bytes = canonical_serialized_bytes;
                    raw_row.allocated_disk_bytes = canonical_allocated_bytes;
                    raw_row.auxiliary_bytes = build.auxiliary_bytes;
                    raw_row.learned_index_bytes = build.learned_index_bytes;
                    raw_row.vector_skipped = true;
                    raw_row.total_matches = *streaming_total_matches;
                    raw_row.total_smems = streaming_total_smems.value_or(0);
                    raw_row.count_checksum = *streaming_count_checksum;
                    raw_row.status = "skipped_high_frequency";
                    raw.push_back(std::move(raw_row));
                }
                continue;
            }
            QueryResultRow summary;
            summary.dataset = dataset.name;
            summary.method = method;
            summary.algorithm = build.algorithm;
            summary.acceleration = build.acceleration;
            summary.operation = operation.name;
            summary.min_length = min_length;
            summary.min_occurrences = min_occurrences;
            summary.query_count = dataset.queries.size();
            summary.query_bases = dataset.query_bases;
            std::vector<double> query_peak_rss;
            for (std::uint32_t repetition = 0;
                 repetition < options.query_repetitions; ++repetition) {
                const auto result_path = scratch / (method + ".query." +
                    std::to_string(min_length) + "." + operation.name + "." +
                    std::to_string(repetition) + ".bin");
                const auto error_path = scratch / (method + ".query." +
                    std::to_string(min_length) + "." + operation.name + "." +
                    std::to_string(repetition) + ".error");
                const auto usage = run_clean_exec_worker(error_path, {
                    "query", method, options.workload, query_path.string(),
                    canonical_path.string(), result_path.string(),
                    std::to_string(min_length),
                    std::to_string(min_occurrences), operation.name,
                    std::to_string(options.warmups)});
                const auto measured = read_phase_result<QueryPhaseResult>(result_path);
                if (repetition != 0 &&
                    (summary.total_matches != measured.total_matches ||
                     summary.total_smems != measured.total_smems ||
                     summary.reported_matches != measured.reported_matches ||
                     summary.count_checksum != measured.count_checksum ||
                     summary.checksum != measured.result_checksum))
                    throw Error(ErrorCode::kBuildFailure,
                        "right-maximal exact match checksum changed between repetitions for " +
                        method + " operation=" + operation.name);
                summary.seconds.push_back(measured.seconds);
                summary.total_matches = measured.total_matches;
                summary.total_smems = measured.total_smems;
                summary.reported_matches = measured.reported_matches;
                summary.count_checksum = measured.count_checksum;
                summary.checksum = measured.result_checksum;
                summary.statistics = measured.statistics;
                query_peak_rss.push_back(usage.peak_rss_mb);
                RawRow raw_row;
                raw_row.dataset = dataset.name;
                raw_row.method = method;
                raw_row.operation = operation.name;
                raw_row.min_length = min_length;
                raw_row.min_occurrences = min_occurrences;
                raw_row.repetition = repetition;
                raw_row.seconds = measured.seconds;
                raw_row.user_cpu_seconds = measured.user_cpu_seconds;
                raw_row.system_cpu_seconds = measured.system_cpu_seconds;
                raw_row.peak_rss_mb = usage.peak_rss_mb;
                raw_row.peak_rss_scope = summary.peak_rss_scope;
                raw_row.query_bases = dataset.query_bases;
                raw_row.serialized_bytes = canonical_serialized_bytes;
                raw_row.allocated_disk_bytes = canonical_allocated_bytes;
                raw_row.auxiliary_bytes = build.auxiliary_bytes;
                raw_row.learned_index_bytes = build.learned_index_bytes;
                raw_row.total_matches = measured.total_matches;
                raw_row.total_smems = measured.total_smems;
                raw_row.reported_matches = measured.reported_matches;
                raw_row.count_checksum = measured.count_checksum;
                raw_row.checksum = measured.result_checksum;
                raw_row.statistics = measured.statistics;
                raw.push_back(std::move(raw_row));
            }
            summary.peak_rss_mb = query_peak_rss.empty() ? 0.0 :
                *std::max_element(query_peak_rss.begin(), query_peak_rss.end());
            if (operation.streaming) {
                streaming_total_matches = summary.total_matches;
                streaming_total_smems = summary.total_smems;
                streaming_count_checksum = summary.count_checksum;
            } else if (!streaming_total_matches ||
                       summary.total_matches != *streaming_total_matches ||
                       !streaming_total_smems ||
                       summary.total_smems != *streaming_total_smems ||
                       summary.count_checksum != *streaming_count_checksum) {
                throw Error(ErrorCode::kBuildFailure,
                    "right-maximal operation count differs from the required "
                    "streaming preflight for " + method + " operation=" +
                    operation.name);
            }
            queries.push_back(std::move(summary));
      }
        }
    }
}

template <class Value>
void write_worker_value(std::ostream& output, const Value& value) {
    static_assert(std::is_trivially_copyable<Value>::value, "worker value must be POD-like");
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <class Value>
Value read_worker_value(std::istream& input, const char* label) {
    static_assert(std::is_trivially_copyable<Value>::value, "worker value must be POD-like");
    Value value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) throw Error(ErrorCode::kBuildFailure,
                            std::string("truncated right-maximal exact match worker field: ") + label);
    return value;
}

void write_worker_string(std::ostream& output, const std::string& value) {
    write_worker_value(output, static_cast<std::uint64_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string read_worker_string(std::istream& input, const char* label) {
    const auto size = read_worker_value<std::uint64_t>(input, label);
    if (size > 1U << 20) throw Error(ErrorCode::kBuildFailure,
                                     std::string("invalid right-maximal exact match worker string: ") + label);
    std::string value(static_cast<std::size_t>(size), '\0');
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!input) throw Error(ErrorCode::kBuildFailure,
                            std::string("truncated right-maximal exact match worker string: ") + label);
    return value;
}

[[maybe_unused]] void write_internal_worker_file(
    const std::filesystem::path& path,
    const std::vector<BuildResult>& builds,
    const std::vector<QueryResultRow>& queries,
    const std::vector<RawRow>& raw) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw Error(ErrorCode::kIoError, "cannot create right-maximal exact match worker result");
    write_worker_value(output, static_cast<std::uint64_t>(builds.size()));
    for (const auto& row : builds) {
        write_worker_string(output, row.dataset);
        write_worker_string(output, row.method);
        write_worker_string(output, row.algorithm);
        write_worker_string(output, row.acceleration);
        write_worker_string(output, row.build_peak_rss_scope);
        write_worker_string(output, row.save_peak_rss_scope);
        write_worker_string(output, row.load_peak_rss_scope);
        write_worker_string(output, row.status);
        write_worker_value(output, row.build_seconds);
        write_worker_value(output, row.sa_build_seconds);
        write_worker_value(output, row.storage_compaction_seconds);
        write_worker_value(output, row.isa_build_seconds);
        write_worker_value(output, row.lcp_build_seconds);
        write_worker_value(output, row.child_build_seconds);
        write_worker_value(output, row.learned_index_build_seconds);
        write_worker_value(output, row.save_seconds);
        write_worker_value(output, row.load_seconds);
        write_worker_value(output, row.build_peak_rss_mb);
        write_worker_value(output, row.save_peak_rss_mb);
        write_worker_value(output, row.load_peak_rss_mb);
        write_worker_value(output, row.serialized_bytes);
        write_worker_value(output, row.allocated_disk_bytes);
        write_worker_value(output, row.auxiliary_bytes);
        write_worker_value(output, row.learned_index_bytes);
        write_worker_value(output, row.sa_bytes);
        write_worker_value(output, row.isa_bytes);
        write_worker_value(output, row.lcp_bytes);
        write_worker_value(output, row.resident_core_bytes);
        write_worker_value(output, row.construction_coordinate_width);
        write_worker_value(output, row.stored_coordinate_width);
        write_worker_value(output, row.resource_profile);
        write_worker_value(output, row.lcp_encoding);
        write_worker_value(output, row.repetitions);
        write_worker_value(output, row.sampling_rate);
    }
    write_worker_value(output, static_cast<std::uint64_t>(queries.size()));
    for (const auto& row : queries) {
        write_worker_string(output, row.dataset);
        write_worker_string(output, row.method);
        write_worker_string(output, row.algorithm);
        write_worker_string(output, row.acceleration);
        write_worker_string(output, row.operation);
        write_worker_string(output, row.peak_rss_scope);
        write_worker_string(output, row.status);
        write_worker_value(output, row.min_length);
        write_worker_value(output, row.min_occurrences);
        write_worker_value(output, row.query_count);
        write_worker_value(output, row.query_bases);
        write_worker_value(output, row.total_matches);
        write_worker_value(output, row.total_smems);
        write_worker_value(output, row.reported_matches);
        write_worker_value(output, row.count_checksum);
        write_worker_value(output, row.checksum);
        write_worker_value(output, row.peak_rss_mb);
        write_worker_value(output, row.materialization_match_threshold);
        write_worker_value(output, row.vector_skipped);
        write_worker_value(output, row.statistics);
        write_worker_value(output, static_cast<std::uint64_t>(row.seconds.size()));
        for (const auto seconds : row.seconds) write_worker_value(output, seconds);
    }
    write_worker_value(output, static_cast<std::uint64_t>(raw.size()));
    for (const auto& row : raw) {
        write_worker_string(output, row.dataset);
        write_worker_string(output, row.method);
        write_worker_string(output, row.operation);
        write_worker_string(output, row.peak_rss_scope);
        write_worker_string(output, row.status);
        write_worker_value(output, row.min_length);
        write_worker_value(output, row.min_occurrences);
        write_worker_value(output, row.repetition);
        write_worker_value(output, row.seconds);
        write_worker_value(output, row.user_cpu_seconds);
        write_worker_value(output, row.system_cpu_seconds);
        write_worker_value(output, row.peak_rss_mb);
        write_worker_value(output, row.query_bases);
        write_worker_value(output, row.serialized_bytes);
        write_worker_value(output, row.allocated_disk_bytes);
        write_worker_value(output, row.auxiliary_bytes);
        write_worker_value(output, row.learned_index_bytes);
        write_worker_value(output, row.materialization_match_threshold);
        write_worker_value(output, row.vector_skipped);
        write_worker_value(output, row.total_matches);
        write_worker_value(output, row.total_smems);
        write_worker_value(output, row.reported_matches);
        write_worker_value(output, row.count_checksum);
        write_worker_value(output, row.checksum);
        write_worker_value(output, row.statistics);
    }
    if (!output) throw Error(ErrorCode::kIoError, "cannot finish right-maximal exact match worker result");
}

[[maybe_unused]] void read_internal_worker_file(
    const std::filesystem::path& path,
    std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries,
    std::vector<RawRow>& raw) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw Error(ErrorCode::kBuildFailure, "right-maximal exact match worker result is missing");
    const auto build_count = read_worker_value<std::uint64_t>(input, "build count");
    for (std::uint64_t index = 0; index < build_count; ++index) {
        BuildResult row;
        row.dataset = read_worker_string(input, "build dataset");
        row.method = read_worker_string(input, "build method");
        row.algorithm = read_worker_string(input, "build algorithm");
        row.acceleration = read_worker_string(input, "build acceleration");
        row.build_peak_rss_scope = read_worker_string(input, "build RSS scope");
        row.save_peak_rss_scope = read_worker_string(input, "save RSS scope");
        row.load_peak_rss_scope = read_worker_string(input, "load RSS scope");
        row.status = read_worker_string(input, "build status");
        row.build_seconds = read_worker_value<double>(input, "build seconds");
        row.sa_build_seconds = read_worker_value<double>(input, "SA seconds");
        row.storage_compaction_seconds =
            read_worker_value<double>(input, "storage compaction seconds");
        row.isa_build_seconds = read_worker_value<double>(input, "ISA seconds");
        row.lcp_build_seconds = read_worker_value<double>(input, "LCP seconds");
        row.child_build_seconds = read_worker_value<double>(input, "CHILD seconds");
        row.learned_index_build_seconds = read_worker_value<double>(input, "model seconds");
        row.save_seconds = read_worker_value<double>(input, "save seconds");
        row.load_seconds = read_worker_value<double>(input, "load seconds");
        row.build_peak_rss_mb = read_worker_value<double>(input, "build peak RSS");
        row.save_peak_rss_mb = read_worker_value<double>(input, "save peak RSS");
        row.load_peak_rss_mb = read_worker_value<double>(input, "load peak RSS");
        row.serialized_bytes = read_worker_value<std::uint64_t>(input, "serialized bytes");
        row.allocated_disk_bytes = read_worker_value<std::uint64_t>(input, "allocated disk bytes");
        row.auxiliary_bytes = read_worker_value<std::uint64_t>(input, "auxiliary bytes");
        row.learned_index_bytes = read_worker_value<std::uint64_t>(input, "model bytes");
        row.sa_bytes = read_worker_value<std::uint64_t>(input, "SA bytes");
        row.isa_bytes = read_worker_value<std::uint64_t>(input, "ISA bytes");
        row.lcp_bytes = read_worker_value<std::uint64_t>(input, "LCP bytes");
        row.resident_core_bytes =
            read_worker_value<std::uint64_t>(input, "resident core bytes");
        row.construction_coordinate_width = read_worker_value<std::uint8_t>(
            input, "construction coordinate width");
        row.stored_coordinate_width = read_worker_value<std::uint8_t>(
            input, "stored coordinate width");
        row.resource_profile = read_worker_value<SaResourceProfile>(
            input, "SA resource profile");
        row.lcp_encoding =
            read_worker_value<SaLcpEncoding>(input, "LCP encoding");
        row.repetitions = read_worker_value<std::uint32_t>(input, "build repetitions");
        row.sampling_rate = read_worker_value<std::uint32_t>(input, "SA sampling rate");
        builds.push_back(std::move(row));
    }
    const auto query_count = read_worker_value<std::uint64_t>(input, "query count");
    for (std::uint64_t index = 0; index < query_count; ++index) {
        QueryResultRow row;
        row.dataset = read_worker_string(input, "query dataset");
        row.method = read_worker_string(input, "query method");
        row.algorithm = read_worker_string(input, "query algorithm");
        row.acceleration = read_worker_string(input, "query acceleration");
        row.operation = read_worker_string(input, "query operation");
        row.peak_rss_scope = read_worker_string(input, "query RSS scope");
        row.status = read_worker_string(input, "query status");
        row.min_length = read_worker_value<std::uint64_t>(input, "minimum length");
        row.min_occurrences = read_worker_value<std::uint64_t>(
            input, "minimum occurrences");
        row.query_count = read_worker_value<std::uint64_t>(input, "query cardinality");
        row.query_bases = read_worker_value<std::uint64_t>(input, "query bases");
        row.total_matches = read_worker_value<std::uint64_t>(input, "total matches");
        row.total_smems = read_worker_value<std::uint64_t>(input, "total SMEMs");
        row.reported_matches = read_worker_value<std::uint64_t>(input, "reported matches");
        row.count_checksum = read_worker_value<std::uint64_t>(input, "count checksum");
        row.checksum = read_worker_value<std::uint64_t>(input, "query checksum");
        row.peak_rss_mb = read_worker_value<double>(input, "query peak RSS");
        row.materialization_match_threshold = read_worker_value<std::uint64_t>(
            input, "query materialization threshold");
        row.vector_skipped = read_worker_value<bool>(input, "query vector skipped");
        row.statistics = read_worker_value<RightMaximalSearchStatistics>(input, "query statistics");
        const auto repetitions = read_worker_value<std::uint64_t>(input, "query repetitions");
        if (repetitions > 1000) throw Error(ErrorCode::kBuildFailure,
                                            "invalid right-maximal exact match worker repetition count");
        row.seconds.reserve(static_cast<std::size_t>(repetitions));
        for (std::uint64_t repetition = 0; repetition < repetitions; ++repetition)
            row.seconds.push_back(read_worker_value<double>(input, "query seconds"));
        queries.push_back(std::move(row));
    }
    const auto raw_count = read_worker_value<std::uint64_t>(input, "raw count");
    for (std::uint64_t index = 0; index < raw_count; ++index) {
        RawRow row;
        row.dataset = read_worker_string(input, "raw dataset");
        row.method = read_worker_string(input, "raw method");
        row.operation = read_worker_string(input, "raw operation");
        row.peak_rss_scope = read_worker_string(input, "raw RSS scope");
        row.status = read_worker_string(input, "raw status");
        row.min_length = read_worker_value<std::uint64_t>(input, "raw minimum length");
        row.min_occurrences = read_worker_value<std::uint64_t>(
            input, "raw minimum occurrences");
        row.repetition = read_worker_value<std::uint32_t>(input, "raw repetition");
        row.seconds = read_worker_value<double>(input, "raw seconds");
        row.user_cpu_seconds = read_worker_value<double>(input, "raw user CPU seconds");
        row.system_cpu_seconds = read_worker_value<double>(input, "raw system CPU seconds");
        row.peak_rss_mb = read_worker_value<double>(input, "raw peak RSS");
        row.query_bases = read_worker_value<std::uint64_t>(input, "raw query bases");
        row.serialized_bytes = read_worker_value<std::uint64_t>(input, "raw serialized bytes");
        row.allocated_disk_bytes = read_worker_value<std::uint64_t>(input, "raw allocated disk bytes");
        row.auxiliary_bytes = read_worker_value<std::uint64_t>(input, "raw auxiliary bytes");
        row.learned_index_bytes = read_worker_value<std::uint64_t>(input, "raw learned-index bytes");
        row.materialization_match_threshold = read_worker_value<std::uint64_t>(
            input, "raw materialization threshold");
        row.vector_skipped = read_worker_value<bool>(input, "raw vector skipped");
        row.total_matches = read_worker_value<std::uint64_t>(input, "raw matches");
        row.total_smems = read_worker_value<std::uint64_t>(
            input, "raw SMEMs");
        row.reported_matches = read_worker_value<std::uint64_t>(input, "raw reported matches");
        row.count_checksum = read_worker_value<std::uint64_t>(input, "raw count checksum");
        row.checksum = read_worker_value<std::uint64_t>(input, "raw checksum");
        row.statistics = read_worker_value<RightMaximalSearchStatistics>(input, "raw statistics");
        raw.push_back(std::move(row));
    }
    if (input.peek() != std::char_traits<char>::eof())
        throw Error(ErrorCode::kBuildFailure, "right-maximal exact match worker result has trailing bytes");
}

void benchmark_internal_clean_exec(
    const Dataset& dataset,
    const Options& options,
    const std::string& method,
    const std::filesystem::path& scratch,
    std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries,
    std::vector<RawRow>& raw) {
    // Every measured phase below uses exec(), so this orchestration layer no
    // longer needs a fork that would leak the controller's resident heap into
    // all phase RSS measurements.
    benchmark_internal(dataset, options, method, scratch, builds, queries, raw);
}

struct ProcessResult {
    int status = -1;
    double seconds = 0;
    double user_cpu_seconds = 0;
    double system_cpu_seconds = 0;
    double peak_rss_mb = 0;
};

ProcessResult run_process(const std::vector<std::string>& arguments,
    const std::filesystem::path& stdout_path, const std::filesystem::path& stderr_path) {
    const auto begin = Clock::now();
    const pid_t pid = fork();
    if (pid < 0) throw Error(ErrorCode::kIoError, "cannot fork MUMmer4 benchmark worker");
    if (pid == 0) {
        const int out = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int err = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out < 0 || err < 0 || dup2(out, STDOUT_FILENO) < 0 || dup2(err, STDERR_FILENO) < 0) _exit(126);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const auto& argument : arguments) argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        execv(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    rusage usage{};
    if (wait4(pid, &status, 0, &usage) < 0) throw Error(ErrorCode::kIoError, "cannot wait for MUMmer4 benchmark worker");
    ProcessResult result;
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    result.seconds = seconds_since(begin);
    result.user_cpu_seconds = timeval_seconds(usage.ru_utime);
    result.system_cpu_seconds = timeval_seconds(usage.ru_stime);
    result.peak_rss_mb = static_cast<double>(usage.ru_maxrss) / 1024.0;
    return result;
}

bool is_elf_binary(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    char magic[4]{};
    input.read(magic, sizeof(magic));
    return input.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
           static_cast<unsigned char>(magic[0]) == 0x7fU &&
           magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

std::string fingerprint_file(const std::filesystem::path& path,
    const std::filesystem::path& scratch, const std::string& stem,
    const std::string& label) {
    const auto hash_out = scratch / (stem + "-sha256.txt");
    const auto hash_err = scratch / (stem + "-sha256.err");
    const auto hash_result = run_process(
        {"/usr/bin/sha256sum", path.string()}, hash_out, hash_err);
    std::ifstream hash_input(hash_out);
    std::string hash;
    hash_input >> hash;
    if (hash_result.status != 0 || hash.size() != 64) {
        throw Error(ErrorCode::kIoError,
                    "cannot fingerprint the " + label);
    }
    return hash;
}

struct MiniBwaSmemOutput {
    std::uint64_t total_smems = 0;
    std::uint64_t total_matches = 0;
    std::uint64_t reported_matches = 0;
    std::uint64_t count_checksum = checksum_seed();
    std::uint64_t result_checksum = 0;
    bool complete_coordinates = true;
    bool high_frequency_truncation = false;
};

MiniBwaSmemOutput parse_minibwa_smem_output(
    const std::filesystem::path& path, std::size_t query_count) {
    std::ifstream input(path);
    if (!input)
        throw Error(ErrorCode::kIoError, "cannot read MiniBWA SMEM output");
    std::vector<std::uint64_t> per_query_matches(query_count, 0);
    MiniBwaSmemOutput result;
    std::optional<std::size_t> query_id;
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("SQ\t", 0) == 0) {
            std::istringstream row(line);
            std::string tag;
            std::string name;
            row >> tag >> name;
            if (name.size() < 2 || name.front() != 'q') {
                throw Error(ErrorCode::kBuildFailure,
                            "unexpected MiniBWA query name");
            }
            const auto parsed = static_cast<std::size_t>(
                parse_number(name.substr(1), "MiniBWA query id"));
            if (parsed >= query_count)
                throw Error(ErrorCode::kBuildFailure,
                            "MiniBWA query id is out of range");
            query_id = parsed;
            continue;
        }
        if (line.rfind("EM\t", 0) != 0) continue;
        if (!query_id)
            throw Error(ErrorCode::kBuildFailure,
                        "MiniBWA emitted an SMEM before a query header");
        std::istringstream row(line);
        std::string tag;
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
        std::uint64_t occurrences = 0;
        if (!(row >> tag >> begin >> end >> occurrences) || end < begin) {
            throw Error(ErrorCode::kBuildFailure,
                        "invalid MiniBWA SMEM row");
        }
        ++result.total_smems;
        result.total_matches += occurrences;
        per_query_matches[*query_id] += occurrences;
        auto interval_hash = checksum_seed();
        mix(interval_hash, *query_id);
        mix(interval_hash, begin);
        mix(interval_hash, end);
        mix(interval_hash, occurrences);
        std::string coordinate;
        std::uint64_t coordinates = 0;
        while (row >> coordinate) {
            if (coordinate == "*") {
                result.complete_coordinates = false;
                result.high_frequency_truncation = true;
                continue;
            }
            if (coordinate == ".") {
                result.complete_coordinates = false;
                continue;
            }
            ++coordinates;
            interval_hash = hash_bytes(interval_hash, coordinate);
        }
        if (coordinates != occurrences) {
            result.complete_coordinates = false;
        }
        result.reported_matches += coordinates;
        result.result_checksum += interval_hash;
    }
    for (std::size_t id = 0; id < per_query_matches.size(); ++id) {
        mix(result.count_checksum, id);
        mix(result.count_checksum, per_query_matches[id]);
    }
    return result;
}

std::string minibwa_external_status(const MiniBwaSmemOutput& output) {
    if (output.complete_coordinates) return "external_fmd_scope";
    return output.high_frequency_truncation
        ? "external_high_frequency"
        : "external_incomplete_coordinates";
}

void benchmark_minibwa(
    const Dataset& dataset, const Options& options,
    const std::filesystem::path& scratch, std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries, std::vector<RawRow>& raw) {
    constexpr std::uint64_t kCoordinateLimit = 1'000'000;
    const auto executable = options.minibwa->string();
    const auto reference_path = scratch / "minibwa-reference.fa";
    const auto query_path = scratch / "minibwa-queries.fa";
    write_fasta(reference_path, dataset.reference);
    auto numbered_queries = dataset.queries;
    for (std::size_t index = 0; index < numbered_queries.size(); ++index) {
        numbered_queries[index].name = "q" + std::to_string(index);
    }
    write_fasta(query_path, numbered_queries);

    BuildResult build;
    build.dataset = dataset.name;
    build.method = "minibwa";
    build.algorithm = "minibwa-fmd-smem";
    build.acceleration = "external-fmd-index";
    build.repetitions = options.build_repetitions;
    build.build_peak_rss_scope = "minibwa_index_process";
    build.save_peak_rss_scope = "included_in_external_index_build";
    build.load_peak_rss_scope = "included_in_external_fastmap_query";
    std::vector<double> build_times;
    std::vector<double> build_rss;
    std::filesystem::path prefix;
    for (std::uint32_t repetition = 0;
         repetition < options.build_repetitions; ++repetition) {
        prefix = scratch / ("minibwa-index-" + std::to_string(repetition));
        const auto output = scratch /
            ("minibwa-build-" + std::to_string(repetition) + ".out");
        const auto error = scratch /
            ("minibwa-build-" + std::to_string(repetition) + ".err");
        const auto measured = run_process(
            {executable, "index", "-t1", reference_path.string(),
             prefix.string()}, output, error);
        if (measured.status != 0)
            throw Error(ErrorCode::kBuildFailure,
                        "MiniBWA index construction failed");
        build_times.push_back(measured.seconds);
        build_rss.push_back(measured.peak_rss_mb);
        append_phase_raw(raw, dataset.name, "minibwa", "build", repetition,
                         measured.seconds,
                         {measured.user_cpu_seconds,
                          measured.system_cpu_seconds, measured.peak_rss_mb},
                         build.build_peak_rss_scope);
    }
    build.build_seconds = median(build_times);
    build.build_peak_rss_mb = median(build_rss);
    for (const auto& entry : std::filesystem::directory_iterator(scratch)) {
        if (entry.path().filename().string().find(
                prefix.filename().string() + ".") != 0) {
            continue;
        }
        build.serialized_bytes += serialized_size(entry.path());
        build.allocated_disk_bytes += allocated_disk_size(entry.path());
    }
    builds.push_back(build);

    for (const auto min_length : options.min_lengths) {
        for (const auto min_occurrences : options.min_occurrences) {
            QueryResultRow summary;
            summary.dataset = dataset.name;
            summary.method = "minibwa";
            summary.algorithm = build.algorithm;
            summary.acceleration = build.acceleration;
            summary.operation = "external-load+query";
            summary.min_length = min_length;
            summary.min_occurrences = min_occurrences;
            summary.query_count = dataset.queries.size();
            summary.query_bases = dataset.query_bases;
            summary.peak_rss_scope = "minibwa_fastmap_process_load_plus_query";
            summary.status = "external_fmd_scope";
            for (std::uint32_t warmup = 0; warmup < options.warmups; ++warmup) {
                const auto output = scratch / ("minibwa-warmup-" +
                    std::to_string(min_length) + "-" +
                    std::to_string(min_occurrences) + "-" +
                    std::to_string(warmup) + ".out");
                const auto error = output.string() + ".err";
                const auto process = run_process(
                    {executable, "fastmap", "-l",
                     std::to_string(min_length), "-s",
                     std::to_string(min_occurrences), "-w",
                     std::to_string(kCoordinateLimit), prefix.string(),
                     query_path.string()}, output, error);
                if (process.status != 0)
                    throw Error(ErrorCode::kBuildFailure,
                                "MiniBWA SMEM warm-up failed");
            }
            std::vector<double> query_rss;
            for (std::uint32_t repetition = 0;
                 repetition < options.query_repetitions; ++repetition) {
                const auto output = scratch / ("minibwa-query-" +
                    std::to_string(min_length) + "-" +
                    std::to_string(min_occurrences) + "-" +
                    std::to_string(repetition) + ".out");
                const auto error = output.string() + ".err";
                const auto process = run_process(
                    {executable, "fastmap", "-l",
                     std::to_string(min_length), "-s",
                     std::to_string(min_occurrences), "-w",
                     std::to_string(kCoordinateLimit), prefix.string(),
                     query_path.string()}, output, error);
                if (process.status != 0)
                    throw Error(ErrorCode::kBuildFailure,
                                "MiniBWA SMEM query failed");
                const auto parsed = parse_minibwa_smem_output(
                    output, dataset.queries.size());
                if (repetition != 0 &&
                    (summary.total_smems != parsed.total_smems ||
                     summary.total_matches != parsed.total_matches ||
                     summary.reported_matches != parsed.reported_matches ||
                     summary.count_checksum != parsed.count_checksum ||
                     summary.checksum != parsed.result_checksum ||
                     summary.status != minibwa_external_status(parsed))) {
                    throw Error(ErrorCode::kBuildFailure,
                                "MiniBWA SMEM checksum changed between repetitions");
                }
                summary.seconds.push_back(process.seconds);
                summary.total_smems = parsed.total_smems;
                summary.total_matches = parsed.total_matches;
                summary.reported_matches = parsed.reported_matches;
                summary.count_checksum = parsed.count_checksum;
                summary.checksum = parsed.result_checksum;
                summary.status = minibwa_external_status(parsed);
                query_rss.push_back(process.peak_rss_mb);
                RawRow raw_row;
                raw_row.dataset = dataset.name;
                raw_row.method = "minibwa";
                raw_row.operation = summary.operation;
                raw_row.min_length = min_length;
                raw_row.min_occurrences = min_occurrences;
                raw_row.repetition = repetition;
                raw_row.seconds = process.seconds;
                raw_row.user_cpu_seconds = process.user_cpu_seconds;
                raw_row.system_cpu_seconds = process.system_cpu_seconds;
                raw_row.peak_rss_mb = process.peak_rss_mb;
                raw_row.peak_rss_scope = summary.peak_rss_scope;
                raw_row.query_bases = dataset.query_bases;
                raw_row.serialized_bytes = build.serialized_bytes;
                raw_row.allocated_disk_bytes = build.allocated_disk_bytes;
                raw_row.total_smems = parsed.total_smems;
                raw_row.total_matches = parsed.total_matches;
                raw_row.reported_matches = parsed.reported_matches;
                raw_row.count_checksum = parsed.count_checksum;
                raw_row.checksum = parsed.result_checksum;
                // Keep every external row visibly outside the internal
                // forward-only parity group while retaining its raw timing.
                raw_row.status = minibwa_external_status(parsed);
                raw.push_back(std::move(raw_row));
            }
            summary.peak_rss_mb = query_rss.empty() ? 0.0 :
                *std::max_element(query_rss.begin(), query_rss.end());
            queries.push_back(std::move(summary));
        }
    }
}

void parse_mummer_output(const std::filesystem::path& path,
    const std::map<std::string, SequenceId>& reference_ids,
    const std::vector<SequenceRecord>& queries,
    Strand strand,
    std::vector<std::vector<RightMaximalMatch>>& per_query) {
    std::ifstream input(path);
    if (!input) throw Error(ErrorCode::kIoError, "cannot read MUMmer4 output");
    std::string line;
    std::size_t query_id = 0;
    bool have_query = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() == '>') {
            std::istringstream header(line.substr(1));
            std::string name;
            header >> name;
            if (name.size() < 2 || name.front() != 'q') throw Error(ErrorCode::kBuildFailure, "unexpected MUMmer4 query header");
            query_id = static_cast<std::size_t>(parse_number(name.substr(1), "MUMmer query id"));
            if (query_id >= per_query.size()) throw Error(ErrorCode::kBuildFailure, "MUMmer4 query id is out of range");
            have_query = true;
            continue;
        }
        if (!have_query || line.empty()) continue;
        std::istringstream row(line);
        std::string reference_name;
        std::uint64_t reference_position = 0, query_position = 0, length = 0;
        if (!(row >> reference_name >> reference_position >> query_position >> length)) continue;
        const auto id = reference_ids.find(reference_name);
        if (id == reference_ids.end() || reference_position == 0 || query_position == 0)
            throw Error(ErrorCode::kBuildFailure, "invalid MUMmer4 match row");
        const auto zero_query = query_position - 1;
        if (strand == Strand::kReverseComplement && zero_query + length > queries[query_id].sequence.size())
            throw Error(ErrorCode::kBuildFailure, "MUMmer4 reverse query coordinate is out of range");
        const auto normalized_query = strand == Strand::kReverseComplement
            ? queries[query_id].sequence.size() - (zero_query + length) : zero_query;
        RightMaximalMatch match{id->second, reference_position - 1, normalized_query, length, strand};
        per_query[query_id].push_back(match);
    }
}

void benchmark_mummer(const Dataset& dataset, const Options& options,
    const std::filesystem::path& scratch, std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries, std::vector<RawRow>& raw) {
    const auto executable = options.mummer4->string();
    const auto reference_path = scratch / "reference.fa";
    const auto query_path = scratch / "queries.fa";
    const auto nohit_path = scratch / "nohit.fa";
    write_fasta(reference_path, dataset.reference);
    auto numbered_queries = dataset.queries;
    for (std::size_t index = 0; index < numbered_queries.size(); ++index)
        numbered_queries[index].name = "q" + std::to_string(index);
    write_fasta(query_path, numbered_queries);
    write_fasta(nohit_path, {{"nohit", "", std::string(1024, 'N')}});
    std::map<std::string, SequenceId> reference_ids;
    for (std::size_t id = 0; id < dataset.reference.size(); ++id)
        reference_ids.emplace(dataset.reference[id].name, static_cast<SequenceId>(id));
    const auto min_length = options.min_lengths.front();
    const auto mummer_mode = options.workload == "mam" ? "-mumreference" :
                             options.workload == "mum" ? "-mum" :
                                                         "-maxmatch";
    std::vector<std::string> common{executable, mummer_mode,
        "-n", "-F", "-k", "1"};
    if (options.workload != "mam" && options.workload != "mum") {
        common.insert(common.end(), {"-skip", "1"});
    }
    common.insert(common.end(), {"-suflink", "1", "-child", "0",
                                 "-kmer", "0", "-threads", "1",
                                 "-qthreads", "1"});
    BuildResult build;
    build.dataset = dataset.name;
    build.method = "mummer4";
    build.algorithm = "mummer4-suffix-link";
    build.acceleration = "K=1,auto-suffix-link";
    build.build_peak_rss_scope = "mummer4_build_process_including_save";
    build.save_peak_rss_scope = "not_measured_separately";
    build.load_peak_rss_scope = "not_measured_separately";
    build.repetitions = options.build_repetitions;
    std::vector<double> build_times;
    std::vector<double> build_peak_rss;
    auto prefix = scratch / "mummer-index-0";
    for (std::uint32_t repetition = 0; repetition < options.build_repetitions; ++repetition) {
        prefix = scratch / ("mummer-index-" + std::to_string(repetition));
        const auto build_stdout = scratch /
            ("mummer-build-" + std::to_string(repetition) + ".stdout");
        const auto build_stderr = scratch /
            ("mummer-build-" + std::to_string(repetition) + ".stderr");
        auto build_args = common;
        build_args.insert(build_args.end(), {"-l", std::to_string(min_length), "-save", prefix.string(),
                                             reference_path.string(), nohit_path.string()});
        const auto process = run_process(build_args, build_stdout, build_stderr);
        if (process.status != 0)
            throw Error(ErrorCode::kBuildFailure, "MUMmer4 index construction failed");
        build_times.push_back(process.seconds);
        build_peak_rss.push_back(process.peak_rss_mb);
        std::uint64_t phase_serialized_bytes = 0;
        std::uint64_t phase_allocated_bytes = 0;
        for (const auto& entry : std::filesystem::directory_iterator(scratch)) {
            if (entry.path().filename().string().find(prefix.filename().string() + ".") != 0)
                continue;
            phase_serialized_bytes += serialized_size(entry.path());
            phase_allocated_bytes += allocated_disk_size(entry.path());
        }
        const ChildUsage usage{
            process.user_cpu_seconds, process.system_cpu_seconds, process.peak_rss_mb};
        append_phase_raw(raw, dataset.name, "mummer4", "build", repetition,
            process.seconds, usage, build.build_peak_rss_scope, 0,
            phase_serialized_bytes, phase_allocated_bytes);
    }
    build.build_seconds = median(build_times);
    build.build_peak_rss_mb = median(build_peak_rss);
    for (const auto& entry : std::filesystem::directory_iterator(scratch)) {
        if (entry.path().filename().string().find(prefix.filename().string() + ".") == 0) {
            build.serialized_bytes += serialized_size(entry.path());
            build.allocated_disk_bytes += allocated_disk_size(entry.path());
        }
    }
    builds.push_back(build);

    SuffixArrayBuildOptions verification_build_options;
    verification_build_options.acceleration = SaAcceleration::kNone;
    const auto verification_reference = GenomeReference::FromRecords(dataset.reference);
    const auto verification_index = SuffixArray::Build(verification_reference, verification_build_options);

    for (const auto length : options.min_lengths) {
        QueryResultRow summary;
        summary.dataset = dataset.name;
        summary.method = "mummer4";
        summary.algorithm = build.algorithm;
        summary.acceleration = build.acceleration;
        summary.operation = "vector";
        summary.min_length = length;
        summary.query_count = dataset.queries.size();
        summary.query_bases = dataset.query_bases;
        summary.peak_rss_scope = "mummer4_process_load_plus_query";
        const auto forward_preflight = measure_operation(
            verification_index, dataset, options,
            options.workload + "-baseline", length, 1, StrandMode::kForward,
            operation_specs().front());
        const auto reverse_preflight = measure_operation(
            verification_index, dataset, options,
            options.workload + "-baseline", length, 1,
            StrandMode::kReverseComplement, operation_specs().front());
        if (forward_preflight.total_matches >
                kVectorMaterializationMatchThreshold ||
            reverse_preflight.total_matches >
                kVectorMaterializationMatchThreshold) {
            summary.total_matches = forward_preflight.total_matches;
            summary.count_checksum = forward_preflight.count_checksum;
            summary.vector_skipped = true;
            summary.peak_rss_scope = "not_applicable";
            summary.status = "skipped_high_frequency";
            queries.push_back(summary);
            for (std::uint32_t repetition = 0;
                 repetition < options.query_repetitions; ++repetition) {
                RawRow raw_row;
                raw_row.dataset = dataset.name;
                raw_row.method = "mummer4";
                raw_row.operation = "vector";
                raw_row.min_length = length;
                raw_row.repetition = repetition;
                raw_row.peak_rss_scope = "not_applicable";
                raw_row.query_bases = dataset.query_bases;
                raw_row.serialized_bytes = build.serialized_bytes;
                raw_row.allocated_disk_bytes = build.allocated_disk_bytes;
                raw_row.vector_skipped = true;
                raw_row.total_matches = forward_preflight.total_matches;
                raw_row.count_checksum = forward_preflight.count_checksum;
                raw_row.status = "skipped_high_frequency";
                raw.push_back(std::move(raw_row));
            }
            continue;
        }
        std::vector<double> query_peak_rss;
        for (std::uint32_t warmup = 0; warmup < options.warmups; ++warmup) {
            const auto output = scratch /
                ("mummer-warmup-" + std::to_string(length) + "-" + std::to_string(warmup) + ".out");
            const auto error = scratch /
                ("mummer-warmup-" + std::to_string(length) + "-" + std::to_string(warmup) + ".err");
            auto args = common;
            args.insert(args.end(), {"-l", std::to_string(length), "-load", prefix.string(),
                                     reference_path.string(), query_path.string()});
            if (run_process(args, output, error).status != 0)
                throw Error(ErrorCode::kBuildFailure, "MUMmer4 query warm-up failed");
        }
        for (std::uint32_t repetition = 0;
             repetition < options.query_repetitions; ++repetition) {
            const auto output = scratch / ("mummer-" + std::to_string(length) + "-" + std::to_string(repetition) + ".out");
            const auto error = scratch / ("mummer-" + std::to_string(length) + "-" + std::to_string(repetition) + ".err");
            auto args = common;
            args.insert(args.end(), {"-l", std::to_string(length), "-load", prefix.string(),
                                     reference_path.string(), query_path.string()});
            const auto measured = run_process(args, output, error);
            if (measured.status != 0) throw Error(ErrorCode::kBuildFailure, "MUMmer4 query failed");
            std::vector<std::vector<RightMaximalMatch>> per_query(dataset.queries.size());
            (void)parse_mummer_output(output, reference_ids, dataset.queries,
                                      Strand::kForward, per_query);
            std::uint64_t count_checksum = checksum_seed();
            std::uint64_t checksum = 0;
            std::uint64_t total = 0;
            std::uint64_t reported = 0;
            for (std::size_t query_id = 0; query_id < per_query.size(); ++query_id) {
                auto& matches = per_query[query_id];
                std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
                    return std::tie(left.query_position, left.sequence_id, left.reference_position, left.length, left.strand) <
                           std::tie(right.query_position, right.sequence_id, right.reference_position, right.length, right.strand);
                });
                mix(count_checksum, query_id);
                mix(count_checksum, matches.size());
                total += matches.size();
                reported += matches.size();
                for (const auto& match : matches)
                    checksum += unordered_match_hash(query_id, match);
            }
            if (repetition != 0 &&
                (summary.total_matches != total || summary.reported_matches != reported ||
                 summary.count_checksum != count_checksum || summary.checksum != checksum))
                throw Error(ErrorCode::kBuildFailure,
                    "MUMmer4 right-maximal exact match checksum changed between repetitions");
            summary.seconds.push_back(measured.seconds);
            summary.total_matches = total;
            summary.reported_matches = reported;
            summary.count_checksum = count_checksum;
            summary.checksum = checksum;
            query_peak_rss.push_back(measured.peak_rss_mb);
            RawRow raw_row;
            raw_row.dataset = dataset.name;
            raw_row.method = "mummer4";
            raw_row.operation = "vector";
            raw_row.min_length = length;
            raw_row.repetition = repetition;
            raw_row.seconds = measured.seconds;
            raw_row.user_cpu_seconds = measured.user_cpu_seconds;
            raw_row.system_cpu_seconds = measured.system_cpu_seconds;
            raw_row.peak_rss_mb = measured.peak_rss_mb;
            raw_row.peak_rss_scope = summary.peak_rss_scope;
            raw_row.query_bases = dataset.query_bases;
            raw_row.serialized_bytes = build.serialized_bytes;
            raw_row.allocated_disk_bytes = build.allocated_disk_bytes;
            raw_row.total_matches = total;
            raw_row.reported_matches = reported;
            raw_row.count_checksum = count_checksum;
            raw_row.checksum = checksum;
            raw.push_back(std::move(raw_row));
        }
        summary.peak_rss_mb = query_peak_rss.empty() ? 0.0 :
            *std::max_element(query_peak_rss.begin(), query_peak_rss.end());
        const auto reverse_output = scratch / ("mummer-reverse-" + std::to_string(length) + ".out");
        const auto reverse_error = scratch / ("mummer-reverse-" + std::to_string(length) + ".err");
        auto reverse_args = common;
        reverse_args.insert(reverse_args.end(), {"-r", "-l", std::to_string(length), "-load", prefix.string(),
                                                 reference_path.string(), query_path.string()});
        const auto reverse_process = run_process(reverse_args, reverse_output, reverse_error);
        if (reverse_process.status != 0) throw Error(ErrorCode::kBuildFailure, "MUMmer4 reverse query failed");
        std::vector<std::vector<RightMaximalMatch>> reverse_matches(dataset.queries.size());
        (void)parse_mummer_output(reverse_output, reference_ids, dataset.queries,
                                  Strand::kReverseComplement, reverse_matches);
        std::uint64_t mummer_checksum = checksum_seed();
        std::uint64_t internal_checksum = checksum_seed();
        std::uint64_t mummer_total = 0;
        std::uint64_t internal_total = 0;
        for (std::size_t query_id = 0; query_id < dataset.queries.size(); ++query_id) {
            auto& matches = reverse_matches[query_id];
            std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
                return std::tie(left.query_position, left.sequence_id, left.reference_position, left.length, left.strand) <
                       std::tie(right.query_position, right.sequence_id, right.reference_position, right.length, right.strand);
            });
            std::vector<RightMaximalMatch> internal_matches;
            std::uint64_t internal_count = 0;
            if (options.workload == "mem") {
                MemOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm = MemSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindMems(
                    dataset.queries[query_id].sequence, internal_options);
                internal_count = internal.total_matches;
                for (const auto& match : internal.matches)
                    internal_matches.push_back(AsComparableMatch(match));
            } else if (options.workload == "mam") {
                MamOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm = MemSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindMams(
                    dataset.queries[query_id].sequence, internal_options);
                internal_count = internal.total_matches;
                for (const auto& match : internal.matches)
                    internal_matches.push_back(AsComparableMatch(match));
            } else if (options.workload == "mum") {
                MumOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm = MemSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindMums(
                    dataset.queries[query_id].sequence, internal_options);
                internal_count = internal.total_matches;
                for (const auto& match : internal.matches)
                    internal_matches.push_back(AsComparableMatch(match));
            } else {
                RightMaximalOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm =
                    RightMaximalSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindRightMaximalMatches(
                    dataset.queries[query_id].sequence, internal_options);
                internal_count = internal.total_matches;
                internal_matches = internal.matches;
            }
            mix(mummer_checksum, query_id);
            mix(mummer_checksum, matches.size());
            mummer_total += matches.size();
            for (const auto& match : matches) mix_match(mummer_checksum, match);
            mix(internal_checksum, query_id);
            mix(internal_checksum, internal_count);
            internal_total += internal_count;
            for (const auto& match : internal_matches)
                mix_match(internal_checksum, match);
        }
        if (mummer_total != internal_total || mummer_checksum != internal_checksum)
            throw Error(ErrorCode::kBuildFailure, "MUMmer4 reverse-complement right-maximal exact match correctness mismatch");
        queries.push_back(std::move(summary));
    }
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    if (values.empty()) return 0;
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

void add_fm_capability_rows(
    const std::vector<Dataset>& datasets,
    const Options& options,
    std::vector<QueryResultRow>& rows,
    std::vector<RawRow>& raw) {
    static const std::array<const char*, 3> fm_methods{{
        "fm-huff", "fm-balanced", "fm-epr"}};
    const auto thresholds = options.workload == "smem"
        ? options.min_occurrences : std::vector<std::uint64_t>{1};
    for (const auto& dataset : datasets) {
        for (const auto min_length : options.min_lengths) {
          for (const auto min_occurrences : thresholds) {
            for (const auto* method : fm_methods) {
                for (const auto& operation : operation_specs()) {
                    QueryResultRow row;
                    row.dataset = dataset.name;
                    row.method = method;
                    row.algorithm = "not_supported";
                    row.acceleration = "fm-index";
                    row.operation = operation.name;
                    row.min_length = min_length;
                    row.min_occurrences = min_occurrences;
                    row.query_count = dataset.queries.size();
                    row.query_bases = dataset.query_bases;
                    row.peak_rss_scope = "not_applicable";
                    row.status = "not_supported";
                    rows.push_back(std::move(row));
                    RawRow raw_row;
                    raw_row.dataset = dataset.name;
                    raw_row.method = method;
                    raw_row.operation = operation.name;
                    raw_row.min_length = min_length;
                    raw_row.min_occurrences = min_occurrences;
                    raw_row.peak_rss_scope = "not_applicable";
                    raw_row.query_bases = dataset.query_bases;
                    raw_row.status = "not_supported";
                    raw.push_back(std::move(raw_row));
                }
          }
            }
        }
    }
}

void validate(const std::vector<QueryResultRow>& rows) {
    using ResultKey = std::tuple<std::string, std::uint64_t, std::uint64_t,
                                 std::string>;
    using ResultValue = std::tuple<std::uint64_t, std::uint64_t,
                                   std::uint64_t, std::uint64_t,
                                   std::uint64_t>;
    std::map<ResultKey, ResultValue> expected;
    std::map<std::tuple<std::string, std::uint64_t, std::uint64_t,
                        std::string>,
             std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>
        counts_by_method;
    std::map<std::tuple<std::string, std::uint64_t, std::uint64_t,
                        std::string>, std::uint64_t>
        complete_results_by_method;
    std::vector<const QueryResultRow*> skipped_vectors;
    for (const auto& row : rows) {
        if (row.status == "skipped_high_frequency") {
            if (row.operation != "vector" || !row.vector_skipped ||
                !row.seconds.empty() || row.peak_rss_scope != "not_applicable" ||
                row.total_matches <= row.materialization_match_threshold ||
                row.reported_matches != 0 || row.checksum != 0) {
                throw Error(ErrorCode::kBuildFailure,
                    "invalid right-maximal high-frequency vector skip for " +
                    row.method);
            }
            skipped_vectors.push_back(&row);
            continue;
        }
        if (row.status != "ok") continue;
        if (row.vector_skipped)
            throw Error(ErrorCode::kBuildFailure,
                "successful right-maximal row is incorrectly marked skipped");
        if (row.seconds.empty())
            throw Error(ErrorCode::kBuildFailure,
                "right-maximal exact match benchmark produced no measured repetitions");
        if (row.operation == "max_matches=0" &&
            (row.reported_matches != 0 || row.checksum != 0))
            throw Error(ErrorCode::kBuildFailure,
                "right-maximal max_matches=0 retained unexpected matches");
        if (row.reported_matches > row.total_matches)
            throw Error(ErrorCode::kBuildFailure,
                "right-maximal benchmark reported more matches than it enumerated");
        const auto key = std::make_tuple(row.dataset, row.min_length,
                                         row.min_occurrences, row.operation);
        const auto value = std::make_tuple(
            row.total_matches, row.total_smems, row.reported_matches,
            row.count_checksum, row.checksum);
        const auto it = expected.find(key);
        if (it == expected.end()) expected.emplace(key, value);
        else if (it->second != value)
            throw Error(ErrorCode::kBuildFailure, "right-maximal exact match benchmark correctness mismatch for " + row.dataset +
                " min_length=" + std::to_string(row.min_length) + " method=" + row.method +
                " operation=" + row.operation);

        const auto method_key = std::make_tuple(
            row.dataset, row.min_length, row.min_occurrences, row.method);
        const auto count = std::make_tuple(
            row.total_matches, row.total_smems, row.count_checksum);
        const auto count_it = counts_by_method.find(method_key);
        if (count_it == counts_by_method.end()) counts_by_method.emplace(method_key, count);
        else if (count_it->second != count)
            throw Error(ErrorCode::kBuildFailure,
                "right-maximal operation count mismatch for " + row.method);
        if (row.operation == "streaming" || row.operation == "vector") {
            const auto full_it = complete_results_by_method.find(method_key);
            if (full_it == complete_results_by_method.end())
                complete_results_by_method.emplace(method_key, row.checksum);
            else if (full_it->second != row.checksum)
                throw Error(ErrorCode::kBuildFailure,
                    "right-maximal streaming/vector checksum mismatch for " + row.method);
        }
    }
    for (const auto* row : skipped_vectors) {
        const auto method_key = std::make_tuple(
            row->dataset, row->min_length, row->min_occurrences, row->method);
        const auto count_it = counts_by_method.find(method_key);
        bool matches_preflight = count_it != counts_by_method.end() &&
            std::get<0>(count_it->second) == row->total_matches &&
            std::get<1>(count_it->second) == row->total_smems &&
            std::get<2>(count_it->second) == row->count_checksum;
        // MUMmer4 cannot expose a streaming callback. Its high-frequency row
        // is guarded by the private SuffixArray streaming preflight executed
        // immediately before the external process would be launched.
        if (!matches_preflight && row->method == "mummer4")
            matches_preflight = row->count_checksum != 0;
        if (!matches_preflight) {
            const auto streaming_it = expected.find(std::make_tuple(
                row->dataset, row->min_length, row->min_occurrences,
                std::string("streaming")));
            matches_preflight = streaming_it != expected.end() &&
                std::get<0>(streaming_it->second) == row->total_matches &&
                std::get<1>(streaming_it->second) == row->total_smems &&
                std::get<3>(streaming_it->second) == row->count_checksum;
        }
        if (!matches_preflight) {
            throw Error(ErrorCode::kBuildFailure,
                "right-maximal skipped vector row does not match its streaming "
                "preflight for " + row->method);
        }
    }
}

void write_outputs(const Options& options, const std::vector<Dataset>& datasets,
    const std::vector<BuildResult>& builds, const std::vector<QueryResultRow>& queries,
    const std::vector<RawRow>& raw,
    const std::vector<CorrectnessResult>& correctness) {
    std::filesystem::create_directories(options.output_directory);
    {
        std::ofstream out(options.output_directory / "run_metadata.tsv");
        out << "profile\tscenario\tseed\tdataset\tdataset_fingerprint\ttotal_bases\tcontigs\tquery_count\tquery_bases"
               "\tbuild_repetitions\tquery_repetitions\twarmups"
               "\tvector_materialization_match_threshold"
               "\tnaive_right_maximal_oracle_status\toracle_reference_bases\toracle_query_bases"
               "\tlearned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits"
               "\tcompiler\tcmake_version\tbuild_type\tos\tarchitecture\tlogical_cpus\tmummer_version\tmummer_launcher_sha256\tmummer_runtime_sha256\tworkload\tworker_process_model"
               "\tmin_occurrences\tminibwa_version\tminibwa_sha256\n";
        for (const auto& dataset : datasets) {
            out << options.profile << '\t' << dataset.scenario
            << '\t' << options.seed << '\t' << dataset.name << '\t' << std::hex << dataset.fingerprint << std::dec
            << '\t' << dataset.total_bases << '\t' << dataset.reference.size() << '\t'
            << dataset.queries.size() << '\t' << dataset.query_bases << '\t'
            << options.build_repetitions << '\t' << options.query_repetitions << '\t'
            << options.warmups << '\t' << kVectorMaterializationMatchThreshold << '\t'
            << dataset.oracle_status << '\t' << dataset.oracle_reference_bases << '\t'
            << dataset.oracle_query_bases << '\t'
            << options.learned_k << '\t' << options.learned_memory_overhead_basis_points << '\t'
            << (options.learned_bucket_bits ? std::to_string(*options.learned_bucket_bits) : "auto") << '\t'
            << __VERSION__ << '\t' << SUFKIT_BENCH_CMAKE_VERSION << '\t' << SUFKIT_BENCH_BUILD_TYPE
            << "\tLinux\tx86_64\t" << sysconf(_SC_NPROCESSORS_ONLN) << '\t'
            << options.mummer_version << '\t'
            << options.mummer_launcher_sha256 << '\t'
            << options.mummer_runtime_sha256 << '\t'
            << options.workload << "\tclean-exec-phase-v1\t"
            << join_numbers(options.min_occurrences)
            << '\t' << options.minibwa_version << '\t'
            << options.minibwa_sha256 << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "correctness_summary.tsv");
        out << "dataset\tscenario\toracle\tmin_length\treference_bases\tquery_count\tquery_bases\ttotal_matches\tresult_checksum\tstatus\tmin_occurrences\ttotal_smems\n";
        for (const auto& row : correctness) {
            out << row.dataset << '\t' << row.scenario << '\t' << row.oracle << '\t'
                << row.min_length << '\t' << row.reference_bases << '\t'
                << row.query_count << '\t' << row.query_bases << '\t'
                << row.total_matches << '\t' << std::hex << row.checksum << std::dec
                << '\t' << row.status << '\t' << row.min_occurrences << '\t'
                << row.total_smems << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "build_results.tsv");
        out << "dataset\tmethod\talgorithm\tsa_acceleration\tsa_sampling_rate\trepetitions\tbuild_seconds\tsa_build_seconds\tisa_build_seconds\tlcp_build_seconds\tchild_build_seconds\tlearned_index_build_seconds\tsave_seconds\tload_seconds\tbuild_peak_rss_mb_median\tsave_peak_rss_mb_median\tload_peak_rss_mb_median\tbuild_peak_rss_scope\tsave_peak_rss_scope\tload_peak_rss_scope\tserialized_bytes\tallocated_disk_bytes\tauxiliary_bytes\tlearned_index_bytes\tbits_per_base\tstatus\tstorage_compaction_seconds\tconstruction_coordinate_width\tstored_coordinate_width\tsa_resource_profile\tlcp_encoding\tsa_bytes\tisa_bytes\tlcp_bytes\tresident_core_bytes\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : builds) {
            const auto data = std::find_if(datasets.begin(), datasets.end(), [&](const auto& value) { return value.name == row.dataset; });
            const double bits = data == datasets.end() || data->total_bases == 0 ? 0 :
                static_cast<double>(row.serialized_bytes) * 8.0 / static_cast<double>(data->total_bases);
            out << row.dataset << '\t' << row.method << '\t' << row.algorithm << '\t' << row.acceleration << '\t'
                << row.sampling_rate << '\t' << row.repetitions << '\t'
                << row.build_seconds << '\t' << row.sa_build_seconds << '\t' << row.isa_build_seconds << '\t'
                << row.lcp_build_seconds << '\t' << row.child_build_seconds << '\t'
                << row.learned_index_build_seconds << '\t' << row.save_seconds << '\t' << row.load_seconds << '\t'
                << row.build_peak_rss_mb << '\t' << row.save_peak_rss_mb << '\t'
                << row.load_peak_rss_mb << '\t' << row.build_peak_rss_scope << '\t'
                << row.save_peak_rss_scope << '\t' << row.load_peak_rss_scope << '\t'
                << row.serialized_bytes << '\t' << row.allocated_disk_bytes << '\t'
                << row.auxiliary_bytes << '\t'
                << row.learned_index_bytes << '\t'
                << bits << '\t' << row.status << '\t'
                << row.storage_compaction_seconds << '\t'
                << static_cast<unsigned>(row.construction_coordinate_width)
                << '\t' << static_cast<unsigned>(row.stored_coordinate_width)
                << '\t' << ToString(row.resource_profile) << '\t'
                << ToString(row.lcp_encoding) << '\t' << row.sa_bytes << '\t'
                << row.isa_bytes << '\t' << row.lcp_bytes << '\t'
                << row.resident_core_bytes << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "query_results.tsv");
        out << "dataset\tmethod\talgorithm\tsa_acceleration\toperation\tmin_length\tquery_count\tquery_bases\tseconds_median\tseconds_min\tseconds_max\tquery_bases_per_second\tmatches_per_second\tquery_peak_rss_mb_max\tpeak_rss_scope\ttotal_matches\treported_matches\tcount_checksum\tresult_checksum\tlookup_calls\tbinary_lookup_calls\tlearned_lookup_calls\tsuffix_link_attempts\tsuffix_link_successes\tsuffix_link_success_rate\tsuffix_link_fallbacks\tprevious_empty_lookups\tlookup_character_comparisons\tlookup_sa_row_accesses\tpredictions\tprediction_error_mean\tprediction_error_max\tlocal_window_rows_mean\tlocal_window_rows_max\tfull_binary_fallbacks\tmaterialization_match_threshold\tvector_skipped\tstatus\tmin_occurrences\ttotal_smems\tsmems_per_second\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : queries) {
            out << row.dataset << '\t' << row.method << '\t' << row.algorithm << '\t'
                << row.acceleration << '\t' << row.operation << '\t' << row.min_length << '\t'
                << row.query_count << '\t' << row.query_bases;
            if (row.seconds.empty()) {
                for (int column = 0; column < 6; ++column) out << "\tNA";
                out << '\t' << row.peak_rss_scope << '\t'
                    << row.total_matches << '\t' << row.reported_matches << '\t'
                    << std::hex << row.count_checksum << '\t' << row.checksum
                    << std::dec;
                for (int column = 0; column < 16; ++column) out << "\t0";
                out << '\t' << row.materialization_match_threshold << '\t'
                    << (row.vector_skipped ? 1 : 0) << '\t' << row.status
                    << '\t' << row.min_occurrences << '\t' << row.total_smems
                    << "\tNA\n";
                continue;
            }
            const auto med = median(row.seconds);
            out << '\t' << med << '\t'
                << *std::min_element(row.seconds.begin(), row.seconds.end()) << '\t'
                << *std::max_element(row.seconds.begin(), row.seconds.end()) << '\t'
                << static_cast<double>(row.query_bases) / med << '\t' << static_cast<double>(row.total_matches) / med << '\t'
                << row.peak_rss_mb << '\t' << row.peak_rss_scope << '\t'
                << row.total_matches << '\t' << row.reported_matches << '\t'
                << std::hex << row.count_checksum << '\t' << row.checksum << std::dec << '\t'
                << row.statistics.lookup_calls << '\t' << row.statistics.binary_lookup_calls << '\t'
                << row.statistics.learned_lookup_calls << '\t' << row.statistics.suffix_link_attempts << '\t'
                << row.statistics.suffix_link_successes << '\t'
                << (row.statistics.suffix_link_attempts == 0 ? 0.0 :
                    static_cast<double>(row.statistics.suffix_link_successes) /
                    static_cast<double>(row.statistics.suffix_link_attempts)) << '\t'
                << row.statistics.suffix_link_fallbacks << '\t' << row.statistics.previous_empty_lookups << '\t'
                << row.statistics.lookup.character_comparisons << '\t'
                << row.statistics.lookup.suffix_comparisons << '\t'
                << row.statistics.lookup.predictions << '\t'
                << (row.statistics.lookup.predictions == 0 ? 0.0 :
                    static_cast<double>(row.statistics.lookup.prediction_absolute_error_sum) /
                    static_cast<double>(row.statistics.lookup.predictions)) << '\t'
                << row.statistics.lookup.prediction_absolute_error_max << '\t'
                << (row.statistics.lookup.predictions == 0 ? 0.0 :
                    static_cast<double>(row.statistics.lookup.local_window_rows) /
                    static_cast<double>(row.statistics.lookup.predictions)) << '\t'
                << row.statistics.lookup.local_window_rows_max << '\t'
                << row.statistics.lookup.full_binary_fallbacks << '\t'
                << row.materialization_match_threshold << '\t'
                << (row.vector_skipped ? 1 : 0) << '\t' << row.status << '\t'
                << row.min_occurrences << '\t' << row.total_smems << '\t'
                << static_cast<double>(row.total_smems) / med << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "raw_repetitions.tsv");
        out << "dataset\tmethod\toperation\tmin_length\trepetition\tseconds\tuser_cpu_seconds\tsystem_cpu_seconds\tpeak_rss_mb\tpeak_rss_scope\tquery_bases\tserialized_bytes\tallocated_disk_bytes\tauxiliary_bytes\tlearned_index_bytes\tmaterialization_match_threshold\tvector_skipped\ttotal_matches\treported_matches\tcount_checksum\tresult_checksum\tlookup_calls\tbinary_lookup_calls\tlearned_lookup_calls\tsuffix_link_attempts\tsuffix_link_successes\tsuffix_link_fallbacks\tprevious_empty_lookups\tlookup_character_comparisons\tlookup_sa_row_accesses\tpredictions\tprediction_error_sum\tprediction_error_max\tlocal_window_rows\tlocal_window_rows_max\tfull_binary_fallbacks\tstatus\tmin_occurrences\ttotal_smems\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : raw) {
            out << row.dataset << '\t' << row.method << '\t' << row.operation << '\t'
                << row.min_length << '\t' << row.repetition;
            const bool has_measurements =
                row.status == "ok" || row.status.rfind("external_", 0) == 0;
            if (!has_measurements) {
                out << "\tNA\tNA\tNA\tNA\t" << row.peak_rss_scope << '\t'
                    << row.query_bases << '\t' << row.serialized_bytes << '\t'
                    << row.allocated_disk_bytes << '\t' << row.auxiliary_bytes << '\t'
                    << row.learned_index_bytes << '\t'
                    << row.materialization_match_threshold << '\t'
                    << (row.vector_skipped ? 1 : 0) << '\t'
                    << row.total_matches << '\t' << row.reported_matches << '\t'
                    << std::hex << row.count_checksum << '\t' << row.checksum
                    << std::dec;
                for (int column = 0; column < 15; ++column) out << "\t0";
                out << '\t' << row.status << '\t' << row.min_occurrences << '\t'
                    << row.total_smems << '\n';
                continue;
            }
            out << '\t' << row.seconds << '\t' << row.user_cpu_seconds << '\t'
                << row.system_cpu_seconds << '\t' << row.peak_rss_mb << '\t'
                << row.peak_rss_scope << '\t' << row.query_bases << '\t'
                << row.serialized_bytes << '\t' << row.allocated_disk_bytes << '\t'
                << row.auxiliary_bytes << '\t' << row.learned_index_bytes << '\t'
                << row.materialization_match_threshold << '\t'
                << (row.vector_skipped ? 1 : 0) << '\t'
                << row.total_matches << '\t' << row.reported_matches << '\t'
                << std::hex << row.count_checksum << '\t' << row.checksum << std::dec << '\t'
                << row.statistics.lookup_calls << '\t' << row.statistics.binary_lookup_calls << '\t'
                << row.statistics.learned_lookup_calls << '\t' << row.statistics.suffix_link_attempts << '\t'
                << row.statistics.suffix_link_successes << '\t' << row.statistics.suffix_link_fallbacks << '\t'
                << row.statistics.previous_empty_lookups << '\t' << row.statistics.lookup.character_comparisons << '\t'
                << row.statistics.lookup.suffix_comparisons << '\t' << row.statistics.lookup.predictions << '\t'
                << row.statistics.lookup.prediction_absolute_error_sum << '\t'
                << row.statistics.lookup.prediction_absolute_error_max << '\t'
                << row.statistics.lookup.local_window_rows << '\t'
                << row.statistics.lookup.local_window_rows_max << '\t'
                << row.statistics.lookup.full_binary_fallbacks << '\t' << row.status
                << '\t' << row.min_occurrences << '\t' << row.total_smems
                << '\n';
        }
    }
}

} // namespace

int run(const std::vector<std::string>& arguments) {
    auto options = parse(arguments);
    std::filesystem::create_directories(options.output_directory);
    const auto scratch = options.output_directory / "work";
    std::filesystem::create_directories(scratch);
    if (options.mummer4) {
        const auto version_out = scratch / "mummer-version.txt";
        const auto version_err = scratch / "mummer-version.err";
        const auto version_result = run_process(
            {options.mummer4->string(), "--version"}, version_out, version_err);
        std::ifstream version_input(version_out);
        std::getline(version_input, options.mummer_version);
        if (version_result.status != 0 || options.mummer_version != "4.0.1")
            throw Error(ErrorCode::kUnsupportedBackend, "right-maximal exact match benchmark requires MUMmer4 version 4.0.1");
        options.mummer_launcher_sha256 = fingerprint_file(
            *options.mummer4, scratch, "mummer-launcher", "MUMmer4 launcher");
        if (!options.mummer4_runtime && is_elf_binary(*options.mummer4)) {
            options.mummer4_runtime = options.mummer4;
        }
        if (!options.mummer4_runtime) {
            throw Error(ErrorCode::kInvalidInput,
                "MUMmer4 launcher is not an ELF binary; provide the actual "
                "runtime ELF with --mummer4-runtime PATH");
        }
        if (!is_elf_binary(*options.mummer4_runtime)) {
            throw Error(ErrorCode::kInvalidInput,
                "--mummer4-runtime must name an ELF binary");
        }
        options.mummer_runtime_sha256 = fingerprint_file(
            *options.mummer4_runtime, scratch, "mummer-runtime",
            "MUMmer4 runtime ELF");
    }
    if (options.minibwa) {
        const auto version_out = scratch / "minibwa-version.txt";
        const auto version_err = scratch / "minibwa-version.err";
        const auto version_result = run_process(
            {options.minibwa->string(), "version"}, version_out, version_err);
        std::ifstream version_input(version_out);
        std::getline(version_input, options.minibwa_version);
        if (version_result.status != 0 || options.minibwa_version.empty()) {
            throw Error(ErrorCode::kUnsupportedBackend,
                        "cannot read the MiniBWA version");
        }
        const auto hash_out = scratch / "minibwa-sha256.txt";
        const auto hash_err = scratch / "minibwa-sha256.err";
        const auto hash_result = run_process(
            {"/usr/bin/sha256sum", options.minibwa->string()}, hash_out,
            hash_err);
        std::ifstream hash_input(hash_out);
        hash_input >> options.minibwa_sha256;
        if (hash_result.status != 0 || options.minibwa_sha256.size() != 64) {
            throw Error(ErrorCode::kIoError,
                        "cannot fingerprint the MiniBWA executable");
        }
    }
    std::vector<Dataset> datasets;
    std::vector<BuildResult> builds;
    std::vector<QueryResultRow> queries;
    std::vector<RawRow> raw;
    std::vector<CorrectnessResult> correctness;
    const auto scenarios = options.reference ? std::vector<std::string>{"user"} : options.scenarios;
    for (const auto& scenario : scenarios) {
        datasets.push_back(options.reference ? load_dataset(options) : generate_dataset(options, scenario));
        auto& dataset = datasets.back();
        auto oracle_rows = run_naive_oracle(dataset, options);
        correctness.insert(correctness.end(),
            std::make_move_iterator(oracle_rows.begin()),
            std::make_move_iterator(oracle_rows.end()));
        const auto dataset_scratch = scratch / scenario;
        std::filesystem::create_directories(dataset_scratch);
        for (const auto& method : options.methods) {
            std::cerr << "benchmarking " << dataset.name << " with " << method << "...\n";
            if (method == "mummer4") benchmark_mummer(dataset, options, dataset_scratch, builds, queries, raw);
            else if (method == "minibwa") benchmark_minibwa(
                dataset, options, dataset_scratch, builds, queries, raw);
            else benchmark_internal_clean_exec(
                dataset, options, method, dataset_scratch, builds, queries, raw);
        }
        // Finish correctness checks while this scenario's reference is still
        // resident, then release its large sequence buffers before the next
        // scenario is generated. Metadata only needs the retained record
        // count, names, and precomputed aggregate values.
        validate(queries);
        for (auto& record : dataset.reference) {
            record.sequence.clear();
            record.sequence.shrink_to_fit();
        }
    }
    add_fm_capability_rows(datasets, options, queries, raw);
    write_outputs(options, datasets, builds, queries, raw, correctness);
    validate(queries);
    std::error_code cleanup_error;
    std::filesystem::remove_all(scratch, cleanup_error);
    if (cleanup_error) throw Error(ErrorCode::kIoError, "cannot remove right-maximal exact match benchmark scratch directory");
    std::cerr << options.workload << " benchmark results written to "
              << options.output_directory << '\n';
    return 0;
}

int run_worker(const std::vector<std::string>& arguments) {
    return run_clean_exec_phase(arguments);
}

} // namespace sufkit::app::right_maximal_bench

namespace sufkit::app {

int run_maximal_benchmark_worker(const std::vector<std::string>& arguments) {
    return right_maximal_bench::run_worker(arguments);
}

} // namespace sufkit::app
