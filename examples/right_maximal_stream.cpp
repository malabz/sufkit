#include <sufkit/sufkit.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sufkit_example_right_maximal_stream reference.fa[.gz]\n";
        return 2;
    }

    auto reference = sufkit::GenomeReference::from_fasta(argv[1]);
    auto index = sufkit::SuffixArray::build(reference);

    constexpr std::string_view query = "GGGACGTACGTNNNGATTACA";
    sufkit::RightMaximalOptions options;
    options.min_length = 4;
    options.strands = sufkit::StrandMode::both;
    index.for_each_right_maximal_match(query, options, [&](const sufkit::RightMaximalMatch& match) {
        std::cout << match.sequence_id << '\t'
                  << match.reference_position << '\t'
                  << match.query_position << '\t'
                  << match.length << '\t'
                  << sufkit::to_string(match.strand) << '\n';
    });
}
