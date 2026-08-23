#include "mem_benchmark.hpp"
#include "app_support.hpp"

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sufkit::app::mem_bench {
namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::string profile = "smoke";
    std::vector<std::string> scenarios{"mixed"};
    std::vector<std::string> methods{"mem-baseline", "mem-lcp", "mem-child", "mem-suffix-link", "mem-full"};
    std::vector<std::uint64_t> min_lengths{20, 50};
    std::filesystem::path output_directory;
    std::optional<std::filesystem::path> mummer4;
    std::optional<std::filesystem::path> reference;
    std::optional<std::filesystem::path> query_file;
    std::string mummer_version;
    std::string mummer_sha256;
    std::uint64_t seed = 20260822;
};

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
    double save_seconds = 0;
    double load_seconds = 0;
    double peak_rss_mb = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint64_t auxiliary_bytes = 0;
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
    std::string status = "ok";
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
    std::string status = "ok";
};

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto end = text.find(',', begin);
        const auto value = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (value.empty()) throw Error(ErrorCode::invalid_input, "empty comma-separated benchmark value");
        result.push_back(value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

std::uint64_t parse_number(const std::string& value, const char* name) {
    std::size_t consumed = 0;
    std::uint64_t result = 0;
    try { result = std::stoull(value, &consumed); }
    catch (...) { throw Error(ErrorCode::invalid_input, std::string(name) + " requires an integer"); }
    if (consumed != value.size()) throw Error(ErrorCode::invalid_input, std::string(name) + " requires an integer");
    return result;
}

Options parse(const std::vector<std::string>& arguments) {
    Options result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& name = arguments[index];
        if (name == "--workload") {
            if (++index >= arguments.size() || arguments[index] != "mem")
                throw Error(ErrorCode::invalid_input, "MEM benchmark requires --workload mem");
        } else {
            if (++index >= arguments.size()) throw Error(ErrorCode::invalid_input, "missing value for " + name);
            const auto& value = arguments[index];
            if (name == "--profile") result.profile = value;
            else if (name == "--scenarios") result.scenarios = split(value);
            else if (name == "--methods") result.methods = split(value);
            else if (name == "--min-lengths") {
                result.min_lengths.clear();
                for (const auto& item : split(value)) result.min_lengths.push_back(parse_number(item, "--min-lengths"));
            } else if (name == "--output-dir") result.output_directory = value;
            else if (name == "--mummer4") result.mummer4 = std::filesystem::path(value);
            else if (name == "--reference") result.reference = std::filesystem::path(value);
            else if (name == "--queries") result.query_file = std::filesystem::path(value);
            else if (name == "--seed") result.seed = parse_number(value, "--seed");
            else throw Error(ErrorCode::invalid_input, "unknown MEM benchmark option: " + name);
        }
    }
    if (!result.reference && result.profile != "smoke" && result.profile != "quick" &&
        result.profile != "standard")
        throw Error(ErrorCode::invalid_input, "MEM benchmark currently runs smoke, quick, or standard profiles");
    if (result.query_file && !result.reference)
        throw Error(ErrorCode::invalid_input, "--queries requires --reference");
    if (result.output_directory.empty()) throw Error(ErrorCode::invalid_input, "--output-dir is required");
    if (result.min_lengths.empty() || std::any_of(result.min_lengths.begin(), result.min_lengths.end(), [](auto value) { return value == 0; }))
        throw Error(ErrorCode::invalid_input, "MEM minimum lengths must be positive");
    const std::set<std::string> valid_scenarios{
        "mixed", "balanced", "gc-skewed", "repeat-rich", "n-islands", "many-contig"};
    for (const auto& scenario : result.scenarios)
        if (valid_scenarios.count(scenario) == 0)
            throw Error(ErrorCode::invalid_input, "unknown MEM benchmark scenario: " + scenario);
    const std::set<std::string> valid_methods{
        "mem-baseline", "mem-lcp", "mem-child", "mem-suffix-link", "mem-full", "mummer4"};
    for (const auto& method : result.methods) {
        if (valid_methods.count(method) == 0) throw Error(ErrorCode::invalid_input, "unknown MEM benchmark method: " + method);
        if (method == "mummer4" && !result.mummer4) throw Error(ErrorCode::invalid_input, "mummer4 method requires --mummer4 PATH");
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
    dataset.name = "synthetic-mem-" + options.profile + "-" + scenario;
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
        std::size_t position = 0;
        do {
            if (scenario == "repeat-rich" && id % 200 == 0) {
                const auto repeat_slots = std::max<std::size_t>(1, (source.size() * 2 / 5 - query_length) /
                    repeat_template.size());
                position = static_cast<std::size_t>(next_random(state) % repeat_slots) * repeat_template.size();
            } else if (scenario == "repeat-rich") {
                const auto random_begin = source.size() * 2 / 5;
                position = random_begin + static_cast<std::size_t>(next_random(state) %
                    (source.size() - random_begin - query_length));
            } else {
                position = static_cast<std::size_t>(next_random(state) % (source.size() - query_length));
            }
        } while (source.substr(position, query_length).find('N') != std::string::npos);
        auto query = source.substr(position, query_length);
        if (id % 3 != 0) {
            const auto mutation = static_cast<std::size_t>(query.size() / 2 + id % 7);
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
    dataset.name = "user-mem-reference";
    dataset.reference = read_fasta_records(*options.reference);
    if (dataset.reference.empty()) throw Error(ErrorCode::invalid_input, "reference FASTA contains no records");
    std::uint64_t state = options.seed;
    for (auto& record : dataset.reference) {
        for (auto& base : record.sequence) {
            base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
            if (base != 'A' && base != 'C' && base != 'G' && base != 'T') base = 'N';
        }
        dataset.total_bases += record.sequence.size();
    }
    if (options.query_file) {
        dataset.queries = read_fasta_records(*options.query_file);
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
    if (dataset.queries.empty()) throw Error(ErrorCode::invalid_input, "no usable MEM benchmark queries");
    for (const auto& query : dataset.queries) dataset.query_bases += query.sequence.size();
    std::uint64_t fingerprint = 1469598103934665603ULL;
    for (const auto& record : dataset.reference) fingerprint = hash_bytes(fingerprint, record.sequence);
    dataset.fingerprint = fingerprint;
    return dataset;
}

void write_fasta(const std::filesystem::path& path, const std::vector<SequenceRecord>& records) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw Error(ErrorCode::io_error, "cannot create benchmark FASTA: " + path.string());
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

void mix_match(std::uint64_t& checksum, const MemMatch& match) {
    mix(checksum, match.query_position);
    mix(checksum, match.sequence_id);
    mix(checksum, match.reference_position);
    mix(checksum, match.length);
    mix(checksum, static_cast<std::uint8_t>(match.strand));
}

SaAcceleration acceleration_for(const std::string& method) {
    if (method == "mem-baseline") return SaAcceleration::none;
    if (method == "mem-lcp") return SaAcceleration::lcp;
    if (method == "mem-child") return SaAcceleration::lcp_child;
    if (method == "mem-suffix-link") return SaAcceleration::lcp_suffix_link;
    return SaAcceleration::full;
}

MemSearchAlgorithm algorithm_for(const std::string& method) {
    if (method == "mem-baseline") return MemSearchAlgorithm::baseline;
    if (method == "mem-lcp") return MemSearchAlgorithm::lcp;
    if (method == "mem-child") return MemSearchAlgorithm::child;
    if (method == "mem-suffix-link") return MemSearchAlgorithm::suffix_link;
    return MemSearchAlgorithm::full;
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
    build.algorithm = to_string(algorithm_for(method));
    build.acceleration = to_string(acceleration_for(method));
    const auto reference = GenomeReference::from_records(dataset.reference);
    SuffixArrayBuildOptions build_options;
    build_options.acceleration = acceleration_for(method);
    const auto build_begin = Clock::now();
    auto index = SuffixArray::build(reference, build_options);
    build.build_seconds = seconds_since(build_begin);
    const auto index_path = scratch / (method + ".sufidx");
    const auto save_begin = Clock::now();
    index.save(index_path, {true});
    build.save_seconds = seconds_since(save_begin);
    build.serialized_bytes = serialized_size(index_path);
    build.auxiliary_bytes = index.info().auxiliary_bytes;
    const auto load_begin = Clock::now();
    auto loaded = SuffixArray::load(index_path);
    build.load_seconds = seconds_since(load_begin);
    build.peak_rss_mb = peak_rss_mb_self();
    builds.push_back(build);

    const std::uint32_t repetitions = options.profile == "smoke" ? 3 : 5;
    for (const auto min_length : options.min_lengths) {
        MemOptions mem_options;
        mem_options.min_length = min_length;
        mem_options.algorithm = algorithm_for(method);
        for (const auto& query : dataset.queries) (void)loaded.find_mems(query.sequence, mem_options);
        QueryResultRow summary;
        summary.dataset = dataset.name;
        summary.method = method;
        summary.algorithm = build.algorithm;
        summary.acceleration = build.acceleration;
        summary.min_length = min_length;
        summary.query_count = dataset.queries.size();
        summary.query_bases = dataset.query_bases;
        for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
            const auto begin = Clock::now();
            std::uint64_t checksum = checksum_seed();
            std::uint64_t total = 0;
            for (std::size_t query_id = 0; query_id < dataset.queries.size(); ++query_id) {
                const auto result = loaded.find_mems(dataset.queries[query_id].sequence, mem_options);
                mix(checksum, query_id);
                mix(checksum, result.total_matches);
                total += result.total_matches;
                for (const auto& match : result.matches) mix_match(checksum, match);
            }
            const auto elapsed = seconds_since(begin);
            summary.seconds.push_back(elapsed);
            summary.total_matches = total;
            summary.checksum = checksum;
            raw.push_back({dataset.name, method, "mem", min_length, repetition, elapsed, total, checksum, "ok"});
        }
        queries.push_back(std::move(summary));
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
    if (pid < 0) throw Error(ErrorCode::io_error, "cannot fork MUMmer4 benchmark worker");
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
    if (wait4(pid, &status, 0, &usage) < 0) throw Error(ErrorCode::io_error, "cannot wait for MUMmer4 benchmark worker");
    ProcessResult result;
    result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    result.seconds = seconds_since(begin);
    result.peak_rss_mb = static_cast<double>(usage.ru_maxrss) / 1024.0;
    return result;
}

std::vector<MemMatch> parse_mummer_output(const std::filesystem::path& path,
    const std::map<std::string, SequenceId>& reference_ids,
    const std::vector<SequenceRecord>& queries,
    Strand strand,
    std::vector<std::vector<MemMatch>>& per_query) {
    std::ifstream input(path);
    if (!input) throw Error(ErrorCode::io_error, "cannot read MUMmer4 output");
    std::string line;
    std::size_t query_id = 0;
    bool have_query = false;
    std::vector<MemMatch> all;
    while (std::getline(input, line)) {
        if (!line.empty() && line.front() == '>') {
            std::istringstream header(line.substr(1));
            std::string name;
            header >> name;
            if (name.size() < 2 || name.front() != 'q') throw Error(ErrorCode::build_failure, "unexpected MUMmer4 query header");
            query_id = static_cast<std::size_t>(parse_number(name.substr(1), "MUMmer query id"));
            if (query_id >= per_query.size()) throw Error(ErrorCode::build_failure, "MUMmer4 query id is out of range");
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
            throw Error(ErrorCode::build_failure, "invalid MUMmer4 match row");
        const auto zero_query = query_position - 1;
        if (strand == Strand::reverse_complement && zero_query + length > queries[query_id].sequence.size())
            throw Error(ErrorCode::build_failure, "MUMmer4 reverse query coordinate is out of range");
        const auto normalized_query = strand == Strand::reverse_complement
            ? queries[query_id].sequence.size() - (zero_query + length) : zero_query;
        MemMatch match{id->second, reference_position - 1, normalized_query, length, strand};
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
    const std::vector<std::string> common{executable, "-maxmatch", "-n", "-F", "-k", "1", "-skip", "1",
        "-kmer", "0", "-threads", "1", "-qthreads", "1"};
    auto build_args = common;
    build_args.insert(build_args.end(), {"-l", std::to_string(min_length), "-save", prefix.string(),
                                         reference_path.string(), nohit_path.string()});
    const auto process = run_process(build_args, build_stdout, build_stderr);
    if (process.status != 0) throw Error(ErrorCode::build_failure, "MUMmer4 index construction failed");
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
    verification_build_options.acceleration = SaAcceleration::none;
    const auto verification_reference = GenomeReference::from_records(dataset.reference);
    const auto verification_index = SuffixArray::build(verification_reference, verification_build_options);

    const std::uint32_t repetitions = options.profile == "smoke" ? 3 : 5;
    for (const auto length : options.min_lengths) {
        QueryResultRow summary;
        summary.dataset = dataset.name;
        summary.method = "mummer4";
        summary.algorithm = build.algorithm;
        summary.acceleration = build.acceleration;
        summary.min_length = length;
        summary.query_count = dataset.queries.size();
        summary.query_bases = dataset.query_bases;
        for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
            const auto output = scratch / ("mummer-" + std::to_string(length) + "-" + std::to_string(repetition) + ".out");
            const auto error = scratch / ("mummer-" + std::to_string(length) + "-" + std::to_string(repetition) + ".err");
            auto args = common;
            args.insert(args.end(), {"-l", std::to_string(length), "-load", prefix.string(),
                                     reference_path.string(), query_path.string()});
            const auto measured = run_process(args, output, error);
            if (measured.status != 0) throw Error(ErrorCode::build_failure, "MUMmer4 query failed");
            std::vector<std::vector<MemMatch>> per_query(dataset.queries.size());
            (void)parse_mummer_output(output, reference_ids, dataset.queries,
                                      Strand::forward, per_query);
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
            raw.push_back({dataset.name, "mummer4", "load+mem", length, repetition,
                           measured.seconds, total, checksum, "ok"});
        }
        const auto reverse_output = scratch / ("mummer-reverse-" + std::to_string(length) + ".out");
        const auto reverse_error = scratch / ("mummer-reverse-" + std::to_string(length) + ".err");
        auto reverse_args = common;
        reverse_args.insert(reverse_args.end(), {"-r", "-l", std::to_string(length), "-load", prefix.string(),
                                                 reference_path.string(), query_path.string()});
        const auto reverse_process = run_process(reverse_args, reverse_output, reverse_error);
        if (reverse_process.status != 0) throw Error(ErrorCode::build_failure, "MUMmer4 reverse query failed");
        std::vector<std::vector<MemMatch>> reverse_matches(dataset.queries.size());
        (void)parse_mummer_output(reverse_output, reference_ids, dataset.queries,
                                  Strand::reverse_complement, reverse_matches);
        std::uint64_t mummer_checksum = checksum_seed();
        std::uint64_t internal_checksum = checksum_seed();
        std::uint64_t mummer_total = 0;
        std::uint64_t internal_total = 0;
        MemOptions reverse_options;
        reverse_options.min_length = length;
        reverse_options.strands = StrandMode::reverse_complement;
        reverse_options.algorithm = MemSearchAlgorithm::baseline;
        for (std::size_t query_id = 0; query_id < dataset.queries.size(); ++query_id) {
            auto& matches = reverse_matches[query_id];
            std::sort(matches.begin(), matches.end(), [](const auto& left, const auto& right) {
                return std::tie(left.query_position, left.sequence_id, left.reference_position, left.length, left.strand) <
                       std::tie(right.query_position, right.sequence_id, right.reference_position, right.length, right.strand);
            });
            const auto internal = verification_index.find_mems(dataset.queries[query_id].sequence, reverse_options);
            mix(mummer_checksum, query_id);
            mix(mummer_checksum, matches.size());
            mummer_total += matches.size();
            for (const auto& match : matches) mix_match(mummer_checksum, match);
            mix(internal_checksum, query_id);
            mix(internal_checksum, internal.total_matches);
            internal_total += internal.total_matches;
            for (const auto& match : internal.matches) mix_match(internal_checksum, match);
        }
        if (mummer_total != internal_total || mummer_checksum != internal_checksum)
            throw Error(ErrorCode::build_failure, "MUMmer4 reverse-complement MEM correctness mismatch");
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
    std::map<std::tuple<std::string, std::uint64_t>, std::pair<std::uint64_t, std::uint64_t>> expected;
    for (const auto& row : rows) {
        const auto key = std::make_tuple(row.dataset, row.min_length);
        const auto it = expected.find(key);
        if (it == expected.end()) expected.emplace(key, std::make_pair(row.total_matches, row.checksum));
        else if (it->second != std::make_pair(row.total_matches, row.checksum))
            throw Error(ErrorCode::build_failure, "MEM benchmark correctness mismatch for " + row.dataset +
                " min_length=" + std::to_string(row.min_length) + " method=" + row.method);
    }
}

void write_outputs(const Options& options, const std::vector<Dataset>& datasets,
    const std::vector<BuildResult>& builds, const std::vector<QueryResultRow>& queries,
    const std::vector<RawRow>& raw) {
    std::filesystem::create_directories(options.output_directory);
    {
        std::ofstream out(options.output_directory / "run_metadata.tsv");
        out << "profile\tscenario\tseed\tdataset\tdataset_fingerprint\ttotal_bases\tcontigs\tquery_count\tquery_bases"
               "\tcompiler\tcmake_version\tbuild_type\tos\tarchitecture\tlogical_cpus\tmummer_version\tmummer_sha256\n";
        for (const auto& dataset : datasets) {
            const auto scenario = dataset.name.find("repeat-rich") != std::string::npos ? "repeat-rich" :
                (dataset.name.find("many-contig") != std::string::npos ? "many-contig" :
                 dataset.name.substr(dataset.name.rfind('-') + 1));
            out << (options.reference ? "user" : options.profile) << '\t' << scenario
            << '\t' << options.seed << '\t' << dataset.name << '\t' << std::hex << dataset.fingerprint << std::dec
            << '\t' << dataset.total_bases << '\t' << dataset.reference.size() << '\t'
            << dataset.queries.size() << '\t' << dataset.query_bases << '\t'
            << __VERSION__ << '\t' << SUFKIT_BENCH_CMAKE_VERSION << '\t' << SUFKIT_BENCH_BUILD_TYPE
            << "\tLinux\tx86_64\t" << sysconf(_SC_NPROCESSORS_ONLN) << '\t'
            << options.mummer_version << '\t' << options.mummer_sha256 << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "build_results.tsv");
        out << "dataset\tmethod\talgorithm\tsa_acceleration\tbuild_seconds\tsave_seconds\tload_seconds\tpeak_rss_mb\tserialized_bytes\tauxiliary_bytes\tbits_per_base\tstatus\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : builds) {
            const auto data = std::find_if(datasets.begin(), datasets.end(), [&](const auto& value) { return value.name == row.dataset; });
            const double bits = data == datasets.end() || data->total_bases == 0 ? 0 :
                static_cast<double>(row.serialized_bytes) * 8.0 / static_cast<double>(data->total_bases);
            out << row.dataset << '\t' << row.method << '\t' << row.algorithm << '\t' << row.acceleration << '\t'
                << row.build_seconds << '\t' << row.save_seconds << '\t' << row.load_seconds << '\t'
                << row.peak_rss_mb << '\t' << row.serialized_bytes << '\t' << row.auxiliary_bytes << '\t'
                << bits << '\t' << row.status << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "query_results.tsv");
        out << "dataset\tmethod\talgorithm\tsa_acceleration\tmin_length\tquery_count\tquery_bases\tseconds_median\tseconds_min\tseconds_max\tquery_bases_per_second\tmatches_per_second\ttotal_matches\tresult_checksum\tstatus\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : queries) {
            const auto med = median(row.seconds);
            out << row.dataset << '\t' << row.method << '\t' << row.algorithm << '\t' << row.acceleration << '\t'
                << row.min_length << '\t' << row.query_count << '\t' << row.query_bases << '\t' << med << '\t'
                << *std::min_element(row.seconds.begin(), row.seconds.end()) << '\t'
                << *std::max_element(row.seconds.begin(), row.seconds.end()) << '\t'
                << static_cast<double>(row.query_bases) / med << '\t' << static_cast<double>(row.total_matches) / med << '\t'
                << row.total_matches << '\t' << std::hex << row.checksum << std::dec << '\t' << row.status << '\n';
        }
    }
    {
        std::ofstream out(options.output_directory / "raw_repetitions.tsv");
        out << "dataset\tmethod\toperation\tmin_length\trepetition\tseconds\ttotal_matches\tresult_checksum\tstatus\n";
        out << std::fixed << std::setprecision(6);
        for (const auto& row : raw) out << row.dataset << '\t' << row.method << '\t' << row.operation << '\t'
            << row.min_length << '\t' << row.repetition << '\t' << row.seconds << '\t' << row.total_matches
            << '\t' << std::hex << row.checksum << std::dec << '\t' << row.status << '\n';
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
            throw Error(ErrorCode::unsupported_backend, "MEM benchmark requires MUMmer4 version 4.0.1");
        const auto hash_out = scratch / "mummer-sha256.txt";
        const auto hash_err = scratch / "mummer-sha256.err";
        const auto hash_result = run_process(
            {"/usr/bin/sha256sum", options.mummer4->string()}, hash_out, hash_err);
        std::ifstream hash_input(hash_out);
        hash_input >> options.mummer_sha256;
        if (hash_result.status != 0 || options.mummer_sha256.size() != 64)
            throw Error(ErrorCode::io_error, "cannot fingerprint the MUMmer4 executable");
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
            else benchmark_internal(dataset, options, method, dataset_scratch, builds, queries, raw);
        }
    }
    write_outputs(options, datasets, builds, queries, raw);
    validate(queries);
    std::error_code cleanup_error;
    std::filesystem::remove_all(scratch, cleanup_error);
    if (cleanup_error) throw Error(ErrorCode::io_error, "cannot remove MEM benchmark scratch directory");
    std::cerr << "MEM benchmark results written to " << options.output_directory << '\n';
    return 0;
}

} // namespace sufkit::app::mem_bench
