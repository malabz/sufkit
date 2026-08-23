#include <sufkit/sufkit.hpp>

#include <iostream>
#include <string_view>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sufkit_example_fm_batch reference.fa[.gz]\n";
        return 2;
    }

    auto reference = sufkit::GenomeReference::from_fasta(argv[1]);
    sufkit::FmIndexBuildOptions build;
    build.backend = sufkit::FmBackend::sdsl_csa_wt_huff;
    auto index = sufkit::FmIndex::build(reference, build);

    const std::vector<std::string_view> patterns{"ACGT", "GATTACA", "TTTT"};
    sufkit::FmBatchOptions batch;
    batch.strands = sufkit::StrandMode::both;
    batch.batch_width = 16;
    const auto counts = index.count_batch(patterns, batch);
    for (std::size_t i = 0; i < patterns.size(); ++i) {
        std::cout << patterns[i] << '\t' << counts[i] << '\n';
    }
}
