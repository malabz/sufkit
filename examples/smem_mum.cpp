// SPDX-License-Identifier: MIT

#include <iostream>
#include <string_view>

#include <sufkit/sufkit.hpp>

int main() {
  auto reference = sufkit::GenomeReference::FromRecords(
      {{"chr1", "repeated seed and unique match",
        "TTTACGTACGTTTTGATTACACCC"},
       {"chr2", "second seed occurrence", "GGGACGTACGTAAA"}});
  auto index = sufkit::SuffixArray::Build(reference);
  constexpr std::string_view kQuery = "CCACGTACGTGGGCTGATTACAGG";

  sufkit::SmemOptions smem_options;
  smem_options.min_length = 8;
  smem_options.min_occurrences = 2;
  const auto smems = index.FindSmems(kQuery, smem_options);
  std::cout << "SMEM intervals=" << smems.total_smems
            << " coordinates=" << smems.total_matches << '\n';
  for (const auto& match : smems.matches) {
    const auto sequence = index.GetSequenceInfo(match.sequence_id);
    std::cout << "SMEM\t" << sequence.name << '\t'
              << match.reference_position << '\t' << match.query_position
              << '\t' << match.length << '\t'
              << match.reference_occurrences << '\t'
              << sufkit::ToString(match.strand) << '\n';
  }

  sufkit::MumOptions mum_options;
  mum_options.min_length = 8;
  const auto mums = index.FindMums(kQuery, mum_options);
  std::cout << "MUM matches=" << mums.total_matches << '\n';
  for (const auto& match : mums.matches) {
    const auto sequence = index.GetSequenceInfo(match.sequence_id);
    std::cout << "MUM\t" << sequence.name << '\t'
              << match.reference_position << '\t' << match.query_position
              << '\t' << match.length << '\t'
              << sufkit::ToString(match.strand) << '\n';
  }

  if (smems.total_smems == 0 || smems.total_matches < 2 ||
      mums.total_matches == 0) {
    std::cerr << "example data did not produce the expected match classes\n";
    return 1;
  }
  return 0;
}
