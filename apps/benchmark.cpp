#include "benchmark.hpp"

#include "app_support.hpp"
#include "benchmark_common.hpp"
#include "right_maximal_benchmark.hpp"

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace sufkit::app {
namespace {

using namespace bench;

std::vector<std::string> split_csv(const std::string& text, const std::string& option) {
    if (text.empty()) throw Error(ErrorCode::invalid_input, option + " must not be empty");
    std::vector<std::string> values;
    std::set<std::string> unique;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        const auto value = text.substr(begin, end == std::string::npos ? text.size() - begin : end - begin);
        if (value.empty()) throw Error(ErrorCode::invalid_input, option + " contains an empty value");
        if (!unique.insert(value).second) {
            throw Error(ErrorCode::invalid_input, option + " contains a duplicate value: " + value);
        }
        values.push_back(value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return values;
}

std::uint32_t parse_u32(
    const std::string& value,
    const std::string& option,
    bool allow_zero) {
    const auto parsed = parse_unsigned(value, option);
    if ((!allow_zero && parsed == 0) || parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(ErrorCode::invalid_input, "invalid value for " + option + ": " + value);
    }
    return static_cast<std::uint32_t>(parsed);
}

std::vector<std::string> parse_methods(const std::string& text) {
    const std::set<std::string> supported{
        "naive", "sa32", "sa64", "sa32-none", "sa64-none",
        "fm", "fm-huff", "fm-balanced", "fm-epr",
        "sa32-binary", "sa32-lcp-binary", "sa32-sapling", "sa32-child"};
    auto methods = split_csv(text, "--methods");
    for (const auto& method : methods) {
        if (supported.count(method) == 0) {
            throw Error(ErrorCode::invalid_input, "invalid benchmark method: " + method);
        }
    }
    if (std::find(methods.begin(), methods.end(), "fm") != methods.end() &&
        std::find(methods.begin(), methods.end(), "fm-huff") != methods.end()) {
        throw Error(ErrorCode::invalid_input, "fm and fm-huff are aliases and cannot be selected together");
    }
    return methods;
}

std::vector<std::string> parse_fm_query_modes(const std::string& text) {
    const std::set<std::string> supported{"scalar", "batch"};
    auto modes = split_csv(text, "--fm-query-modes");
    for (const auto& mode : modes) {
        if (supported.count(mode) == 0) {
            throw Error(ErrorCode::invalid_input, "invalid FM query mode: " + mode);
        }
    }
    return modes;
}

std::vector<std::uint32_t> parse_fm_batch_widths(const std::string& text) {
    std::vector<std::uint32_t> widths;
    for (const auto& value : split_csv(text, "--fm-batch-widths")) {
        const auto width = parse_u32(value, "--fm-batch-widths", false);
        if (width > 256) {
            throw Error(ErrorCode::invalid_input, "FM batch width must be in [1,256]");
        }
        widths.push_back(width);
    }
    return widths;
}

std::vector<Scenario> parse_scenarios(const std::string& text) {
    std::vector<Scenario> scenarios;
    for (const auto& value : split_csv(text, "--scenarios")) scenarios.push_back(parse_scenario(value));
    return scenarios;
}

std::vector<std::uint64_t> parse_lengths(const std::string& text) {
    std::vector<std::uint64_t> lengths;
    for (const auto& value : split_csv(text, "--pattern-lengths")) {
        const auto parsed = parse_unsigned(value, "--pattern-lengths");
        if (parsed == 0 || parsed > 1000000) {
            throw Error(ErrorCode::invalid_input, "invalid pattern length: " + value);
        }
        lengths.push_back(parsed);
    }
    return lengths;
}

std::vector<LocateLimit> parse_locate_limits(const std::string& text) {
    std::vector<LocateLimit> limits;
    for (const auto& value : split_csv(text, "--locate-limits")) {
        if (value == "all") limits.push_back({true, 0});
        else limits.push_back({false, parse_unsigned(value, "--locate-limits")});
    }
    return limits;
}

std::string require_value(const std::vector<std::string>& arguments, std::size_t& index) {
    const auto& option = arguments[index];
    if (index + 1 >= arguments.size()) {
        throw Error(ErrorCode::invalid_input, "missing value for " + option);
    }
    return arguments[++index];
}

template <class Values, class Formatter>
std::string join_values(const Values& values, Formatter&& formatter) {
    std::ostringstream output;
    bool first = true;
    for (const auto& value : values) {
        if (!first) output << ',';
        first = false;
        output << formatter(value);
    }
    return output.str();
}

Options parse_benchmark_options(const std::vector<std::string>& arguments) {
    Options options;
    std::optional<std::string> methods;
    std::optional<std::string> scenarios;
    std::optional<std::string> lengths;
    std::optional<std::string> limits;
    std::optional<std::string> fm_query_modes;
    std::optional<std::string> fm_batch_widths;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& option = arguments[index];
        if (option == "--quick") options.legacy_quick = true;
        else if (option == "--smoke") options.legacy_smoke = true;
        else if (option == "--profile") {
            options.profile = parse_profile(require_value(arguments, index));
            options.profile_explicit = true;
        } else if (option == "--reference") options.reference_path = require_value(arguments, index);
        else if (option == "--queries") options.query_path = require_value(arguments, index);
        else if (option == "--output") options.output_path = require_value(arguments, index);
        else if (option == "--output-dir") options.output_directory = require_value(arguments, index);
        else if (option == "--methods") methods = require_value(arguments, index);
        else if (option == "--scenarios") scenarios = require_value(arguments, index);
        else if (option == "--pattern-lengths") lengths = require_value(arguments, index);
        else if (option == "--locate-limits") limits = require_value(arguments, index);
        else if (option == "--fm-query-modes") fm_query_modes = require_value(arguments, index);
        else if (option == "--fm-batch-widths") fm_batch_widths = require_value(arguments, index);
        else if (option == "--seed") options.seed = parse_unsigned(require_value(arguments, index), option);
        else if (option == "--build-repetitions") {
            options.build_repetitions = parse_u32(require_value(arguments, index), option, false);
        } else if (option == "--query-repetitions") {
            options.query_repetitions = parse_u32(require_value(arguments, index), option, false);
        } else if (option == "--warmups") {
            options.warmups = parse_u32(require_value(arguments, index), option, true);
        } else if (option == "--learned-k") {
            options.learned_k = parse_u32(require_value(arguments, index), option, false);
            if (options.learned_k > 31)
                throw Error(ErrorCode::invalid_input, "--learned-k must be in [1,31]");
        } else if (option == "--learned-memory-bp") {
            options.learned_memory_overhead_basis_points =
                parse_u32(require_value(arguments, index), option, false);
        } else if (option == "--learned-bucket-bits") {
            options.learned_bucket_bits = parse_u32(require_value(arguments, index), option, true);
            if (*options.learned_bucket_bits > 31)
                throw Error(ErrorCode::invalid_input, "--learned-bucket-bits must be at most 31");
        } else {
            throw Error(ErrorCode::invalid_input, "unknown benchmark option: " + option);
        }
    }

    const auto synthetic_selectors = static_cast<unsigned>(options.profile_explicit) +
        static_cast<unsigned>(options.legacy_quick) + static_cast<unsigned>(options.legacy_smoke);
    if (synthetic_selectors > 1) {
        throw Error(ErrorCode::invalid_input, "--profile, --quick, and --smoke are mutually exclusive");
    }
    if (options.reference_path && synthetic_selectors != 0) {
        throw Error(ErrorCode::invalid_input, "--reference cannot be combined with a synthetic profile");
    }
    if (options.query_path && !options.reference_path) {
        throw Error(ErrorCode::invalid_input, "--queries requires --reference");
    }
    if (!options.reference_path && synthetic_selectors == 0) {
        throw Error(ErrorCode::invalid_input, "provide --profile, --quick, --smoke, or --reference");
    }
    if (options.output_path.has_value() == options.output_directory.has_value()) {
        throw Error(ErrorCode::invalid_input, "provide exactly one of --output or --output-dir");
    }

    if (options.reference_path) options.profile = Profile::user;
    else if (options.legacy_smoke) options.profile = Profile::smoke;
    else if (options.legacy_quick) options.profile = Profile::quick;

    if (methods) options.methods = parse_methods(*methods);
    else if (options.profile == Profile::smoke || options.profile == Profile::quick) {
        options.methods = {"naive", "sa32", "sa64", "fm"};
    } else {
        options.methods = {"sa32", "sa64", "fm"};
    }

    if (scenarios) {
        if (options.profile == Profile::user) {
            throw Error(ErrorCode::invalid_input, "--scenarios is invalid with --reference");
        }
        options.scenarios = parse_scenarios(*scenarios);
    } else if (options.profile == Profile::standard) {
        options.scenarios = {Scenario::mixed, Scenario::balanced, Scenario::gc_skewed,
            Scenario::repeat_rich, Scenario::n_islands, Scenario::many_contig};
    } else if (options.profile == Profile::user) {
        options.scenarios = {Scenario::user};
    } else {
        options.scenarios = {Scenario::mixed};
    }

    options.pattern_lengths = lengths
        ? parse_lengths(*lengths) : std::vector<std::uint64_t>{20, 50, 100, 200, 500};
    options.locate_limits = limits
        ? parse_locate_limits(*limits) : std::vector<LocateLimit>{{false, 1}, {false, 10}, {false, 1000}};
    if (fm_query_modes) options.fm_query_modes = parse_fm_query_modes(*fm_query_modes);
    if (fm_batch_widths) options.fm_batch_widths = parse_fm_batch_widths(*fm_batch_widths);
    if (options.output_path && options.scenarios.size() != 1) {
        throw Error(ErrorCode::invalid_input, "--output supports one scenario; use --output-dir for multiple scenarios");
    }
    return options;
}

class ScratchDirectory {
public:
    explicit ScratchDirectory(std::filesystem::path base_directory) {
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        if (base_directory.empty()) base_directory = std::filesystem::current_path();
        std::filesystem::create_directories(base_directory);
        path_ = base_directory /
            ("sufkit-benchmark-" + std::to_string(static_cast<long long>(getpid())) + "-" +
             std::to_string(static_cast<long long>(tick)));
        if (!std::filesystem::create_directory(path_)) {
            throw Error(ErrorCode::io_error, "cannot create benchmark scratch directory: " + path_.string());
        }
    }

    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    ~ScratchDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
    if (gmtime_r(&value, &utc) == nullptr) return "unknown";
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

void classify_user_dataset(Dataset& dataset, const std::filesystem::path& scratch) {
    if (dataset.queries.empty() || dataset.queries.front().group != "user") return;
    auto reference = GenomeReference::from_records(dataset.records);
    auto classifier = FmIndex::build(reference);
    const auto path = scratch / "user-query-classifier.sufidx";
    classifier.save(path);
    classify_user_queries(dataset, path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void print_help() {
    std::cout <<
        "sufkit bench --profile smoke|quick|standard|full --output-dir DIR [options]\n"
        "sufkit bench --reference REF.fa[.gz] [--queries Q.fa[.gz]] --output-dir DIR [options]\n"
        "Compatibility: sufkit bench --smoke|--quick --output RESULTS.tsv\n\n"
        "Options:\n"
        "  --scenarios mixed,balanced,gc-skewed,repeat-rich,n-islands,many-contig\n"
        "  --methods naive,sa32,sa64,sa32-none,sa64-none,fm,fm-huff,fm-balanced,"
        "fm-epr,sa32-binary,sa32-lcp-binary,sa32-sapling,sa32-child\n"
        "  --fm-query-modes scalar,batch --fm-batch-widths 1,4,8,16,32\n"
        "  --learned-k 20 --learned-memory-bp 100 [--learned-bucket-bits N]\n"
        "  --pattern-lengths 20,50,100,200,500\n"
        "  --locate-limits 1,10,1000,all\n"
        "  --seed 20260822\n"
        "  --build-repetitions N --query-repetitions N --warmups N\n\n"
        "right-maximal exact match workload:\n"
        "  --workload right-maximal --profile smoke|quick --output-dir DIR\n"
        "  --methods right-maximal-baseline,right-maximal-lcp,right-maximal-child,right-maximal-suffix-link,right-maximal-full,\n"
        "            right-maximal-suffix-link-binary,right-maximal-suffix-link-sapling,mummer4\n"
        "  --min-lengths 20,50,100 [--mummer4 PATH]\n"
        "  --learned-k 20 --learned-memory-bp 100 [--learned-bucket-bits N]\n"
        "  or --workload right-maximal --reference REF.fa[.gz] [--queries Q.fa[.gz]]\n";
}

} // namespace

int run_benchmark(const std::vector<std::string>& arguments) {
    for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == "--workload" && arguments[index + 1] == "right-maximal") {
            return right_maximal_bench::run(arguments);
        }
    }
    if (arguments.size() == 1 && arguments.front() == "--help") {
        print_help();
        return 0;
    }
    const auto options = parse_benchmark_options(arguments);
    const auto output_target = options.output_directory
        ? *options.output_directory : *options.output_path;
    ScratchDirectory scratch(output_target.parent_path());

    std::vector<Dataset> datasets;
    if (options.reference_path) {
        datasets.push_back(load_user_dataset(
            *options.reference_path,
            options.query_path,
            options.seed,
            profile_spec(Profile::user).query_count,
            options.pattern_lengths));
        classify_user_dataset(datasets.back(), scratch.path());
    } else {
        for (const auto scenario : options.scenarios) {
            datasets.push_back(generate_dataset(
                options.profile, scenario, options.seed, options.pattern_lengths));
        }
    }

    std::vector<std::vector<MethodResult>> all_results;
    all_results.reserve(datasets.size());
    for (std::size_t dataset_index = 0; dataset_index < datasets.size(); ++dataset_index) {
        const auto& dataset = datasets[dataset_index];
        std::vector<MethodResult> results;
        const auto dataset_scratch = scratch.path() / ("dataset-" + std::to_string(dataset_index));
        std::filesystem::create_directory(dataset_scratch);
        for (const auto& method : options.methods) {
            std::cerr << "benchmarking " << dataset.name << " with " << method << "...\n";
            results.push_back(run_method_isolated(method, dataset, options, dataset_scratch));
        }
        all_results.push_back(std::move(results));
    }

    if (options.output_directory) {
        RunContext context;
        context.timestamp = utc_timestamp();
        context.run_id = std::string(to_string(options.profile)) + "-" + context.timestamp + "-" +
            fingerprint_hex(datasets.front().fingerprint).substr(0, 8);
        context.profile = options.profile;
        context.seed = options.seed;
        context.methods = join_values(options.methods, [](const auto& value) { return value; });
        context.pattern_lengths = join_values(options.pattern_lengths, [](const auto value) {
            return std::to_string(value);
        });
        context.locate_limits = join_values(options.locate_limits, [](const auto& value) {
            return value.all ? std::string("all") : std::to_string(value.value);
        });
        context.fm_query_modes = join_values(options.fm_query_modes, [](const auto& value) {
            return value;
        });
        context.fm_batch_widths = join_values(options.fm_batch_widths, [](const auto value) {
            return std::to_string(value);
        });
        const auto spec = profile_spec(options.profile);
        context.build_repetitions = std::to_string(
            options.build_repetitions.value_or(spec.build_repetitions));
        context.query_repetitions = options.query_repetitions
            ? std::to_string(*options.query_repetitions)
            : (options.profile == Profile::quick &&
                std::find(options.methods.begin(), options.methods.end(), "naive") != options.methods.end()
                ? std::to_string(spec.query_repetitions) + ";naive=1"
                : std::to_string(spec.query_repetitions));
        context.warmups = options.warmups
            ? std::to_string(*options.warmups)
            : (options.profile == Profile::quick &&
                std::find(options.methods.begin(), options.methods.end(), "naive") != options.methods.end()
                ? std::to_string(spec.warmups) + ";naive=0"
                : std::to_string(spec.warmups));
        context.learned_k = std::to_string(options.learned_k);
        context.learned_memory_overhead_basis_points =
            std::to_string(options.learned_memory_overhead_basis_points);
        context.learned_bucket_bits = options.learned_bucket_bits
            ? std::to_string(*options.learned_bucket_bits) : "auto";
        write_result_directory(*options.output_directory, context, datasets, all_results);
    } else {
        write_legacy_output(*options.output_path, datasets.front(), all_results.front());
    }
    for (std::size_t index = 0; index < datasets.size(); ++index) {
        validate_results(datasets[index], all_results[index], options.profile);
    }
    const auto& destination = options.output_directory ? *options.output_directory : *options.output_path;
    std::cerr << "benchmark results written to " << destination.string() << '\n';
    return 0;
}

} // namespace sufkit::app
