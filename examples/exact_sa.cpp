#include <sufkit/sufkit.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sufkit_example_exact_sa reference.fa[.gz]\n";
        return 2;
    }

    auto reference = sufkit::GenomeReference::from_fasta(argv[1]);
    sufkit::SuffixArrayBuildOptions build;
    build.acceleration = sufkit::SaAcceleration::lcp_suffix_link;
    auto index = sufkit::SuffixArray::build(reference, build);

    sufkit::LocateOptions locate;
    locate.strands = sufkit::StrandMode::both;
    locate.max_hits = 10;
    const auto result = index.locate("ACGT", locate);
    std::cout << "total=" << result.total_hits
              << " retained=" << result.hits.size() << '\n';
    for (const auto& hit : result.hits) {
        const auto sequence = index.sequence_info(hit.sequence_id);
        std::cout << sequence.name << '\t' << hit.position << '\t'
                  << sufkit::to_string(hit.strand) << '\n';
    }
}
