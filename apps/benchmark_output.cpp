#include "benchmark_common.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>

#include <sys/utsname.h>

#ifndef SUFKIT_BENCH_CMAKE_VERSION
#define SUFKIT_BENCH_CMAKE_VERSION "unknown"
#endif
#ifndef SUFKIT_BENCH_BUILD_TYPE
#define SUFKIT_BENCH_BUILD_TYPE "unknown"
#endif

namespace sufkit::app::bench {
namespace {

std::string compiler_name() {
#if defined(__clang__)
    return "Clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "unknown";
#endif
}

std::string compiler_version() {
#if defined(__clang__)
    return __clang_version__;
#elif defined(__GNUC__)
    return __VERSION__;
#else
    return "unknown";
#endif
}

std::pair<std::string, std::string> os_and_architecture() {
    struct utsname value {};
    if (uname(&value) != 0) return {"unknown", "unknown"};
    return {std::string(value.sysname) + " " + value.release, value.machine};
}

std::string cpu_model() {
    std::ifstream input("/proc/cpuinfo");
    std::string line;
    while (std::getline(input, line)) {
        const auto marker = line.find("model name");
        if (marker == std::string::npos) continue;
        const auto colon = line.find(':', marker);
        if (colon == std::string::npos) continue;
        auto value = line.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.erase(value.begin());
        return value;
    }
    return "unknown";
}

void require_output(std::ofstream& output, const std::filesystem::path& path) {
    if (!output) throw Error(ErrorCode::io_error, "cannot write benchmark output: " + path.string());
}

std::string fraction(double value) {
    if (value < 0.0) return "NA";
    std::ostringstream output;
    output << std::fixed << std::setprecision(8) << value;
    return output.str();
}

std::uint64_t query_set_checksum(const Dataset& dataset) {
    auto checksum = checksum_seed();
    const auto mix_text = [&](std::string_view value) {
        mix_checksum(checksum, value.size());
        for (const unsigned char byte : value) mix_checksum(checksum, byte);
    };
    mix_checksum(checksum, dataset.queries.size());
    for (const auto& query : dataset.queries) {
        mix_text(query.id);
        mix_text(query.group);
        mix_text(query.pattern_length);
        mix_text(query.source);
        mix_text(query.sequence);
    }
    return checksum;
}

std::string build_status(const MethodResult& result) {
    for (const auto& raw : result.builds) {
        if (raw.status != "ok") return raw.status;
    }
    return "ok";
}

struct QueryAggregate {
    std::string group;
    std::string pattern_length;
    std::string strand;
    std::string operation;
    std::string max_hits;
    std::string fm_query_mode;
    std::string fm_batch_width;
    std::string status;
    std::uint64_t query_count = 0;
    std::uint64_t skipped_high_frequency_queries = 0;
    std::uint64_t query_bases = 0;
    std::uint64_t total_hits = 0;
    std::uint64_t reported_hits = 0;
    std::uint64_t checksum = 0;
    std::vector<double> seconds;
    double peak_rss_mb = 0.0;
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
};

std::vector<QueryAggregate> aggregate_queries(const MethodResult& result) {
    using Key = std::tuple<std::string, std::string, std::string, std::string,
                           std::string, std::string, std::string, std::string>;
    std::map<Key, QueryAggregate> values;
    for (const auto& raw : result.queries) {
        const Key key{raw.group, raw.pattern_length, raw.strand, raw.operation, raw.max_hits,
                      raw.fm_query_mode, raw.fm_batch_width, raw.status};
        auto& aggregate = values[key];
        aggregate.group = raw.group;
        aggregate.pattern_length = raw.pattern_length;
        aggregate.strand = raw.strand;
        aggregate.operation = raw.operation;
        aggregate.max_hits = raw.max_hits;
        aggregate.fm_query_mode = raw.fm_query_mode;
        aggregate.fm_batch_width = raw.fm_batch_width;
        aggregate.status = raw.status;
        aggregate.query_count = raw.query_count;
        aggregate.skipped_high_frequency_queries = raw.skipped_high_frequency_queries;
        aggregate.query_bases = raw.query_bases;
        aggregate.total_hits = raw.total_hits;
        aggregate.reported_hits = raw.reported_hits;
        aggregate.checksum = raw.checksum;
        aggregate.peak_rss_mb = std::max(aggregate.peak_rss_mb, raw.peak_rss_mb);
        aggregate.suffix_comparisons = raw.suffix_comparisons;
        aggregate.character_comparisons = raw.character_comparisons;
        aggregate.gallop_probes = raw.gallop_probes;
        aggregate.local_window_rows = raw.local_window_rows;
        aggregate.local_window_rows_max = raw.local_window_rows_max;
        aggregate.predictions = raw.predictions;
        aggregate.prediction_absolute_error_sum = raw.prediction_absolute_error_sum;
        aggregate.prediction_absolute_error_max = raw.prediction_absolute_error_max;
        aggregate.prediction_error_p50 = raw.prediction_error_p50;
        aggregate.prediction_error_p95 = raw.prediction_error_p95;
        aggregate.prediction_error_p99 = raw.prediction_error_p99;
        aggregate.local_window_rows_p50 = raw.local_window_rows_p50;
        aggregate.local_window_rows_p95 = raw.local_window_rows_p95;
        aggregate.local_window_rows_p99 = raw.local_window_rows_p99;
        aggregate.full_binary_fallbacks = raw.full_binary_fallbacks;
        if (raw.status == "ok") aggregate.seconds.push_back(raw.seconds);
    }
    std::vector<QueryAggregate> result_values;
    for (auto& value : values) result_values.push_back(std::move(value.second));
    return result_values;
}

std::vector<double> values_for(
    const std::vector<BuildRaw>& values,
    const std::function<double(const BuildRaw&)>& selector) {
    std::vector<double> result;
    for (const auto& value : values) if (value.status == "ok") result.push_back(selector(value));
    return result;
}

std::vector<double> values_for(
    const std::vector<LoadRaw>& values,
    const std::function<double(const LoadRaw&)>& selector) {
    std::vector<double> result;
    for (const auto& value : values) if (value.status == "ok") result.push_back(selector(value));
    return result;
}

bool is_save_worker_result(const LoadRaw& value) {
    return value.status == "save_worker_load_plus_save";
}

std::vector<double> save_values_for(
    const std::vector<LoadRaw>& values,
    const std::function<double(const LoadRaw&)>& selector) {
    std::vector<double> result;
    for (const auto& value : values) {
        if (is_save_worker_result(value)) result.push_back(selector(value));
    }
    return result;
}

} // namespace

void write_result_directory(
    const std::filesystem::path& directory,
    const RunContext& context,
    const std::vector<Dataset>& datasets,
    const std::vector<std::vector<MethodResult>>& results) {
    if (datasets.size() != results.size()) {
        throw Error(ErrorCode::build_failure, "benchmark dataset/result cardinality mismatch");
    }
    const std::array<std::string, 4> names{{
        "run_metadata.tsv", "build_results.tsv", "query_results.tsv", "raw_repetitions.tsv"}};
    for (const auto& name : names) {
        if (std::filesystem::exists(directory / name)) {
            throw Error(ErrorCode::io_error,
                "benchmark output already exists: " + (directory / name).string());
        }
    }
    std::filesystem::create_directories(directory);
    const auto metadata_path = directory / names[0];
    const auto build_path = directory / names[1];
    const auto query_path = directory / names[2];
    const auto raw_path = directory / names[3];
    std::ofstream metadata(metadata_path);
    std::ofstream builds(build_path);
    std::ofstream queries(query_path);
    std::ofstream raw(raw_path);
    if (!metadata || !builds || !queries || !raw) {
        throw Error(ErrorCode::io_error, "cannot create benchmark output directory files");
    }

    const auto platform = os_and_architecture();
    metadata << "run_id\ttimestamp\tprofile\tscenario\tseed\tdataset\tdataset_fingerprint\t"
                "query_set_checksum\tquery_count\tquery_bases\t"
                "total_bases\tcontigs\tgc_fraction\tambiguous_fraction\trepeat_fraction\t"
                "methods\tpattern_lengths\tlocate_limits\tbuild_repetitions\t"
                "query_repetitions\twarmups\tsa_threads\t"
                "learned_k\tlearned_memory_overhead_basis_points\tlearned_bucket_bits\t"
                "fm_query_modes\tfm_batch_widths\tfm_batch_width_overrides\t"
                "reference_seconds\tnormalization_seconds\tcompiler\tcompiler_version\t"
                "cmake_version\tbuild_type\tos\tarchitecture\tcpu_model\tlogical_cpus\n";
    for (const auto& dataset : datasets) {
        metadata << context.run_id << '\t' << context.timestamp << '\t' << to_string(context.profile) << '\t'
                 << to_string(dataset.scenario) << '\t' << context.seed << '\t' << dataset.name << '\t'
                 << fingerprint_hex(dataset.fingerprint) << '\t'
                 << fingerprint_hex(query_set_checksum(dataset)) << '\t'
                 << dataset.queries.size() << '\t'
                 << std::accumulate(
                        dataset.queries.begin(), dataset.queries.end(), std::uint64_t{0},
                        [](const auto total, const auto& query) {
                            return total + static_cast<std::uint64_t>(query.sequence.size());
                        }) << '\t'
                 << dataset.total_bases << '\t'
                 << dataset.contigs << '\t' << fraction(dataset.gc_fraction) << '\t'
                 << fraction(dataset.ambiguous_fraction) << '\t' << fraction(dataset.repeat_fraction) << '\t'
                 << context.methods << '\t' << context.pattern_lengths << '\t'
                 << context.locate_limits << '\t' << context.build_repetitions << '\t'
                 << context.query_repetitions << '\t' << context.warmups << '\t'
                 << context.sa_threads << '\t'
                 << context.learned_k << '\t'
                 << context.learned_memory_overhead_basis_points << '\t'
                 << context.learned_bucket_bits << '\t'
                 << context.fm_query_modes << '\t' << context.fm_batch_widths << '\t'
                 << context.fm_batch_width_overrides << '\t'
                 << std::fixed << std::setprecision(6) << dataset.reference_seconds << '\t'
                 << dataset.normalization_seconds << '\t' << compiler_name() << '\t'
                 << compiler_version() << '\t' << SUFKIT_BENCH_CMAKE_VERSION << '\t'
                 << SUFKIT_BENCH_BUILD_TYPE << '\t' << platform.first << '\t' << platform.second << '\t'
                 << cpu_model() << '\t' << std::thread::hardware_concurrency() << '\n';
    }

    builds << "run_id\tdataset\tscenario\tmethod\tbackend\tbackend_signature\tsdsl_version\t"
              "coordinate_width\tsa_sampling_rate\tthreads\tbuild_seconds_median\tbuild_seconds_min\tbuild_seconds_max\t"
              "build_user_cpu_seconds_median\tbuild_system_cpu_seconds_median\tsa_build_seconds_median\t"
              "isa_build_seconds_median\tlcp_build_seconds_median\tchild_build_seconds_median\t"
              "learned_index_build_seconds_median\tall_phase_worker_peak_rss_mb_max\t"
              "build_worker_peak_rss_mb_median\t"
              "save_seconds_median\tsave_worker_peak_rss_mb_median\tserialized_bytes\t"
              "allocated_disk_bytes\tlearned_index_bytes\tbits_per_base\t"
              "load_seconds_median\tload_worker_peak_rss_mb_median\tquery_worker_peak_rss_mb_max\tstatus\n";
    queries << "run_id\tdataset\tscenario\tmethod\tquery_group\tpattern_length\tstrand\toperation\t"
               "max_hits\tquery_count\tskipped_high_frequency_queries\tseconds_median\tseconds_min\tseconds_max\tqps_median\t"
               "nanoseconds_per_query_median\tquery_worker_peak_rss_mb\ttotal_hits\treported_hits\tresult_checksum\t"
               "suffix_comparisons\tcharacter_comparisons\tgallop_probes\tlocal_window_rows\t"
               "local_window_rows_max\tpredictions\tprediction_error_mean\t"
               "prediction_error_p50\tprediction_error_p95\tprediction_error_p99\t"
               "prediction_error_max\tlocal_window_rows_p50\tlocal_window_rows_p95\t"
               "local_window_rows_p99\tfull_binary_fallbacks\tstatus\t"
               "fm_query_mode\tfm_batch_width\tquery_bases\tquery_bases_per_second\t"
               "speedup_vs_fm_huff_scalar\n";
    raw << "run_id\tdataset\tscenario\tmethod\tphase\tquery_group\tpattern_length\tstrand\t"
           "operation\tmax_hits\trepetition\tquery_count\tskipped_high_frequency_queries\tseconds\tuser_cpu_seconds\t"
           "system_cpu_seconds\tpeak_rss_mb\tpeak_rss_scope\ttotal_hits\treported_hits\tresult_checksum\tstatus\t"
           "query_id\tquery_source\tsuffix_comparisons\tcharacter_comparisons\t"
           "gallop_probes\tlocal_window_rows\tlocal_window_rows_max\tpredictions\t"
           "prediction_error_sum\tprediction_error_max\tfull_binary_fallbacks\t"
           "fm_query_mode\tfm_batch_width\tquery_bases\t"
           "query_bases_per_second\tspeedup_vs_fm_huff_scalar\tthreads\ttotal_bases\t"
           "serialized_bytes\tallocated_disk_bytes\tlearned_index_bytes\t"
           "sa_build_seconds\tisa_build_seconds\tlcp_build_seconds\t"
           "child_build_seconds\tlearned_index_build_seconds\t"
           "backend\tbackend_signature\tsdsl_version\tcoordinate_width\t"
           "sa_sampling_rate\tcanary_total_hits\tcanary_reported_hits\t"
           "canary_checksum\tquery_threads\n";
    builds << std::fixed << std::setprecision(6);
    queries << std::fixed << std::setprecision(6);
    raw << std::fixed << std::setprecision(6);

    for (std::size_t dataset_index = 0; dataset_index < datasets.size(); ++dataset_index) {
        const auto& dataset = datasets[dataset_index];
        for (const auto& query : dataset.queries) {
            raw << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario)
                << "\tdataset\tquery_definition\t" << query.group << '\t' << query.pattern_length
                << "\tNA\tdefinition\tNA\t0\t1\t0\t0\t0\t0\t0\tnot_applicable\t0\t0\t0000000000000000\tok\t"
                << query.id << '\t' << query.source
                << "\t0\t0\t0\t0\t0\t0\t0\t0\t0\tNA\tNA\t0\t0\tNA\t1\t"
                << dataset.total_bases << "\t0\t0\t0\t0\t0\t0\t0\t0"
                << "\tNA\tNA\tNA\t0\t0\t0\t0\t0000000000000000\t0\n";
        }

        const auto query_identity = [](const QueryAggregate& value) {
            return value.group + "\t" + value.pattern_length + "\t" + value.strand + "\t" +
                   value.operation + "\t" + value.max_hits;
        };
        std::map<std::string, double> huffman_scalar_seconds;
        for (const auto& candidate : results[dataset_index]) {
            if (candidate.method != "fm" && candidate.method != "fm-huff") continue;
            for (const auto& value : aggregate_queries(candidate)) {
                if (value.status == "ok" && value.fm_query_mode == "scalar") {
                    huffman_scalar_seconds[query_identity(value)] = median(value.seconds);
                }
        }
        }
        for (const auto& result : results[dataset_index]) {
            const auto build_seconds = values_for(result.builds, [](const auto& value) { return value.build_seconds; });
            const auto build_user = values_for(result.builds, [](const auto& value) { return value.build_user_seconds; });
            const auto build_system = values_for(result.builds, [](const auto& value) { return value.build_system_seconds; });
            const auto sa_build = values_for(result.builds, [](const auto& value) { return value.sa_build_seconds; });
            const auto isa_build = values_for(result.builds, [](const auto& value) { return value.isa_build_seconds; });
            const auto lcp_build = values_for(result.builds, [](const auto& value) { return value.lcp_build_seconds; });
            const auto child_build = values_for(result.builds, [](const auto& value) { return value.child_build_seconds; });
            const auto learned_build = values_for(result.builds, [](const auto& value) { return value.learned_index_build_seconds; });
            const auto build_peak_rss = values_for(result.builds, [](const auto& value) { return value.peak_rss_mb; });
            const auto save_seconds = values_for(result.builds, [](const auto& value) { return value.save_seconds; });
            const auto save_peak_rss = save_values_for(result.loads, [](const auto& value) { return value.peak_rss_mb; });
            const auto load_seconds = values_for(result.loads, [](const auto& value) { return value.seconds; });
            const auto load_peak_rss = values_for(result.loads, [](const auto& value) { return value.peak_rss_mb; });
            const auto serialized = result.builds.empty() ? 0 : result.builds.front().serialized_bytes;
            const auto allocated = result.builds.empty() ? 0 : result.builds.front().allocated_disk_bytes;
            double query_peak_rss = 0.0;
            for (const auto& value : result.queries) {
                if (value.status == "ok") query_peak_rss = std::max(query_peak_rss, value.peak_rss_mb);
            }
            const auto bits_per_base = dataset.total_bases == 0
                ? 0.0 : static_cast<double>(serialized) * 8.0 / static_cast<double>(dataset.total_bases);
            builds << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario) << '\t'
                   << result.method << '\t' << result.backend << '\t' << result.signature << '\t'
                   << result.sdsl_version << '\t' << static_cast<unsigned>(result.coordinate_width) << '\t'
                   << result.sa_sampling_rate << '\t' << result.threads << '\t'
                   << median(build_seconds) << '\t' << minimum(build_seconds) << '\t'
                   << maximum(build_seconds) << '\t' << median(build_user) << '\t'
                   << median(build_system) << '\t' << median(sa_build) << '\t'
                   << median(isa_build) << '\t' << median(lcp_build) << '\t'
                   << median(child_build) << '\t' << median(learned_build) << '\t'
                   << result.peak_rss_mb << '\t' << median(build_peak_rss) << '\t'
                   << median(save_seconds) << '\t' << median(save_peak_rss) << '\t'
                   << serialized << '\t' << allocated << '\t'
                   << (result.builds.empty() ? 0 : result.builds.front().learned_index_bytes) << '\t'
                   << bits_per_base << '\t' << median(load_seconds) << '\t'
                   << median(load_peak_rss) << '\t' << query_peak_rss << '\t'
                   << build_status(result) << '\n';

            for (const auto& value : aggregate_queries(result)) {
                const auto seconds_median = median(value.seconds);
                const auto qps = seconds_median == 0.0
                    ? 0.0 : static_cast<double>(value.query_count) / seconds_median;
                const auto ns_per_query = value.query_count == 0
                    ? 0.0 : seconds_median * 1.0e9 / static_cast<double>(value.query_count);
                const auto query_bases_per_second = seconds_median == 0.0
                    ? 0.0 : static_cast<double>(value.query_bases) / seconds_median;
                const auto baseline = huffman_scalar_seconds.find(query_identity(value));
                const auto speedup = baseline == huffman_scalar_seconds.end() || seconds_median == 0.0
                    ? 0.0 : baseline->second / seconds_median;
                queries << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario) << '\t'
                        << result.method << '\t' << value.group << '\t' << value.pattern_length << '\t'
                        << value.strand << '\t' << value.operation << '\t' << value.max_hits << '\t'
                        << value.query_count << '\t' << value.skipped_high_frequency_queries << '\t'
                        << seconds_median << '\t' << minimum(value.seconds) << '\t'
                        << maximum(value.seconds) << '\t' << qps << '\t' << ns_per_query << '\t'
                        << value.peak_rss_mb << '\t'
                        << value.total_hits << '\t' << value.reported_hits << '\t'
                        << fingerprint_hex(value.checksum) << '\t' << value.suffix_comparisons << '\t'
                        << value.character_comparisons << '\t' << value.gallop_probes << '\t'
                        << value.local_window_rows << '\t' << value.local_window_rows_max << '\t'
                        << value.predictions << '\t'
                        << (value.predictions == 0 ? 0.0 :
                            static_cast<double>(value.prediction_absolute_error_sum) /
                            static_cast<double>(value.predictions)) << '\t'
                        << value.prediction_error_p50 << '\t' << value.prediction_error_p95 << '\t'
                        << value.prediction_error_p99 << '\t' << value.prediction_absolute_error_max << '\t'
                        << value.local_window_rows_p50 << '\t' << value.local_window_rows_p95 << '\t'
                        << value.local_window_rows_p99 << '\t'
                        << value.full_binary_fallbacks << '\t' << value.status << '\t'
                        << value.fm_query_mode << '\t' << value.fm_batch_width << '\t'
                        << value.query_bases << '\t' << query_bases_per_second << '\t';
                if (baseline == huffman_scalar_seconds.end()) queries << "NA\n";
                else queries << speedup << '\n';
            }

            std::map<std::uint32_t, const LoadRaw*> save_workers;
            for (const auto& value : result.loads) {
                if (is_save_worker_result(value)) save_workers[value.repetition] = &value;
            }
            for (const auto& value : result.builds) {
                raw << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario) << '\t'
                    << result.method << "\tbuild\tNA\tNA\tNA\tbuild\tNA\t" << value.repetition
                    << "\t0\t0\t" << value.build_seconds << '\t' << value.build_user_seconds << '\t'
                    << value.build_system_seconds << '\t' << value.peak_rss_mb
                    << "\tbuild_worker_inherited_controller_dataset_plus_reference_plus_build"
                    << "\t0\t0\t0000000000000000\t" << value.status
                    << "\tNA\tNA\t0\t0\t0\t0\t0\t0\t0\t0\t0\tNA\tNA\t0\t0\tNA\t"
                    << result.threads << '\t'
                    << dataset.total_bases << '\t' << value.serialized_bytes << '\t'
                    << value.allocated_disk_bytes << '\t' << value.learned_index_bytes << '\t'
                    << value.sa_build_seconds << '\t' << value.isa_build_seconds << '\t'
                    << value.lcp_build_seconds << '\t' << value.child_build_seconds << '\t'
                    << value.learned_index_build_seconds << '\t'
                    << result.backend << '\t' << result.signature << '\t' << result.sdsl_version << '\t'
                    << static_cast<unsigned>(result.coordinate_width) << '\t'
                    << result.sa_sampling_rate << '\t' << result.canary_total_hits << '\t'
                    << result.canary_reported_hits << '\t'
                    << (result.has_canary
                        ? fingerprint_hex(result.canary_checksum)
                        : std::string("0000000000000000")) << "\t0\n";
                const auto save = save_workers.find(value.repetition);
                const auto* save_value = save == save_workers.end() ? nullptr : save->second;
                raw << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario) << '\t'
                    << result.method << "\tsave\tNA\tNA\tNA\tsave\tNA\t" << value.repetition
                    << "\t0\t0\t" << value.save_seconds << '\t'
                    << (save_value == nullptr ? 0.0 : save_value->user_seconds) << '\t'
                    << (save_value == nullptr ? 0.0 : save_value->system_seconds) << '\t'
                    << (save_value == nullptr ? 0.0 : save_value->peak_rss_mb) << '\t'
                    << (save_value == nullptr
                        ? "not_applicable"
                        : "save_worker_load_plus_save_inherited_controller_dataset")
                    << "\t0\t0\t0000000000000000\t" << value.status
                    << "\tNA\tNA\t0\t0\t0\t0\t0\t0\t0\t0\t0\tNA\tNA\t0\t0\tNA\t"
                    << result.threads << '\t'
                    << dataset.total_bases << '\t' << value.serialized_bytes << '\t'
                    << value.allocated_disk_bytes << '\t' << value.learned_index_bytes
                    << "\t0\t0\t0\t0\t0\t"
                    << result.backend << '\t' << result.signature << '\t' << result.sdsl_version << '\t'
                    << static_cast<unsigned>(result.coordinate_width) << '\t'
                    << result.sa_sampling_rate << '\t' << result.canary_total_hits << '\t'
                    << result.canary_reported_hits << '\t'
                    << (result.has_canary
                        ? fingerprint_hex(result.canary_checksum)
                        : std::string("0000000000000000")) << "\t0\n";
            }
            for (const auto& value : result.loads) {
                if (is_save_worker_result(value)) continue;
                raw << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario) << '\t'
                    << result.method << "\tload\tNA\tNA\tNA\tload\tNA\t" << value.repetition
                    << "\t0\t0\t" << value.seconds << '\t' << value.user_seconds << '\t'
                    << value.system_seconds << '\t' << value.peak_rss_mb
                    << "\tload_worker_inherited_controller_dataset_plus_load_plus_canary"
                    << "\t0\t0\t0000000000000000\t" << value.status
                    << "\tNA\tNA\t0\t0\t0\t0\t0\t0\t0\t0\t0\tNA\tNA\t0\t0\tNA\t"
                    << result.threads << '\t'
                    << dataset.total_bases << '\t' << serialized << '\t' << allocated << '\t'
                    << (result.builds.empty() ? 0 : result.builds.front().learned_index_bytes)
                    << "\t0\t0\t0\t0\t0\t"
                    << result.backend << '\t' << result.signature << '\t' << result.sdsl_version << '\t'
                    << static_cast<unsigned>(result.coordinate_width) << '\t'
                    << result.sa_sampling_rate << '\t' << result.canary_total_hits << '\t'
                    << result.canary_reported_hits << '\t'
                    << (result.has_canary
                        ? fingerprint_hex(result.canary_checksum)
                        : std::string("0000000000000000")) << "\t0\n";
            }
            for (const auto& value : result.queries) {
                raw << context.run_id << '\t' << dataset.name << '\t' << to_string(dataset.scenario) << '\t'
                    << result.method << "\tquery\t" << value.group << '\t' << value.pattern_length << '\t'
                    << value.strand << '\t' << value.operation << '\t' << value.max_hits << '\t'
                    << value.repetition << '\t' << value.query_count << '\t'
                    << value.skipped_high_frequency_queries << '\t' << value.seconds << '\t'
                    << value.user_seconds << '\t' << value.system_seconds << '\t' << value.peak_rss_mb << '\t'
                    << (value.operation == "count"
                        ? "count_worker_inherited_controller_dataset_plus_load_plus_query"
                        : "locate_worker_" + value.max_hits +
                          "_inherited_controller_dataset_plus_load_plus_query") << '\t'
                    << value.total_hits << '\t' << value.reported_hits << '\t'
                    << fingerprint_hex(value.checksum) << '\t' << value.status << "\tNA\tNA\t"
                    << value.suffix_comparisons << '\t' << value.character_comparisons << '\t'
                    << value.gallop_probes << '\t' << value.local_window_rows << '\t'
                    << value.local_window_rows_max << '\t' << value.predictions << '\t'
                    << value.prediction_absolute_error_sum << '\t'
                    << value.prediction_absolute_error_max << '\t'
                    << value.full_binary_fallbacks << '\t'
                    << value.fm_query_mode << '\t' << value.fm_batch_width << '\t'
                    << value.query_bases << '\t'
                    << (value.seconds == 0.0 ? 0.0 : static_cast<double>(value.query_bases) / value.seconds)
                    << "\tNA\t" << result.threads << '\t' << dataset.total_bases << '\t'
                    << serialized << '\t'
                    << allocated << '\t'
                    << (result.builds.empty() ? 0 : result.builds.front().learned_index_bytes)
                    << "\t0\t0\t0\t0\t0\t"
                    << result.backend << '\t' << result.signature << '\t' << result.sdsl_version << '\t'
                    << static_cast<unsigned>(result.coordinate_width) << '\t'
                    << result.sa_sampling_rate << '\t' << result.canary_total_hits << '\t'
                    << result.canary_reported_hits << '\t'
                    << (result.has_canary
                        ? fingerprint_hex(result.canary_checksum)
                        : std::string("0000000000000000")) << "\t1\n";
            }
        }
    }
    metadata.flush();
    builds.flush();
    queries.flush();
    raw.flush();
    require_output(metadata, metadata_path);
    require_output(builds, build_path);
    require_output(queries, query_path);
    require_output(raw, raw_path);
}

void write_legacy_output(
    const std::filesystem::path& path,
    const Dataset& dataset,
    const std::vector<MethodResult>& results) {
    std::ofstream output(path);
    if (!output) throw Error(ErrorCode::io_error, "cannot create benchmark output: " + path.string());
    output << "dataset\tdataset_fingerprint\ttotal_bases\tcontigs\tmethod\tbackend\t"
              "backend_signature\tsdsl_version\tcoordinate_width\tthreads\tbuild_seconds\t"
              "peak_rss_mb\tserialized_bytes\tload_seconds\tquery_count\tcount_qps\t"
              "locate_qps\ttotal_hits\treported_hits\tresult_checksum\t"
              "sa_sampling_rate\tallocated_disk_bytes\tbuild_worker_peak_rss_mb\t"
              "save_worker_peak_rss_mb\tload_worker_peak_rss_mb\tquery_worker_peak_rss_mb\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& result : results) {
        const auto build_seconds = values_for(result.builds, [](const auto& value) { return value.build_seconds; });
        const auto build_peak_rss = values_for(result.builds, [](const auto& value) { return value.peak_rss_mb; });
        const auto save_peak_rss = save_values_for(result.loads, [](const auto& value) { return value.peak_rss_mb; });
        const auto load_seconds = values_for(result.loads, [](const auto& value) { return value.seconds; });
        const auto load_peak_rss = values_for(result.loads, [](const auto& value) { return value.peak_rss_mb; });
        std::map<std::uint32_t, double> count_seconds;
        std::map<std::uint32_t, double> locate_seconds;
        std::uint64_t total_hits = 0;
        std::uint64_t reported_hits = 0;
        std::uint64_t checksum = checksum_seed();
        std::uint64_t query_count = 0;
        for (const auto& raw : result.queries) {
            if (raw.status != "ok" || raw.strand != "forward" ||
                raw.fm_query_mode != "scalar") continue;
            if (raw.operation == "count") {
                count_seconds[raw.repetition] += raw.seconds;
                if (raw.repetition == 0) {
                    total_hits += raw.total_hits;
                    query_count += raw.query_count;
                }
            } else if (raw.operation == "locate" && raw.max_hits == "1000") {
                locate_seconds[raw.repetition] += raw.seconds;
                if (raw.repetition == 0) {
                    reported_hits += raw.reported_hits;
                    mix_checksum(checksum, raw.checksum);
                }
            }
        }
        std::vector<double> count_values;
        std::vector<double> locate_values;
        for (const auto& value : count_seconds) count_values.push_back(value.second);
        for (const auto& value : locate_seconds) locate_values.push_back(value.second);
        const auto count_median = median(count_values);
        const auto locate_median = median(locate_values);
        const auto serialized = result.builds.empty() ? 0 : result.builds.front().serialized_bytes;
        const auto allocated = result.builds.empty() ? 0 : result.builds.front().allocated_disk_bytes;
        double query_peak_rss = 0.0;
        for (const auto& raw : result.queries) {
            if (raw.status == "ok") query_peak_rss = std::max(query_peak_rss, raw.peak_rss_mb);
        }
        output << dataset.name << '\t' << fingerprint_hex(dataset.fingerprint) << '\t'
               << dataset.total_bases << '\t' << dataset.contigs << '\t' << result.method << '\t'
               << result.backend << '\t' << result.signature << '\t' << result.sdsl_version << '\t'
               << static_cast<unsigned>(result.coordinate_width) << '\t' << result.threads << '\t'
               << median(build_seconds) << '\t'
               << result.peak_rss_mb << '\t' << serialized << '\t' << median(load_seconds) << '\t'
               << query_count << '\t' << (count_median == 0.0
                   ? 0.0 : static_cast<double>(query_count) / count_median) << '\t'
               << (locate_median == 0.0
                   ? 0.0 : static_cast<double>(query_count) / locate_median) << '\t'
               << total_hits << '\t' << reported_hits << '\t' << fingerprint_hex(checksum) << '\t'
               << result.sa_sampling_rate << '\t' << allocated << '\t' << median(build_peak_rss) << '\t'
               << median(save_peak_rss) << '\t' << median(load_peak_rss) << '\t'
               << query_peak_rss << '\n';
    }
    output.flush();
    require_output(output, path);
}

} // namespace sufkit::app::bench
