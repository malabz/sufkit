#include <iostream>

#include <sufkit/sufkit.hpp>

int main() {
    auto reference = sufkit::GenomeReference::from_records({
        {"chr1", "example", "ACGTNACGT"},
        {"chr2", "example", "TTTACGTAAA"}
    });
    auto index = sufkit::FmIndex::build(reference);
    const auto result = index.locate("ACGT");
    for (const auto& match : result.hits) {
        std::cout << reference.sequence_info(match.sequence_id).name << '\t'
                  << match.position << '\n';
    }
    return 0;
}

