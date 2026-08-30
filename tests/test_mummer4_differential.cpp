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

std::vector<Match> ReadMummer(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open MUMmer4 output");
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
      throw std::runtime_error("invalid MUMmer4 output line: " + line);
    }
    matches.emplace_back(query, reference, reference_position - 1,
                         query_position - 1, length);
  }
  std::sort(matches.begin(), matches.end());
  return matches;
}

template <class Result>
void AppendSufkit(const std::string& query, const Result& result,
                  const sufkit::SuffixArray& index,
                  std::vector<Match>& matches) {
  for (const auto& match : result.matches) {
    matches.emplace_back(query, index.GetSequenceInfo(match.sequence_id).name,
                         match.reference_position, match.query_position,
                         match.length);
  }
}

void RequireEqual(const std::vector<Match>& sufkit_matches,
                  const std::vector<Match>& mummer_matches,
                  const char* label) {
  if (sufkit_matches == mummer_matches) {
    return;
  }
  std::cerr << label << " differs: sufkit=" << sufkit_matches.size()
            << " mummer4=" << mummer_matches.size() << '\n';
  const auto count = std::min(sufkit_matches.size(), mummer_matches.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (sufkit_matches[index] != mummer_matches[index]) {
      std::cerr << "first differing tuple is row " << index << '\n';
      break;
    }
  }
  throw std::runtime_error(std::string(label) + " mismatch");
}

}  // namespace

int main() {
  const auto root = std::filesystem::path("/tmp") /
                    ("sufkit-mummer4-diff-" +
                     std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(root);
  try {
    const auto reference_path = root / "reference.fa";
    const auto query_path = root / "query.fa";
    const auto mem_path = root / "mem.out";
    const auto mam_path = root / "mam.out";
    const auto mum_path = root / "mum.out";
    {
      std::ofstream reference(reference_path);
      reference << ">ref\n"
                   "TTACGTACGTGGGCCACGTACGACCCGATTACATTAACCGGTTA\n";
      std::ofstream query(query_path);
      query << ">q1\nAAACGTACGTTTT\n>q2\nCCGATTACAGG\n";
    }
    const auto common =
        Quote(SUFKIT_TEST_MUMMER4) +
        " -n -F -l 4 -k 1 -skip 1 -kmer 0 -threads 1 -qthreads 1 " +
        Quote(reference_path) + " " + Quote(query_path);
    if (std::system((common + " -maxmatch > " + Quote(mem_path)).c_str()) !=
            0 ||
        std::system((common + " -mumreference > " + Quote(mam_path)).c_str()) !=
            0 ||
        std::system((common + " -mum > " + Quote(mum_path)).c_str()) !=
            0) {
      throw std::runtime_error("MUMmer4 command failed");
    }

    const auto reference = sufkit::GenomeReference::FromRecords({
        {"ref", "", "TTACGTACGTGGGCCACGTACGACCCGATTACATTAACCGGTTA"}});
    sufkit::SuffixArrayBuildOptions build;
    build.acceleration = sufkit::SaAcceleration::kFull;
    const auto index = sufkit::SuffixArray::Build(reference, build);
    const std::map<std::string, std::string> queries{
        {"q1", "AAACGTACGTTTT"}, {"q2", "CCGATTACAGG"}};

    sufkit::MemOptions mem_options;
    mem_options.min_length = 4;
    mem_options.algorithm = sufkit::MemSearchAlgorithm::kFull;
    mem_options.skip_multiplier = 1;
    std::vector<Match> mems;
    for (const auto& [name, query] : queries) {
      AppendSufkit(name, index.FindMems(query, mem_options), index, mems);
    }
    std::sort(mems.begin(), mems.end());
    RequireEqual(mems, ReadMummer(mem_path), "MEM");

    sufkit::MamOptions mam_options;
    mam_options.min_length = 4;
    mam_options.algorithm = sufkit::MemSearchAlgorithm::kFull;
    std::vector<Match> mams;
    for (const auto& [name, query] : queries) {
      AppendSufkit(name, index.FindMams(query, mam_options), index, mams);
    }
    std::sort(mams.begin(), mams.end());
    RequireEqual(mams, ReadMummer(mam_path), "reference-MAM");

    sufkit::MumOptions mum_options;
    mum_options.min_length = 4;
    mum_options.algorithm = sufkit::MemSearchAlgorithm::kFull;
    std::vector<Match> mums;
    for (const auto& [name, query] : queries) {
      AppendSufkit(name, index.FindMums(query, mum_options), index, mums);
    }
    std::sort(mums.begin(), mums.end());
    RequireEqual(mums, ReadMummer(mum_path), "MUM");
  } catch (const std::exception& error) {
    std::cerr << "MUMmer4 differential failed: " << error.what() << '\n';
    std::filesystem::remove_all(root);
    return 1;
  }
  std::filesystem::remove_all(root);
  std::cout << "MUMmer4 MEM/MAM/MUM differential passed\n";
  return 0;
}
