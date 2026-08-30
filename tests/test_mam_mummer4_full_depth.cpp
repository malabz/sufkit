// SPDX-License-Identifier: MIT

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <sufkit/sufkit.hpp>

namespace {

using Match = std::tuple<std::string, std::string, std::uint64_t,
                         std::uint64_t, std::uint64_t>;

std::string Quote(const std::filesystem::path& path) {
  std::string result = "'";
  for (const char value : path.string()) {
    result += value == '\'' ? "'\\''" : std::string(1, value);
  }
  return result + "'";
}

std::uint64_t NextRandom(std::uint64_t& state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 2685821657736338717ULL;
}

std::string RandomDna(std::uint64_t& state, std::size_t length) {
  std::string result(length, 'A');
  for (char& base : result) {
    base = "ACGT"[NextRandom(state) % 4];
  }
  return result;
}

std::vector<Match> ReadMummer(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open MUMmer4 MAM output");
  }
  std::vector<Match> matches;
  std::string query;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    if (line.front() == '>') {
      std::istringstream header(line.substr(1));
      header >> query;
      continue;
    }
    std::istringstream fields(line);
    std::string reference;
    std::uint64_t reference_position = 0;
    std::uint64_t query_position = 0;
    std::uint64_t length = 0;
    if (!(fields >> reference >> reference_position >> query_position >>
          length)) {
      throw std::runtime_error("invalid MUMmer4 MAM output line: " + line);
    }
    matches.emplace_back(query, reference, reference_position - 1,
                         query_position - 1, length);
  }
  std::sort(matches.begin(), matches.end());
  return matches;
}

std::vector<Match> RunSufkit(
    const sufkit::SuffixArray& index,
    const std::map<std::string, std::string>& queries,
    sufkit::MemSearchAlgorithm algorithm) {
  sufkit::MamOptions options;
  options.min_length = 16;
  options.algorithm = algorithm;
  options.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  std::vector<Match> matches;
  for (const auto& [name, query] : queries) {
    const auto result = index.FindMams(query, options);
    for (const auto& match : result.matches) {
      matches.emplace_back(
          name, index.GetSequenceInfo(match.sequence_id).name,
          match.reference_position, match.query_position, match.length);
    }
  }
  std::sort(matches.begin(), matches.end());
  return matches;
}

void RequireEqual(const std::vector<Match>& observed,
                  const std::vector<Match>& expected,
                  const std::string& label) {
  if (observed == expected) {
    return;
  }
  std::cerr << label << " differs: sufkit=" << observed.size()
            << " mummer4=" << expected.size() << '\n';
  const auto common = std::min(observed.size(), expected.size());
  for (std::size_t index = 0; index < common; ++index) {
    if (observed[index] != expected[index]) {
      std::cerr << "first differing tuple is row " << index << '\n';
      break;
    }
  }
  throw std::runtime_error(label + " mismatch");
}

}  // namespace

int main() {
  const auto root = std::filesystem::path("/tmp") /
                    ("sufkit-mam-full-depth-mummer4-" +
                     std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(root);
  try {
    std::uint64_t state = 2026083004ULL;
    auto reference_sequence = RandomDna(state, 8192);
    const std::string repeated = "ACGTTGCAACGATTCGGTACCTAGGCTAACGT";
    reference_sequence.replace(700, repeated.size(), repeated);
    reference_sequence.replace(6200, repeated.size(), repeated);

    std::map<std::string, std::string> queries;
    queries["q_exact"] = reference_sequence.substr(211, 800);
    queries["q_mutated"] = reference_sequence.substr(1700, 1200);
    for (std::size_t position = 83;
         position < queries["q_mutated"].size(); position += 97) {
      char& base = queries["q_mutated"][position];
      base = base == 'A' ? 'C' : 'A';
    }
    queries["q_joined"] = reference_sequence.substr(4000, 350) +
                           reference_sequence.substr(5100, 420);
    queries["q_repeat"] = "TT" + repeated + "GG" + repeated + "CC";

    const auto reference_path = root / "reference.fa";
    const auto query_path = root / "query.fa";
    const auto output_path = root / "mam.out";
    {
      std::ofstream reference(reference_path);
      reference << ">ref\n" << reference_sequence << '\n';
      std::ofstream query(query_path);
      for (const auto& [name, sequence] : queries) {
        query << '>' << name << '\n' << sequence << '\n';
      }
    }

    const auto command =
        Quote(SUFKIT_TEST_MUMMER4) +
        " -mumreference -n -F -l 16 -k 1 -skip 1 -kmer 0 -threads 1 "
        "-qthreads 1 " +
        Quote(reference_path) + " " + Quote(query_path) + " > " +
        Quote(output_path);
    if (std::system(command.c_str()) != 0) {
      throw std::runtime_error("MUMmer4 MAM command failed");
    }
    const auto expected = ReadMummer(output_path);

    auto build = sufkit::FastSuffixArrayBuildOptions();
    build.acceleration = sufkit::SaAcceleration::kFull;
    const auto index = sufkit::SuffixArray::Build(
        sufkit::GenomeReference::FromRecords(
            {{"ref", "", reference_sequence}}),
        build);
    for (const auto algorithm :
         {sufkit::MemSearchAlgorithm::kBaseline,
          sufkit::MemSearchAlgorithm::kLcp,
          sufkit::MemSearchAlgorithm::kChild,
          sufkit::MemSearchAlgorithm::kSuffixLink,
          sufkit::MemSearchAlgorithm::kFull,
          sufkit::MemSearchAlgorithm::kAutoSelect}) {
      RequireEqual(RunSufkit(index, queries, algorithm), expected,
                   "full-depth reference-MAM algorithm " +
                       std::to_string(static_cast<unsigned int>(algorithm)));
    }
  } catch (const std::exception& error) {
    std::cerr << "Full-depth MUMmer4 MAM differential failed: " << error.what()
              << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
  std::filesystem::remove_all(root);
  std::cout << "Full-depth MUMmer4 MAM differential passed\n";
  return 0;
}
