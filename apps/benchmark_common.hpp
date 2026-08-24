#pragma once

#include "benchmark_provenance.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <sufkit/sufkit.hpp>

namespace sufkit::app::bench {

enum class Profile { smoke, quick, standard, full, user };
enum class Scenario { mixed, balanced, gc_skewed, repeat_rich, n_islands, many_contig, user };

struct ProfileSpec {
    std::uint64_t total_bases = 0;
    std::uint64_t query_count = 0;
    std::uint32_t build_repetitions = 1;
    std::uint32_t warmups = 1;
    std::uint32_t query_repetitions = 1;
};

struct LocateLimit {
    bool all = false;
    std::uint64_t value = 0;
};

struct Options {
    Profile profile = Profile::quick;
    bool profile_explicit = false;
    bool legacy_quick = false;
    bool legacy_smoke = false;
    std::optional<std::filesystem::path> reference_path;
    std::optional<std::filesystem::path> query_path;
    std::optional<std::filesystem::path> output_path;
    std::optional<std::filesystem::path> output_directory;
    std::vector<std::string> methods;
    std::vector<Scenario> scenarios;
    std::vector<std::uint64_t> pattern_lengths;
    std::vector<LocateLimit> locate_limits;
    std::vector<std::string> fm_query_modes{"scalar"};
    std::vector<std::uint32_t> fm_batch_widths{16};
    std::uint64_t seed = 20260822;
    std::optional<std::uint32_t> build_repetitions;
    std::optional<std::uint32_t> query_repetitions;
    std::optional<std::uint32_t> warmups;
    std::uint32_t learned_k = 20;
    std::uint32_t learned_memory_overhead_basis_points = 100;
    std::optional<std::uint32_t> learned_bucket_bits;
};

struct QueryCase {
    std::string id;
    std::string sequence;
    std::string group;
    std::string pattern_length;
    std::string source;
};

struct QueryGroupSpec {
    std::string group;
    std::string pattern_length;
};

struct Dataset {
    std::string name;
    Scenario scenario = Scenario::mixed;
    std::vector<SequenceRecord> records;
    std::vector<QueryCase> queries;
    std::vector<QueryGroupSpec> groups;
    std::uint64_t fingerprint = 0;
    std::uint64_t total_bases = 0;
    std::uint64_t contigs = 0;
    double gc_fraction = 0.0;
    double ambiguous_fraction = 0.0;
    double repeat_fraction = 0.0;
    double reference_seconds = 0.0;
    double normalization_seconds = 0.0;
};

struct BuildRaw {
    std::uint32_t repetition = 0;
    double build_seconds = 0.0;
    double build_user_seconds = 0.0;
    double build_system_seconds = 0.0;
    double sa_build_seconds = 0.0;
    double isa_build_seconds = 0.0;
    double lcp_build_seconds = 0.0;
    double child_build_seconds = 0.0;
    double learned_index_build_seconds = 0.0;
    double save_seconds = 0.0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    std::string status = "ok";
};

struct LoadRaw {
    std::uint32_t repetition = 0;
    double seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    std::string status = "ok";
};

struct QueryRaw {
    std::string group;
    std::string pattern_length;
    std::string strand;
    std::string operation;
    std::string max_hits;
    std::string fm_query_mode = "scalar";
    std::string fm_batch_width = "NA";
    std::uint32_t repetition = 0;
    std::uint64_t query_count = 0;
    std::uint64_t query_bases = 0;
    std::uint64_t measurement_iterations = 1;
    double seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    std::uint64_t total_hits = 0;
    std::uint64_t reported_hits = 0;
    std::uint64_t checksum = 0;
    std::uint64_t suffix_comparisons = 0;
    std::uint64_t character_comparisons = 0;
    std::uint64_t gallop_probes = 0;
    std::uint64_t local_window_rows = 0;
    std::uint64_t local_window_rows_max = 0;
    std::uint64_t predictions = 0;
    std::uint64_t prediction_absolute_error_sum = 0;
    std::uint64_t prediction_absolute_error_max = 0;
    std::uint64_t prediction_error_p50 = 0;
    std::uint64_t prediction_error_p95 = 0;
    std::uint64_t prediction_error_p99 = 0;
    std::uint64_t local_window_rows_p50 = 0;
    std::uint64_t local_window_rows_p95 = 0;
    std::uint64_t local_window_rows_p99 = 0;
    std::uint64_t full_binary_fallbacks = 0;
    std::string status = "ok";
};

struct MethodResult {
    std::string method;
    std::string backend;
    std::string signature;
    std::string sdsl_version;
    std::uint8_t coordinate_width = 0;
    std::uint32_t sa_sampling_rate = 0;
    double peak_rss_mb = 0.0;
    std::filesystem::path canonical_index;
    std::vector<BuildRaw> builds;
    std::vector<LoadRaw> loads;
    std::vector<QueryRaw> queries;
};

struct RunContext {
    std::string run_id;
    std::string timestamp;
    Profile profile = Profile::quick;
    std::uint64_t seed = 20260822;
    std::string methods;
    std::string pattern_lengths;
    std::string locate_limits;
    std::string build_repetitions;
    std::string query_repetitions;
    std::string warmups;
    std::string learned_k;
    std::string learned_memory_overhead_basis_points;
    std::string learned_bucket_bits;
    std::string fm_query_modes;
    std::string fm_batch_widths;
    BenchmarkProvenance provenance;
};

const char* to_string(Profile value) noexcept;
const char* to_string(Scenario value) noexcept;
Profile parse_profile(const std::string& value);
Scenario parse_scenario(const std::string& value);
ProfileSpec profile_spec(Profile value);
std::string fingerprint_hex(std::uint64_t value);
std::uint64_t checksum_seed() noexcept;
void mix_checksum(std::uint64_t& hash, std::uint64_t value) noexcept;
double median(std::vector<double> values);
double minimum(const std::vector<double>& values);
double maximum(const std::vector<double>& values);

Dataset generate_dataset(
    Profile profile,
    Scenario scenario,
    std::uint64_t seed,
    const std::vector<std::uint64_t>& pattern_lengths);

Dataset load_user_dataset(
    const std::filesystem::path& reference,
    const std::optional<std::filesystem::path>& queries,
    std::uint64_t seed,
    std::uint64_t generated_query_count,
    const std::vector<std::uint64_t>& pattern_lengths);

void classify_user_queries(Dataset& dataset, const std::filesystem::path& classifier_index);

MethodResult run_method_isolated(
    const std::string& method,
    const Dataset& dataset,
    const Options& options,
    const std::filesystem::path& scratch_directory);

void validate_results(
    const Dataset& dataset,
    const std::vector<MethodResult>& results,
    Profile profile);

void write_result_directory(
    const std::filesystem::path& directory,
    const RunContext& context,
    const std::vector<Dataset>& datasets,
    const std::vector<std::vector<MethodResult>>& results);

void write_legacy_output(
    const std::filesystem::path& path,
    const Dataset& dataset,
    const std::vector<MethodResult>& results);

} // namespace sufkit::app::bench
