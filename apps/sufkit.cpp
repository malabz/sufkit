// SPDX-License-Identifier: MIT

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
#include <sufkit/sufkit.hpp>

namespace {

enum class MaximalCommand {
  kMem,
  kMam,
  kSmem,
  kMum,
};

const char* MaximalCommandName(MaximalCommand command) {
  switch (command) {
    case MaximalCommand::kMem:
      return "mem";
    case MaximalCommand::kMam:
      return "mam";
    case MaximalCommand::kSmem:
      return "smem";
    case MaximalCommand::kMum:
      return "mum";
  }
  return "maximal";
}

const char* MaximalSearchName(MaximalCommand command) {
  switch (command) {
    case MaximalCommand::kMem:
      return "MEM";
    case MaximalCommand::kMam:
      return "reference-MAM";
    case MaximalCommand::kSmem:
      return "SMEM";
    case MaximalCommand::kMum:
      return "MUM";
  }
  return "maximal-match";
}

const char* MaximalResultName(MaximalCommand command) {
  switch (command) {
    case MaximalCommand::kMem:
      return "MEMs";
    case MaximalCommand::kMam:
      return "reference-MAMs";
    case MaximalCommand::kSmem:
      return "SMEM coordinate matches";
    case MaximalCommand::kMum:
      return "MUMs";
  }
  return "maximal matches";
}

struct ParsedOptions {
  std::map<std::string, std::string> values;
  std::set<std::string> flags;

  bool Has(const std::string& name) const {
    return values.count(name) != 0 || flags.count(name) != 0;
  }

  const std::string& Require(const std::string& name) const {
    const auto it = values.find(name);
    if (it == values.end()) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "missing required option " + name);
    }
    return it->second;
  }

  std::string ValueOr(const std::string& name, std::string fallback) const {
    const auto it = values.find(name);
    return it == values.end() ? std::move(fallback) : it->second;
  }
};

ParsedOptions ParseOptions(const std::vector<std::string>& arguments,
                           const std::set<std::string>& value_options,
                           const std::set<std::string>& flag_options) {
  ParsedOptions result;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto& name = arguments[index];
    if (value_options.count(name) != 0) {
      if (result.Has(name) || index + 1 >= arguments.size()) {
        throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                            "invalid or duplicate option " + name);
      }
      result.values.emplace(name, arguments[++index]);
    } else if (flag_options.count(name) != 0) {
      if (!result.flags.insert(name).second) {
        throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                            "duplicate option " + name);
      }
    } else {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "unknown option " + name);
    }
  }
  return result;
}

sufkit::StrandMode ParseStrand(const std::string& value) {
  if (value == "forward") {
    return sufkit::StrandMode::kForward;
  }
  if (value == "reverse") {
    return sufkit::StrandMode::kReverseComplement;
  }
  if (value == "both") {
    return sufkit::StrandMode::kBoth;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "invalid strand mode: " + value);
}

sufkit::FmBackend ParseFmBackend(const std::string& value) {
  if (value == "sdsl-csa-wt-huff") {
    return sufkit::FmBackend::kSdslCsaWtHuff;
  }
  if (value == "sdsl-csa-wt-balanced") {
    return sufkit::FmBackend::kSdslCsaWtBalanced;
  }
  if (value == "sdsl-csa-sada") {
    return sufkit::FmBackend::kSdslCsaSada;
  }
  if (value == "sdsl-csa-wt-epr") {
    return sufkit::FmBackend::kSdslCsaWtEpr;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "invalid FM backend: " + value);
}

sufkit::SaAcceleration ParseSaAcceleration(const std::string& value) {
  if (value == "none") {
    return sufkit::SaAcceleration::kNone;
  }
  if (value == "lcp") {
    return sufkit::SaAcceleration::kLcp;
  }
  if (value == "child") {
    return sufkit::SaAcceleration::kLcpChild;
  }
  if (value == "suffix-link") {
    return sufkit::SaAcceleration::kLcpSuffixLink;
  }
  if (value == "full") {
    return sufkit::SaAcceleration::kFull;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "invalid SA acceleration: " + value);
}

sufkit::RightMaximalSearchAlgorithm ParseRightMaximalAlgorithm(
    const std::string& value) {
  if (value == "auto") {
    return sufkit::RightMaximalSearchAlgorithm::kAutoSelect;
  }
  if (value == "baseline") {
    return sufkit::RightMaximalSearchAlgorithm::kBaseline;
  }
  if (value == "lcp") {
    return sufkit::RightMaximalSearchAlgorithm::kLcp;
  }
  if (value == "child") {
    return sufkit::RightMaximalSearchAlgorithm::kChild;
  }
  if (value == "suffix-link") {
    return sufkit::RightMaximalSearchAlgorithm::kSuffixLink;
  }
  if (value == "full") {
    return sufkit::RightMaximalSearchAlgorithm::kFull;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "invalid right-maximal exact match algorithm: " + value);
}

sufkit::MemSearchAlgorithm ParseMemAlgorithm(const std::string& value) {
  if (value == "auto") {
    return sufkit::MemSearchAlgorithm::kAutoSelect;
  }
  if (value == "baseline") {
    return sufkit::MemSearchAlgorithm::kBaseline;
  }
  if (value == "lcp") {
    return sufkit::MemSearchAlgorithm::kLcp;
  }
  if (value == "child") {
    return sufkit::MemSearchAlgorithm::kChild;
  }
  if (value == "suffix-link") {
    return sufkit::MemSearchAlgorithm::kSuffixLink;
  }
  if (value == "full") {
    return sufkit::MemSearchAlgorithm::kFull;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "invalid maximal-match search algorithm: " + value);
}

sufkit::SaSearchAlgorithm ParseSaSearchAlgorithm(const std::string& value) {
  if (value == "auto") {
    return sufkit::SaSearchAlgorithm::kAutoSelect;
  }
  if (value == "binary") {
    return sufkit::SaSearchAlgorithm::kBinary;
  }
  if (value == "lcp-binary") {
    return sufkit::SaSearchAlgorithm::kLcpBinary;
  }
  if (value == "sapling-pwl") {
    return sufkit::SaSearchAlgorithm::kSaplingPwl;
  }
  if (value == "child") {
    return sufkit::SaSearchAlgorithm::kChild;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "invalid SA search algorithm: " + value);
}

void PrintUsage(std::ostream& output) {
  output << "sufkit " << SUFKIT_VERSION_STRING
         << " - genome suffix arrays, maximal exact match search, and SDSL "
            "FM-indexes\n\n"
            "Commands:\n"
            "  sufkit build --type sa|fm --input REF.fa[.gz] --output "
            "REF.sufidx [options]\n"
            "  sufkit query --index REF.sufidx (--pattern ACGT | --query "
            "Q.fa[.gz]) [options]\n"
            "  sufkit right-maximal --index REF.sufidx --query Q.fa[.gz] "
            "[options]\n"
            "  sufkit mem --index REF.sufidx --query Q.fa[.gz] [options]\n"
            "  sufkit mam --index REF.sufidx --query Q.fa[.gz] [options]\n"
            "  sufkit smem --index REF.sufidx --query Q.fa[.gz] [options]\n"
            "  sufkit mum --index REF.sufidx --query Q.fa[.gz] [options]\n"
            "  sufkit inspect --index REF.sufidx\n"
            "  sufkit bench --profile smoke|quick|standard|full --output-dir "
            "DIR\n\n"
            "Run a command with --help for its option summary.\n";
}

int RunBuild(const std::vector<std::string>& arguments) {
  if (arguments.size() == 1 && arguments.front() == "--help") {
    std::cout
        << "sufkit build --type sa|fm --input PATH --output PATH [--force]\n"
           "  SA: --sa-backend auto|divsufsort|caps --sa-width auto|32|64 "
           "--sa-storage-width auto|32|40|48|64 "
           "--sa-profile fast|low-memory "
           "--threads N\n"
           "      --sa-sampling-rate K\n"
           "      --sa-acceleration none|lcp|child|suffix-link|full\n"
           "      [--learned-index] [--learned-k N] [--learned-memory-bp N]\n"
           "      [--learned-bucket-bits N]\n"
           "  FM: --fm-backend "
           "sdsl-csa-wt-huff|sdsl-csa-wt-balanced|sdsl-csa-wt-epr\n";
    return 0;
  }
  const auto options = ParseOptions(
      arguments,
      {"--type", "--input", "--output", "--sa-backend", "--sa-width",
       "--sa-storage-width", "--sa-profile", "--threads", "--fm-backend",
       "--sa-acceleration", "--sa-sampling-rate", "--learned-k",
       "--learned-memory-bp", "--learned-bucket-bits"},
      {"--force", "--learned-index"});
  const auto type = options.Require("--type");
  const auto input = std::filesystem::path(options.Require("--input"));
  const auto output = std::filesystem::path(options.Require("--output"));
  const auto reference = sufkit::GenomeReference::FromFasta(input);
  const sufkit::SaveOptions save_options{options.Has("--force")};

  if (type == "sa") {
    if (options.Has("--fm-backend")) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--fm-backend is invalid for --type sa");
    }
    const auto profile = options.ValueOr("--sa-profile", "fast");
    sufkit::SuffixArrayBuildOptions build_options;
    if (profile == "fast") {
      build_options = sufkit::FastSuffixArrayBuildOptions();
    } else if (profile == "low-memory") {
      build_options = sufkit::LowMemorySuffixArrayBuildOptions();
    } else {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "invalid SA resource profile: " + profile);
    }
    const auto backend = options.ValueOr("--sa-backend", "auto");
    if (backend == "auto") {
      build_options.backend = sufkit::SaBackend::kAutoSelect;
    } else if (backend == "divsufsort") {
      build_options.backend = sufkit::SaBackend::kDivsufsort;
    } else if (backend == "caps") {
      build_options.backend = sufkit::SaBackend::kCaps;
    } else {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "invalid SA backend: " + backend);
    }

    const auto width = options.ValueOr("--sa-width", "auto");
    if (width == "auto") {
      build_options.coordinate_width = sufkit::CoordinateWidth::kAutoSelect;
    } else if (width == "32") {
      build_options.coordinate_width = sufkit::CoordinateWidth::kBits32;
    } else if (width == "64") {
      build_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
    } else {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "invalid SA width: " + width);
    }

    const auto storage_width = options.ValueOr("--sa-storage-width", "auto");
    if (storage_width == "auto") {
      build_options.storage_width =
          sufkit::CoordinateStorageWidth::kAutoSelect;
    } else if (storage_width == "32") {
      build_options.storage_width = sufkit::CoordinateStorageWidth::kBits32;
    } else if (storage_width == "40") {
      build_options.storage_width = sufkit::CoordinateStorageWidth::kBits40;
    } else if (storage_width == "48") {
      build_options.storage_width = sufkit::CoordinateStorageWidth::kBits48;
    } else if (storage_width == "64") {
      build_options.storage_width = sufkit::CoordinateStorageWidth::kBits64;
    } else {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "invalid SA storage width: " + storage_width);
    }

    const auto threads = sufkit::app::ParseUnsigned(
        options.ValueOr("--threads", "1"), "--threads");
    if (threads == 0 || threads > std::numeric_limits<std::uint32_t>::max()) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--threads is out of range");
    }
    build_options.threads = static_cast<std::uint32_t>(threads);
    const auto sampling_rate = sufkit::app::ParseUnsigned(
        options.ValueOr("--sa-sampling-rate", "1"), "--sa-sampling-rate");
    if (sampling_rate == 0 ||
        sampling_rate > std::numeric_limits<std::uint32_t>::max()) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--sa-sampling-rate is out of range");
    }
    build_options.sampling_rate = static_cast<std::uint32_t>(sampling_rate);
    if (options.Has("--sa-acceleration")) {
      build_options.acceleration =
          ParseSaAcceleration(options.Require("--sa-acceleration"));
    }
    const bool learned_option = options.Has("--learned-index") ||
                                options.Has("--learned-k") ||
                                options.Has("--learned-memory-bp") ||
                                options.Has("--learned-bucket-bits");
    if (profile == "low-memory") {
      if (sampling_rate != 1) {
        throw sufkit::Error(
            sufkit::ErrorCode::kInvalidInput,
            "--sa-profile low-memory requires --sa-sampling-rate 1");
      }
      if (options.Has("--sa-acceleration") &&
          build_options.acceleration != sufkit::SaAcceleration::kLcp) {
        throw sufkit::Error(
            sufkit::ErrorCode::kInvalidInput,
            "--sa-profile low-memory only supports --sa-acceleration lcp");
      }
      if (learned_option) {
        throw sufkit::Error(
            sufkit::ErrorCode::kInvalidInput,
            "--sa-profile low-memory does not retain a learned index");
      }
    }
    build_options.learned_index.enabled = learned_option;
    if (options.Has("--learned-k")) {
      const auto value = sufkit::app::ParseUnsigned(
          options.Require("--learned-k"), "--learned-k");
      if (value == 0 || value > 31) {
        throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                            "--learned-k is out of range");
      }
      build_options.learned_index.k = static_cast<std::uint32_t>(value);
    }
    if (options.Has("--learned-memory-bp")) {
      const auto value = sufkit::app::ParseUnsigned(
          options.Require("--learned-memory-bp"), "--learned-memory-bp");
      if (value == 0 || value > std::numeric_limits<std::uint32_t>::max()) {
        throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                            "--learned-memory-bp is out of range");
      }
      build_options.learned_index.memory_overhead_basis_points =
          static_cast<std::uint32_t>(value);
    }
    if (options.Has("--learned-bucket-bits")) {
      const auto value = sufkit::app::ParseUnsigned(
          options.Require("--learned-bucket-bits"), "--learned-bucket-bits");
      if (value > 31) {
        throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                            "--learned-bucket-bits is out of range");
      }
      build_options.learned_index.bucket_bits =
          static_cast<std::uint32_t>(value);
    }
    if (build_options.learned_index.bucket_bits &&
        *build_options.learned_index.bucket_bits >
            2U * build_options.learned_index.k) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--learned-bucket-bits must not exceed 2*k");
    }
    auto index = sufkit::SuffixArray::Build(reference, build_options);
    index.Save(output, save_options);
    std::cerr << "built " << index.GetInfo().backend << " index with "
              << index.GetInfo().text_symbols << " symbols\n";
    return 0;
  }
  if (type == "fm") {
    if (options.Has("--sa-backend") || options.Has("--sa-width") ||
        options.Has("--sa-storage-width") || options.Has("--sa-profile") ||
        options.Has("--threads") || options.Has("--sa-acceleration") ||
        options.Has("--sa-sampling-rate") || options.Has("--learned-index") ||
        options.Has("--learned-k") || options.Has("--learned-memory-bp") ||
        options.Has("--learned-bucket-bits")) {
      throw sufkit::Error(
          sufkit::ErrorCode::kInvalidInput,
          "SA backend, width, and thread options are invalid for --type fm");
    }
    sufkit::FmIndexBuildOptions build_options;
    build_options.backend =
        ParseFmBackend(options.ValueOr("--fm-backend", "sdsl-csa-wt-huff"));
    auto index = sufkit::FmIndex::Build(reference, build_options);
    index.Save(output, save_options);
    std::cerr << "built " << index.GetInfo().backend << " index with "
              << index.GetInfo().text_symbols << " symbols\n";
    return 0;
  }
  throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                      "--type must be sa or fm");
}

int RunRightMaximal(const std::vector<std::string>& arguments) {
  if (arguments.size() == 1 && arguments.front() == "--help") {
    std::cout
        << "sufkit right-maximal --index PATH --query Q.fa[.gz] [--min-length "
           "N]\n"
           "  [--strand forward|reverse|both]\n"
           "  [--algorithm auto|baseline|lcp|child|suffix-link|full]\n"
           "  [--lookup-algorithm auto|binary|lcp-binary|sapling-pwl|child]\n"
           "  [--max-matches N]\n";
    return 0;
  }
  const auto options =
      ParseOptions(arguments,
                   {"--index", "--query", "--min-length", "--strand",
                    "--algorithm", "--lookup-algorithm", "--max-matches"},
                   {});
  const auto index_path = std::filesystem::path(options.Require("--index"));
  const auto info = sufkit::InspectIndex(index_path);
  if (info.kind != sufkit::IndexKind::kSuffixArray) {
    throw sufkit::Error(sufkit::ErrorCode::kUnsupportedBackend,
                        std::string("right-maximal exact match search requires "
                                    "a suffix-array index in sufkit ") +
                            SUFKIT_VERSION_STRING);
  }
  auto queries = sufkit::app::ReadFastaRecords(options.Require("--query"));
  if (queries.empty()) {
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "query FASTA contains no records");
  }
  sufkit::RightMaximalOptions right_maximal_options;
  right_maximal_options.min_length = sufkit::app::ParseUnsigned(
      options.ValueOr("--min-length", "20"), "--min-length");
  right_maximal_options.strands =
      ParseStrand(options.ValueOr("--strand", "forward"));
  right_maximal_options.algorithm =
      ParseRightMaximalAlgorithm(options.ValueOr("--algorithm", "auto"));
  right_maximal_options.lookup_algorithm =
      ParseSaSearchAlgorithm(options.ValueOr("--lookup-algorithm", "auto"));
  std::optional<std::uint64_t> max_matches;
  if (options.Has("--max-matches")) {
    max_matches = sufkit::app::ParseUnsigned(options.Require("--max-matches"),
                                             "--max-matches");
  }
  const auto index = sufkit::SuffixArray::Load(index_path);
  std::unordered_set<std::string> names;
  std::cout << "query_id\tsequence_id\tsequence_name\treference_start\tquery_"
               "start\tlength\tstrand\n";
  for (const auto& query : queries) {
    if (query.name.empty() || !names.insert(query.name).second) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "query names must be non-empty and unique");
    }
    const auto result = index.FindRightMaximalMatches(
        query.sequence, right_maximal_options, max_matches);
    for (const auto& match : result.matches) {
      const auto sequence = index.GetSequenceInfo(match.sequence_id);
      std::cout << query.name << '\t' << match.sequence_id << '\t'
                << sequence.name << '\t' << match.reference_position << '\t'
                << match.query_position << '\t' << match.length << '\t'
                << sufkit::ToString(match.strand) << '\n';
    }
    if (result.truncated) {
      std::cerr << "query " << query.name << " truncated: reported "
                << result.matches.size() << " of " << result.total_matches
                << " right-maximal exact matches\n";
    }
  }
  return 0;
}

template <class Match>
void EmitMaximalMatch(const sufkit::SuffixArray& index,
                      const std::string& query_name, const Match& match) {
  const auto sequence = index.GetSequenceInfo(match.sequence_id);
  std::cout << query_name << '\t' << match.sequence_id << '\t' << sequence.name
            << '\t' << match.reference_position << '\t' << match.query_position
            << '\t' << match.length << '\t' << sufkit::ToString(match.strand)
            << '\n';
}

void EmitMaximalMatch(const sufkit::SuffixArray& index,
                      const std::string& query_name,
                      const sufkit::SmemMatch& match) {
  const auto sequence = index.GetSequenceInfo(match.sequence_id);
  std::cout << query_name << '\t' << match.sequence_id << '\t' << sequence.name
            << '\t' << match.reference_position << '\t' << match.query_position
            << '\t' << match.length << '\t' << match.reference_occurrences
            << '\t' << sufkit::ToString(match.strand) << '\n';
}

template <class Result>
void EmitMaximalResult(const sufkit::SuffixArray& index,
                       const std::string& query_name, const Result& result,
                       MaximalCommand command) {
  for (const auto& match : result.matches) {
    EmitMaximalMatch(index, query_name, match);
  }
  if (result.truncated) {
    std::cerr << "query " << query_name << " truncated: reported "
              << result.matches.size() << " of " << result.total_matches << ' '
              << MaximalResultName(command) << '\n';
  }
}

int RunMaximal(const std::vector<std::string>& arguments,
               MaximalCommand command) {
  const auto command_name = MaximalCommandName(command);
  if (arguments.size() == 1 && arguments.front() == "--help") {
    std::cout << "sufkit " << command_name
              << " --index PATH --query Q.fa[.gz] [--min-length N]\n"
                 "  [--strand forward|reverse|both]\n"
                 "  [--algorithm auto|baseline|lcp|child|suffix-link|full]\n"
                 "  [--lookup-algorithm "
                 "auto|binary|lcp-binary|sapling-pwl|child]\n";
    if (command == MaximalCommand::kMem) {
      std::cout << "  [--skip N]\n";
    }
    if (command == MaximalCommand::kSmem) {
      std::cout << "  [--min-occurrences N]\n";
    }
    std::cout << "  [--max-matches N]\n";
    return 0;
  }
  const auto options =
      ParseOptions(arguments,
                   {"--index", "--query", "--min-length", "--strand",
                    "--algorithm", "--lookup-algorithm", "--skip",
                    "--min-occurrences", "--max-matches"},
                   {});
  if (command != MaximalCommand::kMem && options.Has("--skip")) {
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "--skip is valid only for MEM search");
  }
  if (command != MaximalCommand::kSmem &&
      options.Has("--min-occurrences")) {
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "--min-occurrences is valid only for SMEM search");
  }
  const auto index_path = std::filesystem::path(options.Require("--index"));
  const auto info = sufkit::InspectIndex(index_path);
  if (info.kind != sufkit::IndexKind::kSuffixArray) {
    throw sufkit::Error(sufkit::ErrorCode::kUnsupportedBackend,
                        std::string(MaximalSearchName(command)) +
                            " search requires a suffix-array index");
  }
  if ((command == MaximalCommand::kSmem ||
       command == MaximalCommand::kMum) &&
      info.sa_sampling_rate != 1) {
    throw sufkit::Error(sufkit::ErrorCode::kUnsupportedBackend,
                        std::string(MaximalSearchName(command)) +
                            " search requires a complete suffix array");
  }
  auto queries = sufkit::app::ReadFastaRecords(options.Require("--query"));
  if (queries.empty()) {
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "query FASTA contains no records");
  }
  const auto min_length = sufkit::app::ParseUnsigned(
      options.ValueOr("--min-length", "20"), "--min-length");
  if (min_length == 0) {
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "--min-length must be greater than zero");
  }
  const auto strands = ParseStrand(options.ValueOr("--strand", "forward"));
  const auto algorithm =
      ParseMemAlgorithm(options.ValueOr("--algorithm", "auto"));
  const auto lookup = ParseSaSearchAlgorithm(
      options.ValueOr("--lookup-algorithm", "auto"));
  std::uint64_t min_occurrences = 1;
  if (command == MaximalCommand::kSmem) {
    min_occurrences = sufkit::app::ParseUnsigned(
        options.ValueOr("--min-occurrences", "1"), "--min-occurrences");
    if (min_occurrences == 0) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--min-occurrences must be greater than zero");
    }
  }
  std::optional<std::uint64_t> max_matches;
  if (options.Has("--max-matches")) {
    max_matches = sufkit::app::ParseUnsigned(options.Require("--max-matches"),
                                             "--max-matches");
  }
  std::optional<std::uint32_t> skip;
  if (options.Has("--skip")) {
    const auto parsed =
        sufkit::app::ParseUnsigned(options.Require("--skip"), "--skip");
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--skip exceeds the supported range");
    }
    if (parsed == 0) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "--skip must be greater than zero");
    }
    skip = static_cast<std::uint32_t>(parsed);
  }

  const auto index = sufkit::SuffixArray::Load(index_path);
  std::unordered_set<std::string> names;
  for (const auto& query : queries) {
    if (query.name.empty() || !names.insert(query.name).second) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "query names must be non-empty and unique");
    }
  }
  if (command == MaximalCommand::kSmem) {
    std::cout
        << "query_id\tsequence_id\tsequence_name\treference_start\tquery_"
           "start\tlength\treference_occurrences\tstrand\n";
  } else {
    std::cout << "query_id\tsequence_id\tsequence_name\treference_start\tquery_"
                 "start\tlength\tstrand\n";
  }
  for (const auto& query : queries) {
    switch (command) {
      case MaximalCommand::kMem: {
        sufkit::MemOptions search;
        search.min_length = min_length;
        search.strands = strands;
        search.algorithm = algorithm;
        search.lookup_algorithm = lookup;
        search.skip_multiplier = skip;
        EmitMaximalResult(
            index, query.name,
            index.FindMems(query.sequence, search, max_matches), command);
        break;
      }
      case MaximalCommand::kMam: {
        sufkit::MamOptions search;
        search.min_length = min_length;
        search.strands = strands;
        search.algorithm = algorithm;
        search.lookup_algorithm = lookup;
        EmitMaximalResult(
            index, query.name,
            index.FindMams(query.sequence, search, max_matches), command);
        break;
      }
      case MaximalCommand::kSmem: {
        sufkit::SmemOptions search;
        search.min_length = min_length;
        search.min_occurrences = min_occurrences;
        search.strands = strands;
        search.algorithm = algorithm;
        search.lookup_algorithm = lookup;
        EmitMaximalResult(
            index, query.name,
            index.FindSmems(query.sequence, search, max_matches), command);
        break;
      }
      case MaximalCommand::kMum: {
        sufkit::MumOptions search;
        search.min_length = min_length;
        search.strands = strands;
        search.algorithm = algorithm;
        search.lookup_algorithm = lookup;
        EmitMaximalResult(
            index, query.name,
            index.FindMums(query.sequence, search, max_matches), command);
        break;
      }
    }
  }
  return 0;
}

template <class Index>
void EmitQueries(const Index& index,
                 const std::vector<sufkit::SequenceRecord>& queries,
                 sufkit::StrandMode strands, bool count_only,
                 std::optional<std::uint64_t> max_hits) {
  std::unordered_set<std::string> names;
  if (count_only) {
    std::cout << "query_id\ttotal_hits\n";
  } else {
    std::cout << "query_id\tsequence_id\tsequence_name\tstart\tend\tstrand\n";
  }
  for (const auto& query : queries) {
    if (query.name.empty() || !names.insert(query.name).second) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "query names must be non-empty and unique");
    }
    if (count_only) {
      std::cout << query.name << '\t' << index.Count(query.sequence, strands)
                << '\n';
      continue;
    }
    sufkit::LocateOptions locate_options;
    locate_options.strands = strands;
    locate_options.max_hits = max_hits;
    const auto result = index.Locate(query.sequence, locate_options);
    for (const auto& match : result.hits) {
      const auto sequence = index.GetSequenceInfo(match.sequence_id);
      std::cout << query.name << '\t' << match.sequence_id << '\t'
                << sequence.name << '\t' << match.position << '\t'
                << match.position + match.length << '\t'
                << sufkit::ToString(match.strand) << '\n';
    }
    if (result.truncated) {
      std::cerr << "query " << query.name << " truncated: reported "
                << result.hits.size() << " of " << result.total_hits
                << " hits\n";
    }
  }
}

void EmitSaQueries(const sufkit::SuffixArray& index,
                   const std::vector<sufkit::SequenceRecord>& queries,
                   sufkit::StrandMode strands, bool count_only,
                   std::optional<std::uint64_t> max_hits,
                   sufkit::SaSearchAlgorithm algorithm) {
  std::unordered_set<std::string> names;
  if (count_only) {
    std::cout << "query_id\ttotal_hits\n";
  } else {
    std::cout << "query_id\tsequence_id\tsequence_name\tstart\tend\tstrand\n";
  }
  for (const auto& query : queries) {
    if (query.name.empty() || !names.insert(query.name).second) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "query names must be non-empty and unique");
    }
    if (count_only) {
      std::cout << query.name << '\t'
                << index.Count(query.sequence, strands, algorithm) << '\n';
      continue;
    }
    sufkit::LocateOptions locate_options;
    locate_options.strands = strands;
    locate_options.max_hits = max_hits;
    const auto result = index.Locate(query.sequence, locate_options, algorithm);
    for (const auto& match : result.hits) {
      const auto sequence = index.GetSequenceInfo(match.sequence_id);
      std::cout << query.name << '\t' << match.sequence_id << '\t'
                << sequence.name << '\t' << match.position << '\t'
                << match.position + match.length << '\t'
                << sufkit::ToString(match.strand) << '\n';
    }
    if (result.truncated) {
      std::cerr << "query " << query.name << " truncated: reported "
                << result.hits.size() << " of " << result.total_hits
                << " hits\n";
    }
  }
}

int RunQuery(const std::vector<std::string>& arguments) {
  if (arguments.size() == 1 && arguments.front() == "--help") {
    std::cout
        << "sufkit query --index PATH (--pattern ACGT | --query Q.fa[.gz])\n"
           "  [--strand forward|reverse|both] [--count-only] [--max-hits N]\n"
           "  [--algorithm auto|binary|lcp-binary|sapling-pwl|child]\n";
    return 0;
  }
  const auto options = ParseOptions(arguments,
                                    {"--index", "--pattern", "--query",
                                     "--strand", "--max-hits", "--algorithm"},
                                    {"--count-only"});
  if (options.Has("--pattern") == options.Has("--query")) {
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "exactly one of --pattern and --query is required");
  }
  std::vector<sufkit::SequenceRecord> queries;
  if (options.Has("--pattern")) {
    queries.push_back({"query_0", "", options.Require("--pattern")});
  } else {
    queries = sufkit::app::ReadFastaRecords(options.Require("--query"));
    if (queries.empty()) {
      throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                          "query FASTA contains no records");
    }
  }
  std::optional<std::uint64_t> max_hits;
  if (options.Has("--max-hits")) {
    max_hits =
        sufkit::app::ParseUnsigned(options.Require("--max-hits"), "--max-hits");
  }
  const auto strands = ParseStrand(options.ValueOr("--strand", "forward"));
  const auto path = std::filesystem::path(options.Require("--index"));
  const auto info = sufkit::InspectIndex(path);
  if (info.kind == sufkit::IndexKind::kSuffixArray) {
    const auto index = sufkit::SuffixArray::Load(path);
    EmitSaQueries(
        index, queries, strands, options.Has("--count-only"), max_hits,
        ParseSaSearchAlgorithm(options.ValueOr("--algorithm", "auto")));
  } else {
    if (options.Has("--algorithm") &&
        options.Require("--algorithm") != "auto") {
      throw sufkit::Error(
          sufkit::ErrorCode::kInvalidInput,
          "--algorithm is only supported by suffix-array indexes");
    }
    const auto index = sufkit::FmIndex::Load(path);
    EmitQueries(index, queries, strands, options.Has("--count-only"), max_hits);
  }
  return 0;
}

int RunInspect(const std::vector<std::string>& arguments) {
  if (arguments.size() == 1 && arguments.front() == "--help") {
    std::cout << "sufkit inspect --index PATH\n";
    return 0;
  }
  const auto options = ParseOptions(arguments, {"--index"}, {});
  const auto info = sufkit::InspectIndex(options.Require("--index"));
  std::cout << "key\tvalue\n"
            << "kind\t" << sufkit::ToString(info.kind) << '\n'
            << "format_version\t" << info.format_version << '\n'
            << "library_version\t" << info.library_version << '\n'
            << "backend\t" << info.backend << '\n'
            << "construction_backend\t" << info.backend << '\n'
            << "backend_signature\t" << info.backend_signature << '\n'
            << "sdsl_version\t" << info.sdsl_version << '\n'
            << "coordinate_width\t"
            << static_cast<unsigned>(info.coordinate_width) << '\n'
            << "construction_coordinate_width\t"
            << static_cast<unsigned>(info.coordinate_width) << '\n'
            << "stored_coordinate_width\t"
            << static_cast<unsigned>(info.stored_coordinate_width) << '\n'
            << "sequence_count\t" << info.sequence_count << '\n'
            << "total_bases\t" << info.total_bases << '\n'
            << "text_symbols\t" << info.text_symbols << '\n'
            << "suffix_count\t" << info.suffix_count << '\n'
            << "sa_sampling_rate\t" << info.sa_sampling_rate << '\n'
            << "ambiguous_bases\t" << info.ambiguous_bases << '\n'
            << "fingerprint\t" << std::hex << std::setfill('0') << std::setw(16)
            << info.fingerprint << std::dec << '\n'
            << "serialized_bytes\t" << info.serialized_bytes << '\n';
  if (info.kind == sufkit::IndexKind::kSuffixArray) {
    std::cout << "sa_acceleration\t" << sufkit::ToString(info.sa_acceleration)
              << '\n'
              << "sa_resource_profile\t"
              << sufkit::ToString(info.sa_resource_profile) << '\n'
              << "lcp_encoding\t" << sufkit::ToString(info.lcp_encoding)
              << '\n'
              << "auxiliary_bytes\t" << info.auxiliary_bytes << '\n'
              << "text_bytes\t" << info.text_bytes << '\n'
              << "sa_bytes\t" << info.sa_bytes << '\n'
              << "isa_bytes\t" << info.isa_bytes << '\n'
              << "lcp_bytes\t" << info.lcp_bytes << '\n'
              << "lcp_primary_bytes\t" << info.lcp_primary_bytes << '\n'
              << "lcp_overflow_anchors\t" << info.lcp_overflow_anchors
              << '\n'
              << "lcp_overflow_bytes\t" << info.lcp_overflow_bytes << '\n'
              << "lcp_guide_bytes\t" << info.lcp_guide_bytes << '\n'
              << "child_bytes\t" << info.child_bytes << '\n'
              << "resident_core_bytes\t" << info.resident_core_bytes << '\n'
              << "lookup_acceleration\t"
              << sufkit::ToString(info.sa_lookup_acceleration) << '\n'
              << "learned_index_bytes\t" << info.learned_index_bytes << '\n'
              << "learned_k\t" << info.learned_k << '\n'
              << "learned_bucket_bits\t" << info.learned_bucket_bits << '\n'
              << "learned_memory_overhead_basis_points\t"
              << info.learned_memory_overhead_basis_points << '\n';
  }
  return 0;
}

int ExitCodeFor(sufkit::ErrorCode code) {
  switch (code) {
    case sufkit::ErrorCode::kInvalidInput:
      return 2;
    case sufkit::ErrorCode::kIoError:
      return 3;
    case sufkit::ErrorCode::kCorruptIndex:
    case sufkit::ErrorCode::kVersionMismatch:
      return 4;
    case sufkit::ErrorCode::kUnsupportedBackend:
    case sufkit::ErrorCode::kBuildFailure:
      return 5;
  }
  return 5;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    PrintUsage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  std::vector<std::string> arguments;
  for (int index = 2; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  try {
    if (command == "--help" || command == "help") {
      PrintUsage(std::cout);
      return 0;
    }
    if (command == "--version") {
      std::cout << SUFKIT_VERSION_STRING << '\n';
      return 0;
    }
    if (command == "build") {
      return RunBuild(arguments);
    }
    if (command == "query") {
      return RunQuery(arguments);
    }
    if (command == "right-maximal") {
      return RunRightMaximal(arguments);
    }
    if (command == "mem") {
      return RunMaximal(arguments, MaximalCommand::kMem);
    }
    if (command == "mam") {
      return RunMaximal(arguments, MaximalCommand::kMam);
    }
    if (command == "smem") {
      return RunMaximal(arguments, MaximalCommand::kSmem);
    }
    if (command == "mum") {
      return RunMaximal(arguments, MaximalCommand::kMum);
    }
    if (command == "inspect") {
      return RunInspect(arguments);
    }
    if (command == "bench") {
      return sufkit::app::run_benchmark(arguments);
    }
    if (command == "__benchmark-worker") {
      return sufkit::app::run_benchmark_worker(arguments);
    }
    if (command == "__maximal-benchmark-worker") {
      return sufkit::app::run_maximal_benchmark_worker(arguments);
    }
    throw sufkit::Error(sufkit::ErrorCode::kInvalidInput,
                        "unknown command: " + command);
  } catch (const sufkit::Error& error) {
    std::cerr << "sufkit: " << sufkit::ToString(error.Code()) << ": "
              << error.what() << '\n';
    return ExitCodeFor(error.Code());
  } catch (const std::exception& error) {
    std::cerr << "sufkit: unexpected error: " << error.what() << '\n';
    return 5;
  }
}
