// SPDX-License-Identifier: MIT

#include <sufkit/sufkit.hpp>

int main() {
  auto reference =
      sufkit::GenomeReference::FromRecords({{"chr1", "", "ACGTACGT"}});
  auto index = sufkit::SuffixArray::Build(reference);
  return index.Count("ACGT") == 2 ? 0 : 1;
}
