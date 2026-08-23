// SPDX-License-Identifier: MIT

#include <iostream>

#include <sufkit/sufkit.hpp>

int main() {
  auto reference = sufkit::GenomeReference::FromRecords(
      {{"chr1", "example", "ACGTNACGT"}, {"chr2", "example", "TTTACGTAAA"}});
  auto index = sufkit::FmIndex::Build(reference);
  const auto result = index.Locate("ACGT");
  for (const auto& match : result.hits) {
    std::cout << reference.GetSequenceInfo(match.sequence_id).name << '\t'
              << match.position << '\n';
  }
  return 0;
}
