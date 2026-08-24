// SPDX-License-Identifier: MIT

#include <iostream>
#include <string_view>

#include <sufkit/sufkit.hpp>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr
        << "usage: sufkit_example_right_maximal_stream reference.fa[.gz]\n";
    return 2;
  }

  auto reference = sufkit::GenomeReference::FromFasta(argv[1]);
  auto index = sufkit::SuffixArray::Build(reference);

  constexpr std::string_view kQuery = "GGGACGTACGTNNNGATTACA";
  sufkit::RightMaximalOptions options;
  options.min_length = 4;
  options.strands = sufkit::StrandMode::kBoth;
  index.ForEachRightMaximalMatch(
      kQuery, options, [&](const sufkit::RightMaximalMatch& match) {
        std::cout << match.sequence_id << '\t' << match.reference_position
                  << '\t' << match.query_position << '\t' << match.length
                  << '\t' << sufkit::ToString(match.strand) << '\n';
      });
}
