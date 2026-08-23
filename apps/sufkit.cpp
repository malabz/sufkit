#include <sufkit/sufkit.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app_support.hpp"
#include "benchmark.hpp"

namespace {

struct ParsedOptions {
    std::map<std::string, std::string> values;
    std::set<std::string> flags;

    bool has(const std::string& name) const {
        return values.count(name) != 0 || flags.count(name) != 0;
    }

    const std::string& require(const std::string& name) const {
        const auto it = values.find(name);
        if (it == values.end()) {
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "missing required option " + name);
        }
        return it->second;
    }

    std::string value_or(const std::string& name, std::string fallback) const {
        const auto it = values.find(name);
        return it == values.end() ? std::move(fallback) : it->second;
    }
};

ParsedOptions parse_options(
    const std::vector<std::string>& arguments,
    const std::set<std::string>& value_options,
    const std::set<std::string>& flag_options) {
    ParsedOptions result;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& name = arguments[index];
        if (value_options.count(name) != 0) {
            if (result.has(name) || index + 1 >= arguments.size()) {
                throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid or duplicate option " + name);
            }
            result.values.emplace(name, arguments[++index]);
        } else if (flag_options.count(name) != 0) {
            if (!result.flags.insert(name).second) {
                throw sufkit::Error(sufkit::ErrorCode::invalid_input, "duplicate option " + name);
            }
        } else {
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "unknown option " + name);
        }
    }
    return result;
}

sufkit::StrandMode parse_strand(const std::string& value) {
    if (value == "forward") return sufkit::StrandMode::forward;
    if (value == "reverse") return sufkit::StrandMode::reverse_complement;
    if (value == "both") return sufkit::StrandMode::both;
    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid strand mode: " + value);
}

sufkit::FmBackend parse_fm_backend(const std::string& value) {
    if (value == "sdsl-csa-wt-huff") return sufkit::FmBackend::sdsl_csa_wt_huff;
    if (value == "sdsl-csa-wt-balanced") return sufkit::FmBackend::sdsl_csa_wt_balanced;
    if (value == "sdsl-csa-sada") return sufkit::FmBackend::sdsl_csa_sada;
    if (value == "sdsl-csa-wt-epr") return sufkit::FmBackend::sdsl_csa_wt_epr;
    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid FM backend: " + value);
}

sufkit::SaAcceleration parse_sa_acceleration(const std::string& value) {
    if (value == "none") return sufkit::SaAcceleration::none;
    if (value == "lcp") return sufkit::SaAcceleration::lcp;
    if (value == "child") return sufkit::SaAcceleration::lcp_child;
    if (value == "suffix-link") return sufkit::SaAcceleration::lcp_suffix_link;
    if (value == "full") return sufkit::SaAcceleration::full;
    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid SA acceleration: " + value);
}

sufkit::RightMaximalSearchAlgorithm parse_right_maximal_algorithm(const std::string& value) {
    if (value == "auto") return sufkit::RightMaximalSearchAlgorithm::auto_select;
    if (value == "baseline") return sufkit::RightMaximalSearchAlgorithm::baseline;
    if (value == "lcp") return sufkit::RightMaximalSearchAlgorithm::lcp;
    if (value == "child") return sufkit::RightMaximalSearchAlgorithm::child;
    if (value == "suffix-link") return sufkit::RightMaximalSearchAlgorithm::suffix_link;
    if (value == "full") return sufkit::RightMaximalSearchAlgorithm::full;
    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid right-maximal exact match algorithm: " + value);
}

sufkit::SaSearchAlgorithm parse_sa_search_algorithm(const std::string& value) {
    if (value == "auto") return sufkit::SaSearchAlgorithm::auto_select;
    if (value == "binary") return sufkit::SaSearchAlgorithm::binary;
    if (value == "lcp-binary") return sufkit::SaSearchAlgorithm::lcp_binary;
    if (value == "sapling-pwl") return sufkit::SaSearchAlgorithm::sapling_pwl;
    if (value == "child") return sufkit::SaSearchAlgorithm::child;
    throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                        "invalid SA search algorithm: " + value);
}

void print_usage(std::ostream& output) {
    output <<
        "sufkit 0.1.1 - genome suffix arrays, ESA right-maximal exact match search, and SDSL FM-indexes\n\n"
        "Commands:\n"
        "  sufkit build --type sa|fm --input REF.fa[.gz] --output REF.sufidx [options]\n"
        "  sufkit query --index REF.sufidx (--pattern ACGT | --query Q.fa[.gz]) [options]\n"
        "  sufkit right-maximal --index REF.sufidx --query Q.fa[.gz] [options]\n"
        "  sufkit inspect --index REF.sufidx\n"
        "  sufkit bench --profile smoke|quick|standard|full --output-dir DIR\n\n"
        "Run a command with --help for its option summary.\n";
}

int run_build(const std::vector<std::string>& arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        std::cout <<
            "sufkit build --type sa|fm --input PATH --output PATH [--force]\n"
            "  SA: --sa-backend auto|divsufsort|caps --sa-width auto|32|64 --threads N\n"
            "      --sa-sampling-rate K\n"
            "      --sa-acceleration none|lcp|child|suffix-link|full\n"
            "      [--learned-index] [--learned-k N] [--learned-memory-bp N]\n"
            "      [--learned-bucket-bits N]\n"
            "  FM: --fm-backend sdsl-csa-wt-huff|sdsl-csa-wt-balanced|sdsl-csa-wt-epr\n";
        return 0;
    }
    const auto options = parse_options(
        arguments,
        {"--type", "--input", "--output", "--sa-backend", "--sa-width", "--threads", "--fm-backend",
         "--sa-acceleration", "--sa-sampling-rate", "--learned-k", "--learned-memory-bp", "--learned-bucket-bits"},
        {"--force", "--learned-index"});
    const auto type = options.require("--type");
    const auto input = std::filesystem::path(options.require("--input"));
    const auto output = std::filesystem::path(options.require("--output"));
    const auto reference = sufkit::GenomeReference::from_fasta(input);
    const sufkit::SaveOptions save_options{options.has("--force")};

    if (type == "sa") {
        if (options.has("--fm-backend")) {
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "--fm-backend is invalid for --type sa");
        }
        sufkit::SuffixArrayBuildOptions build_options;
        const auto backend = options.value_or("--sa-backend", "auto");
        if (backend == "auto") build_options.backend = sufkit::SaBackend::auto_select;
        else if (backend == "divsufsort") build_options.backend = sufkit::SaBackend::divsufsort;
        else if (backend == "caps") build_options.backend = sufkit::SaBackend::caps;
        else throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid SA backend: " + backend);

        const auto width = options.value_or("--sa-width", "auto");
        if (width == "auto") build_options.coordinate_width = sufkit::CoordinateWidth::auto_select;
        else if (width == "32") build_options.coordinate_width = sufkit::CoordinateWidth::bits32;
        else if (width == "64") build_options.coordinate_width = sufkit::CoordinateWidth::bits64;
        else throw sufkit::Error(sufkit::ErrorCode::invalid_input, "invalid SA width: " + width);

        const auto threads = sufkit::app::parse_unsigned(options.value_or("--threads", "1"), "--threads");
        if (threads == 0 || threads > std::numeric_limits<std::uint32_t>::max()) {
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "--threads is out of range");
        }
        build_options.threads = static_cast<std::uint32_t>(threads);
        const auto sampling_rate = sufkit::app::parse_unsigned(
            options.value_or("--sa-sampling-rate", "1"), "--sa-sampling-rate");
        if (sampling_rate == 0 || sampling_rate > std::numeric_limits<std::uint32_t>::max())
            throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                                "--sa-sampling-rate is out of range");
        build_options.sampling_rate = static_cast<std::uint32_t>(sampling_rate);
        build_options.acceleration = parse_sa_acceleration(
            options.value_or("--sa-acceleration", "suffix-link"));
        const bool learned_option = options.has("--learned-index") || options.has("--learned-k") ||
            options.has("--learned-memory-bp") || options.has("--learned-bucket-bits");
        build_options.learned_index.enabled = learned_option;
        if (options.has("--learned-k")) {
            const auto value = sufkit::app::parse_unsigned(options.require("--learned-k"), "--learned-k");
            if (value == 0 || value > 31)
                throw sufkit::Error(sufkit::ErrorCode::invalid_input, "--learned-k is out of range");
            build_options.learned_index.k = static_cast<std::uint32_t>(value);
        }
        if (options.has("--learned-memory-bp")) {
            const auto value = sufkit::app::parse_unsigned(
                options.require("--learned-memory-bp"), "--learned-memory-bp");
            if (value == 0 || value > std::numeric_limits<std::uint32_t>::max())
                throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                                    "--learned-memory-bp is out of range");
            build_options.learned_index.memory_overhead_basis_points =
                static_cast<std::uint32_t>(value);
        }
        if (options.has("--learned-bucket-bits")) {
            const auto value = sufkit::app::parse_unsigned(
                options.require("--learned-bucket-bits"), "--learned-bucket-bits");
            if (value > 31)
                throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                                    "--learned-bucket-bits is out of range");
            build_options.learned_index.bucket_bits = static_cast<std::uint32_t>(value);
        }
        if (build_options.learned_index.bucket_bits &&
            *build_options.learned_index.bucket_bits > 2U * build_options.learned_index.k)
            throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                                "--learned-bucket-bits must not exceed 2*k");
        auto index = sufkit::SuffixArray::build(reference, build_options);
        index.save(output, save_options);
        std::cerr << "built " << index.info().backend << " index with "
                  << index.info().text_symbols << " symbols\n";
        return 0;
    }
    if (type == "fm") {
        if (options.has("--sa-backend") || options.has("--sa-width") || options.has("--threads") ||
            options.has("--sa-acceleration") || options.has("--sa-sampling-rate") ||
            options.has("--learned-index") ||
            options.has("--learned-k") || options.has("--learned-memory-bp") ||
            options.has("--learned-bucket-bits")) {
            throw sufkit::Error(
                sufkit::ErrorCode::invalid_input,
                "SA backend, width, and thread options are invalid for --type fm");
        }
        sufkit::FmIndexBuildOptions build_options;
        build_options.backend = parse_fm_backend(options.value_or("--fm-backend", "sdsl-csa-wt-huff"));
        auto index = sufkit::FmIndex::build(reference, build_options);
        index.save(output, save_options);
        std::cerr << "built " << index.info().backend << " index with "
                  << index.info().text_symbols << " symbols\n";
        return 0;
    }
    throw sufkit::Error(sufkit::ErrorCode::invalid_input, "--type must be sa or fm");
}

int run_right_maximal(const std::vector<std::string>& arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        std::cout <<
            "sufkit right-maximal --index PATH --query Q.fa[.gz] [--min-length N]\n"
            "  [--strand forward|reverse|both]\n"
            "  [--algorithm auto|baseline|lcp|child|suffix-link|full]\n"
            "  [--lookup-algorithm auto|binary|lcp-binary|sapling-pwl|child]\n"
            "  [--max-matches N]\n";
        return 0;
    }
    const auto options = parse_options(arguments,
        {"--index", "--query", "--min-length", "--strand", "--algorithm",
         "--lookup-algorithm", "--max-matches"}, {});
    const auto index_path = std::filesystem::path(options.require("--index"));
    const auto info = sufkit::inspect_index(index_path);
    if (info.kind != sufkit::IndexKind::suffix_array) {
        throw sufkit::Error(sufkit::ErrorCode::unsupported_backend,
            "right-maximal exact match search requires a suffix-array index in sufkit 0.1.1");
    }
    auto queries = sufkit::app::read_fasta_records(options.require("--query"));
    if (queries.empty()) throw sufkit::Error(sufkit::ErrorCode::invalid_input, "query FASTA contains no records");
    sufkit::RightMaximalOptions right_maximal_options;
    right_maximal_options.min_length = sufkit::app::parse_unsigned(options.value_or("--min-length", "20"), "--min-length");
    right_maximal_options.strands = parse_strand(options.value_or("--strand", "forward"));
    right_maximal_options.algorithm = parse_right_maximal_algorithm(options.value_or("--algorithm", "auto"));
    right_maximal_options.lookup_algorithm = parse_sa_search_algorithm(
        options.value_or("--lookup-algorithm", "auto"));
    std::optional<std::uint64_t> max_matches;
    if (options.has("--max-matches"))
        max_matches = sufkit::app::parse_unsigned(options.require("--max-matches"), "--max-matches");
    const auto index = sufkit::SuffixArray::load(index_path);
    std::unordered_set<std::string> names;
    std::cout << "query_id\tsequence_id\tsequence_name\treference_start\tquery_start\tlength\tstrand\n";
    for (const auto& query : queries) {
        if (query.name.empty() || !names.insert(query.name).second)
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "query names must be non-empty and unique");
        const auto result = index.find_right_maximal_matches(query.sequence, right_maximal_options, max_matches);
        for (const auto& match : result.matches) {
            const auto sequence = index.sequence_info(match.sequence_id);
            std::cout << query.name << '\t' << match.sequence_id << '\t' << sequence.name << '\t'
                      << match.reference_position << '\t' << match.query_position << '\t'
                      << match.length << '\t' << sufkit::to_string(match.strand) << '\n';
        }
        if (result.truncated) {
            std::cerr << "query " << query.name << " truncated: reported " << result.matches.size()
                      << " of " << result.total_matches << " right-maximal exact matches\n";
        }
    }
    return 0;
}

template <class Index>
void emit_queries(
    const Index& index,
    const std::vector<sufkit::SequenceRecord>& queries,
    sufkit::StrandMode strands,
    bool count_only,
    std::optional<std::uint64_t> max_hits) {
    std::unordered_set<std::string> names;
    if (count_only) {
        std::cout << "query_id\ttotal_hits\n";
    } else {
        std::cout << "query_id\tsequence_id\tsequence_name\tstart\tend\tstrand\n";
    }
    for (const auto& query : queries) {
        if (query.name.empty() || !names.insert(query.name).second) {
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "query names must be non-empty and unique");
        }
        if (count_only) {
            std::cout << query.name << '\t' << index.count(query.sequence, strands) << '\n';
            continue;
        }
        sufkit::LocateOptions locate_options;
        locate_options.strands = strands;
        locate_options.max_hits = max_hits;
        const auto result = index.locate(query.sequence, locate_options);
        for (const auto& match : result.hits) {
            const auto sequence = index.sequence_info(match.sequence_id);
            std::cout << query.name << '\t'
                      << match.sequence_id << '\t'
                      << sequence.name << '\t'
                      << match.position << '\t'
                      << match.position + match.length << '\t'
                      << sufkit::to_string(match.strand) << '\n';
        }
        if (result.truncated) {
            std::cerr << "query " << query.name << " truncated: reported "
                      << result.hits.size() << " of " << result.total_hits << " hits\n";
        }
    }
}

void emit_sa_queries(
    const sufkit::SuffixArray& index,
    const std::vector<sufkit::SequenceRecord>& queries,
    sufkit::StrandMode strands,
    bool count_only,
    std::optional<std::uint64_t> max_hits,
    sufkit::SaSearchAlgorithm algorithm) {
    std::unordered_set<std::string> names;
    if (count_only) std::cout << "query_id\ttotal_hits\n";
    else std::cout << "query_id\tsequence_id\tsequence_name\tstart\tend\tstrand\n";
    for (const auto& query : queries) {
        if (query.name.empty() || !names.insert(query.name).second)
            throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                                "query names must be non-empty and unique");
        if (count_only) {
            std::cout << query.name << '\t' << index.count(query.sequence, strands, algorithm) << '\n';
            continue;
        }
        sufkit::LocateOptions locate_options;
        locate_options.strands = strands;
        locate_options.max_hits = max_hits;
        const auto result = index.locate(query.sequence, locate_options, algorithm);
        for (const auto& match : result.hits) {
            const auto sequence = index.sequence_info(match.sequence_id);
            std::cout << query.name << '\t' << match.sequence_id << '\t' << sequence.name << '\t'
                      << match.position << '\t' << match.position + match.length << '\t'
                      << sufkit::to_string(match.strand) << '\n';
        }
        if (result.truncated)
            std::cerr << "query " << query.name << " truncated: reported " << result.hits.size()
                      << " of " << result.total_hits << " hits\n";
    }
}

int run_query(const std::vector<std::string>& arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        std::cout <<
            "sufkit query --index PATH (--pattern ACGT | --query Q.fa[.gz])\n"
            "  [--strand forward|reverse|both] [--count-only] [--max-hits N]\n"
            "  [--algorithm auto|binary|lcp-binary|sapling-pwl|child]\n";
        return 0;
    }
    const auto options = parse_options(
        arguments,
        {"--index", "--pattern", "--query", "--strand", "--max-hits", "--algorithm"},
        {"--count-only"});
    if (options.has("--pattern") == options.has("--query")) {
        throw sufkit::Error(
            sufkit::ErrorCode::invalid_input,
            "exactly one of --pattern and --query is required");
    }
    std::vector<sufkit::SequenceRecord> queries;
    if (options.has("--pattern")) {
        queries.push_back({"query_0", "", options.require("--pattern")});
    } else {
        queries = sufkit::app::read_fasta_records(options.require("--query"));
        if (queries.empty()) {
            throw sufkit::Error(sufkit::ErrorCode::invalid_input, "query FASTA contains no records");
        }
    }
    std::optional<std::uint64_t> max_hits;
    if (options.has("--max-hits")) {
        max_hits = sufkit::app::parse_unsigned(options.require("--max-hits"), "--max-hits");
    }
    const auto strands = parse_strand(options.value_or("--strand", "forward"));
    const auto path = std::filesystem::path(options.require("--index"));
    const auto info = sufkit::inspect_index(path);
    if (info.kind == sufkit::IndexKind::suffix_array) {
        const auto index = sufkit::SuffixArray::load(path);
        emit_sa_queries(index, queries, strands, options.has("--count-only"), max_hits,
                        parse_sa_search_algorithm(options.value_or("--algorithm", "auto")));
    } else {
        if (options.has("--algorithm") && options.require("--algorithm") != "auto")
            throw sufkit::Error(sufkit::ErrorCode::invalid_input,
                                "--algorithm is only supported by suffix-array indexes");
        const auto index = sufkit::FmIndex::load(path);
        emit_queries(index, queries, strands, options.has("--count-only"), max_hits);
    }
    return 0;
}

int run_inspect(const std::vector<std::string>& arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        std::cout << "sufkit inspect --index PATH\n";
        return 0;
    }
    const auto options = parse_options(arguments, {"--index"}, {});
    const auto info = sufkit::inspect_index(options.require("--index"));
    std::cout << "key\tvalue\n"
              << "kind\t" << sufkit::to_string(info.kind) << '\n'
              << "format_version\t" << info.format_version << '\n'
              << "library_version\t" << info.library_version << '\n'
              << "backend\t" << info.backend << '\n'
              << "backend_signature\t" << info.backend_signature << '\n'
              << "sdsl_version\t" << info.sdsl_version << '\n'
              << "coordinate_width\t" << static_cast<unsigned>(info.coordinate_width) << '\n'
              << "sequence_count\t" << info.sequence_count << '\n'
              << "total_bases\t" << info.total_bases << '\n'
              << "text_symbols\t" << info.text_symbols << '\n'
              << "suffix_count\t" << info.suffix_count << '\n'
              << "sa_sampling_rate\t" << info.sa_sampling_rate << '\n'
              << "ambiguous_bases\t" << info.ambiguous_bases << '\n'
              << "fingerprint\t" << std::hex << std::setfill('0') << std::setw(16)
              << info.fingerprint << std::dec << '\n'
              << "serialized_bytes\t" << info.serialized_bytes << '\n';
    if (info.kind == sufkit::IndexKind::suffix_array) {
        std::cout << "sa_acceleration\t" << sufkit::to_string(info.sa_acceleration) << '\n'
                  << "auxiliary_bytes\t" << info.auxiliary_bytes << '\n'
                  << "lookup_acceleration\t" << sufkit::to_string(info.sa_lookup_acceleration) << '\n'
                  << "learned_index_bytes\t" << info.learned_index_bytes << '\n'
                  << "learned_k\t" << info.learned_k << '\n'
                  << "learned_bucket_bits\t" << info.learned_bucket_bits << '\n'
                  << "learned_memory_overhead_basis_points\t"
                  << info.learned_memory_overhead_basis_points << '\n';
    }
    return 0;
}

int exit_code_for(sufkit::ErrorCode code) {
    switch (code) {
    case sufkit::ErrorCode::invalid_input: return 2;
    case sufkit::ErrorCode::io_error: return 3;
    case sufkit::ErrorCode::corrupt_index:
    case sufkit::ErrorCode::version_mismatch: return 4;
    case sufkit::ErrorCode::unsupported_backend:
    case sufkit::ErrorCode::build_failure: return 5;
    }
    return 5;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(std::cerr);
        return 2;
    }
    const std::string command = argv[1];
    std::vector<std::string> arguments;
    for (int index = 2; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    try {
        if (command == "--help" || command == "help") {
            print_usage(std::cout);
            return 0;
        }
        if (command == "--version") {
            std::cout << SUFKIT_VERSION_STRING << '\n';
            return 0;
        }
        if (command == "build") return run_build(arguments);
        if (command == "query") return run_query(arguments);
        if (command == "right-maximal") return run_right_maximal(arguments);
        if (command == "inspect") return run_inspect(arguments);
        if (command == "bench") return sufkit::app::run_benchmark(arguments);
        throw sufkit::Error(sufkit::ErrorCode::invalid_input, "unknown command: " + command);
    } catch (const sufkit::Error& error) {
        std::cerr << "sufkit: " << sufkit::to_string(error.code()) << ": " << error.what() << '\n';
        return exit_code_for(error.code());
    } catch (const std::exception& error) {
        std::cerr << "sufkit: unexpected error: " << error.what() << '\n';
        return 5;
    }
}
