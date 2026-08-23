#include "benchmark_common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sufkit::app::bench {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kCompleteLocateSafetyLimit = 100000;

struct CpuUsage {
    double user = 0.0;
    double system = 0.0;
};

double timeval_seconds(const timeval& value) {
    return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1000000.0;
}

CpuUsage usage_now() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw Error(ErrorCode::kIoError, "cannot read benchmark process resource usage");
    }
    return {timeval_seconds(usage.ru_utime), timeval_seconds(usage.ru_stime)};
}

CpuUsage usage_delta(const CpuUsage& begin, const CpuUsage& end) {
    return {end.user - begin.user, end.system - begin.system};
}

double elapsed(Clock::time_point begin) {
    return std::chrono::duration<double>(Clock::now() - begin).count();
}

std::string normalize_sequence(std::string sequence) {
    for (auto& raw : sequence) {
        switch (static_cast<char>(std::toupper(static_cast<unsigned char>(raw)))) {
        case 'A': raw = 'A'; break;
        case 'C': raw = 'C'; break;
        case 'G': raw = 'G'; break;
        case 'T': raw = 'T'; break;
        default: raw = 'N'; break;
        }
    }
    return sequence;
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
        default: throw Error(ErrorCode::kInvalidInput, "benchmark query contains a non-ACGT symbol");
        }
    }
    return result;
}

std::vector<SequenceRecord> normalized_records(const std::vector<SequenceRecord>& records) {
    auto result = records;
    for (auto& record : result) record.sequence = normalize_sequence(std::move(record.sequence));
    return result;
}

void collect_naive(
    const std::vector<SequenceRecord>& records,
    const std::string& pattern,
    Strand strand,
    std::vector<Match>& matches) {
    for (std::size_t sequence_id = 0; sequence_id < records.size(); ++sequence_id) {
        auto position = records[sequence_id].sequence.find(pattern);
        while (position != std::string::npos) {
            matches.push_back({
                static_cast<SequenceId>(sequence_id), position, pattern.size(), strand});
            position = records[sequence_id].sequence.find(pattern, position + 1);
        }
    }
}

QueryResult naive_locate(
    const std::vector<SequenceRecord>& records,
    const std::string& raw_pattern,
    StrandMode strands,
    std::optional<std::uint64_t> max_hits) {
    const auto pattern = normalize_sequence(raw_pattern);
    if (pattern.empty() || pattern.find('N') != std::string::npos) {
        throw Error(ErrorCode::kInvalidInput, "benchmark query must contain only A/C/G/T");
    }
    const auto reverse = reverse_complement(pattern);
    std::vector<Match> matches;
    if (strands == StrandMode::kForward) {
        collect_naive(records, pattern, Strand::kForward, matches);
    } else if (strands == StrandMode::kReverseComplement) {
        collect_naive(records, reverse, Strand::kReverseComplement, matches);
    } else if (pattern == reverse) {
        collect_naive(records, pattern, Strand::kBoth, matches);
    } else {
        collect_naive(records, pattern, Strand::kForward, matches);
        collect_naive(records, reverse, Strand::kReverseComplement, matches);
    }
    const auto order = [](const Match& left, const Match& right) {
        return std::tie(left.sequence_id, left.position, left.length, left.strand) <
               std::tie(right.sequence_id, right.position, right.length, right.strand);
    };
    std::sort(matches.begin(), matches.end(), order);
    std::vector<Match> merged;
    for (const auto& match : matches) {
        if (!merged.empty() && merged.back().sequence_id == match.sequence_id &&
            merged.back().position == match.position && merged.back().length == match.length) {
            if (merged.back().strand != match.strand) merged.back().strand = Strand::kBoth;
        } else {
            merged.push_back(match);
        }
    }
    QueryResult result;
    result.total_hits = merged.size();
    const auto limit = max_hits.value_or(result.total_hits);
    const auto kept = std::min<std::uint64_t>(limit, result.total_hits);
    result.hits.assign(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(kept));
    result.truncated = kept < result.total_hits;
    return result;
}

std::uint64_t naive_count(
    const std::vector<SequenceRecord>& records,
    const std::string& raw_pattern,
    StrandMode strands) {
    const auto pattern = normalize_sequence(raw_pattern);
    if (pattern.empty() || pattern.find('N') != std::string::npos) {
        throw Error(ErrorCode::kInvalidInput, "benchmark query must contain only A/C/G/T");
    }
    const auto count_one = [&](const std::string& needle) {
        std::uint64_t count = 0;
        for (const auto& record : records) {
            auto position = record.sequence.find(needle);
            while (position != std::string::npos) {
                ++count;
                position = record.sequence.find(needle, position + 1);
            }
        }
        return count;
    };
    if (strands == StrandMode::kForward) return count_one(pattern);
    const auto reverse = reverse_complement(pattern);
    if (strands == StrandMode::kReverseComplement) return count_one(reverse);
    if (pattern == reverse) return count_one(pattern);
    return count_one(pattern) + count_one(reverse);
}

struct LoadedIndex {
    std::string method;
    std::vector<SequenceRecord> records;
    std::optional<SuffixArray> suffix_array;
    std::optional<FmIndex> fm_index;
    SaSearchAlgorithm sa_algorithm = SaSearchAlgorithm::kAutoSelect;

    std::uint64_t count(const std::string& pattern, StrandMode strands) const {
        if (method == "naive") return naive_count(records, pattern, strands);
        if (suffix_array) return suffix_array->Count(pattern, strands, sa_algorithm);
        return fm_index->Count(pattern, strands);
    }

    std::vector<std::uint64_t> count_batch(
        const std::vector<std::string_view>& patterns,
        StrandMode strands,
        std::uint32_t batch_width) const {
        if (!fm_index) {
            throw Error(ErrorCode::kBuildFailure, "batch count requires an FM-index benchmark method");
        }
        FmBatchOptions options;
        options.strands = strands;
        options.batch_width = batch_width;
        return fm_index->CountBatch(patterns, options);
    }

    QueryResult locate(
        const std::string& pattern,
        StrandMode strands,
        std::optional<std::uint64_t> max_hits) const {
        if (method == "naive") return naive_locate(records, pattern, strands, max_hits);
        LocateOptions options;
        options.strands = strands;
        options.max_hits = max_hits;
        if (suffix_array) return suffix_array->Locate(pattern, options, sa_algorithm);
        return fm_index->Locate(pattern, options);
    }
};

bool is_fm_method(const std::string& method) {
    return method == "fm" || method == "fm-huff" ||
           method == "fm-balanced" || method == "fm-epr";
}

FmBackend fm_backend_for_method(const std::string& method) {
    if (method == "fm" || method == "fm-huff") return FmBackend::kSdslCsaWtHuff;
    if (method == "fm-balanced") return FmBackend::kSdslCsaWtBalanced;
    if (method == "fm-epr") return FmBackend::kSdslCsaWtEpr;
    throw Error(ErrorCode::kInvalidInput, "method is not an FM-index backend: " + method);
}

LoadedIndex load_method(
    const std::string& method,
    const Dataset& dataset,
    const std::filesystem::path& path) {
    LoadedIndex loaded;
    loaded.method = method;
    if (method == "naive") {
        loaded.records = normalized_records(dataset.records);
    } else if (method.rfind("sa32", 0) == 0 || method.rfind("sa64", 0) == 0) {
        loaded.suffix_array.emplace(SuffixArray::Load(path));
        if (method.find("lcp-binary") != std::string::npos)
            loaded.sa_algorithm = SaSearchAlgorithm::kLcpBinary;
        else if (method.find("sapling") != std::string::npos)
            loaded.sa_algorithm = SaSearchAlgorithm::kSaplingPwl;
        else if (method.find("child") != std::string::npos)
            loaded.sa_algorithm = SaSearchAlgorithm::kChild;
        else if (method.find("binary") != std::string::npos)
            loaded.sa_algorithm = SaSearchAlgorithm::kBinary;
    } else if (is_fm_method(method)) {
        loaded.fm_index.emplace(FmIndex::Load(path));
    } else {
        throw Error(ErrorCode::kInvalidInput, "unknown benchmark method: " + method);
    }
    return loaded;
}

StrandMode strand_mode(const std::string& name) {
    if (name == "forward") return StrandMode::kForward;
    if (name == "reverse-complement") return StrandMode::kReverseComplement;
    if (name == "both") return StrandMode::kBoth;
    throw Error(ErrorCode::kBuildFailure, "internal benchmark strand is invalid");
}

std::vector<const QueryCase*> select_queries(
    const Dataset& dataset,
    const QueryGroupSpec& group) {
    std::vector<const QueryCase*> queries;
    for (const auto& query : dataset.queries) {
        if (query.group == group.group && query.pattern_length == group.pattern_length) {
            queries.push_back(&query);
        }
    }
    return queries;
}

void mix_match_checksum(std::uint64_t& checksum, const QueryResult& result) {
    mix_checksum(checksum, result.total_hits);
    for (const auto& match : result.hits) {
        mix_checksum(checksum, match.sequence_id);
        mix_checksum(checksum, match.position);
        mix_checksum(checksum, match.length);
        mix_checksum(checksum, static_cast<std::uint8_t>(match.strand));
    }
}

std::uint64_t nearest_rank_percentile(
    std::vector<std::uint64_t> values,
    std::uint32_t percentile) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto rank = (static_cast<std::uint64_t>(percentile) * values.size() + 99U) / 100U;
    return values[static_cast<std::size_t>(std::max<std::uint64_t>(1, rank) - 1)];
}

QueryRaw measure_group(
    const LoadedIndex& index,
    const std::vector<const QueryCase*>& queries,
    const QueryGroupSpec& group,
    const std::string& strand_name,
    const std::string& operation,
    const LocateLimit& limit,
    std::uint32_t repetition,
    const std::string& fm_query_mode = "scalar",
    std::uint32_t fm_batch_width = 0) {
    QueryRaw result;
    result.group = group.group;
    result.pattern_length = group.pattern_length;
    result.strand = strand_name;
    result.operation = operation;
    result.max_hits = operation == "count" ? "NA" : (limit.all ? "all" : std::to_string(limit.value));
    result.fm_query_mode = fm_query_mode;
    result.fm_batch_width = fm_query_mode == "batch" ? std::to_string(fm_batch_width) : "NA";
    result.repetition = repetition;
    result.query_count = queries.size();
    for (const auto* query : queries) result.query_bases += query->sequence.size();
    if (queries.empty()) {
        result.status = "not_applicable";
        return result;
    }
    const auto strands = strand_mode(strand_name);
    std::vector<std::string_view> batch_patterns;
    if (fm_query_mode == "batch") {
        if (operation != "count") {
            throw Error(ErrorCode::kBuildFailure, "batch benchmark mode only supports count");
        }
        batch_patterns.reserve(queries.size());
        for (const auto* query : queries) batch_patterns.push_back(query->sequence);
    }
    if (operation == "locate" && limit.all) {
        for (const auto* query : queries) {
            if (index.count(query->sequence, strands) > kCompleteLocateSafetyLimit) {
                result.status = "skipped_high_frequency";
                return result;
            }
        }
    }
    const auto cpu_begin = usage_now();
    const auto wall_begin = Clock::now();
    std::uint64_t checksum = checksum_seed();
    std::vector<std::uint64_t> prediction_errors;
    std::vector<std::uint64_t> local_windows;
    if (fm_query_mode == "batch") {
        const auto counts = index.count_batch(batch_patterns, strands, fm_batch_width);
        if (counts.size() != queries.size()) {
            throw Error(ErrorCode::kBuildFailure, "FM batch result cardinality mismatch");
        }
        for (const auto hits : counts) {
            result.total_hits += hits;
            mix_checksum(checksum, hits);
        }
    } else for (const auto* query : queries) {
        SaSearchStatistics search_statistics;
        if (operation == "count") {
            std::uint64_t hits = 0;
            if (index.suffix_array)
                hits = index.suffix_array->Count(
                    query->sequence, strands, index.sa_algorithm, &search_statistics);
            else hits = index.count(query->sequence, strands);
            result.total_hits += hits;
            mix_checksum(checksum, hits);
        } else {
            QueryResult located;
            if (index.suffix_array) {
                LocateOptions locate_options;
                locate_options.strands = strands;
                locate_options.max_hits = limit.all
                    ? std::optional<std::uint64_t>{}
                    : std::optional<std::uint64_t>{limit.value};
                located = index.suffix_array->Locate(
                    query->sequence, locate_options, index.sa_algorithm, &search_statistics);
            } else {
                located = index.locate(
                    query->sequence, strands,
                    limit.all ? std::optional<std::uint64_t>{}
                              : std::optional<std::uint64_t>{limit.value});
            }
            result.total_hits += located.total_hits;
            result.reported_hits += located.hits.size();
            mix_match_checksum(checksum, located);
        }
        result.suffix_comparisons += search_statistics.suffix_comparisons;
        result.character_comparisons += search_statistics.character_comparisons;
        result.gallop_probes += search_statistics.gallop_probes;
        result.local_window_rows += search_statistics.local_window_rows;
        result.local_window_rows_max =
            std::max(result.local_window_rows_max, search_statistics.local_window_rows_max);
        result.predictions += search_statistics.predictions;
        result.prediction_absolute_error_sum += search_statistics.prediction_absolute_error_sum;
        result.prediction_absolute_error_max = std::max(
            result.prediction_absolute_error_max,
            search_statistics.prediction_absolute_error_max);
        result.full_binary_fallbacks += search_statistics.full_binary_fallbacks;
        if (search_statistics.predictions != 0) {
            prediction_errors.push_back(search_statistics.prediction_absolute_error_max);
            local_windows.push_back(search_statistics.local_window_rows_max);
        }
    }
    result.prediction_error_p50 = nearest_rank_percentile(prediction_errors, 50);
    result.prediction_error_p95 = nearest_rank_percentile(prediction_errors, 95);
    result.prediction_error_p99 = nearest_rank_percentile(prediction_errors, 99);
    result.local_window_rows_p50 = nearest_rank_percentile(local_windows, 50);
    result.local_window_rows_p95 = nearest_rank_percentile(local_windows, 95);
    result.local_window_rows_p99 = nearest_rank_percentile(local_windows, 99);
    result.seconds = elapsed(wall_begin);
    const auto cpu = usage_delta(cpu_begin, usage_now());
    result.user_seconds = cpu.user;
    result.system_seconds = cpu.system;
    result.checksum = checksum;
    return result;
}

struct WorkerHeader {
    std::int32_t status = 0;
    char error[512]{};
    char method[32]{};
    char backend[64]{};
    char signature[192]{};
    char sdsl_version[32]{};
    char canonical_index[512]{};
    std::uint8_t coordinate_width = 0;
    std::uint64_t build_count = 0;
    std::uint64_t load_count = 0;
    std::uint64_t query_count = 0;
};

struct BuildWire {
    std::uint32_t repetition = 0;
    double build_seconds = 0.0;
    double build_user_seconds = 0.0;
    double build_system_seconds = 0.0;
    double phase_seconds[5]{};
    double save_seconds = 0.0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    char status[40]{};
};

struct LoadWire {
    std::uint32_t repetition = 0;
    double seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    char status[40]{};
};

struct QueryWire {
    char group[48]{};
    char pattern_length[32]{};
    char strand[32]{};
    char operation[16]{};
    char max_hits[32]{};
    char fm_query_mode[16]{};
    char fm_batch_width[16]{};
    char status[40]{};
    std::uint32_t repetition = 0;
    std::uint64_t query_count = 0;
    std::uint64_t query_bases = 0;
    double seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    std::uint64_t total_hits = 0;
    std::uint64_t reported_hits = 0;
    std::uint64_t checksum = 0;
    std::uint64_t search_statistics[15]{};
};

template <std::size_t Size>
void copy_text(char (&destination)[Size], const std::string& source) {
    std::snprintf(destination, Size, "%s", source.c_str());
}

bool write_exact(int descriptor, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::size_t written = 0;
    while (written < size) {
        const auto amount = ::write(descriptor, bytes + written, size - written);
        if (amount <= 0) return false;
        written += static_cast<std::size_t>(amount);
    }
    return true;
}

bool read_exact(int descriptor, void* data, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    std::size_t received = 0;
    while (received < size) {
        const auto amount = ::read(descriptor, bytes + received, size - received);
        if (amount <= 0) return false;
        received += static_cast<std::size_t>(amount);
    }
    return true;
}

MethodResult run_worker(
    const std::string& method,
    const Dataset& dataset,
    const Options& options,
    const std::filesystem::path& scratch_directory) {
    MethodResult result;
    result.method = method;
    const auto spec = profile_spec(options.profile);
    const auto build_repetitions = options.build_repetitions.value_or(spec.build_repetitions);
    auto query_repetitions = options.query_repetitions.value_or(spec.query_repetitions);
    auto warmups = options.warmups.value_or(spec.warmups);
    if (method == "naive" && options.profile == Profile::quick) {
        if (!options.query_repetitions) query_repetitions = 1;
        if (!options.warmups) warmups = 0;
    }
    const auto canary = std::find_if(dataset.queries.begin(), dataset.queries.end(), [](const auto& query) {
        return query.group == "exact_unique";
    });
    const auto& canary_query = canary == dataset.queries.end()
        ? dataset.queries.front() : *canary;
    std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> canary_result;
    const auto record_canary = [&](const QueryResult& located) {
        auto checksum = checksum_seed();
        mix_match_checksum(checksum, located);
        const auto value = std::make_tuple(
            located.total_hits, static_cast<std::uint64_t>(located.hits.size()), checksum);
        if (canary_result && *canary_result != value) {
            throw Error(ErrorCode::kBuildFailure,
                "query checksum changed between index construction repetitions");
        }
        canary_result = value;
    };

    if (method == "naive") {
        result.backend = "naive";
        result.signature = "std::string::find per normalized contig";
        result.builds.push_back({});
        result.loads.push_back({});
    } else if (method.rfind("sa32", 0) == 0 &&
               dataset.total_bases + dataset.contigs + 1 >
               static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
        result.backend = "divsufsort32";
        result.signature = "libdivsufsort-2.0.2/saidx_t";
        BuildRaw build;
        build.status = "unsupported_input_size";
        result.builds.push_back(build);
        for (const auto& group : dataset.groups) {
            QueryRaw query;
            query.group = group.group;
            query.pattern_length = group.pattern_length;
            query.status = "unsupported_input_size";
            result.queries.push_back(std::move(query));
        }
        return result;
    } else {
        for (std::uint32_t repetition = 0; repetition < build_repetitions; ++repetition) {
            auto reference = GenomeReference::FromRecords(dataset.records);
            const auto index_path = scratch_directory /
                (method + "-" + std::to_string(repetition) + ".sufidx");
            BuildRaw raw;
            raw.repetition = repetition;
            if (method.rfind("sa32", 0) == 0 || method.rfind("sa64", 0) == 0) {
                SuffixArrayBuildOptions build_options;
                build_options.backend = SaBackend::kDivsufsort;
                build_options.coordinate_width = method.rfind("sa32", 0) == 0
                    ? CoordinateWidth::kBits32 : CoordinateWidth::kBits64;
                build_options.acceleration = method == "sa32-none" || method == "sa64-none"
                    ? SaAcceleration::kNone :
                    (method.find("child") != std::string::npos
                        ? SaAcceleration::kFull : SaAcceleration::kLcpSuffixLink);
                if (method.find("sapling") != std::string::npos) {
                    build_options.learned_index.enabled = true;
                    build_options.learned_index.k = options.learned_k;
                    build_options.learned_index.memory_overhead_basis_points =
                        options.learned_memory_overhead_basis_points;
                    build_options.learned_index.bucket_bits = options.learned_bucket_bits;
                }
                SuffixArrayBuildStatistics build_statistics;
                build_options.statistics = &build_statistics;
                const auto cpu_begin = usage_now();
                const auto wall_begin = Clock::now();
                auto index = SuffixArray::Build(reference, build_options);
                raw.build_seconds = elapsed(wall_begin);
                const auto cpu = usage_delta(cpu_begin, usage_now());
                raw.build_user_seconds = cpu.user;
                raw.build_system_seconds = cpu.system;
                raw.sa_build_seconds = build_statistics.sa_seconds;
                raw.isa_build_seconds = build_statistics.isa_seconds;
                raw.lcp_build_seconds = build_statistics.lcp_seconds;
                raw.child_build_seconds = build_statistics.child_seconds;
                raw.learned_index_build_seconds = build_statistics.learned_index_seconds;
                const auto info = index.GetInfo();
                result.backend = info.backend;
                result.signature = info.backend_signature;
                result.coordinate_width = info.coordinate_width;
                raw.learned_index_bytes = info.learned_index_bytes;
                record_canary(index.Locate(canary_query.sequence));
                const auto save_begin = Clock::now();
                index.Save(index_path);
                raw.save_seconds = elapsed(save_begin);
            } else if (is_fm_method(method)) {
                FmIndexBuildOptions build_options;
                build_options.backend = fm_backend_for_method(method);
                const auto cpu_begin = usage_now();
                const auto wall_begin = Clock::now();
                auto index = FmIndex::Build(reference, build_options);
                raw.build_seconds = elapsed(wall_begin);
                const auto cpu = usage_delta(cpu_begin, usage_now());
                raw.build_user_seconds = cpu.user;
                raw.build_system_seconds = cpu.system;
                const auto info = index.GetInfo();
                result.backend = info.backend;
                result.signature = info.backend_signature;
                result.sdsl_version = info.sdsl_version;
                result.coordinate_width = info.coordinate_width;
                record_canary(index.Locate(canary_query.sequence));
                const auto save_begin = Clock::now();
                index.Save(index_path);
                raw.save_seconds = elapsed(save_begin);
            } else {
                throw Error(ErrorCode::kInvalidInput, "unknown benchmark method: " + method);
            }
            raw.serialized_bytes = std::filesystem::file_size(index_path);
            if (repetition == 0) {
                result.canonical_index = index_path;
            } else {
                std::error_code ignored;
                std::filesystem::remove(index_path, ignored);
            }
            result.builds.push_back(raw);
        }

        for (std::uint32_t repetition = 0; repetition < build_repetitions; ++repetition) {
            LoadRaw raw;
            raw.repetition = repetition;
            const auto cpu_begin = usage_now();
            const auto wall_begin = Clock::now();
            if (method.rfind("sa32", 0) == 0 || method.rfind("sa64", 0) == 0) {
                auto index = SuffixArray::Load(result.canonical_index);
                (void)index.GetInfo();
                auto checksum = checksum_seed();
                const auto located = index.Locate(canary_query.sequence);
                mix_match_checksum(checksum, located);
                if (!canary_result || *canary_result != std::make_tuple(
                        located.total_hits, static_cast<std::uint64_t>(located.hits.size()), checksum)) {
                    throw Error(ErrorCode::kBuildFailure,
                        "suffix-array query checksum changed after save/load");
                }
            } else {
                auto index = FmIndex::Load(result.canonical_index);
                (void)index.GetInfo();
                auto checksum = checksum_seed();
                const auto located = index.Locate(canary_query.sequence);
                mix_match_checksum(checksum, located);
                if (!canary_result || *canary_result != std::make_tuple(
                        located.total_hits, static_cast<std::uint64_t>(located.hits.size()), checksum)) {
                    throw Error(ErrorCode::kBuildFailure,
                        "FM-index query checksum changed after save/load");
                }
            }
            raw.seconds = elapsed(wall_begin);
            const auto cpu = usage_delta(cpu_begin, usage_now());
            raw.user_seconds = cpu.user;
            raw.system_seconds = cpu.system;
            result.loads.push_back(raw);
        }
    }

    auto index = load_method(method, dataset, result.canonical_index);
    const bool fm_method = is_fm_method(method);
    const bool scalar_enabled = !fm_method ||
        std::find(options.fm_query_modes.begin(), options.fm_query_modes.end(), "scalar") !=
            options.fm_query_modes.end();
    const bool batch_enabled = fm_method &&
        std::find(options.fm_query_modes.begin(), options.fm_query_modes.end(), "batch") !=
            options.fm_query_modes.end();
    static const std::array<std::string, 3> strands{{
        "forward", "reverse-complement", "both"}};
    for (const auto& group : dataset.groups) {
        const auto queries = select_queries(dataset, group);
        for (const auto& strand : strands) {
            LocateLimit count_limit;
            if (scalar_enabled) {
                for (std::uint32_t warmup = 0; warmup < warmups && !queries.empty(); ++warmup) {
                    (void)measure_group(index, queries, group, strand, "count", count_limit, warmup);
                }
                for (std::uint32_t repetition = 0; repetition < query_repetitions; ++repetition) {
                    result.queries.push_back(measure_group(
                        index, queries, group, strand, "count", count_limit, repetition));
                }
            }
            if (batch_enabled) {
                for (const auto width : options.fm_batch_widths) {
                    for (std::uint32_t warmup = 0; warmup < warmups && !queries.empty(); ++warmup) {
                        (void)measure_group(index, queries, group, strand, "count", count_limit,
                                            warmup, "batch", width);
                    }
                    for (std::uint32_t repetition = 0; repetition < query_repetitions; ++repetition) {
                        result.queries.push_back(measure_group(
                            index, queries, group, strand, "count", count_limit,
                            repetition, "batch", width));
                    }
                }
            }
            if (scalar_enabled) {
                for (const auto& limit : options.locate_limits) {
                    for (std::uint32_t warmup = 0; warmup < warmups && !queries.empty(); ++warmup) {
                        (void)measure_group(index, queries, group, strand, "locate", limit, warmup);
                    }
                    for (std::uint32_t repetition = 0; repetition < query_repetitions; ++repetition) {
                        result.queries.push_back(measure_group(
                            index, queries, group, strand, "locate", limit, repetition));
                    }
                }
            }
        }
    }
    return result;
}

void write_worker_result(int descriptor, const MethodResult& result, const std::string& error) {
    WorkerHeader header;
    header.status = error.empty() ? 0 : 1;
    copy_text(header.error, error);
    copy_text(header.method, result.method);
    copy_text(header.backend, result.backend);
    copy_text(header.signature, result.signature);
    copy_text(header.sdsl_version, result.sdsl_version);
    copy_text(header.canonical_index, result.canonical_index.string());
    header.coordinate_width = result.coordinate_width;
    header.build_count = result.builds.size();
    header.load_count = result.loads.size();
    header.query_count = result.queries.size();
    if (!write_exact(descriptor, &header, sizeof(header))) return;
    for (const auto& raw : result.builds) {
        BuildWire wire;
        wire.repetition = raw.repetition;
        wire.build_seconds = raw.build_seconds;
        wire.build_user_seconds = raw.build_user_seconds;
        wire.build_system_seconds = raw.build_system_seconds;
        wire.phase_seconds[0] = raw.sa_build_seconds;
        wire.phase_seconds[1] = raw.isa_build_seconds;
        wire.phase_seconds[2] = raw.lcp_build_seconds;
        wire.phase_seconds[3] = raw.child_build_seconds;
        wire.phase_seconds[4] = raw.learned_index_build_seconds;
        wire.save_seconds = raw.save_seconds;
        wire.serialized_bytes = raw.serialized_bytes;
        wire.learned_index_bytes = raw.learned_index_bytes;
        copy_text(wire.status, raw.status);
        if (!write_exact(descriptor, &wire, sizeof(wire))) return;
    }
    for (const auto& raw : result.loads) {
        LoadWire wire;
        wire.repetition = raw.repetition;
        wire.seconds = raw.seconds;
        wire.user_seconds = raw.user_seconds;
        wire.system_seconds = raw.system_seconds;
        copy_text(wire.status, raw.status);
        if (!write_exact(descriptor, &wire, sizeof(wire))) return;
    }
    for (const auto& raw : result.queries) {
        QueryWire wire;
        copy_text(wire.group, raw.group);
        copy_text(wire.pattern_length, raw.pattern_length);
        copy_text(wire.strand, raw.strand);
        copy_text(wire.operation, raw.operation);
        copy_text(wire.max_hits, raw.max_hits);
        copy_text(wire.fm_query_mode, raw.fm_query_mode);
        copy_text(wire.fm_batch_width, raw.fm_batch_width);
        copy_text(wire.status, raw.status);
        wire.repetition = raw.repetition;
        wire.query_count = raw.query_count;
        wire.query_bases = raw.query_bases;
        wire.seconds = raw.seconds;
        wire.user_seconds = raw.user_seconds;
        wire.system_seconds = raw.system_seconds;
        wire.total_hits = raw.total_hits;
        wire.reported_hits = raw.reported_hits;
        wire.checksum = raw.checksum;
        wire.search_statistics[0] = raw.suffix_comparisons;
        wire.search_statistics[1] = raw.character_comparisons;
        wire.search_statistics[2] = raw.gallop_probes;
        wire.search_statistics[3] = raw.local_window_rows;
        wire.search_statistics[4] = raw.local_window_rows_max;
        wire.search_statistics[5] = raw.predictions;
        wire.search_statistics[6] = raw.prediction_absolute_error_sum;
        wire.search_statistics[7] = raw.prediction_absolute_error_max;
        wire.search_statistics[8] = raw.full_binary_fallbacks;
        wire.search_statistics[9] = raw.prediction_error_p50;
        wire.search_statistics[10] = raw.prediction_error_p95;
        wire.search_statistics[11] = raw.prediction_error_p99;
        wire.search_statistics[12] = raw.local_window_rows_p50;
        wire.search_statistics[13] = raw.local_window_rows_p95;
        wire.search_statistics[14] = raw.local_window_rows_p99;
        if (!write_exact(descriptor, &wire, sizeof(wire))) return;
    }
}

MethodResult read_worker_result(int descriptor) {
    WorkerHeader header;
    if (!read_exact(descriptor, &header, sizeof(header))) {
        throw Error(ErrorCode::kBuildFailure, "benchmark worker returned a truncated header");
    }
    if (header.status != 0) {
        throw Error(ErrorCode::kBuildFailure, std::string(header.error));
    }
    MethodResult result;
    result.method = header.method;
    result.backend = header.backend;
    result.signature = header.signature;
    result.sdsl_version = header.sdsl_version;
    result.canonical_index = header.canonical_index;
    result.coordinate_width = header.coordinate_width;
    for (std::uint64_t index = 0; index < header.build_count; ++index) {
        BuildWire wire;
        if (!read_exact(descriptor, &wire, sizeof(wire))) throw Error(ErrorCode::kBuildFailure, "truncated build result");
        BuildRaw raw;
        raw.repetition = wire.repetition;
        raw.build_seconds = wire.build_seconds;
        raw.build_user_seconds = wire.build_user_seconds;
        raw.build_system_seconds = wire.build_system_seconds;
        raw.sa_build_seconds = wire.phase_seconds[0];
        raw.isa_build_seconds = wire.phase_seconds[1];
        raw.lcp_build_seconds = wire.phase_seconds[2];
        raw.child_build_seconds = wire.phase_seconds[3];
        raw.learned_index_build_seconds = wire.phase_seconds[4];
        raw.save_seconds = wire.save_seconds;
        raw.serialized_bytes = wire.serialized_bytes;
        raw.learned_index_bytes = wire.learned_index_bytes;
        raw.status = wire.status;
        result.builds.push_back(std::move(raw));
    }
    for (std::uint64_t index = 0; index < header.load_count; ++index) {
        LoadWire wire;
        if (!read_exact(descriptor, &wire, sizeof(wire))) throw Error(ErrorCode::kBuildFailure, "truncated load result");
        result.loads.push_back({wire.repetition, wire.seconds, wire.user_seconds, wire.system_seconds, wire.status});
    }
    for (std::uint64_t index = 0; index < header.query_count; ++index) {
        QueryWire wire;
        if (!read_exact(descriptor, &wire, sizeof(wire))) throw Error(ErrorCode::kBuildFailure, "truncated query result");
        QueryRaw raw;
        raw.group = wire.group;
        raw.pattern_length = wire.pattern_length;
        raw.strand = wire.strand;
        raw.operation = wire.operation;
        raw.max_hits = wire.max_hits;
        raw.fm_query_mode = wire.fm_query_mode;
        raw.fm_batch_width = wire.fm_batch_width;
        raw.status = wire.status;
        raw.repetition = wire.repetition;
        raw.query_count = wire.query_count;
        raw.query_bases = wire.query_bases;
        raw.seconds = wire.seconds;
        raw.user_seconds = wire.user_seconds;
        raw.system_seconds = wire.system_seconds;
        raw.total_hits = wire.total_hits;
        raw.reported_hits = wire.reported_hits;
        raw.checksum = wire.checksum;
        raw.suffix_comparisons = wire.search_statistics[0];
        raw.character_comparisons = wire.search_statistics[1];
        raw.gallop_probes = wire.search_statistics[2];
        raw.local_window_rows = wire.search_statistics[3];
        raw.local_window_rows_max = wire.search_statistics[4];
        raw.predictions = wire.search_statistics[5];
        raw.prediction_absolute_error_sum = wire.search_statistics[6];
        raw.prediction_absolute_error_max = wire.search_statistics[7];
        raw.full_binary_fallbacks = wire.search_statistics[8];
        raw.prediction_error_p50 = wire.search_statistics[9];
        raw.prediction_error_p95 = wire.search_statistics[10];
        raw.prediction_error_p99 = wire.search_statistics[11];
        raw.local_window_rows_p50 = wire.search_statistics[12];
        raw.local_window_rows_p95 = wire.search_statistics[13];
        raw.local_window_rows_p99 = wire.search_statistics[14];
        result.queries.push_back(std::move(raw));
    }
    return result;
}

std::string query_key(const QueryRaw& raw) {
    return raw.group + "\t" + raw.pattern_length + "\t" + raw.strand + "\t" +
           raw.operation + "\t" + raw.max_hits;
}

bool same_matches(const std::vector<Match>& left, const std::vector<Match>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].sequence_id != right[index].sequence_id ||
            left[index].position != right[index].position ||
            left[index].length != right[index].length ||
            left[index].strand != right[index].strand) {
            return false;
        }
    }
    return true;
}

std::string first_difference(
    const QueryResult& expected,
    const QueryResult& observed) {
    const auto size = std::min(expected.hits.size(), observed.hits.size());
    std::size_t index = 0;
    while (index < size) {
        const std::vector<Match> left{expected.hits[index]};
        const std::vector<Match> right{observed.hits[index]};
        if (!same_matches(left, right)) break;
        ++index;
    }
    const auto describe = [](const QueryResult& value, std::size_t position) {
        if (position >= value.hits.size()) return std::string("<end>");
        const auto& match = value.hits[position];
        return std::to_string(match.sequence_id) + ":" + std::to_string(match.position) + ":" +
            std::to_string(static_cast<unsigned>(match.strand));
    };
    return "first coordinate expected=" + describe(expected, index) +
        ", observed=" + describe(observed, index);
}

std::string diagnose_mismatch(
    const Dataset& dataset,
    const MethodResult& expected_result,
    const MethodResult& observed_result,
    const QueryRaw& aggregate) {
    const auto expected = load_method(
        expected_result.method, dataset, expected_result.canonical_index);
    const auto observed = load_method(
        observed_result.method, dataset, observed_result.canonical_index);
    const auto strands = strand_mode(aggregate.strand);
    std::optional<std::uint64_t> limit;
    if (aggregate.operation == "locate" && aggregate.max_hits != "all") {
        limit = static_cast<std::uint64_t>(std::stoull(aggregate.max_hits));
    }
    for (const auto& query : dataset.queries) {
        if (query.group != aggregate.group || query.pattern_length != aggregate.pattern_length) continue;
        if (aggregate.operation == "count") {
            const auto expected_count = expected.count(query.sequence, strands);
            const auto observed_count = observed.count(query.sequence, strands);
            if (expected_count != observed_count) {
                return "query " + query.id + " count expected=" + std::to_string(expected_count) +
                    ", observed=" + std::to_string(observed_count);
            }
        } else {
            const auto expected_matches = expected.locate(query.sequence, strands, limit);
            const auto observed_matches = observed.locate(query.sequence, strands, limit);
            if (expected_matches.total_hits != observed_matches.total_hits ||
                !same_matches(expected_matches.hits, observed_matches.hits)) {
                return "query " + query.id + " " + first_difference(expected_matches, observed_matches);
            }
        }
    }
    return "aggregate checksum differs, but per-query replay found no coordinate mismatch";
}

} // namespace

MethodResult run_method_isolated(
    const std::string& method,
    const Dataset& dataset,
    const Options& options,
    const std::filesystem::path& scratch_directory) {
    int descriptors[2]{};
    if (pipe(descriptors) != 0) throw Error(ErrorCode::kIoError, "cannot create benchmark worker pipe");
    const auto pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw Error(ErrorCode::kIoError, "cannot create benchmark worker process");
    }
    if (pid == 0) {
        close(descriptors[0]);
        MethodResult result;
        std::string error;
        try {
            result = run_worker(method, dataset, options, scratch_directory);
        } catch (const std::exception& exception) {
            result.method = method;
            error = exception.what();
        }
        write_worker_result(descriptors[1], result, error);
        close(descriptors[1]);
        _exit(error.empty() ? 0 : 1);
    }
    close(descriptors[1]);
    MethodResult result;
    std::string receive_error;
    try {
        result = read_worker_result(descriptors[0]);
    } catch (const std::exception& error) {
        receive_error = error.what();
    }
    close(descriptors[0]);
    int status = 0;
    struct rusage usage {};
    const auto waited = wait4(pid, &status, 0, &usage);
    if (!receive_error.empty()) throw Error(ErrorCode::kBuildFailure, receive_error);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw Error(ErrorCode::kBuildFailure, "benchmark worker failed: " + method);
    }
    result.peak_rss_mb = static_cast<double>(usage.ru_maxrss) / 1024.0;
    return result;
}

void validate_results(
    const Dataset& dataset,
    const std::vector<MethodResult>& results,
    Profile profile) {
    if (results.empty()) throw Error(ErrorCode::kBuildFailure, "benchmark produced no method results");
    for (const auto& result : results) {
        std::map<std::string, std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> expected;
        for (const auto& raw : result.queries) {
            if (raw.status != "ok") continue;
            const auto key = query_key(raw);
            const auto value = std::make_tuple(raw.total_hits, raw.reported_hits, raw.checksum);
            const auto inserted = expected.emplace(key, value);
            if (!inserted.second && inserted.first->second != value) {
                throw Error(ErrorCode::kBuildFailure,
                    "non-deterministic benchmark result for " + result.method + " at " + key);
            }
            if ((raw.group == "n_boundary" || raw.group == "contig_boundary") &&
                raw.operation == "count" && raw.total_hits != 0) {
                throw Error(ErrorCode::kBuildFailure,
                    "boundary query unexpectedly matched for " + result.method + " at " + key);
            }
        }
    }

    std::map<std::string, std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, std::string>> baseline;
    for (const auto& result : results) {
        for (const auto& raw : result.queries) {
            if (raw.repetition != 0 || raw.status != "ok") continue;
            const auto key = query_key(raw);
            const auto value = std::make_tuple(raw.total_hits, raw.reported_hits, raw.checksum, result.method);
            const auto inserted = baseline.emplace(key, value);
            if (!inserted.second &&
                std::tie(raw.total_hits, raw.reported_hits, raw.checksum) !=
                std::tie(std::get<0>(inserted.first->second), std::get<1>(inserted.first->second),
                         std::get<2>(inserted.first->second))) {
                const auto expected_method = std::get<3>(inserted.first->second);
                const auto expected_result = std::find_if(
                    results.begin(), results.end(), [&](const auto& candidate) {
                        return candidate.method == expected_method;
                    });
                const auto detail = expected_result == results.end()
                    ? std::string("baseline method result is unavailable")
                    : diagnose_mismatch(dataset, *expected_result, result, raw);
                throw Error(ErrorCode::kBuildFailure,
                    "benchmark correctness mismatch at " + key + " between " +
                    expected_method + " and " + result.method + "; " + detail);
            }
        }
    }

    const bool has_naive = std::any_of(results.begin(), results.end(), [](const auto& result) {
        return result.method == "naive";
    });
    if ((profile == Profile::standard || profile == Profile::full) && !has_naive) {
        const auto indexed = std::find_if(results.begin(), results.end(), [](const auto& result) {
            return !result.canonical_index.empty();
        });
        if (indexed != results.end()) {
            auto normalized = normalized_records(dataset.records);
            auto index = load_method(indexed->method, dataset, indexed->canonical_index);
            for (const auto& group : dataset.groups) {
                const auto queries = select_queries(dataset, group);
                if (queries.empty()) continue;
                const auto* query = queries.front();
                for (const auto strands : {StrandMode::kForward, StrandMode::kReverseComplement, StrandMode::kBoth}) {
                    const auto expected = naive_locate(normalized, query->sequence, strands, 1000);
                    const auto observed = index.locate(query->sequence, strands, 1000);
                    if (expected.total_hits != observed.total_hits ||
                        !same_matches(expected.hits, observed.hits)) {
                        throw Error(ErrorCode::kBuildFailure,
                            "naive sample mismatch for query " + query->id + " using " + indexed->method);
                    }
                }
            }
        }
    }
}

} // namespace sufkit::app::bench
