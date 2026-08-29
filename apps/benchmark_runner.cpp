#include "benchmark_common.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sufkit::app::bench {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kCompleteLocateSafetyLimit = 100000;
constexpr const char* kSaveWorkerStatus = "save_worker_load_plus_save";

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

double peak_rss_mb(const struct rusage& usage) {
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
}

double current_process_peak_rss_mb() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw Error(ErrorCode::kIoError, "cannot read benchmark worker peak RSS");
    }
    return peak_rss_mb(usage);
}

bool is_sa_method(const std::string& method) {
    return method.rfind("sa32", 0) == 0 || method.rfind("sa64", 0) == 0 ||
           method == "caps32" || method == "caps64";
}

bool is_32bit_sa_method(const std::string& method) {
    return method.rfind("sa32", 0) == 0 || method == "caps32";
}

bool is_caps_method(const std::string& method) {
    return method == "caps32" || method == "caps64";
}

SaResourceProfile sa_resource_profile_for_method(
    const std::string& method) {
    return method.find("low-memory") == std::string::npos
        ? SaResourceProfile::kFast
        : SaResourceProfile::kLowMemory;
}

CoordinateStorageWidth sa_storage_width_for_method(
    const std::string& method) {
    if (method.find("store32") != std::string::npos)
        return CoordinateStorageWidth::kBits32;
    if (method.find("store40") != std::string::npos)
        return CoordinateStorageWidth::kBits40;
    if (method.find("store48") != std::string::npos)
        return CoordinateStorageWidth::kBits48;
    if (method.find("store64") != std::string::npos)
        return CoordinateStorageWidth::kBits64;
    return CoordinateStorageWidth::kAutoSelect;
}

std::uint32_t sampling_rate_for_method(const std::string& method) {
    if (method == "sa32-sampled-k2" || method == "sa64-sampled-k2") return 2;
    if (method == "sa32-sampled-k4" || method == "sa64-sampled-k4") return 4;
    if (method == "sa32-sampled-k8" || method == "sa64-sampled-k8") return 8;
    return 1;
}

std::uint64_t allocated_disk_bytes(const std::filesystem::path& path) {
    struct stat information {};
    if (::stat(path.c_str(), &information) != 0) {
        throw Error(ErrorCode::kIoError,
            "cannot stat benchmark index for allocated size: " + path.string());
    }
    return static_cast<std::uint64_t>(information.st_blocks) * 512ULL;
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

const std::vector<std::uint32_t>& fm_batch_widths_for_method(
    const Options& options,
    const std::string& method) {
    const auto canonical = method == "fm" ? std::string{"fm-huff"} : method;
    const auto found = options.fm_batch_width_overrides.find(canonical);
    return found == options.fm_batch_width_overrides.end()
        ? options.fm_batch_widths : found->second;
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
    } else if (is_sa_method(method)) {
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
    std::vector<const QueryCase*> measured_queries = queries;
    const auto strands = strand_mode(strand_name);
    if (operation == "locate" && limit.all) {
        measured_queries.erase(
            std::remove_if(measured_queries.begin(), measured_queries.end(), [&](const auto* query) {
                const bool skip = index.count(query->sequence, strands) >
                    kCompleteLocateSafetyLimit;
                if (skip) ++result.skipped_high_frequency_queries;
                return skip;
            }),
            measured_queries.end());
    }
    result.query_count = measured_queries.size();
    for (const auto* query : measured_queries) result.query_bases += query->sequence.size();
    if (measured_queries.empty()) {
        result.status = result.skipped_high_frequency_queries == 0
            ? "not_applicable" : "skipped_high_frequency";
        return result;
    }
    std::vector<std::string_view> batch_patterns;
    if (fm_query_mode == "batch") {
        if (operation != "count") {
            throw Error(ErrorCode::kBuildFailure, "batch benchmark mode only supports count");
        }
        batch_patterns.reserve(measured_queries.size());
        for (const auto* query : measured_queries) batch_patterns.push_back(query->sequence);
    }
    const auto cpu_begin = usage_now();
    const auto wall_begin = Clock::now();
    std::uint64_t checksum = checksum_seed();
    std::vector<std::uint64_t> prediction_errors;
    std::vector<std::uint64_t> local_windows;
    if (fm_query_mode == "batch") {
        const auto counts = index.count_batch(batch_patterns, strands, fm_batch_width);
        if (counts.size() != measured_queries.size()) {
            throw Error(ErrorCode::kBuildFailure, "FM batch result cardinality mismatch");
        }
        for (const auto hits : counts) {
            result.total_hits += hits;
            mix_checksum(checksum, hits);
        }
    } else for (const auto* query : measured_queries) {
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
    std::uint8_t stored_coordinate_width = 0;
    std::uint8_t sa_resource_profile = 0;
    std::uint8_t lcp_encoding = 0;
    std::uint64_t sa_bytes = 0;
    std::uint64_t isa_bytes = 0;
    std::uint64_t lcp_bytes = 0;
    std::uint64_t resident_core_bytes = 0;
    std::uint32_t sa_sampling_rate = 1;
    std::uint32_t threads = 1;
    std::uint8_t has_canary = 0;
    std::uint64_t canary_total_hits = 0;
    std::uint64_t canary_reported_hits = 0;
    std::uint64_t canary_checksum = 0;
    std::uint64_t build_count = 0;
    std::uint64_t load_count = 0;
    std::uint64_t query_count = 0;
};

struct BuildWire {
    std::uint32_t repetition = 0;
    double build_seconds = 0.0;
    double build_user_seconds = 0.0;
    double build_system_seconds = 0.0;
    double phase_seconds[6]{};
    double save_seconds = 0.0;
    double peak_rss_mb = 0.0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t allocated_disk_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    char status[40]{};
};

struct LoadWire {
    std::uint32_t repetition = 0;
    double seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    double peak_rss_mb = 0.0;
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
    std::uint64_t skipped_high_frequency_queries = 0;
    std::uint64_t query_bases = 0;
    double seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    double peak_rss_mb = 0.0;
    std::uint64_t total_hits = 0;
    std::uint64_t reported_hits = 0;
    std::uint64_t checksum = 0;
    std::uint64_t search_statistics[15]{};
};

enum class WorkerPhase : std::uint8_t { build, save, load, query };

struct WorkerRequest {
    WorkerPhase phase = WorkerPhase::build;
    std::string method;
    Dataset dataset;
    Options options;
    // Parent-side requests borrow the controller's immutable dataset and
    // options while serializing. A clean-exec worker owns the decoded copies.
    const Dataset* parent_dataset = nullptr;
    const Options* parent_options = nullptr;
    std::filesystem::path scratch_directory;
    std::uint32_t repetition = 0;
    std::filesystem::path canonical_index;
    bool has_canary = false;
    std::uint64_t canary_total_hits = 0;
    std::uint64_t canary_reported_hits = 0;
    std::uint64_t canary_checksum = 0;
    bool count_worker = false;
    LocateLimit locate_limit;
};

constexpr std::uint64_t kWorkerRequestMagic = 0x315145524b465553ULL;
constexpr std::uint32_t kWorkerRequestVersion = 1;
constexpr std::uint64_t kMaximumWorkerCollectionSize = 1ULL << 32U;

template <class Value>
void write_request_value(std::ostream& output, const Value& value) {
    static_assert(std::is_trivially_copyable<Value>::value,
        "benchmark worker request values must be trivially copyable");
    output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    if (!output) {
        throw Error(ErrorCode::kIoError,
            "cannot write benchmark worker request");
    }
}

template <class Value>
Value read_request_value(std::istream& input, const char* label) {
    static_assert(std::is_trivially_copyable<Value>::value,
        "benchmark worker request values must be trivially copyable");
    Value value{};
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    if (!input) {
        throw Error(ErrorCode::kBuildFailure,
            std::string("truncated benchmark worker request: ") + label);
    }
    return value;
}

void write_request_string(std::ostream& output, const std::string& value) {
    write_request_value(output, static_cast<std::uint64_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    if (!output) {
        throw Error(ErrorCode::kIoError,
            "cannot write benchmark worker request string");
    }
}

std::string read_request_string(std::istream& input, const char* label) {
    const auto size = read_request_value<std::uint64_t>(input, label);
    if (size > kMaximumWorkerCollectionSize ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw Error(ErrorCode::kBuildFailure,
            std::string("invalid benchmark worker string size: ") + label);
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    input.read(value.data(), static_cast<std::streamsize>(value.size()));
    if (!input) {
        throw Error(ErrorCode::kBuildFailure,
            std::string("truncated benchmark worker request string: ") + label);
    }
    return value;
}

template <class Value, class Writer>
void write_request_vector(
    std::ostream& output, const std::vector<Value>& values, Writer&& writer) {
    write_request_value(output, static_cast<std::uint64_t>(values.size()));
    for (const auto& value : values) writer(output, value);
}

template <class Value, class Reader>
std::vector<Value> read_request_vector(
    std::istream& input, const char* label, Reader&& reader) {
    const auto count = read_request_value<std::uint64_t>(input, label);
    if (count > kMaximumWorkerCollectionSize ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw Error(ErrorCode::kBuildFailure,
            std::string("invalid benchmark worker collection size: ") + label);
    }
    std::vector<Value> values;
    values.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0; index < count; ++index) {
        values.push_back(reader(input));
    }
    return values;
}

void write_optional_u32(
    std::ostream& output, const std::optional<std::uint32_t>& value) {
    write_request_value(output, static_cast<std::uint8_t>(value.has_value()));
    if (value) write_request_value(output, *value);
}

std::optional<std::uint32_t> read_optional_u32(
    std::istream& input, const char* label) {
    const auto present = read_request_value<std::uint8_t>(input, label);
    if (present > 1) {
        throw Error(ErrorCode::kBuildFailure,
            std::string("invalid benchmark worker optional flag: ") + label);
    }
    if (present == 0) return std::nullopt;
    return read_request_value<std::uint32_t>(input, label);
}

void write_sequence_record(std::ostream& output, const SequenceRecord& value) {
    write_request_string(output, value.name);
    write_request_string(output, value.description);
    write_request_string(output, value.sequence);
}

SequenceRecord read_sequence_record(std::istream& input) {
    SequenceRecord value;
    value.name = read_request_string(input, "sequence name");
    value.description = read_request_string(input, "sequence description");
    value.sequence = read_request_string(input, "sequence bases");
    return value;
}

void write_query_case(std::ostream& output, const QueryCase& value) {
    write_request_string(output, value.id);
    write_request_string(output, value.sequence);
    write_request_string(output, value.group);
    write_request_string(output, value.pattern_length);
    write_request_string(output, value.source);
}

QueryCase read_query_case(std::istream& input) {
    QueryCase value;
    value.id = read_request_string(input, "query id");
    value.sequence = read_request_string(input, "query sequence");
    value.group = read_request_string(input, "query group");
    value.pattern_length = read_request_string(input, "query pattern length");
    value.source = read_request_string(input, "query source");
    return value;
}

void write_query_group(std::ostream& output, const QueryGroupSpec& value) {
    write_request_string(output, value.group);
    write_request_string(output, value.pattern_length);
}

QueryGroupSpec read_query_group(std::istream& input) {
    QueryGroupSpec value;
    value.group = read_request_string(input, "query group name");
    value.pattern_length = read_request_string(input, "query group length");
    return value;
}

void write_dataset(
    std::ostream& output, const Dataset& value, WorkerPhase phase,
    const std::string& method) {
    write_request_string(output, value.name);
    write_request_value(output, static_cast<std::uint8_t>(value.scenario));
    const bool query_phase = phase == WorkerPhase::query;
    const bool needs_records = phase == WorkerPhase::build ||
        (query_phase && method == "naive");
    if (needs_records) {
        write_request_vector(output, value.records, write_sequence_record);
    } else {
        write_request_value(output, std::uint64_t{0});
    }
    if (query_phase) {
        write_request_vector(output, value.queries, write_query_case);
    } else if (phase == WorkerPhase::build || phase == WorkerPhase::load) {
        const auto canary = std::find_if(
            value.queries.begin(), value.queries.end(), [](const auto& query) {
                return query.group == "exact_unique";
            });
        const auto selected = canary == value.queries.end()
            ? value.queries.begin() : canary;
        if (selected == value.queries.end()) {
            write_request_value(output, std::uint64_t{0});
        } else {
            write_request_value(output, std::uint64_t{1});
            write_query_case(output, *selected);
        }
    } else {
        write_request_value(output, std::uint64_t{0});
    }
    if (query_phase) {
        write_request_vector(output, value.groups, write_query_group);
    } else {
        write_request_value(output, std::uint64_t{0});
    }
    write_request_value(output, value.fingerprint);
    write_request_value(output, value.total_bases);
    write_request_value(output, value.contigs);
    write_request_value(output, value.gc_fraction);
    write_request_value(output, value.ambiguous_fraction);
    write_request_value(output, value.repeat_fraction);
    write_request_value(output, value.reference_seconds);
    write_request_value(output, value.normalization_seconds);
}

Dataset read_dataset(std::istream& input) {
    Dataset value;
    value.name = read_request_string(input, "dataset name");
    const auto scenario = read_request_value<std::uint8_t>(input, "scenario");
    if (scenario > static_cast<std::uint8_t>(Scenario::user)) {
        throw Error(ErrorCode::kBuildFailure,
            "invalid scenario in benchmark worker request");
    }
    value.scenario = static_cast<Scenario>(scenario);
    value.records = read_request_vector<SequenceRecord>(
        input, "reference records", read_sequence_record);
    value.queries = read_request_vector<QueryCase>(
        input, "queries", read_query_case);
    value.groups = read_request_vector<QueryGroupSpec>(
        input, "query groups", read_query_group);
    value.fingerprint = read_request_value<std::uint64_t>(input, "fingerprint");
    value.total_bases = read_request_value<std::uint64_t>(input, "total bases");
    value.contigs = read_request_value<std::uint64_t>(input, "contig count");
    value.gc_fraction = read_request_value<double>(input, "GC fraction");
    value.ambiguous_fraction =
        read_request_value<double>(input, "ambiguous fraction");
    value.repeat_fraction = read_request_value<double>(input, "repeat fraction");
    value.reference_seconds =
        read_request_value<double>(input, "reference seconds");
    value.normalization_seconds =
        read_request_value<double>(input, "normalization seconds");
    return value;
}

void write_string_vector(
    std::ostream& output, const std::vector<std::string>& values) {
    write_request_vector(output, values,
        [](std::ostream& stream, const std::string& value) {
            write_request_string(stream, value);
        });
}

std::vector<std::string> read_string_vector(
    std::istream& input, const char* label) {
    return read_request_vector<std::string>(input, label,
        [](std::istream& stream) {
            return read_request_string(stream, "string vector value");
        });
}

void write_u32_vector(
    std::ostream& output, const std::vector<std::uint32_t>& values) {
    write_request_vector(output, values,
        [](std::ostream& stream, std::uint32_t value) {
            write_request_value(stream, value);
        });
}

std::vector<std::uint32_t> read_u32_vector(
    std::istream& input, const char* label) {
    return read_request_vector<std::uint32_t>(input, label,
        [](std::istream& stream) {
            return read_request_value<std::uint32_t>(stream, "u32 vector value");
        });
}

void write_options(std::ostream& output, const Options& value) {
    write_request_value(output, static_cast<std::uint8_t>(value.profile));
    write_optional_u32(output, value.build_repetitions);
    write_optional_u32(output, value.query_repetitions);
    write_optional_u32(output, value.warmups);
    write_request_value(output, value.sa_threads);
    write_request_value(output, value.learned_k);
    write_request_value(output, value.learned_memory_overhead_basis_points);
    write_optional_u32(output, value.learned_bucket_bits);
    write_string_vector(output, value.fm_query_modes);
    write_u32_vector(output, value.fm_batch_widths);
    write_request_value(output,
        static_cast<std::uint64_t>(value.fm_batch_width_overrides.size()));
    for (const auto& [method, widths] : value.fm_batch_width_overrides) {
        write_request_string(output, method);
        write_u32_vector(output, widths);
    }
}

Options read_options(std::istream& input) {
    Options value;
    const auto profile = read_request_value<std::uint8_t>(input, "profile");
    if (profile > static_cast<std::uint8_t>(Profile::user)) {
        throw Error(ErrorCode::kBuildFailure,
            "invalid profile in benchmark worker request");
    }
    value.profile = static_cast<Profile>(profile);
    value.build_repetitions = read_optional_u32(input, "build repetitions");
    value.query_repetitions = read_optional_u32(input, "query repetitions");
    value.warmups = read_optional_u32(input, "warmups");
    value.sa_threads = read_request_value<std::uint32_t>(input, "SA threads");
    value.learned_k = read_request_value<std::uint32_t>(input, "learned k");
    value.learned_memory_overhead_basis_points =
        read_request_value<std::uint32_t>(input, "learned memory basis points");
    value.learned_bucket_bits =
        read_optional_u32(input, "learned bucket bits");
    value.fm_query_modes = read_string_vector(input, "FM query modes");
    value.fm_batch_widths = read_u32_vector(input, "FM batch widths");
    const auto override_count =
        read_request_value<std::uint64_t>(input, "FM override count");
    if (override_count > kMaximumWorkerCollectionSize) {
        throw Error(ErrorCode::kBuildFailure,
            "invalid FM override count in benchmark worker request");
    }
    value.fm_batch_width_overrides.clear();
    for (std::uint64_t index = 0; index < override_count; ++index) {
        auto method = read_request_string(input, "FM override method");
        auto widths = read_u32_vector(input, "FM override widths");
        if (!value.fm_batch_width_overrides.emplace(
                std::move(method), std::move(widths)).second) {
            throw Error(ErrorCode::kBuildFailure,
                "duplicate FM override in benchmark worker request");
        }
    }
    return value;
}

void write_worker_request(
    const std::filesystem::path& path, const WorkerRequest& request) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw Error(ErrorCode::kIoError,
            "cannot create benchmark worker request: " + path.string());
    }
    write_request_value(output, kWorkerRequestMagic);
    write_request_value(output, kWorkerRequestVersion);
    write_request_value(output, static_cast<std::uint8_t>(request.phase));
    write_request_string(output, request.method);
    write_dataset(output,
        request.parent_dataset == nullptr ? request.dataset
                                          : *request.parent_dataset,
        request.phase, request.method);
    write_options(output,
        request.parent_options == nullptr ? request.options
                                          : *request.parent_options);
    write_request_string(output, request.scratch_directory.string());
    write_request_value(output, request.repetition);
    write_request_string(output, request.canonical_index.string());
    write_request_value(output, static_cast<std::uint8_t>(request.has_canary));
    write_request_value(output, request.canary_total_hits);
    write_request_value(output, request.canary_reported_hits);
    write_request_value(output, request.canary_checksum);
    write_request_value(output, static_cast<std::uint8_t>(request.count_worker));
    write_request_value(output, static_cast<std::uint8_t>(request.locate_limit.all));
    write_request_value(output, request.locate_limit.value);
    output.flush();
    if (!output) {
        throw Error(ErrorCode::kIoError,
            "cannot finish benchmark worker request: " + path.string());
    }
}

WorkerRequest read_worker_request(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error(ErrorCode::kIoError,
            "cannot open benchmark worker request: " + path.string());
    }
    if (read_request_value<std::uint64_t>(input, "magic") !=
            kWorkerRequestMagic ||
        read_request_value<std::uint32_t>(input, "version") !=
            kWorkerRequestVersion) {
        throw Error(ErrorCode::kBuildFailure,
            "unsupported benchmark worker request format");
    }
    WorkerRequest request;
    const auto phase = read_request_value<std::uint8_t>(input, "phase");
    if (phase > static_cast<std::uint8_t>(WorkerPhase::query)) {
        throw Error(ErrorCode::kBuildFailure,
            "invalid benchmark worker phase");
    }
    request.phase = static_cast<WorkerPhase>(phase);
    request.method = read_request_string(input, "method");
    request.dataset = read_dataset(input);
    request.options = read_options(input);
    request.scratch_directory =
        read_request_string(input, "scratch directory");
    request.repetition =
        read_request_value<std::uint32_t>(input, "repetition");
    request.canonical_index = read_request_string(input, "canonical index");
    request.has_canary =
        read_request_value<std::uint8_t>(input, "has canary") != 0;
    request.canary_total_hits =
        read_request_value<std::uint64_t>(input, "canary total hits");
    request.canary_reported_hits =
        read_request_value<std::uint64_t>(input, "canary reported hits");
    request.canary_checksum =
        read_request_value<std::uint64_t>(input, "canary checksum");
    request.count_worker =
        read_request_value<std::uint8_t>(input, "count worker") != 0;
    request.locate_limit.all =
        read_request_value<std::uint8_t>(input, "locate all") != 0;
    request.locate_limit.value =
        read_request_value<std::uint64_t>(input, "locate limit");
    if (input.peek() != std::char_traits<char>::eof()) {
        throw Error(ErrorCode::kBuildFailure,
            "benchmark worker request has trailing bytes");
    }
    return request;
}

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

const QueryCase& canary_query_for(const Dataset& dataset) {
    const auto canary = std::find_if(dataset.queries.begin(), dataset.queries.end(), [](const auto& query) {
        return query.group == "exact_unique";
    });
    if (canary != dataset.queries.end()) return *canary;
    if (dataset.queries.empty()) {
        throw Error(ErrorCode::kInvalidInput, "benchmark dataset has no queries");
    }
    return dataset.queries.front();
}

void record_canary(MethodResult& result, const QueryResult& located) {
    auto checksum = checksum_seed();
    mix_match_checksum(checksum, located);
    result.has_canary = true;
    result.canary_total_hits = located.total_hits;
    result.canary_reported_hits = located.hits.size();
    result.canary_checksum = checksum;
}

void verify_canary(const MethodResult& expected, const QueryResult& located, const std::string& phase) {
    auto checksum = checksum_seed();
    mix_match_checksum(checksum, located);
    if (!expected.has_canary ||
        expected.canary_total_hits != located.total_hits ||
        expected.canary_reported_hits != located.hits.size() ||
        expected.canary_checksum != checksum) {
        throw Error(ErrorCode::kBuildFailure,
            "query checksum changed during isolated " + phase + " worker");
    }
}

MethodResult run_build_worker(
    const std::string& method,
    const Dataset& dataset,
    const Options& options,
    const std::filesystem::path& scratch_directory,
    std::uint32_t repetition) {
    MethodResult result;
    result.method = method;
    result.sa_sampling_rate = sampling_rate_for_method(method);
    result.threads = is_caps_method(method) ? options.sa_threads : 1;
    const auto& canary_query = canary_query_for(dataset);

    if (method == "naive") {
        result.backend = "naive";
        result.signature = "std::string::find per normalized contig";
        BuildRaw raw;
        raw.repetition = repetition;
        result.builds.push_back(raw);
        return result;
    } else if (is_32bit_sa_method(method) &&
               dataset.total_bases + dataset.contigs + 1 >
               (is_caps_method(method)
                    ? static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
                    : static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))) {
        result.backend = is_caps_method(method) ? "caps32" : "divsufsort32";
        result.signature = is_caps_method(method)
            ? "CaPS-SA/uint32_t" : "libdivsufsort-2.0.2/saidx_t";
        BuildRaw build;
        build.repetition = repetition;
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
    }

    auto reference = GenomeReference::FromRecords(dataset.records);
    const auto canonical_index = scratch_directory / (method + "-0.sufidx");
    BuildRaw raw;
    raw.repetition = repetition;
    if (is_sa_method(method)) {
        SuffixArrayBuildOptions build_options;
        build_options.backend = is_caps_method(method) ? SaBackend::kCaps : SaBackend::kDivsufsort;
        build_options.coordinate_width = is_32bit_sa_method(method)
            ? CoordinateWidth::kBits32 : CoordinateWidth::kBits64;
        build_options.storage_width = sa_storage_width_for_method(method);
        build_options.resource_profile = sa_resource_profile_for_method(method);
        build_options.threads = result.threads;
        build_options.sampling_rate = sampling_rate_for_method(method);
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
        raw.storage_compaction_seconds =
            build_statistics.storage_compaction_seconds;
        raw.isa_build_seconds = build_statistics.isa_seconds;
        raw.lcp_build_seconds = build_statistics.lcp_seconds;
        raw.child_build_seconds = build_statistics.child_seconds;
        raw.learned_index_build_seconds = build_statistics.learned_index_seconds;
        const auto info = index.GetInfo();
        result.backend = info.backend;
        result.signature = info.backend_signature;
        result.coordinate_width = info.coordinate_width;
        result.stored_coordinate_width = info.stored_coordinate_width;
        result.sa_resource_profile = info.sa_resource_profile;
        result.lcp_encoding = info.lcp_encoding;
        result.sa_bytes = info.sa_bytes;
        result.isa_bytes = info.isa_bytes;
        result.lcp_bytes = info.lcp_bytes;
        result.resident_core_bytes = info.resident_core_bytes;
        result.sa_sampling_rate = info.sa_sampling_rate;
        raw.learned_index_bytes = info.learned_index_bytes;
        raw.peak_rss_mb = current_process_peak_rss_mb();
        record_canary(result, index.Locate(canary_query.sequence));
        // Repetition zero creates the transport index used by isolated save,
        // load, and query workers. Its save is intentionally not measured.
        if (repetition == 0) index.Save(canonical_index);
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
        result.sa_sampling_rate = info.sa_sampling_rate;
        raw.peak_rss_mb = current_process_peak_rss_mb();
        record_canary(result, index.Locate(canary_query.sequence));
        if (repetition == 0) index.Save(canonical_index);
    } else {
        throw Error(ErrorCode::kInvalidInput, "unknown benchmark method: " + method);
    }
    raw.serialized_bytes = std::filesystem::file_size(canonical_index);
    raw.allocated_disk_bytes = allocated_disk_bytes(canonical_index);
    if (repetition == 0) result.canonical_index = canonical_index;
    result.builds.push_back(raw);
    return result;
}

MethodResult run_save_worker(
    const std::string& method,
    const std::filesystem::path& canonical_index,
    const std::filesystem::path& scratch_directory,
    std::uint32_t repetition) {
    MethodResult result;
    result.method = method;
    result.canonical_index = canonical_index;
    LoadRaw raw;
    raw.repetition = repetition;
    raw.status = kSaveWorkerStatus;
    const auto output_path = scratch_directory /
        (method + "-save-worker-" + std::to_string(repetition) + ".sufidx");
    if (std::filesystem::exists(output_path)) {
        throw Error(ErrorCode::kIoError,
            "isolated save-worker output already exists: " + output_path.string());
    }

    if (is_sa_method(method)) {
        auto index = SuffixArray::Load(canonical_index);
        const auto cpu_begin = usage_now();
        const auto wall_begin = Clock::now();
        index.Save(output_path);
        raw.seconds = elapsed(wall_begin);
        const auto cpu = usage_delta(cpu_begin, usage_now());
        raw.user_seconds = cpu.user;
        raw.system_seconds = cpu.system;
    } else if (is_fm_method(method)) {
        auto index = FmIndex::Load(canonical_index);
        const auto cpu_begin = usage_now();
        const auto wall_begin = Clock::now();
        index.Save(output_path);
        raw.seconds = elapsed(wall_begin);
        const auto cpu = usage_delta(cpu_begin, usage_now());
        raw.user_seconds = cpu.user;
        raw.system_seconds = cpu.system;
    } else {
        throw Error(ErrorCode::kInvalidInput,
            "isolated save worker requires an indexed benchmark method");
    }
    if (std::filesystem::file_size(output_path) !=
        std::filesystem::file_size(canonical_index)) {
        throw Error(ErrorCode::kBuildFailure,
            "isolated save worker produced a different serialized size");
    }
    std::error_code ignored;
    std::filesystem::remove(output_path, ignored);
    result.loads.push_back(raw);
    return result;
}

MethodResult run_load_worker(
    const std::string& method,
    const Dataset& dataset,
    const MethodResult& build_result,
    std::uint32_t repetition) {
    MethodResult result;
    result.method = method;
    result.canonical_index = build_result.canonical_index;
    LoadRaw raw;
    raw.repetition = repetition;
    if (method == "naive") {
        result.loads.push_back(raw);
        return result;
    }
    const auto cpu_begin = usage_now();
    const auto wall_begin = Clock::now();
    auto index = load_method(method, dataset, build_result.canonical_index);
    raw.seconds = elapsed(wall_begin);
    const auto cpu = usage_delta(cpu_begin, usage_now());
    raw.user_seconds = cpu.user;
    raw.system_seconds = cpu.system;
    if (index.suffix_array &&
        index.suffix_array->SamplingRate() != sampling_rate_for_method(method)) {
        throw Error(ErrorCode::kBuildFailure,
            "loaded suffix-array sampling rate does not match benchmark method");
    }
    verify_canary(
        build_result,
        index.locate(canary_query_for(dataset).sequence, StrandMode::kForward, std::nullopt),
        "load");
    result.loads.push_back(raw);
    return result;
}

MethodResult run_query_worker(
    const std::string& method,
    const Dataset& dataset,
    const Options& options,
    const std::filesystem::path& canonical_index,
    bool count_worker,
    const LocateLimit& locate_limit) {
    MethodResult result;
    result.method = method;
    result.canonical_index = canonical_index;
    const auto spec = profile_spec(options.profile);
    auto query_repetitions = options.query_repetitions.value_or(spec.query_repetitions);
    auto warmups = options.warmups.value_or(spec.warmups);
    if (method == "naive" && options.profile == Profile::quick) {
        if (!options.query_repetitions) query_repetitions = 1;
        if (!options.warmups) warmups = 0;
    }

    auto index = load_method(method, dataset, canonical_index);
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
            if (count_worker && scalar_enabled) {
                for (std::uint32_t warmup = 0; warmup < warmups && !queries.empty(); ++warmup) {
                    (void)measure_group(index, queries, group, strand, "count", count_limit, warmup);
                }
                for (std::uint32_t repetition = 0; repetition < query_repetitions; ++repetition) {
                    result.queries.push_back(measure_group(
                        index, queries, group, strand, "count", count_limit, repetition));
                }
            }
            if (count_worker && batch_enabled) {
                for (const auto width : fm_batch_widths_for_method(options, method)) {
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
            if (!count_worker && scalar_enabled) {
                for (std::uint32_t warmup = 0; warmup < warmups && !queries.empty(); ++warmup) {
                    (void)measure_group(
                        index, queries, group, strand, "locate", locate_limit, warmup);
                }
                for (std::uint32_t repetition = 0; repetition < query_repetitions; ++repetition) {
                    result.queries.push_back(measure_group(
                        index, queries, group, strand, "locate", locate_limit, repetition));
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
    header.stored_coordinate_width = result.stored_coordinate_width;
    header.sa_resource_profile =
        static_cast<std::uint8_t>(result.sa_resource_profile);
    header.lcp_encoding = static_cast<std::uint8_t>(result.lcp_encoding);
    header.sa_bytes = result.sa_bytes;
    header.isa_bytes = result.isa_bytes;
    header.lcp_bytes = result.lcp_bytes;
    header.resident_core_bytes = result.resident_core_bytes;
    header.sa_sampling_rate = result.sa_sampling_rate;
    header.threads = result.threads;
    header.has_canary = result.has_canary ? 1 : 0;
    header.canary_total_hits = result.canary_total_hits;
    header.canary_reported_hits = result.canary_reported_hits;
    header.canary_checksum = result.canary_checksum;
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
        wire.phase_seconds[1] = raw.storage_compaction_seconds;
        wire.phase_seconds[2] = raw.isa_build_seconds;
        wire.phase_seconds[3] = raw.lcp_build_seconds;
        wire.phase_seconds[4] = raw.child_build_seconds;
        wire.phase_seconds[5] = raw.learned_index_build_seconds;
        wire.save_seconds = raw.save_seconds;
        wire.peak_rss_mb = raw.peak_rss_mb;
        wire.serialized_bytes = raw.serialized_bytes;
        wire.allocated_disk_bytes = raw.allocated_disk_bytes;
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
        wire.peak_rss_mb = raw.peak_rss_mb;
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
        wire.skipped_high_frequency_queries = raw.skipped_high_frequency_queries;
        wire.query_bases = raw.query_bases;
        wire.seconds = raw.seconds;
        wire.user_seconds = raw.user_seconds;
        wire.system_seconds = raw.system_seconds;
        wire.peak_rss_mb = raw.peak_rss_mb;
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
    result.stored_coordinate_width = header.stored_coordinate_width;
    result.sa_resource_profile =
        static_cast<SaResourceProfile>(header.sa_resource_profile);
    result.lcp_encoding =
        static_cast<SaLcpEncoding>(header.lcp_encoding);
    result.sa_bytes = header.sa_bytes;
    result.isa_bytes = header.isa_bytes;
    result.lcp_bytes = header.lcp_bytes;
    result.resident_core_bytes = header.resident_core_bytes;
    result.sa_sampling_rate = header.sa_sampling_rate;
    result.threads = header.threads;
    result.has_canary = header.has_canary != 0;
    result.canary_total_hits = header.canary_total_hits;
    result.canary_reported_hits = header.canary_reported_hits;
    result.canary_checksum = header.canary_checksum;
    for (std::uint64_t index = 0; index < header.build_count; ++index) {
        BuildWire wire;
        if (!read_exact(descriptor, &wire, sizeof(wire))) throw Error(ErrorCode::kBuildFailure, "truncated build result");
        BuildRaw raw;
        raw.repetition = wire.repetition;
        raw.build_seconds = wire.build_seconds;
        raw.build_user_seconds = wire.build_user_seconds;
        raw.build_system_seconds = wire.build_system_seconds;
        raw.sa_build_seconds = wire.phase_seconds[0];
        raw.storage_compaction_seconds = wire.phase_seconds[1];
        raw.isa_build_seconds = wire.phase_seconds[2];
        raw.lcp_build_seconds = wire.phase_seconds[3];
        raw.child_build_seconds = wire.phase_seconds[4];
        raw.learned_index_build_seconds = wire.phase_seconds[5];
        raw.save_seconds = wire.save_seconds;
        raw.peak_rss_mb = wire.peak_rss_mb;
        raw.serialized_bytes = wire.serialized_bytes;
        raw.allocated_disk_bytes = wire.allocated_disk_bytes;
        raw.learned_index_bytes = wire.learned_index_bytes;
        raw.status = wire.status;
        result.builds.push_back(std::move(raw));
    }
    for (std::uint64_t index = 0; index < header.load_count; ++index) {
        LoadWire wire;
        if (!read_exact(descriptor, &wire, sizeof(wire))) throw Error(ErrorCode::kBuildFailure, "truncated load result");
        LoadRaw raw;
        raw.repetition = wire.repetition;
        raw.seconds = wire.seconds;
        raw.user_seconds = wire.user_seconds;
        raw.system_seconds = wire.system_seconds;
        raw.peak_rss_mb = wire.peak_rss_mb;
        raw.status = wire.status;
        result.loads.push_back(std::move(raw));
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
        raw.skipped_high_frequency_queries = wire.skipped_high_frequency_queries;
        raw.query_bases = wire.query_bases;
        raw.seconds = wire.seconds;
        raw.user_seconds = wire.user_seconds;
        raw.system_seconds = wire.system_seconds;
        raw.peak_rss_mb = wire.peak_rss_mb;
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
            if (aggregate.max_hits == "all") {
                const auto expected_count = expected.count(query.sequence, strands);
                const auto observed_count = observed.count(query.sequence, strands);
                if (expected_count != observed_count) {
                    return "query " + query.id + " complete-locate preflight count expected=" +
                        std::to_string(expected_count) + ", observed=" +
                        std::to_string(observed_count);
                }
                if (expected_count > kCompleteLocateSafetyLimit) {
                    // The measured path intentionally omits this query. Never
                    // defeat that memory-safety boundary while diagnosing an
                    // unrelated aggregate mismatch.
                    continue;
                }
            }
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

const char* worker_phase_name(WorkerPhase phase) noexcept {
    switch (phase) {
    case WorkerPhase::build: return "build";
    case WorkerPhase::save: return "save";
    case WorkerPhase::load: return "load";
    case WorkerPhase::query: return "query";
    }
    return "unknown";
}

MethodResult execute_worker_request(const WorkerRequest& request) {
    switch (request.phase) {
    case WorkerPhase::build:
        return run_build_worker(
            request.method, request.dataset, request.options,
            request.scratch_directory, request.repetition);
    case WorkerPhase::save:
        return run_save_worker(
            request.method, request.canonical_index,
            request.scratch_directory, request.repetition);
    case WorkerPhase::load: {
        MethodResult build_result;
        build_result.method = request.method;
        build_result.canonical_index = request.canonical_index;
        build_result.has_canary = request.has_canary;
        build_result.canary_total_hits = request.canary_total_hits;
        build_result.canary_reported_hits = request.canary_reported_hits;
        build_result.canary_checksum = request.canary_checksum;
        return run_load_worker(
            request.method, request.dataset, build_result,
            request.repetition);
    }
    case WorkerPhase::query:
        return run_query_worker(
            request.method, request.dataset, request.options,
            request.canonical_index, request.count_worker,
            request.locate_limit);
    }
    throw Error(ErrorCode::kBuildFailure,
        "invalid benchmark worker phase");
}

MethodResult launch_worker(
    const std::string& method,
    WorkerPhase phase,
    WorkerRequest request) {
    static std::uint64_t request_number = 0;
    request.phase = phase;
    request.method = method;
    const auto stem = ".exact-worker-" + method + "-" +
        worker_phase_name(phase) + "-" + std::to_string(getpid()) + "-" +
        std::to_string(request_number++);
    const auto request_path = request.scratch_directory / (stem + ".request");
    const auto result_path = request.scratch_directory / (stem + ".result");
    write_worker_request(request_path, request);
    const auto pid = fork();
    if (pid < 0) {
        std::error_code ignored;
        std::filesystem::remove(request_path, ignored);
        throw Error(ErrorCode::kIoError, "cannot create benchmark worker process");
    }
    if (pid == 0) {
        const auto request_argument = request_path.string();
        const auto result_argument = result_path.string();
        execl("/proc/self/exe", "sufkit", "__benchmark-worker",
            request_argument.c_str(), result_argument.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    struct rusage usage {};
    const auto waited = wait4(pid, &status, 0, &usage);
    MethodResult result;
    std::string receive_error;
    const auto descriptor = open(result_path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        receive_error = "benchmark clean-exec worker did not produce a result";
    } else {
        try {
            result = read_worker_result(descriptor);
        } catch (const std::exception& error) {
            receive_error = error.what();
        }
        close(descriptor);
    }
    std::error_code ignored;
    std::filesystem::remove(request_path, ignored);
    std::filesystem::remove(result_path, ignored);
    if (!receive_error.empty()) {
        throw Error(ErrorCode::kBuildFailure, receive_error);
    }
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw Error(ErrorCode::kBuildFailure,
            "benchmark clean-exec worker failed: " + method);
    }
    const auto rss = peak_rss_mb(usage);
    result.peak_rss_mb = rss;
    if (phase == WorkerPhase::build) {
        for (auto& raw : result.builds) {
            // Build workers snapshot ru_maxrss before the unmeasured
            // canonical transport save. Preserve that phase boundary.
            if (raw.peak_rss_mb == 0.0) raw.peak_rss_mb = rss;
        }
    } else if (phase == WorkerPhase::save || phase == WorkerPhase::load) {
        for (auto& raw : result.loads) raw.peak_rss_mb = rss;
    } else {
        for (auto& raw : result.queries) raw.peak_rss_mb = rss;
    }
    return result;
}

bool same_canary(const MethodResult& left, const MethodResult& right) {
    return left.has_canary == right.has_canary &&
        (!left.has_canary ||
         (left.canary_total_hits == right.canary_total_hits &&
          left.canary_reported_hits == right.canary_reported_hits &&
          left.canary_checksum == right.canary_checksum));
}

} // namespace

MethodResult run_method_isolated(
    const std::string& method,
    const Dataset& dataset,
    const Options& options,
    const std::filesystem::path& scratch_directory) {
    const auto spec = profile_spec(options.profile);
    const auto requested_build_repetitions =
        options.build_repetitions.value_or(spec.build_repetitions);
    const auto build_repetitions = method == "naive" ? 1U : requested_build_repetitions;

    const auto make_request = [&] {
        WorkerRequest request;
        request.method = method;
        request.parent_dataset = &dataset;
        request.parent_options = &options;
        request.scratch_directory = scratch_directory;
        return request;
    };

    MethodResult result;
    for (std::uint32_t repetition = 0; repetition < build_repetitions; ++repetition) {
        auto request = make_request();
        request.repetition = repetition;
        auto phase = launch_worker(
            method, WorkerPhase::build, std::move(request));
        if (repetition == 0) {
            result = std::move(phase);
        } else {
            if (result.backend != phase.backend || result.signature != phase.signature ||
                result.coordinate_width != phase.coordinate_width ||
                result.stored_coordinate_width !=
                    phase.stored_coordinate_width ||
                result.sa_resource_profile != phase.sa_resource_profile ||
                result.lcp_encoding != phase.lcp_encoding ||
                result.sa_bytes != phase.sa_bytes ||
                result.isa_bytes != phase.isa_bytes ||
                result.lcp_bytes != phase.lcp_bytes ||
                result.resident_core_bytes != phase.resident_core_bytes ||
                result.sa_sampling_rate != phase.sa_sampling_rate ||
                result.threads != phase.threads || !same_canary(result, phase)) {
                throw Error(ErrorCode::kBuildFailure,
                    "index metadata or canary changed between build repetitions for " + method);
            }
            result.peak_rss_mb = std::max(result.peak_rss_mb, phase.peak_rss_mb);
            result.builds.insert(
                result.builds.end(), phase.builds.begin(), phase.builds.end());
        }
    }
    if (method != "naive" && result.canonical_index.empty()) return result;

    if (method != "naive") {
        for (std::uint32_t repetition = 0; repetition < requested_build_repetitions; ++repetition) {
            auto request = make_request();
            request.repetition = repetition;
            request.canonical_index = result.canonical_index;
            auto phase = launch_worker(
                method, WorkerPhase::save, std::move(request));
            if (phase.loads.size() != 1 || phase.loads.front().status != kSaveWorkerStatus) {
                throw Error(ErrorCode::kBuildFailure,
                    "isolated save worker returned an invalid result");
            }
            const auto build = std::find_if(
                result.builds.begin(), result.builds.end(), [&](const auto& raw) {
                    return raw.repetition == repetition;
                });
            if (build == result.builds.end()) {
                throw Error(ErrorCode::kBuildFailure,
                    "isolated save worker has no matching build repetition");
            }
            build->save_seconds = phase.loads.front().seconds;
            result.peak_rss_mb = std::max(result.peak_rss_mb, phase.peak_rss_mb);
            result.loads.push_back(std::move(phase.loads.front()));
        }
    }

    const auto load_repetitions = method == "naive" ? 1U : requested_build_repetitions;
    for (std::uint32_t repetition = 0; repetition < load_repetitions; ++repetition) {
        auto request = make_request();
        request.repetition = repetition;
        request.canonical_index = result.canonical_index;
        request.has_canary = result.has_canary;
        request.canary_total_hits = result.canary_total_hits;
        request.canary_reported_hits = result.canary_reported_hits;
        request.canary_checksum = result.canary_checksum;
        auto phase = launch_worker(
            method, WorkerPhase::load, std::move(request));
        result.peak_rss_mb = std::max(result.peak_rss_mb, phase.peak_rss_mb);
        result.loads.insert(result.loads.end(), phase.loads.begin(), phase.loads.end());
    }

    LocateLimit unused_limit;
    auto count_request = make_request();
    count_request.canonical_index = result.canonical_index;
    count_request.count_worker = true;
    count_request.locate_limit = unused_limit;
    auto count_phase = launch_worker(
        method, WorkerPhase::query, std::move(count_request));
    result.peak_rss_mb = std::max(result.peak_rss_mb, count_phase.peak_rss_mb);
    result.queries.insert(
        result.queries.end(), count_phase.queries.begin(), count_phase.queries.end());

    const bool locate_enabled = !is_fm_method(method) ||
        std::find(options.fm_query_modes.begin(), options.fm_query_modes.end(), "scalar") !=
            options.fm_query_modes.end();
    if (locate_enabled) {
        for (const auto& limit : options.locate_limits) {
            auto request = make_request();
            request.canonical_index = result.canonical_index;
            request.count_worker = false;
            request.locate_limit = limit;
            auto locate_phase = launch_worker(
                method, WorkerPhase::query, std::move(request));
            result.peak_rss_mb = std::max(result.peak_rss_mb, locate_phase.peak_rss_mb);
            result.queries.insert(
                result.queries.end(), locate_phase.queries.begin(), locate_phase.queries.end());
        }
    }
    return result;
}

void validate_results(
    const Dataset& dataset,
    const std::vector<MethodResult>& results,
    Profile profile) {
    if (results.empty()) throw Error(ErrorCode::kBuildFailure, "benchmark produced no method results");
    for (const auto& result : results) {
        using StableValue = std::tuple<
            std::uint64_t, std::uint64_t, std::uint64_t,
            std::uint64_t, std::uint64_t, std::uint64_t>;
        std::map<std::string, StableValue> expected;
        for (const auto& raw : result.queries) {
            if (raw.status != "ok") continue;
            const auto key = query_key(raw);
            const auto value = std::make_tuple(
                raw.total_hits, raw.reported_hits, raw.checksum,
                raw.query_count, raw.skipped_high_frequency_queries, raw.query_bases);
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

    using CrossMethodValue = std::tuple<
        std::uint64_t, std::uint64_t, std::uint64_t,
        std::uint64_t, std::uint64_t, std::uint64_t, std::string>;
    std::map<std::string, CrossMethodValue> baseline;
    for (const auto& result : results) {
        for (const auto& raw : result.queries) {
            if (raw.repetition != 0 || raw.status != "ok") continue;
            const auto key = query_key(raw);
            const auto value = std::make_tuple(
                raw.total_hits, raw.reported_hits, raw.checksum,
                raw.query_count, raw.skipped_high_frequency_queries, raw.query_bases,
                result.method);
            const auto inserted = baseline.emplace(key, value);
            if (!inserted.second &&
                std::tie(raw.total_hits, raw.reported_hits, raw.checksum,
                         raw.query_count, raw.skipped_high_frequency_queries, raw.query_bases) !=
                std::tie(std::get<0>(inserted.first->second), std::get<1>(inserted.first->second),
                         std::get<2>(inserted.first->second), std::get<3>(inserted.first->second),
                         std::get<4>(inserted.first->second), std::get<5>(inserted.first->second))) {
                const auto expected_method = std::get<6>(inserted.first->second);
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
                constexpr std::size_t kNaiveOracleQueriesPerGroup = 8;
                const auto sample_count = std::min(
                    queries.size(), kNaiveOracleQueriesPerGroup);
                for (std::size_t sample = 0; sample < sample_count; ++sample) {
                    const auto selected = sample_count == 1 ? 0 :
                        sample * (queries.size() - 1) / (sample_count - 1);
                    const auto* query = queries[selected];
                    for (const auto strands : {StrandMode::kForward,
                                               StrandMode::kReverseComplement,
                                               StrandMode::kBoth}) {
                        const auto expected = naive_locate(
                            normalized, query->sequence, strands, 1000);
                        const auto observed = index.locate(
                            query->sequence, strands, 1000);
                        if (expected.total_hits != observed.total_hits ||
                            !same_matches(expected.hits, observed.hits)) {
                            throw Error(ErrorCode::kBuildFailure,
                                "naive sample mismatch for query " + query->id +
                                " using " + indexed->method);
                        }
                    }
                }
            }
        }
    }
}

int run_clean_exec_worker(const std::vector<std::string>& arguments) {
    if (arguments.size() != 2) {
        throw Error(ErrorCode::kInvalidInput,
            "internal benchmark worker requires request and result paths");
    }
    const std::filesystem::path request_path = arguments[0];
    const std::filesystem::path result_path = arguments[1];
    MethodResult result;
    std::string error;
    try {
        const auto request = read_worker_request(request_path);
        result = execute_worker_request(request);
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    const auto descriptor = open(
        result_path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) {
        throw Error(ErrorCode::kIoError,
            "cannot create benchmark worker result: " + result_path.string());
    }
    write_worker_result(descriptor, result, error);
    close(descriptor);
    return error.empty() ? 0 : 1;
}

} // namespace sufkit::app::bench

namespace sufkit::app {

int run_benchmark_worker(const std::vector<std::string>& arguments) {
    return bench::run_clean_exec_worker(arguments);
}

} // namespace sufkit::app
