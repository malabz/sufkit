#include "right_maximal_benchmark.hpp"
#include "app_support.hpp"
#include "benchmark_provenance.hpp"
#include "suffix_link_diagnostics.hpp"

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
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
#include <sys/wait.h>
#include <unistd.h>

namespace sufkit::app::right_maximal_bench {
namespace {

using Clock = std::chrono::steady_clock;

#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
constexpr std::uint64_t kSuffixLinkScanDiagnosticsAvailable = 1;
#else
constexpr std::uint64_t kSuffixLinkScanDiagnosticsAvailable = 0;
#endif

struct Options {
    std::string workload = "right-maximal";
    std::string profile = "smoke";
    std::vector<std::string> scenarios{"mixed"};
    std::vector<std::string> methods{"right-maximal-baseline", "right-maximal-lcp", "right-maximal-child", "right-maximal-suffix-link", "right-maximal-full"};
    std::vector<std::uint64_t> min_lengths{20, 50};
    std::vector<std::string> strands{"forward"};
    std::filesystem::path output_directory;
    std::optional<std::filesystem::path> mummer4;
    std::optional<std::filesystem::path> reference;
    std::optional<std::filesystem::path> query_file;
    std::string mummer_version;
    std::string mummer_sha256;
    std::uint64_t seed = 20260822;
    std::uint32_t learned_k = 20;
    std::uint32_t learned_memory_overhead_basis_points = 100;
    std::optional<std::uint32_t> learned_bucket_bits;
    std::optional<std::uint32_t> query_repetitions;
    bench::BenchmarkProvenance provenance;
};

double minimum_measurement_seconds(const Options& options) {
    if (options.reference.has_value()) return 0.100;
    if (options.profile == "smoke") return 0.0;
    if (options.profile == "quick") return 0.010;
    return 0.100;
}

double calibration_target_seconds(double minimum_seconds) {
    return minimum_seconds * 1.25;
}

std::uint32_t measured_repetitions(const Options& options) {
    if (options.query_repetitions) return *options.query_repetitions;
    return !options.reference.has_value() && options.profile == "smoke" ? 3
                                                                        : 5;
}

struct Dataset {
    std::string name;
    std::vector<SequenceRecord> reference;
    std::vector<SequenceRecord> queries;
    std::uint64_t total_bases = 0;
    std::uint64_t query_bases = 0;
    std::uint64_t fingerprint = 0;
};

struct BuildResult {
    std::string dataset;
    std::string method;
    std::string algorithm;
    std::string acceleration;
    double build_seconds = 0;
    double sa_build_seconds = 0;
    double isa_build_seconds = 0;
    double lcp_build_seconds = 0;
    double child_build_seconds = 0;
    double learned_index_build_seconds = 0;
    double save_seconds = 0;
    double load_seconds = 0;
    double peak_rss_mb = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    std::string status = "ok";
};

struct QueryResultRow {
    std::string dataset;
    std::string method;
    std::string algorithm;
    std::string acceleration;
    std::uint64_t min_length = 0;
    std::uint64_t query_count = 0;
    std::uint64_t query_bases = 0;
    std::vector<double> seconds;
    std::uint64_t total_matches = 0;
    std::uint64_t checksum = 0;
    std::uint64_t measurement_iterations = 1;
    RightMaximalSearchStatistics statistics;
    detail::SuffixLinkScanSummary suffix_link_scan;
    std::string status = "ok";
    std::string strand = "forward";
};

struct RawRow {
    std::string dataset;
    std::string method;
    std::string operation;
    std::uint64_t min_length = 0;
    std::uint32_t repetition = 0;
    double seconds = 0;
    std::uint64_t total_matches = 0;
    std::uint64_t checksum = 0;
    std::uint64_t measurement_iterations = 1;
    RightMaximalSearchStatistics statistics;
    detail::SuffixLinkScanSummary suffix_link_scan;
    std::string status = "ok";
    std::string strand = "forward";
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

std::string join(const std::vector<std::string>& values) {
    std::ostringstream output;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) output << ',';
        output << values[index];
    }
    return output.str();
}

StrandMode strand_mode(const std::string& value) {
    if (value == "forward") return StrandMode::kForward;
    if (value == "reverse-complement")
        return StrandMode::kReverseComplement;
    if (value == "both") return StrandMode::kBoth;
    throw Error(ErrorCode::kInvalidInput,
                "unknown right-maximal benchmark strand: " + value);
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
                result.workload != "mem" && result.workload != "mam") {
                throw Error(ErrorCode::kInvalidInput,
                            "unknown maximal match benchmark workload: " +
                                result.workload);
            }
        } else {
            if (++index >= arguments.size()) throw Error(ErrorCode::kInvalidInput, "missing value for " + name);
            const auto& value = arguments[index];
            if (name == "--profile") result.profile = value;
            else if (name == "--scenarios") result.scenarios = split(value);
            else if (name == "--methods") {
                result.methods = split(value);
                methods_explicit = true;
            }
            else if (name == "--strands") result.strands = split(value);
            else if (name == "--min-lengths") {
                result.min_lengths.clear();
                for (const auto& item : split(value)) result.min_lengths.push_back(parse_number(item, "--min-lengths"));
            } else if (name == "--output-dir") result.output_directory = value;
            else if (name == "--mummer4") result.mummer4 = std::filesystem::path(value);
            else if (name == "--reference") result.reference = std::filesystem::path(value);
            else if (name == "--queries") result.query_file = std::filesystem::path(value);
            else if (name == "--seed") result.seed = parse_number(value, "--seed");
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
            } else if (name == "--query-repetitions") {
                const auto parsed = parse_number(value, "--query-repetitions");
                if (parsed == 0 ||
                    parsed > std::numeric_limits<std::uint32_t>::max()) {
                    throw Error(ErrorCode::kInvalidInput,
                                "--query-repetitions is out of range");
                }
                result.query_repetitions =
                    static_cast<std::uint32_t>(parsed);
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
    if (!result.reference && result.profile != "smoke" && result.profile != "quick" &&
        result.profile != "standard")
        throw Error(ErrorCode::kInvalidInput, "right-maximal exact match benchmark currently runs smoke, quick, or standard profiles");
    if (result.query_file && !result.reference)
        throw Error(ErrorCode::kInvalidInput, "--queries requires --reference");
    if (result.output_directory.empty()) throw Error(ErrorCode::kInvalidInput, "--output-dir is required");
    if (result.min_lengths.empty() || std::any_of(result.min_lengths.begin(), result.min_lengths.end(), [](auto value) { return value == 0; }))
        throw Error(ErrorCode::kInvalidInput, "right-maximal exact match minimum lengths must be positive");
    const std::set<std::string> valid_scenarios{
        "mixed", "balanced", "gc-skewed", "repeat-rich", "n-islands", "many-contig"};
    for (const auto& scenario : result.scenarios)
        if (valid_scenarios.count(scenario) == 0)
            throw Error(ErrorCode::kInvalidInput, "unknown right-maximal exact match benchmark scenario: " + scenario);
    std::set<std::string> unique_strands;
    for (const auto& strand : result.strands) {
        (void)strand_mode(strand);
        if (!unique_strands.insert(strand).second) {
            throw Error(ErrorCode::kInvalidInput,
                        "duplicate right-maximal benchmark strand: " + strand);
        }
    }
    const std::set<std::string> right_maximal_methods{
        "right-maximal-baseline", "right-maximal-lcp", "right-maximal-child", "right-maximal-suffix-link", "right-maximal-full",
        "right-maximal-suffix-link-binary", "right-maximal-suffix-link-sapling", "mummer4"};
    const std::set<std::string> mem_methods{
        "mem-baseline", "mem-lcp", "mem-child", "mem-suffix-link",
        "mem-full", "mummer4"};
    const std::set<std::string> mam_methods{
        "mam-baseline", "mam-lcp", "mam-child", "mam-suffix-link",
        "mam-full", "mummer4"};
    const auto& valid_methods = result.workload == "right-maximal"
                                    ? right_maximal_methods
                                : result.workload == "mem" ? mem_methods
                                                           : mam_methods;
    for (const auto& method : result.methods) {
        if (valid_methods.count(method) == 0) throw Error(ErrorCode::kInvalidInput, "unknown right-maximal exact match benchmark method: " + method);
        if (method == "mummer4" && !result.mummer4) throw Error(ErrorCode::kInvalidInput, "mummer4 method requires --mummer4 PATH");
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
    const std::uint64_t total_bases = options.profile == "smoke" ? 64ULL * 1024ULL :
        (options.profile == "quick" ? 4ULL * 1024ULL * 1024ULL : 32ULL * 1024ULL * 1024ULL);
    const std::size_t query_count = options.profile == "smoke" ? 100 :
        (options.profile == "quick" ? 1000 : 5000);
    const std::size_t query_length = options.profile == "smoke" ? 128 : 256;
    std::uint64_t state = options.seed ^ hash_bytes(1469598103934665603ULL, scenario);
    const std::array<char, 4> bases{{'A', 'C', 'G', 'T'}};
    const std::size_t contigs = scenario == "many-contig" ? 1024 : 4;
    const std::size_t per_contig = static_cast<std::size_t>(total_bases / contigs);
    dataset.reference.reserve(contigs);
    const std::string repeat_template = "ACGTTGCAACGATTCGGTACCTAGGCTAACGT";
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
            for (std::size_t offset = 0; offset + repeat_template.size() <= repeat_end;
                 offset += repeat_template.size())
                sequence.replace(offset, repeat_template.size(), repeat_template);
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
    for (std::size_t id = 0; id < query_count; ++id) {
        const auto contig = static_cast<std::size_t>(next_random(state) % dataset.reference.size());
        const auto& source = dataset.reference[contig].sequence;
        const auto effective_query_length = std::min(query_length, source.size());
        if (effective_query_length == 0) continue;
        std::size_t position = 0;
        do {
            if (scenario == "repeat-rich" && id % 200 == 0) {
                const auto repeat_extent = source.size() * 2 / 5;
                const auto repeat_slots = repeat_extent > effective_query_length
                    ? std::max<std::size_t>(1, (repeat_extent - effective_query_length) /
                        repeat_template.size())
                    : 1;
                position = static_cast<std::size_t>(next_random(state) % repeat_slots) * repeat_template.size();
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
        if (id % 3 != 0) {
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
        const std::size_t desired = 1000;
        const std::size_t length = 256;
        for (std::size_t id = 0; id < desired; ++id) {
            const auto contig = static_cast<std::size_t>(next_random(state) % dataset.reference.size());
            const auto& sequence = dataset.reference[contig].sequence;
            if (sequence.size() < length) continue;
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
            if (!found && dataset.queries.empty()) continue;
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

double peak_rss_mb_self() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
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

struct RightMaximalPassResult {
    std::uint64_t total_matches = 0;
    std::uint64_t checksum = 0;
    RightMaximalSearchStatistics statistics;
    detail::SuffixLinkScanSummary suffix_link_scan;
};

RightMaximalSearchAlgorithm algorithm_for(const std::string& method);

void add_statistics(
    const RightMaximalSearchStatistics& source,
    RightMaximalSearchStatistics& destination) {
    destination.lookup_calls += source.lookup_calls;
    destination.binary_lookup_calls += source.binary_lookup_calls;
    destination.learned_lookup_calls += source.learned_lookup_calls;
    destination.suffix_link_attempts += source.suffix_link_attempts;
    destination.suffix_link_successes += source.suffix_link_successes;
    destination.suffix_link_fallbacks += source.suffix_link_fallbacks;
    destination.previous_empty_lookups += source.previous_empty_lookups;
    destination.lookup.suffix_comparisons +=
        source.lookup.suffix_comparisons;
    destination.lookup.character_comparisons +=
        source.lookup.character_comparisons;
    destination.lookup.gallop_probes += source.lookup.gallop_probes;
    destination.lookup.local_window_rows +=
        source.lookup.local_window_rows;
    destination.lookup.local_window_rows_max = std::max(
        destination.lookup.local_window_rows_max,
        source.lookup.local_window_rows_max);
    destination.lookup.predictions += source.lookup.predictions;
    destination.lookup.prediction_absolute_error_sum +=
        source.lookup.prediction_absolute_error_sum;
    destination.lookup.prediction_absolute_error_max = std::max(
        destination.lookup.prediction_absolute_error_max,
        source.lookup.prediction_absolute_error_max);
    destination.lookup.full_binary_fallbacks +=
        source.lookup.full_binary_fallbacks;
}

RightMaximalPassResult execute_right_maximal_pass(
    const SuffixArray& index,
    const Dataset& dataset,
    RightMaximalOptions options,
    bool collect_statistics) {
    RightMaximalPassResult pass;
    pass.checksum = checksum_seed();
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    detail::SuffixLinkScanSink scan_sink;
    std::optional<detail::ScopedSuffixLinkScanSink> scan_scope;
    Clock::time_point instrumented_begin{};
    if (collect_statistics) {
        instrumented_begin = Clock::now();
        scan_scope.emplace(scan_sink);
    }
#endif
    for (std::size_t query_id = 0; query_id < dataset.queries.size();
         ++query_id) {
        const auto consume_result = [&](const RightMaximalResult& result) {
            mix(pass.checksum, query_id);
            mix(pass.checksum, result.total_matches);
            pass.total_matches += result.total_matches;
            for (const auto& match : result.matches) {
                mix_match(pass.checksum, match);
            }
        };
        if (collect_statistics) {
            RightMaximalSearchStatistics query_statistics;
            options.statistics = &query_statistics;
            consume_result(index.FindRightMaximalMatches(
                dataset.queries[query_id].sequence, options));
            add_statistics(query_statistics, pass.statistics);
        } else {
            options.statistics = nullptr;
            consume_result(index.FindRightMaximalMatches(
                dataset.queries[query_id].sequence, options));
        }
    }
#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)
    if (collect_statistics) {
        pass.suffix_link_scan.instrumented_wall_seconds =
            seconds_since(instrumented_begin);
        scan_scope.reset();
        const auto scan_summary = scan_sink.Summarize();
        const auto instrumented_wall_seconds =
            pass.suffix_link_scan.instrumented_wall_seconds;
        pass.suffix_link_scan = scan_summary;
        pass.suffix_link_scan.instrumented_wall_seconds =
            instrumented_wall_seconds;
        if (pass.suffix_link_scan.attempts !=
            pass.statistics.suffix_link_attempts) {
            throw Error(
                ErrorCode::kBuildFailure,
                "suffix-link scan diagnostics lost an attempted derivation");
        }
    }
#endif
    return pass;
}

RightMaximalPassResult execute_maximal_pass(
    const SuffixArray& index, const Dataset& dataset, const Options& benchmark,
    const std::string& method, std::uint64_t min_length,
    const std::string& strand, bool collect_statistics) {
    if (benchmark.workload == "right-maximal") {
        RightMaximalOptions options;
        options.min_length = min_length;
        options.strands = strand_mode(strand);
        options.algorithm = algorithm_for(method);
        if (method == "right-maximal-suffix-link-binary") {
            options.lookup_algorithm = SaSearchAlgorithm::kBinary;
        } else if (method == "right-maximal-suffix-link-sapling") {
            options.lookup_algorithm = SaSearchAlgorithm::kSaplingPwl;
        }
        return execute_right_maximal_pass(index, dataset, options,
                                          collect_statistics);
    }

    RightMaximalPassResult pass;
    pass.checksum = checksum_seed();
    for (std::size_t query_id = 0; query_id < dataset.queries.size();
         ++query_id) {
        mix(pass.checksum, query_id);
        if (benchmark.workload == "mem") {
            MemOptions options;
            options.min_length = min_length;
            options.strands = strand_mode(strand);
            options.algorithm = static_cast<MemSearchAlgorithm>(
                static_cast<std::uint8_t>(algorithm_for(method)));
            const auto result =
                index.FindMems(dataset.queries[query_id].sequence, options);
            mix(pass.checksum, result.total_matches);
            pass.total_matches += result.total_matches;
            for (const auto& match : result.matches) {
                mix_match(pass.checksum, match);
            }
        } else {
            MamOptions options;
            options.min_length = min_length;
            options.strands = strand_mode(strand);
            options.algorithm = static_cast<MemSearchAlgorithm>(
                static_cast<std::uint8_t>(algorithm_for(method)));
            const auto result =
                index.FindMams(dataset.queries[query_id].sequence, options);
            mix(pass.checksum, result.total_matches);
            pass.total_matches += result.total_matches;
            for (const auto& match : result.matches) {
                mix_match(pass.checksum, match);
            }
        }
    }
    return pass;
}

std::uint64_t calibrate_maximal_iterations(
    const SuffixArray& index, const Dataset& dataset,
    const Options& benchmark, const std::string& method,
    std::uint64_t min_length, const std::string& strand,
    double minimum_measurement_seconds) {
    if (minimum_measurement_seconds <= 0.0) {
        return 1;
    }
    auto begin = Clock::now();
    (void)execute_maximal_pass(index, dataset, benchmark, method, min_length,
                               strand, false);
    auto seconds = seconds_since(begin);
    if (seconds >= minimum_measurement_seconds) {
        return 1;
    }
    auto iterations = std::max<std::uint64_t>(
        2, static_cast<std::uint64_t>(std::ceil(
               calibration_target_seconds(minimum_measurement_seconds) /
               std::max(seconds, 1.0e-9))));
    for (;;) {
        begin = Clock::now();
        for (std::uint64_t iteration = 0; iteration < iterations;
             ++iteration) {
            (void)execute_maximal_pass(index, dataset, benchmark, method,
                                       min_length, strand, false);
        }
        seconds = seconds_since(begin);
        if (seconds >= minimum_measurement_seconds) {
            return iterations;
        }
        const auto multiplier = std::max<std::uint64_t>(
            2, static_cast<std::uint64_t>(std::ceil(
                   calibration_target_seconds(minimum_measurement_seconds) /
                   std::max(seconds, 1.0e-9))));
        if (iterations >
            std::numeric_limits<std::uint64_t>::max() / multiplier) {
            throw Error(ErrorCode::kBuildFailure,
                        "maximal match benchmark calibration overflow");
        }
        iterations *= multiplier;
    }
}

SaAcceleration acceleration_for(const std::string& method) {
    if (method.find("baseline") != std::string::npos)
        return SaAcceleration::kNone;
    if (method.size() >= 4 && method.substr(method.size() - 4) == "-lcp")
        return SaAcceleration::kLcp;
    if (method.find("child") != std::string::npos)
        return SaAcceleration::kLcpChild;
    if (method.find("suffix-link") != std::string::npos)
        return SaAcceleration::kLcpSuffixLink;
    return SaAcceleration::kFull;
}

RightMaximalSearchAlgorithm algorithm_for(const std::string& method) {
    if (method.find("baseline") != std::string::npos)
        return RightMaximalSearchAlgorithm::kBaseline;
    if (method.size() >= 4 && method.substr(method.size() - 4) == "-lcp")
        return RightMaximalSearchAlgorithm::kLcp;
    if (method.find("child") != std::string::npos)
        return RightMaximalSearchAlgorithm::kChild;
    if (method.find("suffix-link") != std::string::npos)
        return RightMaximalSearchAlgorithm::kSuffixLink;
    return RightMaximalSearchAlgorithm::kFull;
}

std::uint64_t serialized_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    return error ? 0 : size;
}

void benchmark_internal(const Dataset& dataset, const Options& options, const std::string& method,
    const std::filesystem::path& scratch, std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries, std::vector<RawRow>& raw) {
    BuildResult build;
    build.dataset = dataset.name;
    build.method = method;
    build.algorithm = ToString(algorithm_for(method));
    build.acceleration = ToString(acceleration_for(method));
    const auto reference = GenomeReference::FromRecords(dataset.reference);
    SuffixArrayBuildOptions build_options;
    build_options.acceleration = acceleration_for(method);
    if (method == "right-maximal-suffix-link-sapling") {
        build_options.learned_index.enabled = true;
        build_options.learned_index.k = options.learned_k;
        build_options.learned_index.memory_overhead_basis_points =
            options.learned_memory_overhead_basis_points;
        build_options.learned_index.bucket_bits = options.learned_bucket_bits;
    }
    SuffixArrayBuildStatistics build_statistics;
    build_options.statistics = &build_statistics;
    const auto build_begin = Clock::now();
    auto index = SuffixArray::Build(reference, build_options);
    build.build_seconds = seconds_since(build_begin);
    build.sa_build_seconds = build_statistics.sa_seconds;
    build.isa_build_seconds = build_statistics.isa_seconds;
    build.lcp_build_seconds = build_statistics.lcp_seconds;
    build.child_build_seconds = build_statistics.child_seconds;
    build.learned_index_build_seconds = build_statistics.learned_index_seconds;
    const auto index_path = scratch / (method + ".sufidx");
    const auto save_begin = Clock::now();
    index.Save(index_path, {true});
    build.save_seconds = seconds_since(save_begin);
    build.serialized_bytes = serialized_size(index_path);
    build.auxiliary_bytes = index.GetInfo().auxiliary_bytes;
    build.learned_index_bytes = index.GetInfo().learned_index_bytes;
    const auto load_begin = Clock::now();
    auto loaded = SuffixArray::Load(index_path);
    build.load_seconds = seconds_since(load_begin);
    build.peak_rss_mb = peak_rss_mb_self();
    builds.push_back(build);

    const auto repetitions = measured_repetitions(options);
    for (const auto min_length : options.min_lengths) {
        for (const auto& strand : options.strands) {
            for (const auto& query : dataset.queries) {
                if (options.workload == "right-maximal") {
                    RightMaximalOptions query_options;
                    query_options.min_length = min_length;
                    query_options.strands = strand_mode(strand);
                    query_options.algorithm = algorithm_for(method);
                    (void)loaded.FindRightMaximalMatches(query.sequence,
                                                         query_options);
                } else if (options.workload == "mem") {
                    MemOptions query_options;
                    query_options.min_length = min_length;
                    query_options.strands = strand_mode(strand);
                    query_options.algorithm = static_cast<MemSearchAlgorithm>(
                        static_cast<std::uint8_t>(algorithm_for(method)));
                    (void)loaded.FindMems(query.sequence, query_options);
                } else {
                    MamOptions query_options;
                    query_options.min_length = min_length;
                    query_options.strands = strand_mode(strand);
                    query_options.algorithm = static_cast<MemSearchAlgorithm>(
                        static_cast<std::uint8_t>(algorithm_for(method)));
                    (void)loaded.FindMams(query.sequence, query_options);
                }
            }
            const double minimum_seconds =
                minimum_measurement_seconds(options);
            auto measurement_iterations =
                calibrate_maximal_iterations(loaded, dataset, options, method,
                                             min_length, strand,
                                             minimum_seconds);
            QueryResultRow summary;
            summary.dataset = dataset.name;
            summary.method = method;
            summary.algorithm = build.algorithm;
            summary.acceleration = build.acceleration;
            summary.min_length = min_length;
            summary.query_count = dataset.queries.size();
            summary.query_bases = dataset.query_bases;
            summary.strand = strand;
            const auto raw_begin = raw.size();
            std::optional<RightMaximalPassResult> canonical_pass;
            for (;;) {
                summary.measurement_iterations = measurement_iterations;
                summary.seconds.clear();
                raw.resize(raw_begin);
                canonical_pass.reset();
                for (std::uint32_t repetition = 0; repetition < repetitions;
                     ++repetition) {
                    const auto begin = Clock::now();
                    for (std::uint64_t iteration = 0;
                         iteration < measurement_iterations; ++iteration) {
                        auto pass = execute_maximal_pass(
                            loaded, dataset, options, method, min_length,
                            strand, false);
                        if (!canonical_pass) {
                            canonical_pass = pass;
                        } else if (pass.total_matches !=
                                       canonical_pass->total_matches ||
                                   pass.checksum != canonical_pass->checksum) {
                            throw Error(
                                ErrorCode::kBuildFailure,
                                "right-maximal result changed within a measured repetition");
                        }
                    }
                    const auto elapsed =
                        seconds_since(begin) /
                        static_cast<double>(measurement_iterations);
                    if (!canonical_pass) {
                        throw Error(
                            ErrorCode::kBuildFailure,
                            "right-maximal benchmark has no query results");
                    }
                    summary.seconds.push_back(elapsed);
                    summary.total_matches = canonical_pass->total_matches;
                    summary.checksum = canonical_pass->checksum;
                    RawRow raw_row;
                    raw_row.dataset = dataset.name;
                    raw_row.method = method;
                    raw_row.operation = options.workload;
                    raw_row.min_length = min_length;
                    raw_row.repetition = repetition;
                    raw_row.seconds = elapsed;
                    raw_row.total_matches = canonical_pass->total_matches;
                    raw_row.checksum = canonical_pass->checksum;
                    raw_row.measurement_iterations = measurement_iterations;
                    raw_row.strand = strand;
                    raw.push_back(std::move(raw_row));
                }
                const auto shortest = *std::min_element(
                    summary.seconds.begin(), summary.seconds.end());
                const auto shortest_actual =
                    shortest * static_cast<double>(measurement_iterations);
                if (shortest_actual >= minimum_seconds) {
                    break;
                }
                const auto multiplier = std::max<std::uint64_t>(
                    2, static_cast<std::uint64_t>(std::ceil(
                           calibration_target_seconds(minimum_seconds) /
                           std::max(shortest_actual, 1.0e-9))));
                if (measurement_iterations >
                    std::numeric_limits<std::uint64_t>::max() / multiplier) {
                    throw Error(
                        ErrorCode::kBuildFailure,
                        "right-maximal benchmark measurement iteration overflow");
                }
                measurement_iterations *= multiplier;
            }
            const auto instrumented = execute_maximal_pass(
                loaded, dataset, options, method, min_length, strand, true);
            if (!canonical_pass ||
                instrumented.total_matches != canonical_pass->total_matches ||
                instrumented.checksum != canonical_pass->checksum) {
                throw Error(
                    ErrorCode::kBuildFailure,
                    "instrumented right-maximal pass checksum differs from timed pass");
            }
            summary.statistics = instrumented.statistics;
            summary.suffix_link_scan = instrumented.suffix_link_scan;
            for (auto raw_index = raw_begin; raw_index < raw.size();
                 ++raw_index) {
                raw[raw_index].statistics = instrumented.statistics;
                raw[raw_index].suffix_link_scan =
                    instrumented.suffix_link_scan;
            }
            queries.push_back(std::move(summary));
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

void write_internal_worker_file(
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
        write_worker_string(output, row.status);
        write_worker_value(output, row.build_seconds);
        write_worker_value(output, row.sa_build_seconds);
        write_worker_value(output, row.isa_build_seconds);
        write_worker_value(output, row.lcp_build_seconds);
        write_worker_value(output, row.child_build_seconds);
        write_worker_value(output, row.learned_index_build_seconds);
        write_worker_value(output, row.save_seconds);
        write_worker_value(output, row.load_seconds);
        write_worker_value(output, row.peak_rss_mb);
        write_worker_value(output, row.serialized_bytes);
        write_worker_value(output, row.auxiliary_bytes);
        write_worker_value(output, row.learned_index_bytes);
    }
    write_worker_value(output, static_cast<std::uint64_t>(queries.size()));
    for (const auto& row : queries) {
        write_worker_string(output, row.dataset);
        write_worker_string(output, row.method);
        write_worker_string(output, row.algorithm);
        write_worker_string(output, row.acceleration);
        write_worker_string(output, row.status);
        write_worker_string(output, row.strand);
        write_worker_value(output, row.min_length);
        write_worker_value(output, row.query_count);
        write_worker_value(output, row.query_bases);
        write_worker_value(output, row.total_matches);
        write_worker_value(output, row.checksum);
        write_worker_value(output, row.measurement_iterations);
        write_worker_value(output, row.statistics);
        write_worker_value(output, row.suffix_link_scan);
        write_worker_value(output, static_cast<std::uint64_t>(row.seconds.size()));
        for (const auto seconds : row.seconds) write_worker_value(output, seconds);
    }
    write_worker_value(output, static_cast<std::uint64_t>(raw.size()));
    for (const auto& row : raw) {
        write_worker_string(output, row.dataset);
        write_worker_string(output, row.method);
        write_worker_string(output, row.operation);
        write_worker_string(output, row.status);
        write_worker_string(output, row.strand);
        write_worker_value(output, row.min_length);
        write_worker_value(output, row.repetition);
        write_worker_value(output, row.seconds);
        write_worker_value(output, row.total_matches);
        write_worker_value(output, row.checksum);
        write_worker_value(output, row.measurement_iterations);
        write_worker_value(output, row.statistics);
        write_worker_value(output, row.suffix_link_scan);
    }
    if (!output) throw Error(ErrorCode::kIoError, "cannot finish right-maximal exact match worker result");
}

void read_internal_worker_file(
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
        row.status = read_worker_string(input, "build status");
        row.build_seconds = read_worker_value<double>(input, "build seconds");
        row.sa_build_seconds = read_worker_value<double>(input, "SA seconds");
        row.isa_build_seconds = read_worker_value<double>(input, "ISA seconds");
        row.lcp_build_seconds = read_worker_value<double>(input, "LCP seconds");
        row.child_build_seconds = read_worker_value<double>(input, "CHILD seconds");
        row.learned_index_build_seconds = read_worker_value<double>(input, "model seconds");
        row.save_seconds = read_worker_value<double>(input, "save seconds");
        row.load_seconds = read_worker_value<double>(input, "load seconds");
        row.peak_rss_mb = read_worker_value<double>(input, "peak RSS");
        row.serialized_bytes = read_worker_value<std::uint64_t>(input, "serialized bytes");
        row.auxiliary_bytes = read_worker_value<std::uint64_t>(input, "auxiliary bytes");
        row.learned_index_bytes = read_worker_value<std::uint64_t>(input, "model bytes");
        builds.push_back(std::move(row));
    }
    const auto query_count = read_worker_value<std::uint64_t>(input, "query count");
    for (std::uint64_t index = 0; index < query_count; ++index) {
        QueryResultRow row;
        row.dataset = read_worker_string(input, "query dataset");
        row.method = read_worker_string(input, "query method");
        row.algorithm = read_worker_string(input, "query algorithm");
        row.acceleration = read_worker_string(input, "query acceleration");
        row.status = read_worker_string(input, "query status");
        row.strand = read_worker_string(input, "query strand");
        row.min_length = read_worker_value<std::uint64_t>(input, "minimum length");
        row.query_count = read_worker_value<std::uint64_t>(input, "query cardinality");
        row.query_bases = read_worker_value<std::uint64_t>(input, "query bases");
        row.total_matches = read_worker_value<std::uint64_t>(input, "total matches");
        row.checksum = read_worker_value<std::uint64_t>(input, "query checksum");
        row.measurement_iterations =
            read_worker_value<std::uint64_t>(input, "measurement iterations");
        row.statistics = read_worker_value<RightMaximalSearchStatistics>(input, "query statistics");
        row.suffix_link_scan = read_worker_value<detail::SuffixLinkScanSummary>(
            input, "suffix-link scan diagnostics");
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
        row.status = read_worker_string(input, "raw status");
        row.strand = read_worker_string(input, "raw strand");
        row.min_length = read_worker_value<std::uint64_t>(input, "raw minimum length");
        row.repetition = read_worker_value<std::uint32_t>(input, "raw repetition");
        row.seconds = read_worker_value<double>(input, "raw seconds");
        row.total_matches = read_worker_value<std::uint64_t>(input, "raw matches");
        row.checksum = read_worker_value<std::uint64_t>(input, "raw checksum");
        row.measurement_iterations =
            read_worker_value<std::uint64_t>(input, "raw measurement iterations");
        row.statistics = read_worker_value<RightMaximalSearchStatistics>(input, "raw statistics");
        row.suffix_link_scan = read_worker_value<detail::SuffixLinkScanSummary>(
            input, "raw suffix-link scan diagnostics");
        raw.push_back(std::move(row));
    }
    if (input.peek() != std::char_traits<char>::eof())
        throw Error(ErrorCode::kBuildFailure, "right-maximal exact match worker result has trailing bytes");
}

void benchmark_internal_isolated(
    const Dataset& dataset,
    const Options& options,
    const std::string& method,
    const std::filesystem::path& scratch,
    std::vector<BuildResult>& builds,
    std::vector<QueryResultRow>& queries,
    std::vector<RawRow>& raw) {
    const auto result_path = scratch / (method + ".worker.bin");
    const auto error_path = scratch / (method + ".worker.error");
    const pid_t pid = fork();
    if (pid < 0) throw Error(ErrorCode::kIoError, "cannot fork right-maximal exact match benchmark worker");
    if (pid == 0) {
        try {
            std::vector<BuildResult> worker_builds;
            std::vector<QueryResultRow> worker_queries;
            std::vector<RawRow> worker_raw;
            benchmark_internal(
                dataset, options, method, scratch,
                worker_builds, worker_queries, worker_raw);
            write_internal_worker_file(result_path, worker_builds, worker_queries, worker_raw);
            _exit(0);
        } catch (const std::exception& error) {
            std::ofstream error_output(error_path);
            error_output << error.what() << '\n';
            error_output.flush();
            _exit(1);
        }
    }
    int status = 0;
    rusage usage{};
    if (wait4(pid, &status, 0, &usage) < 0)
        throw Error(ErrorCode::kIoError, "cannot wait for right-maximal exact match benchmark worker");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ifstream error_input(error_path);
        std::string message;
        std::getline(error_input, message);
        throw Error(ErrorCode::kBuildFailure,
                    message.empty() ? "right-maximal exact match benchmark worker failed" : message);
    }
    const auto build_begin = builds.size();
    read_internal_worker_file(result_path, builds, queries, raw);
    const auto process_peak_rss_mb =
        static_cast<double>(usage.ru_maxrss) / 1024.0;
    for (auto index = build_begin; index < builds.size(); ++index) {
        builds[index].peak_rss_mb = process_peak_rss_mb;
    }
}

struct ProcessResult {
    int status = -1;
    double seconds = 0;
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
    result.peak_rss_mb = static_cast<double>(usage.ru_maxrss) / 1024.0;
    return result;
}

std::vector<RightMaximalMatch> parse_mummer_output(const std::filesystem::path& path,
    const std::map<std::string, SequenceId>& reference_ids,
    const std::vector<SequenceRecord>& queries,
    Strand strand,
    std::vector<std::vector<RightMaximalMatch>>& per_query) {
    std::ifstream input(path);
    if (!input) throw Error(ErrorCode::kIoError, "cannot read MUMmer4 output");
    std::string line;
    std::size_t query_id = 0;
    bool have_query = false;
    std::vector<RightMaximalMatch> all;
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
        all.push_back(match);
    }
    return all;
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
    const auto prefix = scratch / "mummer-index";
    const auto build_stdout = scratch / "mummer-build.stdout";
    const auto build_stderr = scratch / "mummer-build.stderr";
    const auto min_length = options.min_lengths.front();
    std::vector<std::string> common{
        executable,
        options.workload == "mam" ? "-mumreference" : "-maxmatch",
        "-n", "-F", "-k", "1"};
    if (options.workload != "mam") {
        common.insert(common.end(), {"-skip", "1"});
    }
    common.insert(common.end(),
                  {"-kmer", "0", "-threads", "1", "-qthreads", "1"});
    auto build_args = common;
    build_args.insert(build_args.end(), {"-l", std::to_string(min_length), "-save", prefix.string(),
                                         reference_path.string(), nohit_path.string()});
    const auto process = run_process(build_args, build_stdout, build_stderr);
    if (process.status != 0) throw Error(ErrorCode::kBuildFailure, "MUMmer4 index construction failed");
    BuildResult build;
    build.dataset = dataset.name;
    build.method = "mummer4";
    build.algorithm = "mummer4-suffix-link";
    build.acceleration = "K=1,auto-suffix-link";
    build.build_seconds = process.seconds;
    build.peak_rss_mb = process.peak_rss_mb;
    for (const auto& entry : std::filesystem::directory_iterator(scratch))
        if (entry.path().filename().string().find("mummer-index.") == 0) build.serialized_bytes += serialized_size(entry.path());
    builds.push_back(build);

    SuffixArrayBuildOptions verification_build_options;
    verification_build_options.acceleration = SaAcceleration::kNone;
    const auto verification_reference = GenomeReference::FromRecords(dataset.reference);
    const auto verification_index = SuffixArray::Build(verification_reference, verification_build_options);

    const auto repetitions = measured_repetitions(options);
    for (const auto length : options.min_lengths) {
        QueryResultRow summary;
        summary.dataset = dataset.name;
        summary.method = "mummer4";
        summary.algorithm = build.algorithm;
        summary.acceleration = build.acceleration;
        summary.min_length = length;
        summary.query_count = dataset.queries.size();
        summary.query_bases = dataset.query_bases;
        summary.strand = "forward";
        for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
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
            std::uint64_t checksum = checksum_seed();
            std::uint64_t total = 0;
            for (std::size_t query_id = 0; query_id < per_query.size(); ++query_id) {
                auto& matches = per_query[query_id];
                std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
                    return std::tie(left.query_position, left.sequence_id, left.reference_position, left.length, left.strand) <
                           std::tie(right.query_position, right.sequence_id, right.reference_position, right.length, right.strand);
                });
                mix(checksum, query_id);
                mix(checksum, matches.size());
                total += matches.size();
                for (const auto& match : matches) mix_match(checksum, match);
            }
            summary.seconds.push_back(measured.seconds);
            summary.total_matches = total;
            summary.checksum = checksum;
            RawRow raw_row;
            raw_row.dataset = dataset.name;
            raw_row.method = "mummer4";
            raw_row.operation = "load+" + options.workload;
            raw_row.min_length = length;
            raw_row.repetition = repetition;
            raw_row.seconds = measured.seconds;
            raw_row.total_matches = total;
            raw_row.checksum = checksum;
            raw_row.strand = "forward";
            raw.push_back(std::move(raw_row));
        }
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
            mix(mummer_checksum, query_id);
            mix(mummer_checksum, matches.size());
            mummer_total += matches.size();
            for (const auto& match : matches) mix_match(mummer_checksum, match);
            mix(internal_checksum, query_id);
            if (options.workload == "right-maximal") {
                RightMaximalOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm =
                    RightMaximalSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindRightMaximalMatches(
                    dataset.queries[query_id].sequence, internal_options);
                mix(internal_checksum, internal.total_matches);
                internal_total += internal.total_matches;
                for (const auto& match : internal.matches) {
                    mix_match(internal_checksum, match);
                }
            } else if (options.workload == "mem") {
                MemOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm = MemSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindMems(
                    dataset.queries[query_id].sequence, internal_options);
                mix(internal_checksum, internal.total_matches);
                internal_total += internal.total_matches;
                for (const auto& match : internal.matches) {
                    mix_match(internal_checksum, match);
                }
            } else {
                MamOptions internal_options;
                internal_options.min_length = length;
                internal_options.strands = StrandMode::kReverseComplement;
                internal_options.algorithm = MemSearchAlgorithm::kBaseline;
                const auto internal = verification_index.FindMams(
                    dataset.queries[query_id].sequence, internal_options);
                mix(internal_checksum, internal.total_matches);
                internal_total += internal.total_matches;
                for (const auto& match : internal.matches) {
                    mix_match(internal_checksum, match);
                }
            }
        }
        if (mummer_total != internal_total || mummer_checksum != internal_checksum)
            throw Error(ErrorCode::kBuildFailure,
                        "MUMmer4 reverse-complement maximal match correctness mismatch");
        queries.push_back(std::move(summary));
    }
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    if (values.empty()) return 0;
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0 ? (values[middle - 1] + values[middle]) / 2.0 : values[middle];
}

void validate(const std::vector<QueryResultRow>& rows) {
    std::map<std::tuple<std::string, std::uint64_t, std::string>,
             std::pair<std::uint64_t, std::uint64_t>> expected;
    for (const auto& row : rows) {
        if (row.measurement_iterations == 0) {
            throw Error(
                ErrorCode::kBuildFailure,
                "invalid right-maximal benchmark measurement iteration count for " +
                    row.dataset + " method=" + row.method);
        }
        const auto key =
            std::make_tuple(row.dataset, row.min_length, row.strand);
        const auto normalized =
            std::make_pair(row.total_matches, row.checksum);
        const auto it = expected.find(key);
        if (it == expected.end()) expected.emplace(key, normalized);
        else if (it->second != normalized)
            throw Error(ErrorCode::kBuildFailure, "right-maximal exact match benchmark correctness mismatch for " + row.dataset +
                " min_length=" + std::to_string(row.min_length) +
                " strand=" + row.strand + " method=" + row.method);
    }
}

void write_outputs(const Options& options, const std::vector<Dataset>& datasets,
    const std::vector<BuildResult>& builds, const std::vector<QueryResultRow>& queries,
    const std::vector<RawRow>& raw) {
    std::filesystem::create_directories(options.output_directory);
    {
        std::ofstream out(options.output_directory / "run_metadata.tsv");
        out << "profile\tscenario\tseed\tdataset\tdataset_fingerprint\ttotal_bases\tcontigs\tquery_count\tquery_bases"
               "\tlearned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits"
               "\tcompiler\tcmake_version\tbuild_type\tos\tarchitecture\tlogical_cpus\tmummer_version\tmummer_sha256"
               "\tquery_repetitions\tgit_commit\tgit_dirty\tcompile_flags\tcpu_flags"
               "\texecutable_sha256\tcpu_affinity\tsse42_compiled\tsse42_runtime"
               "\tcommand_line_redacted\tpeak_rss_scope\tstrands\tworkload\n";
        for (const auto& dataset : datasets) {
            const auto scenario = dataset.name.find("repeat-rich") != std::string::npos ? "repeat-rich" :
                (dataset.name.find("many-contig") != std::string::npos ? "many-contig" :
                 dataset.name.substr(dataset.name.rfind('-') + 1));
            out << (options.reference ? "user" : options.profile) << '\t' << scenario
            << '\t' << options.seed << '\t' << dataset.name << '\t' << std::hex << dataset.fingerprint << std::dec
            << '\t' << dataset.total_bases << '\t' << dataset.reference.size() << '\t'
            << dataset.queries.size() << '\t' << dataset.query_bases << '\t'
            << options.learned_k << '\t' << options.learned_memory_overhead_basis_points << '\t'
            << (options.learned_bucket_bits ? std::to_string(*options.learned_bucket_bits) : "auto") << '\t'
            << __VERSION__ << '\t' << SUFKIT_BENCH_CMAKE_VERSION << '\t' << SUFKIT_BENCH_BUILD_TYPE
            << "\tLinux\tx86_64\t" << sysconf(_SC_NPROCESSORS_ONLN) << '\t'
            << options.mummer_version << '\t' << options.mummer_sha256 << '\t'
            << measured_repetitions(options) << '\t'
            << options.provenance.git_commit << '\t'
            << options.provenance.git_dirty << '\t'
            << options.provenance.compile_flags << '\t'
            << options.provenance.cpu_flags << '\t'
            << options.provenance.executable_sha256 << '\t'
            << options.provenance.cpu_affinity << '\t'
            << options.provenance.sse42_compiled << '\t'
            << options.provenance.sse42_runtime << '\t'
            << options.provenance.command_line_redacted << '\t'
            << options.provenance.peak_rss_scope << '\t'
            << join(options.strands) << '\t' << options.workload << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "build_results.tsv");
        out << "dataset\tmethod\talgorithm\tsa_acceleration\tbuild_seconds\tsa_build_seconds\tisa_build_seconds\tlcp_build_seconds\tchild_build_seconds\tlearned_index_build_seconds\tsave_seconds\tload_seconds\tpeak_rss_mb\tserialized_bytes\tauxiliary_bytes\tlearned_index_bytes\tbits_per_base\tstatus\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : builds) {
            const auto data = std::find_if(datasets.begin(), datasets.end(), [&](const auto& value) { return value.name == row.dataset; });
            const double bits = data == datasets.end() || data->total_bases == 0 ? 0 :
                static_cast<double>(row.serialized_bytes) * 8.0 / static_cast<double>(data->total_bases);
            out << row.dataset << '\t' << row.method << '\t' << row.algorithm << '\t' << row.acceleration << '\t'
                << row.build_seconds << '\t' << row.sa_build_seconds << '\t' << row.isa_build_seconds << '\t'
                << row.lcp_build_seconds << '\t' << row.child_build_seconds << '\t'
                << row.learned_index_build_seconds << '\t' << row.save_seconds << '\t' << row.load_seconds << '\t'
                << row.peak_rss_mb << '\t' << row.serialized_bytes << '\t' << row.auxiliary_bytes << '\t'
                << row.learned_index_bytes << '\t'
                << bits << '\t' << row.status << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "query_results.tsv");
        out << "dataset\tmethod\talgorithm\tsa_acceleration\tmin_length\tquery_count\tquery_bases\tseconds_median\tseconds_min\tseconds_max\tquery_bases_per_second\tmatches_per_second\ttotal_matches\tresult_checksum\tlookup_calls\tbinary_lookup_calls\tlearned_lookup_calls\tsuffix_link_attempts\tsuffix_link_successes\tsuffix_link_success_rate\tsuffix_link_fallbacks\tprevious_empty_lookups\tlookup_character_comparisons\tlookup_sa_row_accesses\tpredictions\tprediction_error_mean\tprediction_error_max\tlocal_window_rows_mean\tlocal_window_rows_max\tfull_binary_fallbacks\tstatus\tmeasurement_iterations\tsuffix_link_scan_attempts\tsuffix_link_left_scanned_rows\tsuffix_link_right_scanned_rows\tsuffix_link_scanned_rows_p50\tsuffix_link_scanned_rows_p95\tsuffix_link_scanned_rows_p99\tsuffix_link_scanned_rows_max\tsuffix_link_scan_seconds\tsuffix_link_instrumented_wall_seconds\tsuffix_link_scan_instrumented_wall_fraction\tsuffix_link_scan_diagnostics_available\tstrand\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : queries) {
            const auto med = median(row.seconds);
            out << row.dataset << '\t' << row.method << '\t' << row.algorithm << '\t' << row.acceleration << '\t'
                << row.min_length << '\t' << row.query_count << '\t' << row.query_bases << '\t' << med << '\t'
                << *std::min_element(row.seconds.begin(), row.seconds.end()) << '\t'
                << *std::max_element(row.seconds.begin(), row.seconds.end()) << '\t'
                << static_cast<double>(row.query_bases) / med << '\t' << static_cast<double>(row.total_matches) / med << '\t'
                << row.total_matches << '\t' << std::hex << row.checksum << std::dec << '\t'
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
                << row.statistics.lookup.full_binary_fallbacks << '\t' << row.status
                << '\t' << row.measurement_iterations << '\t'
                << row.suffix_link_scan.attempts << '\t'
                << row.suffix_link_scan.left_rows << '\t'
                << row.suffix_link_scan.right_rows << '\t'
                << row.suffix_link_scan.rows_p50 << '\t'
                << row.suffix_link_scan.rows_p95 << '\t'
                << row.suffix_link_scan.rows_p99 << '\t'
                << row.suffix_link_scan.rows_max << '\t'
                << static_cast<double>(row.suffix_link_scan.scan_nanoseconds) /
                       1.0e9
                << '\t'
                << row.suffix_link_scan.instrumented_wall_seconds << '\t'
                << (row.suffix_link_scan.instrumented_wall_seconds <= 0.0
                        ? 0.0
                        : (static_cast<double>(
                               row.suffix_link_scan.scan_nanoseconds) /
                           1.0e9) /
                              row.suffix_link_scan.instrumented_wall_seconds)
                << '\t' << kSuffixLinkScanDiagnosticsAvailable << '\t'
                << row.strand << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "raw_repetitions.tsv");
        out << "dataset\tmethod\toperation\tmin_length\trepetition\tseconds\ttotal_matches\tresult_checksum\tlookup_calls\tbinary_lookup_calls\tlearned_lookup_calls\tsuffix_link_attempts\tsuffix_link_successes\tsuffix_link_fallbacks\tprevious_empty_lookups\tlookup_character_comparisons\tlookup_sa_row_accesses\tpredictions\tprediction_error_sum\tprediction_error_max\tlocal_window_rows\tlocal_window_rows_max\tfull_binary_fallbacks\tstatus\tmeasurement_iterations\tsuffix_link_scan_attempts\tsuffix_link_left_scanned_rows\tsuffix_link_right_scanned_rows\tsuffix_link_scanned_rows_p50\tsuffix_link_scanned_rows_p95\tsuffix_link_scanned_rows_p99\tsuffix_link_scanned_rows_max\tsuffix_link_scan_seconds\tsuffix_link_instrumented_wall_seconds\tsuffix_link_scan_instrumented_wall_fraction\tsuffix_link_scan_diagnostics_available\tstrand\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : raw) out << row.dataset << '\t' << row.method << '\t' << row.operation << '\t'
            << row.min_length << '\t' << row.repetition << '\t' << row.seconds << '\t' << row.total_matches
            << '\t' << std::hex << row.checksum << std::dec << '\t'
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
            << '\t' << row.measurement_iterations << '\t'
            << row.suffix_link_scan.attempts << '\t'
            << row.suffix_link_scan.left_rows << '\t'
            << row.suffix_link_scan.right_rows << '\t'
            << row.suffix_link_scan.rows_p50 << '\t'
            << row.suffix_link_scan.rows_p95 << '\t'
            << row.suffix_link_scan.rows_p99 << '\t'
            << row.suffix_link_scan.rows_max << '\t'
            << static_cast<double>(row.suffix_link_scan.scan_nanoseconds) /
                   1.0e9
            << '\t'
            << row.suffix_link_scan.instrumented_wall_seconds << '\t'
            << (row.suffix_link_scan.instrumented_wall_seconds <= 0.0
                    ? 0.0
                    : (static_cast<double>(
                           row.suffix_link_scan.scan_nanoseconds) /
                       1.0e9) /
                          row.suffix_link_scan.instrumented_wall_seconds)
            << '\t' << kSuffixLinkScanDiagnosticsAvailable << '\t'
            << row.strand << '\n';
    }
}

} // namespace

int run(const std::vector<std::string>& arguments) {
    auto options = parse(arguments);
    options.provenance = bench::CollectBenchmarkProvenance(arguments);
    if (std::find(options.methods.begin(), options.methods.end(), "mummer4") !=
            options.methods.end() &&
        (options.strands.size() != 1 ||
         options.strands.front() != "forward")) {
        std::cerr
            << "warning: MUMmer4 benchmark rows remain forward-only; "
               "reverse-complement is still checked outside the timed row\n";
    }
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
            throw Error(ErrorCode::kUnsupportedBackend,
                        "maximal match benchmark requires MUMmer4 version 4.0.1");
        const auto hash_out = scratch / "mummer-sha256.txt";
        const auto hash_err = scratch / "mummer-sha256.err";
        const auto hash_result = run_process(
            {"/usr/bin/sha256sum", options.mummer4->string()}, hash_out, hash_err);
        std::ifstream hash_input(hash_out);
        hash_input >> options.mummer_sha256;
        if (hash_result.status != 0 || options.mummer_sha256.size() != 64)
            throw Error(ErrorCode::kIoError, "cannot fingerprint the MUMmer4 executable");
    }
    std::vector<Dataset> datasets;
    std::vector<BuildResult> builds;
    std::vector<QueryResultRow> queries;
    std::vector<RawRow> raw;
    const auto scenarios = options.reference ? std::vector<std::string>{"user"} : options.scenarios;
    for (const auto& scenario : scenarios) {
        datasets.push_back(options.reference ? load_dataset(options) : generate_dataset(options, scenario));
        const auto& dataset = datasets.back();
        const auto dataset_scratch = scratch / scenario;
        std::filesystem::create_directories(dataset_scratch);
        for (const auto& method : options.methods) {
            std::cerr << "benchmarking " << dataset.name << " with " << method << "...\n";
            if (method == "mummer4") benchmark_mummer(dataset, options, dataset_scratch, builds, queries, raw);
            else benchmark_internal_isolated(
                dataset, options, method, dataset_scratch, builds, queries, raw);
        }
    }
    write_outputs(options, datasets, builds, queries, raw);
    validate(queries);
    std::error_code cleanup_error;
    std::filesystem::remove_all(scratch, cleanup_error);
    if (cleanup_error)
        throw Error(ErrorCode::kIoError,
                    "cannot remove maximal match benchmark scratch directory");
    std::cerr << options.workload << " benchmark results written to "
              << options.output_directory << '\n';
    return 0;
}

} // namespace sufkit::app::right_maximal_bench
