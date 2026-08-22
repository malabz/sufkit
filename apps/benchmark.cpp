#include "benchmark.hpp"

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app_support.hpp"

namespace sufkit::app {
namespace {

using Clock = std::chrono::steady_clock;

struct WorkerResult {
    int status = 0;
    char method[32]{};
    char backend[64]{};
    char signature[160]{};
    char sdsl_version[24]{};
    char error[256]{};
    std::uint8_t coordinate_width = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t total_hits = 0;
    std::uint64_t reported_hits = 0;
    std::uint64_t checksum = 0;
    double build_seconds = 0.0;
    double load_seconds = 0.0;
    double count_qps = 0.0;
    double locate_qps = 0.0;
    double peak_rss_mb = 0.0;
};

struct BenchmarkInput {
    std::string dataset_name;
    std::vector<SequenceRecord> reference_records;
    std::vector<SequenceRecord> queries;
};

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

std::string normalize_sequence(std::string sequence) {
    for (auto& raw : sequence) {
        const auto value = static_cast<unsigned char>(raw);
        switch (static_cast<char>(std::toupper(value))) {
        case 'A': raw = 'A'; break;
        case 'C': raw = 'C'; break;
        case 'G': raw = 'G'; break;
        case 'T': raw = 'T'; break;
        default: raw = 'N'; break;
        }
    }
    return sequence;
}

BenchmarkInput generated_input(bool smoke) {
    const std::size_t contig_size = smoke ? 4096 : (1U << 20);
    const std::size_t query_count = smoke ? 100 : 1000;
    SplitMix64 random(20260822);
    BenchmarkInput input;
    input.dataset_name = smoke ? "synthetic-smoke" : "synthetic-quick";
    const std::array<char, 4> bases{{'A', 'C', 'G', 'T'}};
    for (std::size_t contig = 0; contig < 4; ++contig) {
        std::string sequence(contig_size, 'A');
        for (std::size_t position = 0; position < contig_size; ++position) {
            sequence[position] = bases[static_cast<std::size_t>(random.next() & 3U)];
            if ((position + contig * 97U) % 4096U >= 2032U &&
                (position + contig * 97U) % 4096U < 2048U) {
                sequence[position] = 'N';
            }
        }
        if (contig >= 2) {
            for (std::size_t position = 8192; position + 256 < sequence.size(); position += 16384) {
                std::copy_n(sequence.begin() + static_cast<std::ptrdiff_t>(position - 4096),
                            256,
                            sequence.begin() + static_cast<std::ptrdiff_t>(position));
            }
        }
        input.reference_records.push_back({
            "chr" + std::to_string(contig + 1), "synthetic", std::move(sequence)});
    }

    const std::array<std::size_t, 3> lengths{{20, 50, 100}};
    for (std::size_t index = 0; index < query_count; ++index) {
        const auto length = lengths[index % lengths.size()];
        std::string pattern;
        for (int attempt = 0; attempt < 100 && pattern.empty(); ++attempt) {
            const auto contig = static_cast<std::size_t>(random.next() % input.reference_records.size());
            const auto& sequence = input.reference_records[contig].sequence;
            const auto start = static_cast<std::size_t>(random.next() % (sequence.size() - length + 1));
            const auto candidate = sequence.substr(start, length);
            if (candidate.find('N') == std::string::npos) {
                pattern = candidate;
            }
        }
        if (pattern.empty()) {
            pattern.assign(length, 'A');
        }
        if (index % 4 == 3) {
            pattern[pattern.size() / 2] = pattern[pattern.size() / 2] == 'A' ? 'C' : 'A';
        }
        input.queries.push_back({"q" + std::to_string(index), "", std::move(pattern)});
    }
    return input;
}

std::vector<std::string> split_methods(const std::string& text) {
    std::vector<std::string> methods;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        const auto value = text.substr(begin, end == std::string::npos ? end : end - begin);
        if (value.empty()) {
            throw Error(ErrorCode::invalid_input, "--methods contains an empty method");
        }
        methods.push_back(value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    const std::set<std::string> supported{"naive", "sa32", "sa64", "fm"};
    std::set<std::string> unique;
    for (const auto& method : methods) {
        if (supported.count(method) == 0 || !unique.insert(method).second) {
            throw Error(ErrorCode::invalid_input, "invalid or duplicate benchmark method: " + method);
        }
    }
    return methods;
}

double seconds_between(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

void mix_checksum(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU);
        hash *= 1099511628211ULL;
    }
}

QueryResult naive_locate(
    const std::vector<SequenceRecord>& records,
    const std::string& pattern,
    std::uint64_t max_hits) {
    if (pattern.empty()) {
        throw Error(ErrorCode::invalid_input, "benchmark query is empty");
    }
    std::string normalized_pattern = normalize_sequence(pattern);
    if (normalized_pattern.find('N') != std::string::npos) {
        throw Error(ErrorCode::invalid_input, "benchmark query contains a non-ACGT symbol");
    }
    QueryResult result;
    for (std::size_t sequence_id = 0; sequence_id < records.size(); ++sequence_id) {
        const auto sequence = normalize_sequence(records[sequence_id].sequence);
        std::size_t position = sequence.find(normalized_pattern);
        while (position != std::string::npos) {
            ++result.total_hits;
            if (result.hits.size() < max_hits) {
                result.hits.push_back({
                    static_cast<SequenceId>(sequence_id),
                    position,
                    normalized_pattern.size(),
                    Strand::forward});
            }
            position = sequence.find(normalized_pattern, position + 1);
        }
    }
    result.truncated = result.hits.size() < result.total_hits;
    return result;
}

template <class CountFunction, class LocateFunction>
void measure_queries(
    const std::vector<SequenceRecord>& queries,
    CountFunction&& count,
    LocateFunction&& locate,
    WorkerResult& result) {
    for (const auto& query : queries) {
        (void)count(query.sequence);
        (void)locate(query.sequence);
    }

    std::vector<double> count_times;
    std::vector<double> locate_times;
    std::uint64_t expected_total = 0;
    std::uint64_t expected_reported = 0;
    std::uint64_t expected_checksum = 0;
    for (int repetition = 0; repetition < 5; ++repetition) {
        std::uint64_t total = 0;
        auto begin = Clock::now();
        for (const auto& query : queries) {
            total += count(query.sequence);
        }
        count_times.push_back(seconds_between(begin, Clock::now()));

        std::uint64_t reported = 0;
        std::uint64_t checksum = 14695981039346656037ULL;
        begin = Clock::now();
        for (const auto& query : queries) {
            const auto located = locate(query.sequence);
            mix_checksum(checksum, located.total_hits);
            reported += located.hits.size();
            for (const auto& match : located.hits) {
                mix_checksum(checksum, match.sequence_id);
                mix_checksum(checksum, match.position);
                mix_checksum(checksum, match.length);
                mix_checksum(checksum, static_cast<std::uint8_t>(match.strand));
            }
        }
        locate_times.push_back(seconds_between(begin, Clock::now()));
        if (repetition == 0) {
            expected_total = total;
            expected_reported = reported;
            expected_checksum = checksum;
        } else if (total != expected_total || reported != expected_reported || checksum != expected_checksum) {
            throw Error(ErrorCode::build_failure, "benchmark query results are not deterministic");
        }
    }
    result.total_hits = expected_total;
    result.reported_hits = expected_reported;
    result.checksum = expected_checksum;
    result.count_qps = static_cast<double>(queries.size()) / median(std::move(count_times));
    result.locate_qps = static_cast<double>(queries.size()) / median(std::move(locate_times));
}

template <class Index>
void measure_index(
    const Index& index,
    const std::vector<SequenceRecord>& queries,
    WorkerResult& result) {
    LocateOptions options;
    options.max_hits = 1000;
    measure_queries(
        queries,
        [&](const std::string& pattern) { return index.count(pattern); },
        [&](const std::string& pattern) { return index.locate(pattern, options); },
        result);
}

void copy_text(char* destination, std::size_t capacity, const std::string& source) {
    std::snprintf(destination, capacity, "%s", source.c_str());
}

WorkerResult run_worker(
    const std::string& method,
    const BenchmarkInput& input) {
    WorkerResult result;
    copy_text(result.method, sizeof(result.method), method);
    if (method == "naive") {
        copy_text(result.backend, sizeof(result.backend), "naive");
        copy_text(result.signature, sizeof(result.signature), "std::string::find per contig");
        measure_queries(
            input.queries,
            [&](const std::string& pattern) {
                return naive_locate(input.reference_records, pattern, 0).total_hits;
            },
            [&](const std::string& pattern) {
                return naive_locate(input.reference_records, pattern, 1000);
            },
            result);
        return result;
    }

    auto reference = GenomeReference::from_records(input.reference_records);
    const auto temporary = std::filesystem::path("/tmp") /
        ("sufkit-bench-" + std::to_string(static_cast<long long>(getpid())) + "-" + method + ".sufidx");
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    try {
        if (method == "sa32" || method == "sa64") {
            SuffixArrayBuildOptions build_options;
            build_options.backend = SaBackend::divsufsort;
            build_options.coordinate_width = method == "sa32"
                ? CoordinateWidth::bits32
                : CoordinateWidth::bits64;
            auto begin = Clock::now();
            auto index = SuffixArray::build(reference, build_options);
            result.build_seconds = seconds_between(begin, Clock::now());
            const auto info = index.info();
            copy_text(result.backend, sizeof(result.backend), info.backend);
            copy_text(result.signature, sizeof(result.signature), info.backend_signature);
            result.coordinate_width = info.coordinate_width;
            index.save(temporary);
            result.serialized_bytes = std::filesystem::file_size(temporary);
            begin = Clock::now();
            auto loaded = SuffixArray::load(temporary);
            result.load_seconds = seconds_between(begin, Clock::now());
            measure_index(loaded, input.queries, result);
        } else if (method == "fm") {
            auto begin = Clock::now();
            auto index = FmIndex::build(reference);
            result.build_seconds = seconds_between(begin, Clock::now());
            const auto info = index.info();
            copy_text(result.backend, sizeof(result.backend), info.backend);
            copy_text(result.signature, sizeof(result.signature), info.backend_signature);
            copy_text(result.sdsl_version, sizeof(result.sdsl_version), info.sdsl_version);
            result.coordinate_width = info.coordinate_width;
            index.save(temporary);
            result.serialized_bytes = std::filesystem::file_size(temporary);
            begin = Clock::now();
            auto loaded = FmIndex::load(temporary);
            result.load_seconds = seconds_between(begin, Clock::now());
            measure_index(loaded, input.queries, result);
        } else {
            throw Error(ErrorCode::invalid_input, "unknown benchmark method: " + method);
        }
        std::filesystem::remove(temporary, ignored);
        return result;
    } catch (...) {
        std::filesystem::remove(temporary, ignored);
        throw;
    }
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
    std::size_t read_bytes = 0;
    while (read_bytes < size) {
        const auto amount = ::read(descriptor, bytes + read_bytes, size - read_bytes);
        if (amount <= 0) return false;
        read_bytes += static_cast<std::size_t>(amount);
    }
    return true;
}

WorkerResult run_isolated(
    const std::string& method,
    const BenchmarkInput& input) {
    int descriptors[2]{};
    if (pipe(descriptors) != 0) {
        throw Error(ErrorCode::io_error, "cannot create benchmark worker pipe");
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw Error(ErrorCode::io_error, "cannot create benchmark worker process");
    }
    if (pid == 0) {
        close(descriptors[0]);
        WorkerResult result;
        try {
            result = run_worker(method, input);
        } catch (const std::exception& error) {
            result.status = 1;
            copy_text(result.method, sizeof(result.method), method);
            copy_text(result.error, sizeof(result.error), error.what());
        }
        const bool written = write_exact(descriptors[1], &result, sizeof(result));
        close(descriptors[1]);
        _exit(written ? 0 : 1);
    }

    close(descriptors[1]);
    WorkerResult result;
    const bool received = read_exact(descriptors[0], &result, sizeof(result));
    close(descriptors[0]);
    int status = 0;
    struct rusage usage {};
    if (wait4(pid, &status, 0, &usage) < 0 || !received || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw Error(ErrorCode::build_failure, "benchmark worker failed: " + method);
    }
    if (result.status != 0) {
        throw Error(ErrorCode::build_failure, std::string("benchmark worker ") + method + ": " + result.error);
    }
    result.peak_rss_mb = static_cast<double>(usage.ru_maxrss) / 1024.0;
    return result;
}

} // namespace

int run_benchmark(const std::vector<std::string>& arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        std::cout <<
            "sufkit bench (--quick | --smoke | --reference REF.fa --queries Q.fa)\n"
            "  --output RESULTS.tsv [--methods naive,sa32,sa64,fm]\n";
        return 0;
    }

    bool quick = false;
    bool smoke = false;
    std::optional<std::string> output_path;
    std::optional<std::string> reference_path;
    std::optional<std::string> query_path;
    std::string methods_text = "naive,sa32,sa64,fm";
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& option = arguments[index];
        if (option == "--quick") quick = true;
        else if (option == "--smoke") smoke = true;
        else if (option == "--output" || option == "--reference" ||
                 option == "--queries" || option == "--methods") {
            if (index + 1 >= arguments.size()) {
                throw Error(ErrorCode::invalid_input, "missing value for " + option);
            }
            const auto value = arguments[++index];
            if (option == "--output") output_path = value;
            else if (option == "--reference") reference_path = value;
            else if (option == "--queries") query_path = value;
            else methods_text = value;
        } else {
            throw Error(ErrorCode::invalid_input, "unknown benchmark option: " + option);
        }
    }
    if (!output_path) {
        throw Error(ErrorCode::invalid_input, "--output is required");
    }
    if (quick && smoke) {
        throw Error(ErrorCode::invalid_input, "--quick and --smoke are mutually exclusive");
    }

    BenchmarkInput input;
    if (quick || smoke) {
        if (reference_path || query_path) {
            throw Error(ErrorCode::invalid_input, "synthetic modes cannot be combined with input FASTA files");
        }
        input = generated_input(smoke);
    } else {
        if (!reference_path || !query_path) {
            throw Error(
                ErrorCode::invalid_input,
                "provide --quick, --smoke, or both --reference and --queries");
        }
        input.dataset_name = std::filesystem::path(*reference_path).filename().string();
        input.reference_records = read_fasta_records(*reference_path);
        input.queries = read_fasta_records(*query_path);
    }
    if (input.reference_records.empty() || input.queries.empty()) {
        throw Error(ErrorCode::invalid_input, "benchmark reference and query sets must be non-empty");
    }
    const auto methods = split_methods(methods_text);
    auto reference = GenomeReference::from_records(input.reference_records);
    std::vector<WorkerResult> results;
    for (const auto& method : methods) {
        std::cerr << "benchmarking " << method << "...\n";
        results.push_back(run_isolated(method, input));
    }
    for (std::size_t index = 1; index < results.size(); ++index) {
        if (results[index].total_hits != results[0].total_hits ||
            results[index].reported_hits != results[0].reported_hits ||
            results[index].checksum != results[0].checksum) {
            throw Error(
                ErrorCode::build_failure,
                std::string("benchmark correctness mismatch between ") +
                    results[0].method + " and " + results[index].method);
        }
    }

    std::ofstream output(*output_path);
    if (!output) {
        throw Error(ErrorCode::io_error, "cannot create benchmark output: " + *output_path);
    }
    output << "dataset\tdataset_fingerprint\ttotal_bases\tcontigs\tmethod\tbackend\t"
              "backend_signature\tsdsl_version\tcoordinate_width\tthreads\tbuild_seconds\t"
              "peak_rss_mb\tserialized_bytes\tload_seconds\tquery_count\tcount_qps\t"
              "locate_qps\ttotal_hits\treported_hits\tresult_checksum\n";
    output << std::fixed << std::setprecision(6);
    for (const auto& result : results) {
        output << input.dataset_name << '\t'
               << std::hex << std::setfill('0') << std::setw(16) << reference.fingerprint() << std::dec << '\t'
               << reference.total_bases() << '\t'
               << reference.sequence_count() << '\t'
               << result.method << '\t'
               << result.backend << '\t'
               << result.signature << '\t'
               << result.sdsl_version << '\t'
               << static_cast<unsigned>(result.coordinate_width) << '\t'
               << 1 << '\t'
               << result.build_seconds << '\t'
               << result.peak_rss_mb << '\t'
               << result.serialized_bytes << '\t'
               << result.load_seconds << '\t'
               << input.queries.size() << '\t'
               << result.count_qps << '\t'
               << result.locate_qps << '\t'
               << result.total_hits << '\t'
               << result.reported_hits << '\t'
               << std::hex << std::setfill('0') << std::setw(16) << result.checksum << std::dec << '\n';
    }
    if (!output) {
        throw Error(ErrorCode::io_error, "failed to write benchmark output: " + *output_path);
    }
    std::cerr << "benchmark results written to " << *output_path << '\n';
    return 0;
}

} // namespace sufkit::app

