// SPDX-License-Identifier: MIT

#include <iostream>

#include <sufkit/sufkit.hpp>

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: sufkit_example_inspect_error reference.sufidx\n";
    return 2;
  }

  try {
    const auto info = sufkit::InspectIndex(argv[1]);
    std::cout << sufkit::ToString(info.kind) << '\t' << info.backend << '\t'
              << info.format_version << '\t' << info.total_bases << '\n';
  } catch (const sufkit::Error& error) {
    std::cerr << sufkit::ToString(error.Code()) << ": " << error.what() << '\n';
    return 1;
  }
}
