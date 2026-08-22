#include <sufkit/sufkit.hpp>

int main() {
    auto reference = sufkit::GenomeReference::from_records({
        {"chr1", "", "ACGTACGT"}
    });
    auto index = sufkit::SuffixArray::build(reference);
    return index.count("ACGT") == 2 ? 0 : 1;
}

