#include <sufkit/sufkit.hpp>

#include "caps_backend.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string profile;
    std::filesystem::path reference;
    std::filesystem::path output_dir;
    std::vector<std::string> methods;
    std::vector<std::uint32_t> threads;
    sufkit::SaAcceleration acceleration = sufkit::SaAcceleration::none;
    std::uint32_t repetitions = 0;
    std::uint64_t seed = 20260822;
};

struct WorkerResult {
    char method[16]{};
    char backend[32]{};
    char signature[128]{};
    char status[160]{};
    std::uint64_t text_symbols = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t subproblems = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t sa_hash_1 = 0;
    std::uint64_t sa_hash_2 = 0;
    std::uint64_t exact_checksum = 0;
    std::uint64_t mem_checksum = 0;
    std::uint32_t threads = 0;
    std::uint32_t repetition = 0;
    std::uint8_t coordinate_width = 0;
    double build_seconds = 0.0;
    double user_seconds = 0.0;
    double system_seconds = 0.0;
    double peak_rss_mb = 0.0;
};

struct Usage {
    double user = 0.0;
    double system = 0.0;
};

Usage usage_now() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return {};
    const auto seconds = [](const timeval& value) {
        return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_usec) / 1'000'000.0;
    };
    return {seconds(usage.ru_utime), seconds(usage.ru_stime)};
}

double peak_rss_mb() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    return static_cast<double>(usage.ru_maxrss) / 1024.0;
}

void copy_text(char* destination, std::size_t capacity, const std::string& value) {
    if (capacity == 0) return;
    const auto count = std::min(capacity - 1, value.size());
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
}

std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(',', begin);
        const auto part = value.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (part.empty()) throw sufkit::Error(sufkit::ErrorCode::invalid_input, "empty list item");
        result.push_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::uint64_t parse_u64(const std::string& value, const char* name) {
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try { parsed = std::stoull(value, &consumed); }
    catch (...) { throw sufkit::Error(sufkit::ErrorCode::invalid_input, std::string("invalid ") + name); }
    if (consumed != value.size()) throw sufkit::Error(sufkit::ErrorCode::invalid_input, std::string("invalid ") + name);
    return static_cast<std::uint64_t>(parsed);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        if (name == "--help") {
            std::cout
                << "sufkit_sa_build_bench (--profile smoke|quick|standard | --reference REF.fa[.gz])\n"
                << "  --output-dir DIR [--methods div32,div64,caps32,caps64]\n"
                << "  [--threads 1,2,4,8] [--acceleration none|full]\n"
                << "  [--repetitions N] [--seed N]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) throw sufkit::Error(sufkit::ErrorCode::invalid_input, "missing value for " + name);
        const std::string value = argv[++index];
        if (name == "--profile") options.profile = value;
        else if (name == "--reference") options.reference = value;
        else if (name == "--output-dir") options.output_dir = value;
        else if (name == "--methods") options.methods = split(value);
        else if (name == "--threads") {
            for (const auto& item : split(value)) {
                const auto parsed = parse_u64(item, "thread count");
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
                    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "thread count is out of range");
                options.threads.push_back(static_cast<std::uint32_t>(parsed));
            }
        } else if (name == "--acceleration") {
            if (value == "none") options.acceleration = sufkit::SaAcceleration::none;
            else if (value == "full") options.acceleration = sufkit::SaAcceleration::full;
            else throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid acceleration");
        } else if (name == "--repetitions") {
            const auto parsed = parse_u64(value, "repetitions");
            if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
                throw sufkit::Error(sufkit::ErrorCode::invalid_input, "repetitions are out of range");
            options.repetitions = static_cast<std::uint32_t>(parsed);
        } else if (name == "--seed") options.seed = parse_u64(value, "seed");
        else throw sufkit::Error(sufkit::ErrorCode::invalid_input, "unknown option: " + name);
    }
    if (options.output_dir.empty()) throw sufkit::Error(sufkit::ErrorCode::invalid_input, "--output-dir is required");
    if ((!options.profile.empty()) == (!options.reference.empty()))
        throw sufkit::Error(sufkit::ErrorCode::invalid_input, "specify exactly one of --profile or --reference");
    if (!options.profile.empty() && options.profile != "smoke" && options.profile != "quick" && options.profile != "standard")
        throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid profile");
    if (options.methods.empty()) options.methods = {"div32", "caps32"};
    for (const auto& method : options.methods)
        if (method != "div32" && method != "div64" && method != "caps32" && method != "caps64")
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid method: " + method);
    if (options.threads.empty()) {
        if (options.profile == "smoke") options.threads = {1, 2};
        else if (options.profile == "standard") options.threads = {1, 2, 4, 8, 16, 32};
        else options.threads = {1, 2, 4, 8};
    }
    if (options.repetitions == 0) options.repetitions = options.profile == "smoke" ? 1 : 3;
    return options;
}

std::uint64_t profile_bases(const std::string& profile) {
    if (profile == "smoke") return 1ULL << 20;
    if (profile == "quick") return 64ULL << 20;
    return 1ULL << 30;
}

void generate_reference(const std::filesystem::path& path, std::uint64_t total_bases, std::uint64_t seed) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw sufkit::Error(sufkit::ErrorCode::io_error, "cannot create generated reference");
    std::uint64_t state = seed;
    constexpr std::array<char, 4> bases{{'A', 'C', 'G', 'T'}};
    const std::string repeat = "ACGTACGTGATTACATTTTCCCCAAAAGGGG";
    for (std::uint32_t contig = 0; contig < 4; ++contig) {
        output << ">synthetic_" << contig << '\n';
        const auto begin = total_bases * contig / 4;
        const auto end = total_bases * (contig + 1) / 4;
        std::uint64_t column = 0;
        for (auto position = begin; position < end; ++position) {
            state += 0x9e3779b97f4a7c15ULL;
            auto mixed = state;
            mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9ULL;
            mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebULL;
            mixed ^= mixed >> 31;
            char base = bases[mixed & 3ULL];
            if (contig == 1 && (position - begin) % 3 != 0)
                base = repeat[static_cast<std::size_t>((position - begin) % repeat.size())];
            if (contig == 2 && (position - begin) % 9973 < 37) base = 'N';
            output.put(base);
            if (++column == 80) { output.put('\n'); column = 0; }
        }
        if (column != 0) output.put('\n');
    }
    if (!output) throw sufkit::Error(sufkit::ErrorCode::io_error, "failed to write generated reference");
}

void mix(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash *= 1099511628211ULL;
}

std::pair<std::uint64_t, std::uint64_t> sa_checksum(const sufkit::SuffixArray& index) {
    std::uint64_t first = 1469598103934665603ULL;
    std::uint64_t second = 0x6a09e667f3bcc909ULL;
    for (std::uint64_t row = 0; row < index.info().text_symbols; ++row) {
        const auto suffix = index.suffix_at(row);
        first ^= suffix;
        first *= 1099511628211ULL;
        second ^= suffix + 0x9e3779b97f4a7c15ULL + (second << 7) + (second >> 3);
    }
    return {first, second};
}

std::uint64_t exact_checksum(const sufkit::SuffixArray& index) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::string pattern : {"ACGT", "GATTACA", "TTTTCCCC", "AAAAGGGG", "CGTACGTG"}) {
        const auto result = index.locate(pattern);
        mix(hash, result.total_hits);
        for (const auto& match : result.hits) {
            mix(hash, match.sequence_id);
            mix(hash, match.position);
            mix(hash, match.length);
        }
    }
    return hash;
}

std::uint64_t mem_checksum(const sufkit::SuffixArray& index) {
    sufkit::MemOptions options;
    options.min_length = 12;
    options.strands = sufkit::StrandMode::both;
    const auto result = index.find_mems("TTACACGTACGTGATTACATTTTCCCCAAAAGGGGACGT");
    std::uint64_t hash = 0x243f6a8885a308d3ULL;
    mix(hash, result.total_matches);
    for (const auto& match : result.matches) {
        mix(hash, match.sequence_id);
        mix(hash, match.reference_position);
        mix(hash, match.query_position);
        mix(hash, match.length);
        mix(hash, static_cast<std::uint64_t>(match.strand));
    }
    return hash;
}

sufkit::SuffixArrayBuildOptions build_options(
    const std::string& method,
    std::uint32_t threads,
    sufkit::SaAcceleration acceleration) {
    sufkit::SuffixArrayBuildOptions options;
    options.backend = method.rfind("caps", 0) == 0
        ? sufkit::SaBackend::caps
        : sufkit::SaBackend::divsufsort;
    options.coordinate_width = method.size() >= 2 && method.substr(method.size() - 2) == "32"
        ? sufkit::CoordinateWidth::bits32
        : sufkit::CoordinateWidth::bits64;
    options.threads = threads;
    options.acceleration = acceleration;
    return options;
}

WorkerResult run_worker(
    const Options& options,
    const std::filesystem::path& reference_path,
    const std::string& method,
    std::uint32_t threads,
    std::uint32_t repetition,
    const std::filesystem::path& index_path) {
    WorkerResult result;
    copy_text(result.method, sizeof(result.method), method);
    result.threads = threads;
    result.repetition = repetition;
    try {
        const auto reference = sufkit::GenomeReference::from_fasta(reference_path);
        const auto usage_begin = usage_now();
        const auto begin = Clock::now();
        auto index = sufkit::SuffixArray::build(
            reference,
            build_options(method, threads, options.acceleration));
        result.build_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        const auto usage_end = usage_now();
        result.user_seconds = usage_end.user - usage_begin.user;
        result.system_seconds = usage_end.system - usage_begin.system;
        result.peak_rss_mb = peak_rss_mb();
        const auto info = index.info();
        result.text_symbols = info.text_symbols;
        result.fingerprint = info.fingerprint;
        result.coordinate_width = info.coordinate_width;
        result.subproblems = method.rfind("caps", 0) == 0
            ? sufkit::detail::caps_subproblem_count(info.text_symbols, threads)
            : 0;
        copy_text(result.backend, sizeof(result.backend), info.backend);
        copy_text(result.signature, sizeof(result.signature), info.backend_signature);
        const auto hashes = sa_checksum(index);
        result.sa_hash_1 = hashes.first;
        result.sa_hash_2 = hashes.second;
        result.exact_checksum = exact_checksum(index);
        result.mem_checksum = options.acceleration == sufkit::SaAcceleration::full
            ? mem_checksum(index)
            : 0;
        index.save(index_path);
        result.serialized_bytes = std::filesystem::file_size(index_path);
        auto loaded = sufkit::SuffixArray::load(index_path);
        if (exact_checksum(loaded) != result.exact_checksum ||
            (options.acceleration == sufkit::SaAcceleration::full &&
             mem_checksum(loaded) != result.mem_checksum)) {
            throw sufkit::Error(sufkit::ErrorCode::build_failure, "save/load checksum mismatch");
        }
        std::filesystem::remove(index_path);
        copy_text(result.status, sizeof(result.status), "ok");
    } catch (const sufkit::Error& error) {
        copy_text(result.status, sizeof(result.status),
                  std::string(sufkit::to_string(error.code())) + ":" + error.what());
    } catch (const std::exception& error) {
        copy_text(result.status, sizeof(result.status), std::string("error:") + error.what());
    }
    return result;
}

bool write_all(int descriptor, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size != 0) {
        const auto written = write(descriptor, bytes, size);
        if (written <= 0) return false;
        bytes += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

bool read_all(int descriptor, void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size != 0) {
        const auto count = read(descriptor, bytes, size);
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

WorkerResult isolated_worker(
    const Options& options,
    const std::filesystem::path& reference,
    const std::string& method,
    std::uint32_t threads,
    std::uint32_t repetition) {
    std::array<int, 2> descriptors{};
    if (pipe(descriptors.data()) != 0)
        throw sufkit::Error(sufkit::ErrorCode::build_failure, "benchmark pipe failed");
    const auto index_path = options.output_dir /
        ("worker-" + method + "-t" + std::to_string(threads) + "-r" + std::to_string(repetition) + ".sufidx");
    const auto child = fork();
    if (child < 0) throw sufkit::Error(sufkit::ErrorCode::build_failure, "benchmark fork failed");
    if (child == 0) {
        close(descriptors[0]);
        const auto result = run_worker(options, reference, method, threads, repetition, index_path);
        const bool written = write_all(descriptors[1], &result, sizeof(result));
        close(descriptors[1]);
        std::_Exit(written ? 0 : 1);
    }
    close(descriptors[1]);
    WorkerResult result;
    const bool received = read_all(descriptors[0], &result, sizeof(result));
    close(descriptors[0]);
    int status = 0;
    waitpid(child, &status, 0);
    if (!received || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw sufkit::Error(sufkit::ErrorCode::build_failure, "benchmark worker failed");
    return result;
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    if (values.empty()) return 0.0;
    const auto middle = values.size() / 2;
    return values.size() % 2 == 0
        ? (values[middle - 1] + values[middle]) / 2.0
        : values[middle];
}

std::string hex_pair(std::uint64_t first, std::uint64_t second) {
    std::ostringstream output;
    output << std::hex << std::setfill('0') << std::setw(16) << first
           << ':' << std::setw(16) << second;
    return output.str();
}

void write_outputs(
    const Options& options,
    const std::vector<WorkerResult>& results,
    long logical_cpus) {
    std::ofstream metadata(options.output_dir / "run_metadata.tsv");
    metadata << "profile\tseed\tlogical_cpus\tacceleration\ttext_symbols\tdataset_fingerprint\n";
    const auto first_ok = std::find_if(results.begin(), results.end(), [](const auto& value) {
        return std::string(value.status) == "ok";
    });
    metadata << (options.profile.empty() ? "reference" : options.profile) << '\t'
             << options.seed << '\t' << logical_cpus << '\t'
             << sufkit::to_string(options.acceleration) << '\t'
             << (first_ok == results.end() ? 0 : first_ok->text_symbols) << '\t'
             << (first_ok == results.end() ? 0 : first_ok->fingerprint) << '\n';

    std::ofstream raw(options.output_dir / "raw_repetitions.tsv");
    raw << "method\teffective_backend\tbackend_signature\tcoordinate_width\tthreads\t"
           "subproblem_count\tacceleration\trepetition\tbuild_wall_seconds\tuser_cpu_seconds\t"
           "system_cpu_seconds\tpeak_rss_mb\tserialized_bytes\tsa_checksum\texact_checksum\t"
           "mem_checksum\tstatus\n";
    raw << std::fixed << std::setprecision(6);
    for (const auto& result : results) {
        raw << result.method << '\t' << result.backend << '\t' << result.signature << '\t'
            << static_cast<unsigned>(result.coordinate_width) << '\t' << result.threads << '\t'
            << result.subproblems << '\t' << sufkit::to_string(options.acceleration) << '\t'
            << result.repetition << '\t' << result.build_seconds << '\t' << result.user_seconds << '\t'
            << result.system_seconds << '\t' << result.peak_rss_mb << '\t'
            << result.serialized_bytes << '\t' << hex_pair(result.sa_hash_1, result.sa_hash_2) << '\t'
            << result.exact_checksum << '\t' << result.mem_checksum << '\t' << result.status << '\n';
    }

    using Key = std::pair<std::string, std::uint32_t>;
    std::map<Key, std::vector<const WorkerResult*>> groups;
    for (const auto& result : results)
        if (std::string(result.status) == "ok") groups[{result.method, result.threads}].push_back(&result);
    std::map<Key, double> medians;
    for (const auto& [key, group] : groups) {
        std::vector<double> times;
        for (const auto* value : group) times.push_back(value->build_seconds);
        medians[key] = median(std::move(times));
    }

    std::ofstream summary(options.output_dir / "build_results.tsv");
    summary << "method\teffective_backend\tcoordinate_width\tthreads\tacceleration\trepetitions\t"
               "build_seconds_median\tbuild_seconds_min\tbuild_seconds_max\tpeak_rss_mb_median\t"
               "serialized_bytes\tspeedup_vs_divsufsort_same_width\tparallel_efficiency_vs_caps_1_thread\tstatus\n";
    summary << std::fixed << std::setprecision(6);
    for (const auto& [key, group] : groups) {
        std::vector<double> times;
        std::vector<double> rss;
        for (const auto* value : group) {
            times.push_back(value->build_seconds);
            rss.push_back(value->peak_rss_mb);
        }
        const auto current = medians[key];
        const std::string method = key.first;
        const bool is32 = method.size() >= 2 && method.substr(method.size() - 2) == "32";
        const std::string baseline = is32 ? "div32" : "div64";
        const auto baseline_it = medians.find({baseline, key.second});
        const double speedup = baseline_it == medians.end() || current == 0.0
            ? 0.0 : baseline_it->second / current;
        const std::string caps = is32 ? "caps32" : "caps64";
        const auto one_thread = medians.find({caps, 1});
        const double efficiency = method.rfind("caps", 0) != 0 ||
                                  one_thread == medians.end() || current == 0.0
            ? 0.0 : one_thread->second / (current * static_cast<double>(key.second));
        summary << method << '\t' << group.front()->backend << '\t'
                << static_cast<unsigned>(group.front()->coordinate_width) << '\t' << key.second << '\t'
                << sufkit::to_string(options.acceleration) << '\t' << group.size() << '\t'
                << median(times) << '\t' << *std::min_element(times.begin(), times.end()) << '\t'
                << *std::max_element(times.begin(), times.end()) << '\t' << median(rss) << '\t'
                << group.front()->serialized_bytes << '\t' << speedup << '\t' << efficiency << "\tok\n";
    }
    for (const auto& result : results) {
        if (std::string(result.status) == "ok") continue;
        summary << result.method << '\t' << result.backend << '\t'
                << static_cast<unsigned>(result.coordinate_width) << '\t' << result.threads << '\t'
                << sufkit::to_string(options.acceleration) << "\t0\t0\t0\t0\t0\t0\t0\t0\t"
                << result.status << '\n';
    }
}

void correctness_gate(const std::vector<WorkerResult>& results) {
    const WorkerResult* reference = nullptr;
    for (const auto& result : results) {
        if (std::string(result.status) != "ok") continue;
        if (reference == nullptr) { reference = &result; continue; }
        if (result.text_symbols != reference->text_symbols ||
            result.fingerprint != reference->fingerprint ||
            result.sa_hash_1 != reference->sa_hash_1 ||
            result.sa_hash_2 != reference->sa_hash_2 ||
            result.exact_checksum != reference->exact_checksum ||
            result.mem_checksum != reference->mem_checksum) {
            throw sufkit::Error(
                sufkit::ErrorCode::build_failure,
                std::string("correctness mismatch between ") + reference->method + " and " + result.method);
        }
    }
    if (reference == nullptr)
        throw sufkit::Error(sufkit::ErrorCode::build_failure, "no benchmark method completed successfully");
}

int run(int argc, char** argv) {
    const auto options = parse_options(argc, argv);
    std::filesystem::create_directories(options.output_dir);
    for (const char* name : {"run_metadata.tsv", "raw_repetitions.tsv", "build_results.tsv"}) {
        if (std::filesystem::exists(options.output_dir / name))
            throw sufkit::Error(sufkit::ErrorCode::io_error, "benchmark output already exists");
    }
    auto reference = options.reference;
    if (!options.profile.empty()) {
        reference = options.output_dir / "generated_reference.fa";
        if (std::filesystem::exists(reference))
            throw sufkit::Error(sufkit::ErrorCode::io_error, "generated reference already exists");
        generate_reference(reference, profile_bases(options.profile), options.seed);
    }
    const long logical_cpus = std::max<long>(1, sysconf(_SC_NPROCESSORS_ONLN));
    std::vector<WorkerResult> results;
    for (const auto& method : options.methods) {
        for (const auto threads : options.threads) {
            if (threads > static_cast<std::uint64_t>(logical_cpus)) {
                WorkerResult skipped;
                copy_text(skipped.method, sizeof(skipped.method), method);
                copy_text(skipped.status, sizeof(skipped.status), "not_applicable:threads_exceed_logical_cpus");
                skipped.threads = threads;
                results.push_back(skipped);
                continue;
            }
            for (std::uint32_t repetition = 1; repetition <= options.repetitions; ++repetition) {
                std::cerr << "running " << method << " threads=" << threads
                          << " repetition=" << repetition << '/' << options.repetitions << '\n';
                results.push_back(isolated_worker(options, reference, method, threads, repetition));
            }
        }
    }
    write_outputs(options, results, logical_cpus);
    correctness_gate(results);
    std::cout << "SA build benchmark completed successfully\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const sufkit::Error& error) {
        std::cerr << sufkit::to_string(error.code()) << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
