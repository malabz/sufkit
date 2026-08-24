#include <sufkit/sufkit.hpp>

#include "caps_backend.hpp"
#include "benchmark_profiles.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string profile;
    std::filesystem::path reference;
    std::filesystem::path output_dir;
    std::vector<std::string> methods;
    std::vector<std::uint32_t> threads;
    std::vector<std::uint32_t> sampling_rates;
    sufkit::SaAcceleration acceleration = sufkit::SaAcceleration::none;
    bool learned_index = false;
    std::string acceleration_label = "none";
    std::uint32_t repetitions = 0;
    std::uint64_t seed = 20260822;
};

struct WorkerResult {
    char method[16]{};
    char backend[32]{};
    char signature[128]{};
    char status[160]{};
    std::uint64_t text_symbols = 0;
    std::uint64_t suffix_count = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t subproblems = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t allocated_disk_bytes = 0;
    std::uint64_t learned_index_bytes = 0;
    std::uint64_t total_bases = 0;
    std::uint64_t sequence_count = 0;
    std::uint64_t sa_hash_1 = 0;
    std::uint64_t sa_hash_2 = 0;
    std::uint64_t exact_checksum = 0;
    std::uint64_t right_maximal_checksum = 0;
    std::uint32_t threads = 0;
    std::uint32_t sampling_rate = 1;
    std::uint32_t repetition = 0;
    std::uint8_t coordinate_width = 0;
    double reference_read_seconds = 0.0;
    double normalization_seconds = 0.0;
    double sa_seconds = 0.0;
    double isa_seconds = 0.0;
    double lcp_seconds = 0.0;
    double child_seconds = 0.0;
    double sapling_seconds = 0.0;
    double build_seconds = 0.0;
    double build_user_seconds = 0.0;
    double build_system_seconds = 0.0;
    double build_peak_rss_mb = 0.0;
    double save_seconds = 0.0;
    double save_user_seconds = 0.0;
    double save_system_seconds = 0.0;
    double save_peak_rss_mb = 0.0;
    double load_seconds = 0.0;
    double load_user_seconds = 0.0;
    double load_system_seconds = 0.0;
    double load_peak_rss_mb = 0.0;
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
                << "sufkit_sa_build_bench (--profile smoke|quick|standard|full | --reference REF.fa[.gz])\n"
                << "  --output-dir DIR [--methods div32,div64,caps32,caps64]\n"
                << "  [--threads 1,2,4,8] [--sampling-rates 1,2,4,8]\n"
                << "  [--acceleration none|default|suffix-link|full|sapling]\n"
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
        } else if (name == "--sampling-rates") {
            for (const auto& item : split(value)) {
                const auto parsed = parse_u64(item, "sampling rate");
                if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max())
                    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "sampling rate is out of range");
                options.sampling_rates.push_back(static_cast<std::uint32_t>(parsed));
            }
        } else if (name == "--acceleration") {
            options.learned_index = false;
            if (value == "none") {
                options.acceleration = sufkit::SaAcceleration::none;
                options.acceleration_label = "none";
            } else if (value == "default" || value == "suffix-link") {
                options.acceleration = sufkit::SaAcceleration::lcp_suffix_link;
                options.acceleration_label = "suffix-link";
            } else if (value == "full") {
                options.acceleration = sufkit::SaAcceleration::full;
                options.acceleration_label = "full";
            } else if (value == "sapling") {
                options.acceleration = sufkit::SaAcceleration::lcp_suffix_link;
                options.learned_index = true;
                options.acceleration_label = "sapling";
            } else throw sufkit::Error(
                sufkit::ErrorCode::invalid_input,
                "invalid acceleration (expected none, default, suffix-link, full, or sapling)");
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
    if (!options.profile.empty() && options.profile != "smoke" && options.profile != "quick" &&
        options.profile != "standard" && options.profile != "full")
        throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid profile");
    if (options.methods.empty()) options.methods = {"div32", "caps32"};
    for (const auto& method : options.methods)
        if (method != "div32" && method != "div64" && method != "caps32" && method != "caps64")
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid method: " + method);
    if (options.threads.empty()) {
        if (options.profile == "smoke") options.threads = {1, 2};
        else if (options.profile == "standard") options.threads = {1, 2, 4, 8, 16, 32};
        else if (options.profile == "full") options.threads = {1, 8, 32, 64};
        else options.threads = {1, 2, 4, 8};
    }
    if (options.sampling_rates.empty()) options.sampling_rates = {1};
    if (options.repetitions == 0)
        options.repetitions = sufkit::benchmark::profile_definition(
            options.profile.empty() ? std::string_view{"user"} : std::string_view{options.profile})
                                  .build_repetitions;
    return options;
}

std::uint64_t profile_bases(const std::string& profile) {
    return sufkit::benchmark::profile_definition(profile).reference_bases;
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
            if (contig == 3) {
                const auto bucket = mixed % 10ULL;
                base = bucket < 4 ? 'C' : (bucket < 8 ? 'G' : (bucket == 8 ? 'A' : 'T'));
            }
            output.put(base);
            if (++column == 80) { output.put('\n'); column = 0; }
        }
        if (column != 0) output.put('\n');
    }
    if (!output) throw sufkit::Error(sufkit::ErrorCode::io_error, "failed to write generated reference");
}

std::string read_reference_bytes(const std::filesystem::path& path) {
    const auto native = path.string();
    gzFile file = gzopen(native.c_str(), "rb");
    if (file == nullptr)
        throw sufkit::Error(sufkit::ErrorCode::io_error, "cannot open FASTA: " + native);
    struct GzCloser {
        void operator()(gzFile_s* value) const noexcept {
            if (value != nullptr) (void)gzclose(value);
        }
    };
    std::unique_ptr<gzFile_s, GzCloser> guard(file);
    std::string contents;
    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (!size_error && file_bytes <= static_cast<std::uintmax_t>(contents.max_size()))
        contents.reserve(static_cast<std::size_t>(file_bytes));
    std::array<char, 1U << 16> buffer{};
    while (true) {
        const int count = gzread(file, buffer.data(), static_cast<unsigned>(buffer.size()));
        if (count > 0) {
            contents.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) break;
        int code = Z_OK;
        const char* message = gzerror(file, &code);
        throw sufkit::Error(
            sufkit::ErrorCode::io_error,
            "cannot read FASTA: " + native + (message == nullptr ? std::string{} : ": " + std::string(message)));
    }
    return contents;
}

std::vector<sufkit::SequenceRecord> parse_reference_records(const std::string& contents) {
    std::vector<sufkit::SequenceRecord> records;
    std::size_t begin = 0;
    while (begin < contents.size()) {
        auto end = contents.find('\n', begin);
        if (end == std::string::npos) end = contents.size();
        auto line_end = end;
        if (line_end > begin && contents[line_end - 1] == '\r') --line_end;
        const std::string_view line(contents.data() + begin, line_end - begin);
        if (!line.empty() && line.front() == '>') {
            std::size_t header_begin = 1;
            while (header_begin < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[header_begin])) != 0)
                ++header_begin;
            auto name_end = header_begin;
            while (name_end < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[name_end])) == 0)
                ++name_end;
            sufkit::SequenceRecord record;
            record.name.assign(line.substr(header_begin, name_end - header_begin));
            auto description_begin = name_end;
            while (description_begin < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[description_begin])) != 0)
                ++description_begin;
            record.description.assign(line.substr(description_begin));
            records.push_back(std::move(record));
        } else if (!line.empty()) {
            if (records.empty())
                throw sufkit::Error(
                    sufkit::ErrorCode::invalid_input,
                    "FASTA sequence data appears before the first header");
            auto& sequence = records.back().sequence;
            for (const char value : line)
                if (std::isspace(static_cast<unsigned char>(value)) == 0) sequence.push_back(value);
        }
        begin = end == contents.size() ? contents.size() : end + 1;
    }
    return records;
}

struct PreparedReference {
    sufkit::GenomeReference reference;
    double read_seconds = 0.0;
    double normalization_seconds = 0.0;
};

PreparedReference prepare_reference(const std::filesystem::path& path) {
    const auto read_begin = Clock::now();
    auto contents = read_reference_bytes(path);
    const double read_seconds = std::chrono::duration<double>(Clock::now() - read_begin).count();
    const auto normalization_begin = Clock::now();
    auto records = parse_reference_records(contents);
    auto reference = sufkit::GenomeReference::from_records(std::move(records));
    const double normalization_seconds =
        std::chrono::duration<double>(Clock::now() - normalization_begin).count();
    return {std::move(reference), read_seconds, normalization_seconds};
}

std::uint64_t allocated_bytes(const std::filesystem::path& path) {
    struct stat information{};
    if (::stat(path.c_str(), &information) != 0)
        throw sufkit::Error(
            sufkit::ErrorCode::io_error,
            "cannot stat serialized index: " + path.string());
    if (information.st_blocks < 0)
        throw sufkit::Error(sufkit::ErrorCode::io_error, "invalid allocated block count");
    return static_cast<std::uint64_t>(information.st_blocks) * 512ULL;
}

void mix(std::uint64_t& hash, std::uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    hash *= 1099511628211ULL;
}

std::pair<std::uint64_t, std::uint64_t> sa_checksum(const sufkit::SuffixArray& index) {
    std::uint64_t first = 1469598103934665603ULL;
    std::uint64_t second = 0x6a09e667f3bcc909ULL;
    for (std::uint64_t row = 0; row < index.info().suffix_count; ++row) {
        const auto suffix = index.suffix_at(row);
        first ^= suffix;
        first *= 1099511628211ULL;
        second ^= suffix + 0x9e3779b97f4a7c15ULL + (second << 7) + (second >> 3);
    }
    return {first, second};
}

std::uint64_t exact_checksum(const sufkit::SuffixArray& index) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    sufkit::LocateOptions options;
    options.max_hits = 256;
    for (const std::string pattern : {
             "ACGT", "GATTACA", "TTTTCCCC", "AAAAGGGG", "CGTACGTG",
             "ACGTACGTGATTACATTTTCCCCA"}) {
        const auto result = index.locate(pattern, options);
        mix(hash, result.total_hits);
        for (const auto& match : result.hits) {
            mix(hash, match.sequence_id);
            mix(hash, match.position);
            mix(hash, match.length);
        }
    }
    return hash;
}

std::uint64_t right_maximal_checksum(const sufkit::SuffixArray& index) {
    sufkit::RightMaximalOptions options;
    options.min_length = 12;
    options.strands = sufkit::StrandMode::both;
    const auto result = index.find_right_maximal_matches(
        "TTACACGTACGTGATTACATTTTCCCCAAAAGGGGACGT", options, 256);
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
    std::uint32_t sampling_rate,
    const Options& benchmark_options) {
    sufkit::SuffixArrayBuildOptions options;
    options.backend = method.rfind("caps", 0) == 0
        ? sufkit::SaBackend::caps
        : sufkit::SaBackend::divsufsort;
    options.coordinate_width = method.size() >= 2 && method.substr(method.size() - 2) == "32"
        ? sufkit::CoordinateWidth::bits32
        : sufkit::CoordinateWidth::bits64;
    options.threads = threads;
    options.sampling_rate = sampling_rate;
    options.acceleration = benchmark_options.acceleration;
    options.learned_index.enabled = benchmark_options.learned_index;
    return options;
}

WorkerResult run_build_phase(
    const Options& options,
    const std::filesystem::path& reference_path,
    const std::string& method,
    std::uint32_t threads,
    std::uint32_t sampling_rate,
    std::uint32_t repetition,
    const std::filesystem::path& index_path) {
    WorkerResult result;
    copy_text(result.method, sizeof(result.method), method);
    result.threads = threads;
    result.sampling_rate = sampling_rate;
    result.repetition = repetition;
    try {
        auto prepared = prepare_reference(reference_path);
        result.reference_read_seconds = prepared.read_seconds;
        result.normalization_seconds = prepared.normalization_seconds;
        result.total_bases = prepared.reference.total_bases();
        result.sequence_count = prepared.reference.sequence_count();
        sufkit::SuffixArrayBuildStatistics statistics;
        auto configured = build_options(method, threads, sampling_rate, options);
        configured.statistics = &statistics;
        const auto usage_begin = usage_now();
        const auto begin = Clock::now();
        auto index = sufkit::SuffixArray::build(
            prepared.reference, configured);
        result.build_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        const auto usage_end = usage_now();
        result.build_user_seconds = usage_end.user - usage_begin.user;
        result.build_system_seconds = usage_end.system - usage_begin.system;
        result.build_peak_rss_mb = peak_rss_mb();
        result.sa_seconds = statistics.sa_seconds;
        result.isa_seconds = statistics.isa_seconds;
        result.lcp_seconds = statistics.lcp_seconds;
        result.child_seconds = statistics.child_seconds;
        result.sapling_seconds = statistics.learned_index_seconds;
        const auto info = index.info();
        result.text_symbols = info.text_symbols;
        result.suffix_count = info.suffix_count;
        result.fingerprint = info.fingerprint;
        result.coordinate_width = info.coordinate_width;
        result.learned_index_bytes = info.learned_index_bytes;
        result.subproblems = method.rfind("caps", 0) == 0
            ? sufkit::detail::caps_subproblem_count(info.text_symbols, threads)
            : 0;
        copy_text(result.backend, sizeof(result.backend), info.backend);
        copy_text(result.signature, sizeof(result.signature), info.backend_signature);
        const auto hashes = sa_checksum(index);
        result.sa_hash_1 = hashes.first;
        result.sa_hash_2 = hashes.second;
        result.exact_checksum = exact_checksum(index);
        result.right_maximal_checksum = options.acceleration == sufkit::SaAcceleration::full
            ? right_maximal_checksum(index)
            : 0;
        index.save(index_path);
        result.serialized_bytes = static_cast<std::uint64_t>(std::filesystem::file_size(index_path));
        result.allocated_disk_bytes = allocated_bytes(index_path);
        copy_text(result.status, sizeof(result.status), "ok");
    } catch (const sufkit::Error& error) {
        copy_text(result.status, sizeof(result.status),
                  std::string(sufkit::to_string(error.code())) + ":" + error.what());
    } catch (const std::exception& error) {
        copy_text(result.status, sizeof(result.status), std::string("error:") + error.what());
    }
    return result;
}

WorkerResult run_save_phase(
    const std::filesystem::path& source_path,
    const std::filesystem::path& destination_path) {
    WorkerResult result;
    try {
        auto index = sufkit::SuffixArray::load(source_path);
        const auto usage_begin = usage_now();
        const auto begin = Clock::now();
        index.save(destination_path);
        result.save_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        const auto usage_end = usage_now();
        result.save_user_seconds = usage_end.user - usage_begin.user;
        result.save_system_seconds = usage_end.system - usage_begin.system;
        result.save_peak_rss_mb = peak_rss_mb();
        result.serialized_bytes =
            static_cast<std::uint64_t>(std::filesystem::file_size(destination_path));
        result.allocated_disk_bytes = allocated_bytes(destination_path);
        copy_text(result.status, sizeof(result.status), "ok");
    } catch (const sufkit::Error& error) {
        copy_text(result.status, sizeof(result.status),
                  std::string(sufkit::to_string(error.code())) + ":" + error.what());
    } catch (const std::exception& error) {
        copy_text(result.status, sizeof(result.status), std::string("error:") + error.what());
    }
    return result;
}

WorkerResult run_load_phase(
    const Options& options,
    const std::filesystem::path& index_path) {
    WorkerResult result;
    try {
        const auto usage_begin = usage_now();
        const auto begin = Clock::now();
        auto loaded = sufkit::SuffixArray::load(index_path);
        result.load_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        const auto usage_end = usage_now();
        result.load_user_seconds = usage_end.user - usage_begin.user;
        result.load_system_seconds = usage_end.system - usage_begin.system;
        result.load_peak_rss_mb = peak_rss_mb();
        const auto hashes = sa_checksum(loaded);
        result.sa_hash_1 = hashes.first;
        result.sa_hash_2 = hashes.second;
        result.exact_checksum = exact_checksum(loaded);
        result.right_maximal_checksum = options.acceleration == sufkit::SaAcceleration::full
            ? right_maximal_checksum(loaded)
            : 0;
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
        if (written < 0 && errno == EINTR) continue;
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
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

template <class Callback>
WorkerResult isolated_phase(Callback&& callback) {
    std::array<int, 2> descriptors{};
    if (pipe(descriptors.data()) != 0)
        throw sufkit::Error(sufkit::ErrorCode::build_failure, "benchmark pipe failed");
    const auto child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw sufkit::Error(sufkit::ErrorCode::build_failure, "benchmark fork failed");
    }
    if (child == 0) {
        close(descriptors[0]);
        const auto result = callback();
        const bool written = write_all(descriptors[1], &result, sizeof(result));
        close(descriptors[1]);
        std::_Exit(written ? 0 : 1);
    }
    close(descriptors[1]);
    WorkerResult result;
    const bool received = read_all(descriptors[0], &result, sizeof(result));
    close(descriptors[0]);
    int status = 0;
    pid_t waited = -1;
    do { waited = waitpid(child, &status, 0); }
    while (waited < 0 && errno == EINTR);
    if (!received || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        throw sufkit::Error(sufkit::ErrorCode::build_failure, "benchmark phase worker failed");
    return result;
}

WorkerResult isolated_worker(
    const Options& options,
    const std::filesystem::path& reference,
    const std::string& method,
    std::uint32_t threads,
    std::uint32_t sampling_rate,
    std::uint32_t repetition) {
    const auto index_path = options.output_dir /
        ("worker-" + method + "-t" + std::to_string(threads) + "-k" +
         std::to_string(sampling_rate) + "-r" + std::to_string(repetition) + ".sufidx");
    const auto save_path = options.output_dir /
        ("worker-" + method + "-t" + std::to_string(threads) + "-k" +
         std::to_string(sampling_rate) + "-r" + std::to_string(repetition) + "-save.sufidx");
    if (std::filesystem::exists(index_path) || std::filesystem::exists(save_path))
        throw sufkit::Error(sufkit::ErrorCode::io_error, "benchmark phase output already exists");
    const auto cleanup = [&]() noexcept {
        std::error_code error;
        (void)std::filesystem::remove(index_path, error);
        error.clear();
        (void)std::filesystem::remove(save_path, error);
    };
    try {
        auto result = isolated_phase([&]() {
            return run_build_phase(
                options, reference, method, threads, sampling_rate, repetition, index_path);
        });
        if (std::string(result.status) != "ok") {
            cleanup();
            return result;
        }
        const auto saved = isolated_phase([&]() { return run_save_phase(index_path, save_path); });
        if (std::string(saved.status) != "ok") {
            copy_text(result.status, sizeof(result.status), std::string("save_phase:") + saved.status);
            cleanup();
            return result;
        }
        result.save_seconds = saved.save_seconds;
        result.save_user_seconds = saved.save_user_seconds;
        result.save_system_seconds = saved.save_system_seconds;
        result.save_peak_rss_mb = saved.save_peak_rss_mb;
        result.serialized_bytes = saved.serialized_bytes;
        result.allocated_disk_bytes = saved.allocated_disk_bytes;

        const auto loaded = isolated_phase([&]() { return run_load_phase(options, save_path); });
        if (std::string(loaded.status) != "ok") {
            copy_text(result.status, sizeof(result.status), std::string("load_phase:") + loaded.status);
            cleanup();
            return result;
        }
        result.load_seconds = loaded.load_seconds;
        result.load_user_seconds = loaded.load_user_seconds;
        result.load_system_seconds = loaded.load_system_seconds;
        result.load_peak_rss_mb = loaded.load_peak_rss_mb;
        if (result.sa_hash_1 != loaded.sa_hash_1 || result.sa_hash_2 != loaded.sa_hash_2 ||
            result.exact_checksum != loaded.exact_checksum ||
            result.right_maximal_checksum != loaded.right_maximal_checksum) {
            copy_text(result.status, sizeof(result.status), "build_failure:save/load checksum mismatch");
        }
        cleanup();
        return result;
    } catch (...) {
        cleanup();
        throw;
    }
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
    metadata << "profile\tseed\tlogical_cpus\tacceleration\ttext_symbols\ttotal_bases\tsequence_count\t"
                "dataset_fingerprint\treference_read_seconds\tnormalization_seconds\n";
    const auto first_ok = std::find_if(results.begin(), results.end(), [](const auto& value) {
        return std::string(value.status) == "ok";
    });
    metadata << (options.profile.empty() ? "reference" : options.profile) << '\t'
             << options.seed << '\t' << logical_cpus << '\t'
             << options.acceleration_label << '\t'
             << (first_ok == results.end() ? 0 : first_ok->text_symbols) << '\t'
             << (first_ok == results.end() ? 0 : first_ok->total_bases) << '\t'
             << (first_ok == results.end() ? 0 : first_ok->sequence_count) << '\t'
             << (first_ok == results.end() ? 0 : first_ok->fingerprint) << '\t'
             << std::fixed << std::setprecision(6)
             << (first_ok == results.end() ? 0.0 : first_ok->reference_read_seconds) << '\t'
             << (first_ok == results.end() ? 0.0 : first_ok->normalization_seconds) << '\n';

    std::ofstream raw(options.output_dir / "raw_repetitions.tsv");
    raw << "method\teffective_backend\tbackend_signature\tcoordinate_width\tthreads\tsampling_rate\tsuffix_count\t"
           "subproblem_count\tacceleration\trepetition\ttotal_bases\tsequence_count\t"
           "reference_read_seconds\tnormalization_seconds\tsa_seconds\tisa_seconds\tlcp_seconds\t"
           "child_seconds\tsapling_seconds\tbuild_wall_seconds\tuser_cpu_seconds\tsystem_cpu_seconds\t"
           "peak_rss_mb\tbuild_peak_rss_mb\tsave_seconds\tsave_user_cpu_seconds\t"
           "save_system_cpu_seconds\tsave_peak_rss_mb\tload_seconds\tload_user_cpu_seconds\t"
           "load_system_cpu_seconds\tload_peak_rss_mb\tserialized_bytes\tallocated_disk_bytes\t"
           "bits_per_base\tlearned_index_bytes\tsa_checksum\texact_checksum\t"
           "right_maximal_checksum\tstatus\n";
    raw << std::fixed << std::setprecision(6);
    for (const auto& result : results) {
        const double bits_per_base = result.total_bases == 0 ? 0.0 :
            static_cast<double>(result.serialized_bytes) * 8.0 /
                static_cast<double>(result.total_bases);
        raw << result.method << '\t' << result.backend << '\t' << result.signature << '\t'
            << static_cast<unsigned>(result.coordinate_width) << '\t' << result.threads << '\t'
            << result.sampling_rate << '\t' << result.suffix_count << '\t'
            << result.subproblems << '\t' << options.acceleration_label << '\t'
            << result.repetition << '\t' << result.total_bases << '\t' << result.sequence_count << '\t'
            << result.reference_read_seconds << '\t' << result.normalization_seconds << '\t'
            << result.sa_seconds << '\t' << result.isa_seconds << '\t' << result.lcp_seconds << '\t'
            << result.child_seconds << '\t' << result.sapling_seconds << '\t'
            << result.build_seconds << '\t' << result.build_user_seconds << '\t'
            << result.build_system_seconds << '\t' << result.build_peak_rss_mb << '\t'
            << result.build_peak_rss_mb << '\t' << result.save_seconds << '\t'
            << result.save_user_seconds << '\t' << result.save_system_seconds << '\t'
            << result.save_peak_rss_mb << '\t' << result.load_seconds << '\t'
            << result.load_user_seconds << '\t' << result.load_system_seconds << '\t'
            << result.load_peak_rss_mb << '\t' << result.serialized_bytes << '\t'
            << result.allocated_disk_bytes << '\t' << bits_per_base << '\t'
            << result.learned_index_bytes << '\t' << hex_pair(result.sa_hash_1, result.sa_hash_2) << '\t'
            << result.exact_checksum << '\t' << result.right_maximal_checksum << '\t' << result.status << '\n';
    }

    using Key = std::tuple<std::string, std::uint32_t, std::uint32_t>;
    std::map<Key, std::vector<const WorkerResult*>> groups;
    for (const auto& result : results)
        if (std::string(result.status) == "ok")
            groups[{result.method, result.threads, result.sampling_rate}].push_back(&result);
    std::map<Key, double> medians;
    for (const auto& [key, group] : groups) {
        std::vector<double> times;
        for (const auto* value : group) times.push_back(value->build_seconds);
        medians[key] = median(std::move(times));
    }

    std::ofstream summary(options.output_dir / "build_results.tsv");
    summary << "method\teffective_backend\tcoordinate_width\tthreads\tsampling_rate\tsuffix_count\tacceleration\trepetitions\t"
               "reference_read_seconds_median\tnormalization_seconds_median\tsa_seconds_median\t"
               "isa_seconds_median\tlcp_seconds_median\tchild_seconds_median\tsapling_seconds_median\t"
               "build_seconds_median\tbuild_seconds_min\tbuild_seconds_max\tbuild_user_seconds_median\t"
               "build_system_seconds_median\tpeak_rss_mb_median\tbuild_peak_rss_mb_median\t"
               "save_seconds_median\tsave_peak_rss_mb_median\tload_seconds_median\tload_peak_rss_mb_median\t"
               "serialized_bytes\tallocated_disk_bytes\tbits_per_base\tlearned_index_bytes\t"
               "speedup_vs_divsufsort_same_width\tparallel_efficiency_vs_caps_1_thread\tstatus\n";
    summary << std::fixed << std::setprecision(6);
    for (const auto& [key, group] : groups) {
        const auto values = [&group](auto member) {
            std::vector<double> collected;
            collected.reserve(group.size());
            for (const auto* value : group) collected.push_back(value->*member);
            return collected;
        };
        auto times = values(&WorkerResult::build_seconds);
        const auto current = medians[key];
        const std::string method = std::get<0>(key);
        const auto threads = std::get<1>(key);
        const auto sampling_rate = std::get<2>(key);
        const bool is32 = method.size() >= 2 && method.substr(method.size() - 2) == "32";
        const std::string baseline = is32 ? "div32" : "div64";
        auto baseline_it = medians.find({baseline, 1, sampling_rate});
        if (baseline_it == medians.end()) baseline_it = medians.find({baseline, threads, sampling_rate});
        const double speedup = baseline_it == medians.end() || current == 0.0
            ? 0.0 : baseline_it->second / current;
        const std::string caps = is32 ? "caps32" : "caps64";
        const auto one_thread = medians.find({caps, 1, sampling_rate});
        const double efficiency = method.rfind("caps", 0) != 0 ||
                                  one_thread == medians.end() || current == 0.0
            ? 0.0 : one_thread->second / (current * static_cast<double>(threads));
        summary << method << '\t' << group.front()->backend << '\t'
                << static_cast<unsigned>(group.front()->coordinate_width) << '\t' << threads << '\t'
                << sampling_rate << '\t' << group.front()->suffix_count << '\t'
                << options.acceleration_label << '\t' << group.size() << '\t'
                << median(values(&WorkerResult::reference_read_seconds)) << '\t'
                << median(values(&WorkerResult::normalization_seconds)) << '\t'
                << median(values(&WorkerResult::sa_seconds)) << '\t'
                << median(values(&WorkerResult::isa_seconds)) << '\t'
                << median(values(&WorkerResult::lcp_seconds)) << '\t'
                << median(values(&WorkerResult::child_seconds)) << '\t'
                << median(values(&WorkerResult::sapling_seconds)) << '\t'
                << median(times) << '\t' << *std::min_element(times.begin(), times.end()) << '\t'
                << *std::max_element(times.begin(), times.end()) << '\t'
                << median(values(&WorkerResult::build_user_seconds)) << '\t'
                << median(values(&WorkerResult::build_system_seconds)) << '\t'
                << median(values(&WorkerResult::build_peak_rss_mb)) << '\t'
                << median(values(&WorkerResult::build_peak_rss_mb)) << '\t'
                << median(values(&WorkerResult::save_seconds)) << '\t'
                << median(values(&WorkerResult::save_peak_rss_mb)) << '\t'
                << median(values(&WorkerResult::load_seconds)) << '\t'
                << median(values(&WorkerResult::load_peak_rss_mb)) << '\t'
                << group.front()->serialized_bytes << '\t' << group.front()->allocated_disk_bytes << '\t'
                << (group.front()->total_bases == 0 ? 0.0 :
                    static_cast<double>(group.front()->serialized_bytes) * 8.0 /
                        static_cast<double>(group.front()->total_bases)) << '\t'
                << group.front()->learned_index_bytes << '\t'
                << speedup << '\t' << efficiency << "\tok\n";
    }
    for (const auto& result : results) {
        if (std::string(result.status) == "ok") continue;
        summary << result.method << '\t' << result.backend << '\t'
                << static_cast<unsigned>(result.coordinate_width) << '\t' << result.threads << '\t'
                << result.sampling_rate << '\t' << result.suffix_count << '\t'
                << options.acceleration_label << "\t0";
        for (int column = 0; column < 24; ++column) summary << "\t0";
        summary << '\t' << result.status << '\n';
    }
}

void correctness_gate(const std::vector<WorkerResult>& results) {
    const WorkerResult* reference = nullptr;
    std::map<std::uint32_t, const WorkerResult*> sampling_references;
    for (const auto& result : results) {
        if (std::string(result.status) != "ok") continue;
        if (reference == nullptr) reference = &result;
        const auto [it, inserted] = sampling_references.emplace(result.sampling_rate, &result);
        const auto* same_sampling = it->second;
        if (result.total_bases != reference->total_bases ||
            result.sequence_count != reference->sequence_count ||
            result.text_symbols != reference->text_symbols ||
            result.fingerprint != reference->fingerprint ||
            result.exact_checksum != reference->exact_checksum ||
            result.right_maximal_checksum != reference->right_maximal_checksum ||
            (!inserted && (result.suffix_count != same_sampling->suffix_count ||
                           result.sa_hash_1 != same_sampling->sa_hash_1 ||
                           result.sa_hash_2 != same_sampling->sa_hash_2))) {
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
          for (const auto sampling_rate : options.sampling_rates) {
            if (threads > static_cast<std::uint64_t>(logical_cpus)) {
                WorkerResult skipped;
                copy_text(skipped.method, sizeof(skipped.method), method);
                copy_text(skipped.status, sizeof(skipped.status), "not_applicable:threads_exceed_logical_cpus");
                skipped.threads = threads;
                skipped.sampling_rate = sampling_rate;
                results.push_back(skipped);
                continue;
            }
            for (std::uint32_t repetition = 1; repetition <= options.repetitions; ++repetition) {
                std::cerr << "running " << method << " threads=" << threads
                          << " sampling=" << sampling_rate
                          << " repetition=" << repetition << '/' << options.repetitions << '\n';
                results.push_back(isolated_worker(
                    options, reference, method, threads, sampling_rate, repetition));
            }
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
