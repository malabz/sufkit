// SPDX-License-Identifier: MIT

#include <iostream>

#include <sufkit/sufkit.hpp>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sufkit_example_exact_sa reference.fa[.gz]\n";
    return 2;
  }

  auto reference = sufkit::GenomeReference::FromFasta(argv[1]);
  sufkit::SuffixArrayBuildOptions build;
  build.acceleration = sufkit::SaAcceleration::kLcpSuffixLink;
  auto index = sufkit::SuffixArray::Build(reference, build);

  sufkit::LocateOptions locate;
  locate.strands = sufkit::StrandMode::kBoth;
  locate.max_hits = 10;
  const auto result = index.Locate("ACGT", locate);
  std::cout << "total=" << result.total_hits
            << " retained=" << result.hits.size() << '\n';
  for (const auto& hit : result.hits) {
    const auto sequence = index.GetSequenceInfo(hit.sequence_id);
    std::cout << sequence.name << '\t' << hit.position << '\t'
              << sufkit::ToString(hit.strand) << '\n';
  }
}
